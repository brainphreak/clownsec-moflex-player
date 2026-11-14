/* FILE* over HTTP Range requests. See httpfile.h.
 *
 * The file is read in 64 KB blocks, each one Range request on ONE kept-alive connection, by a
 * worker thread that fills a small block cache. A read waits for its block (asking for it ahead
 * of everything else); sequential reading keeps ~1.5 MB fetched ahead. Seeking costs a block, not
 * a new download: a moflex seek binary-searches the file, touching ~20 places, and opening a fresh
 * stream at each took minutes. */
#define _GNU_SOURCE
#include "httpfile.h"
#include "downloader.h"
#include <3ds.h>
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define HB_SIZE     (64 * 1024)
#define HB_SLOTS    48              /* 3 MB of blocks */
#define HB_AHEAD    24              /* fetched ahead while reading straight through (~1.5 MB) */
#define HB_AHEAD_J  2               /* ...and just after a jump, until reading settles */
#define HF_STALL_MS 20000           /* a block that doesn't arrive in this long: the read fails */

enum { SL_EMPTY = 0, SL_PENDING, SL_READY, SL_FAILED };
typedef struct { long long blk; int state, len; u64 used; uint8_t *data; } Slot;

typedef struct {
    char *url;
    long long size, pos;            /* the FILE's position */
    int nslots;
    Slot *slots; uint8_t *mem;
    LightLock lk;
    Thread th;
    volatile int stop;
    long long want;                 /* the block a read is waiting for (-1: none) */
    long long cur;                  /* where reading is: the prefetch window starts here */
    long long last; int seq;        /* sequential-read detection */
    CURL *e; Slot *fill;            /* worker-only */
} HFile;

/* ---- the file's size: one HEAD (cached for the last few URLs; a file is opened several times) ---- */
static struct { char url[256]; long long size; } s_sizes[4];
static int s_sizes_next;

static long long hf_size(const char *url) {
    for (int i = 0; i < 4; i++) if (s_sizes[i].url[0] && !strcmp(s_sizes[i].url, url)) return s_sizes[i].size;
    if (!downloader_init()) return -1;
    CURL *e = curl_easy_init(); if (!e) return -1;
    curl_easy_setopt(e, CURLOPT_URL, url);
    curl_easy_setopt(e, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(e, CURLOPT_TIMEOUT, 15L);
    curl_off_t len = -1; long code = 0;
    if (curl_easy_perform(e) == CURLE_OK) {
        curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &code);
        curl_easy_getinfo(e, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &len);
    }
    curl_easy_cleanup(e);
    if (code != 200 || len < 0) return -1;
    snprintf(s_sizes[s_sizes_next].url, sizeof s_sizes[0].url, "%s", url);
    s_sizes[s_sizes_next].size = (long long)len;
    s_sizes_next = (s_sizes_next + 1) % 4;
    return (long long)len;
}

/* ---- the worker ---- */
static size_t on_block(char *p, size_t sz, size_t nm, void *u) {
    HFile *h = (HFile *)u; size_t k = sz * nm;
    if (h->stop) return 0;
    Slot *s = h->fill;
    if ((size_t)s->len + k > HB_SIZE) return 0;          /* more than asked for: the range was ignored */
    memcpy(s->data + s->len, p, k); s->len += (int)k;
    return k;
}
static int on_abort(void *u, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
    (void)a; (void)b; (void)c; (void)d; return ((HFile *)u)->stop;
}
static int slot_of(HFile *h, long long blk) {
    for (int i = 0; i < h->nslots; i++)
        if (h->slots[i].state != SL_EMPTY && h->slots[i].blk == blk) return i;
    return -1;
}
/* the next block to fetch (caller holds lk): the one a read waits for, else the next missing one
 * in the read-ahead window. Claims a slot for it; -1 when there's nothing to do. */
static int next_job(HFile *h) {
    long long nblk = (h->size + HB_SIZE - 1) / HB_SIZE, target = -1;
    if (h->want >= 0 && slot_of(h, h->want) < 0) target = h->want;
    if (target < 0) {
        int ahead = h->seq >= 3 ? HB_AHEAD : HB_AHEAD_J;
        for (long long b = h->cur; b < h->cur + ahead && b < nblk; b++)
            if (slot_of(h, b) < 0) { target = b; break; }
    }
    if (target < 0) return -1;
    int best = -1; u64 oldest = ~0ULL;               /* an empty slot, else the least recently used
                                                      * one outside the window */
    for (int i = 0; i < h->nslots; i++) {
        Slot *s = &h->slots[i];
        if (s->state == SL_PENDING) continue;
        if (s->state == SL_EMPTY) { best = i; break; }
        if (s->blk >= h->cur - 1 && s->blk < h->cur + HB_AHEAD) continue;
        if (s->used < oldest) { oldest = s->used; best = i; }
    }
    if (best < 0) return -1;
    Slot *s = &h->slots[best];
    s->blk = target; s->state = SL_PENDING; s->len = 0; s->used = osGetTime();
    return best;
}
static void worker(void *arg) {
    HFile *h = (HFile *)arg;
    h->e = curl_easy_init();
    if (h->e) {
        curl_easy_setopt(h->e, CURLOPT_URL, h->url);
        curl_easy_setopt(h->e, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(h->e, CURLOPT_TCP_NODELAY, 1L);
        curl_easy_setopt(h->e, CURLOPT_CONNECTTIMEOUT, 8L);
        curl_easy_setopt(h->e, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(h->e, CURLOPT_LOW_SPEED_TIME, 15L);
        curl_easy_setopt(h->e, CURLOPT_BUFFERSIZE, 64L * 1024L);
        curl_easy_setopt(h->e, CURLOPT_WRITEFUNCTION, on_block);
        curl_easy_setopt(h->e, CURLOPT_WRITEDATA, h);
        curl_easy_setopt(h->e, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(h->e, CURLOPT_XFERINFOFUNCTION, on_abort);
        curl_easy_setopt(h->e, CURLOPT_XFERINFODATA, h);
    }
    while (!h->stop && h->e) {
        LightLock_Lock(&h->lk);
        int i = next_job(h);
        LightLock_Unlock(&h->lk);
        if (i < 0) { svcSleepThread(3 * 1000 * 1000LL); continue; }   /* idle: no socket wait */
        Slot *s = &h->slots[i];
        long long a = s->blk * HB_SIZE, b = a + HB_SIZE - 1;
        if (b >= h->size) b = h->size - 1;
        char range[48]; snprintf(range, sizeof range, "%lld-%lld", a, b);
        h->fill = s;
        curl_easy_setopt(h->e, CURLOPT_RANGE, range);
        CURLcode rc = curl_easy_perform(h->e);                /* the same connection, kept alive */
        long code = 0; curl_easy_getinfo(h->e, CURLINFO_RESPONSE_CODE, &code);
        int ok = rc == CURLE_OK && (code == 206 || (code == 200 && a == 0)) && s->len == (int)(b - a + 1);
        LightLock_Lock(&h->lk);
        s->state = ok ? SL_READY : SL_FAILED;
        LightLock_Unlock(&h->lk);
        if (!ok && !h->stop) svcSleepThread(200 * 1000 * 1000LL);   /* don't hammer a failing server */
    }
    if (h->e) curl_easy_cleanup(h->e);
}

/* ---- the FILE side ---- */
static ssize_t hf_read(void *c, char *buf, size_t n) {
    HFile *h = (HFile *)c;
    if (h->pos >= h->size) return 0;
    long long blk = h->pos / HB_SIZE;
    int off = (int)(h->pos % HB_SIZE), fails = 0;
    u64 t0 = osGetTime();
    LightLock_Lock(&h->lk);
    h->seq = (blk == h->last || blk == h->last + 1) ? h->seq + 1 : 0;   /* straight through, or a jump */
    h->last = blk; h->cur = blk;
    for (;;) {
        int i = slot_of(h, blk);
        if (i >= 0 && h->slots[i].state == SL_READY) {
            Slot *s = &h->slots[i];
            size_t k = (size_t)(s->len - off); if (k > n) k = n;
            memcpy(buf, s->data + off, k);
            s->used = osGetTime();
            h->want = -1;
            h->pos += (long long)k;
            LightLock_Unlock(&h->lk);
            return (ssize_t)k;
        }
        if (i >= 0 && h->slots[i].state == SL_FAILED) {
            h->slots[i].state = SL_EMPTY;                 /* ask again, a couple of times */
            if (++fails > 3) { h->want = -1; LightLock_Unlock(&h->lk); return -1; }
        }
        h->want = blk;
        LightLock_Unlock(&h->lk);
        if (osGetTime() - t0 > HF_STALL_MS) { LightLock_Lock(&h->lk); h->want = -1; LightLock_Unlock(&h->lk); return -1; }
        svcSleepThread(2 * 1000 * 1000LL);
        LightLock_Lock(&h->lk);
    }
}

static int hf_seek(void *c, off_t *off, int whence) {
    HFile *h = (HFile *)c;
    long long np = whence == SEEK_SET ? (long long)*off
                 : whence == SEEK_CUR ? h->pos + (long long)*off
                 : h->size + (long long)*off;
    if (np < 0) return -1;
    h->pos = np;                                           /* the next read fetches what it needs */
    *off = (off_t)np;
    return 0;
}

static int hf_close(void *c) {
    HFile *h = (HFile *)c;
    h->stop = 1;
    if (h->th) { threadJoin(h->th, U64_MAX); threadFree(h->th); }
    free(h->mem); free(h->slots); free(h->url); free(h);
    return 0;
}

FILE *httpfile_open(const char *url, long long size, size_t ring_bytes) {
    APT_SetAppCpuTimeLimit(30);             /* the fetch thread runs on the system core */
    if (size < 0) size = hf_size(url);
    if (size < 0) return NULL;
    if (!downloader_init()) return NULL;
    HFile *h = (HFile *)calloc(1, sizeof *h);
    if (!h) return NULL;
    h->nslots = ring_bytes ? (int)(ring_bytes / HB_SIZE) : HB_SLOTS;
    if (h->nslots < HB_AHEAD + 8) h->nslots = HB_AHEAD + 8;
    h->url = strdup(url); h->size = size; h->want = -1; h->last = -10;
    h->slots = (Slot *)calloc((size_t)h->nslots, sizeof(Slot));
    h->mem = (uint8_t *)malloc((size_t)h->nslots * HB_SIZE);
    if (!h->url || !h->slots || !h->mem) { free(h->mem); free(h->slots); free(h->url); free(h); return NULL; }
    for (int i = 0; i < h->nslots; i++) h->slots[i].data = h->mem + (size_t)i * HB_SIZE;
    LightLock_Init(&h->lk);
    /* the system core: the moflex decoder owns core 2 and would starve a lowest-priority fetch */
    h->th = threadCreate(worker, h, 64 * 1024, 0x3F, 1, false);
    if (!h->th) { s32 prio = 0x30; svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
                  h->th = threadCreate(worker, h, 64 * 1024, prio - 1, -2, false); }
    if (!h->th) { free(h->mem); free(h->slots); free(h->url); free(h); return NULL; }
    cookie_io_functions_t io = { hf_read, NULL, hf_seek, hf_close };
    FILE *f = fopencookie(h, "rb", io);
    if (!f) { hf_close(h); return NULL; }
    setvbuf(f, NULL, _IOFBF, 32 * 1024);
    return f;
}
