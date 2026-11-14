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
#define HB_SLOTS    96              /* 6 MB: what was just played stays, so a seek to here (an audio
                                     * track switch scans ~20 s back for a keyframe) reads memory */
#define HB_SLOTS_2ND 32             /* a second file open at once (the trailer's audio): 2 MB */
#define HB_AHEAD    24              /* fetched ahead while reading straight through (~1.5 MB) */
#define HB_AHEAD_J  2               /* ...and just after a jump, until reading settles */
#define HF_STALL_MS 20000           /* a block that doesn't arrive in this long: the read fails */
#define HB_BATCH    8               /* steady read-ahead fetches up to this many blocks per request
                                     * (512 KB): an Old 3DS pays a lot per request */
#define HF_LOG      "sdmc:/moflex_player/bigtop_log.txt"

/* why the last network read gave up: 0 = it didn't, 1 = the viewer held B, 2 = the server went quiet */
static volatile int g_hf_gave_up;
/* the player's say while a read waits: poll=1 may look at the buttons; non-zero = stop this read
 * now (a new seek target, B). Only the player knows when that's safe and what it means. */
static int (*g_hf_hook)(int poll) = NULL;
void httpfile_set_wait_hook(int (*fn)(int poll)) { g_hf_hook = fn; }
static int g_hf_open;               /* files open now: only the first gets the big cache */
int  httpfile_gave_up(void) { return g_hf_gave_up; }
void httpfile_reset(void) { g_hf_gave_up = 0; }

enum { SL_EMPTY = 0, SL_PENDING, SL_READY, SL_FAILED };
typedef struct { long long blk; int state, len; u64 used; uint8_t *data; } Slot;

/* The player opens a video twice in a row -- a quick look (2D or 3D?), then playback. The first
 * one's blocks are kept here for the second instead of downloading the start again. */
static struct { char *url; Slot *slots; uint8_t *mem; int nslots; u64 t; } g_park;
void httpfile_drop_cache(void) {
    free(g_park.slots); free(g_park.mem); free(g_park.url);
    memset(&g_park, 0, sizeof g_park);
}

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
    int aborted;                    /* the viewer gave up waiting: every read fails from now on */
    /* worker-only */
    CURL *e; Slot *fills[HB_BATCH]; int nfill, fill_i;
    /* for the log */
    u64 t_open, wait_ms, wait_max, fetch_ms; int waits, fetches, failed; long long bytes;
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
/* the response fills the batch's slots in order, each up to a whole block */
static size_t on_block(char *p, size_t sz, size_t nm, void *u) {
    HFile *h = (HFile *)u; size_t k = sz * nm, done = 0;
    if (h->stop) return 0;
    while (done < k) {
        if (h->fill_i >= h->nfill) return 0;            /* more than asked for: the range was ignored */
        Slot *s = h->fills[h->fill_i];
        size_t room = HB_SIZE - (size_t)s->len, n = k - done < room ? k - done : room;
        memcpy(s->data + s->len, p + done, n); s->len += (int)n; done += n;
        if (s->len == HB_SIZE) h->fill_i++;
    }
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
static int claim(HFile *h, long long target);
/* the next blocks to fetch (caller holds lk): the one a read waits for, alone (quick); else the
 * next missing run in the read-ahead window, up to HB_BATCH in one request. Fills h->fills;
 * returns how many (0 = nothing to do). */
static int next_jobs(HFile *h) {
    long long nblk = (h->size + HB_SIZE - 1) / HB_SIZE;
    h->nfill = 0; h->fill_i = 0;
    if (h->want >= 0 && slot_of(h, h->want) < 0) {
        int i = claim(h, h->want);
        if (i >= 0) h->fills[h->nfill++] = &h->slots[i];
        return h->nfill;
    }
    /* the window grows with straight reading: a short look at the file (the player's 2D/3D probe)
     * mustn't pull 1.5 MB it will never read -- that was ~7 s of waiting on an Old 3DS */
    int ahead = h->seq / 2; if (ahead < HB_AHEAD_J) ahead = HB_AHEAD_J; if (ahead > HB_AHEAD) ahead = HB_AHEAD;
    int batch = h->seq >= 16 ? HB_BATCH : h->seq >= 3 ? 4 : 1;
    for (long long b = h->cur; b < h->cur + ahead && b < nblk; b++) {
        if (slot_of(h, b) >= 0) { if (h->nfill) break; continue; }   /* a run of missing blocks only */
        int i = claim(h, b);
        if (i < 0) break;
        h->fills[h->nfill++] = &h->slots[i];
        if (h->nfill == batch) break;
    }
    return h->nfill;
}
/* a slot for block target: an empty one, else the least recently used one outside the window */
static int claim(HFile *h, long long target) {
    int best = -1; u64 oldest = ~0ULL;
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
        int n = next_jobs(h);
        LightLock_Unlock(&h->lk);
        if (!n) { svcSleepThread(3 * 1000 * 1000LL); continue; }   /* idle: no socket wait */
        long long a = h->fills[0]->blk * HB_SIZE, b = (h->fills[n - 1]->blk + 1) * HB_SIZE - 1;
        if (b >= h->size) b = h->size - 1;
        char range[48]; snprintf(range, sizeof range, "%lld-%lld", a, b);
        curl_easy_setopt(h->e, CURLOPT_RANGE, range);
        u64 t = osGetTime();
        CURLcode rc = curl_easy_perform(h->e);                /* the same connection, kept alive */
        long code = 0; curl_easy_getinfo(h->e, CURLINFO_RESPONSE_CODE, &code);
        int got = 0; for (int j = 0; j < n; j++) got += h->fills[j]->len;
        int ok = rc == CURLE_OK && (code == 206 || (code == 200 && a == 0)) && got == (int)(b - a + 1);
        LightLock_Lock(&h->lk);
        for (int j = 0; j < n; j++) h->fills[j]->state = ok ? SL_READY : SL_FAILED;
        h->fetches++; h->fetch_ms += osGetTime() - t; h->bytes += got;
        if (!ok && !h->stop) h->failed++;      /* a request cut off by closing the file isn't a failure */
        LightLock_Unlock(&h->lk);
        if (!ok && !h->stop) svcSleepThread(200 * 1000 * 1000LL);   /* don't hammer a failing server */
    }
    if (h->e) curl_easy_cleanup(h->e);
}

/* ---- the FILE side ---- */
static ssize_t hf_read(void *c, char *buf, size_t n) {
    HFile *h = (HFile *)c;
    if (h->aborted) return -1;
    if (g_hf_hook && g_hf_hook(0)) return -1;            /* a seek being dropped: every read fails fast */
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
            u64 w = osGetTime() - t0;                     /* this read had to wait for the network */
            if (w >= 5) { h->waits++; h->wait_ms += w; if (w > h->wait_max) h->wait_max = w; }
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
        u64 waited = osGetTime() - t0;
        if (waited > HF_STALL_MS) {
            LightLock_Lock(&h->lk); h->want = -1; h->waits++; h->wait_ms += waited; LightLock_Unlock(&h->lk);
            g_hf_gave_up = 2; h->aborted = 1;
            return -1;
        }
        /* a seek is a run of reads with the player's controls dead until it ends: the player may
         * look at the buttons meanwhile and drop it (a new target, or B) */
        if (g_hf_hook && waited > 100 && (waited / 30) % 2 == 0 && g_hf_hook(1)) {
            LightLock_Lock(&h->lk); h->want = -1; LightLock_Unlock(&h->lk);
            return -1;
        }
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
    if (g_hf_open > 0) g_hf_open--;
    /* one line per opened file: was playback waiting on the network, or not? */
    u64 secs = (osGetTime() - h->t_open) / 1000;
    if (secs >= 3) {
        FILE *lf = fopen(HF_LOG, "ab");
        if (lf) {
            const char *nm = strrchr(h->url, '/');
            fprintf(lf, "%llus open, %lld KB in %d requests (%d failed), %llu ms/request avg, "
                        "%.0f KB/s while fetching; reads waited %d times, %llu ms total, longest %llu ms%s  [%s]\n",
                    secs, h->bytes / 1024, h->fetches, h->failed,
                    h->fetches ? h->fetch_ms / (u64)h->fetches : 0ULL,
                    h->fetch_ms ? (double)h->bytes / 1024.0 / ((double)h->fetch_ms / 1000.0) : 0.0,
                    h->waits, h->wait_ms, h->wait_max,
                    h->aborted ? (g_hf_gave_up == 1 ? ", stopped with B" : ", gave up (no data)") : "",
                    nm ? nm : h->url);
            fclose(lf);
        }
    }
    /* keep the blocks a moment for the playback that usually reopens this file right away */
    httpfile_drop_cache();
    for (int i = 0; i < h->nslots; i++) if (h->slots[i].state != SL_READY) h->slots[i].state = SL_EMPTY;
    g_park.url = h->url; g_park.slots = h->slots; g_park.mem = h->mem; g_park.nslots = h->nslots;
    g_park.t = osGetTime();
    free(h);
    return 0;
}

FILE *httpfile_open(const char *url, long long size, size_t ring_bytes) {
    APT_SetAppCpuTimeLimit(30);             /* the fetch thread runs on the system core */
    if (size < 0) size = hf_size(url);
    if (size < 0) return NULL;
    if (!downloader_init()) return NULL;
    HFile *h = (HFile *)calloc(1, sizeof *h);
    if (!h) return NULL;
    h->nslots = ring_bytes ? (int)(ring_bytes / HB_SIZE) : (g_hf_open == 0 ? HB_SLOTS : HB_SLOTS_2ND);
    if (h->nslots < HB_AHEAD + 8) h->nslots = HB_AHEAD + 8;
    h->url = strdup(url); h->size = size; h->want = -1; h->last = -10; h->t_open = osGetTime();
    if (g_park.url && h->url && !strcmp(g_park.url, h->url) && g_park.nslots == h->nslots &&
        osGetTime() - g_park.t < 20000) {
        h->slots = g_park.slots; h->mem = g_park.mem;     /* the same file again: its blocks are here */
        free(g_park.url); memset(&g_park, 0, sizeof g_park);
    } else {
        httpfile_drop_cache();
        h->slots = (Slot *)calloc((size_t)h->nslots, sizeof(Slot));
        h->mem = (uint8_t *)malloc((size_t)h->nslots * HB_SIZE);
        if (h->slots) for (int i = 0; i < h->nslots; i++) h->slots[i].data = h->mem + (size_t)i * HB_SIZE;
    }
    if (!h->url || !h->slots || !h->mem) { free(h->mem); free(h->slots); free(h->url); free(h); return NULL; }
    LightLock_Init(&h->lk);
    s32 prio = 0x30; svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    bool isnew = false; APT_CheckNew3DS(&isnew);
    if (isnew)   /* the system core: the moflex decoder owns core 2 and would starve a lowest-priority fetch */
        h->th = threadCreate(worker, h, 64 * 1024, 0x3F, 1, false);
    else         /* Old 3DS: the engine's audio runs on the system core, and a download sharing that
                  * core's small time budget made the sound stutter. The app core instead, just above
                  * the decoder: the fetch mostly waits on the network, so it only steps in briefly. */
        h->th = threadCreate(worker, h, 64 * 1024, prio - 1, 0, false);
    if (!h->th) h->th = threadCreate(worker, h, 64 * 1024, prio - 1, -2, false);
    if (!h->th) { free(h->mem); free(h->slots); free(h->url); free(h); return NULL; }
    cookie_io_functions_t io = { hf_read, NULL, hf_seek, hf_close };
    g_hf_open++;
    FILE *f = fopencookie(h, "rb", io);
    if (!f) { hf_close(h); return NULL; }
    setvbuf(f, NULL, _IOFBF, 32 * 1024);
    return f;
}
