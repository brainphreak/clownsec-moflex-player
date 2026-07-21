#include "downloader.h"

#include <3ds.h>            /* svcSleepThread for retry backoff */
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* soc service is initialized once by the app (net_soc_init in main); curl uses it. */

static bool g_ready;
static LightLock g_init_lock;   /* serialize the one-time curl_global_init across worker threads */
static bool g_lock_ready;

/* Call once on the main thread at startup (instant) so downloader_init can be called from any
 * worker thread thereafter -- the slow curl_global_init then happens lazily off the main thread. */
void downloader_prime(void) { LightLock_Init(&g_init_lock); g_lock_ready = true; }

bool downloader_init(void) {
    if (g_ready) return true;
    if (g_lock_ready) LightLock_Lock(&g_init_lock);
    if (!g_ready && curl_global_init(CURL_GLOBAL_DEFAULT) == 0) g_ready = true;   /* NOT thread-safe -> locked */
    if (g_lock_ready) LightLock_Unlock(&g_init_lock);
    return g_ready;
}
void downloader_exit(void) {
    if (g_ready) { curl_global_cleanup(); g_ready = false; }
}

/* ---- common easy-handle setup ---- */
static void setup(CURL *e, const char *url) {
    curl_easy_setopt(e, CURLOPT_URL, url);
    curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(e, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(e, CURLOPT_SSL_VERIFYPEER, 0L);   /* 3DS has no cert store */
    curl_easy_setopt(e, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(e, CURLOPT_USERAGENT, "moflex-player/1.0");
    curl_easy_setopt(e, CURLOPT_ACCEPT_ENCODING, "");  /* allow gzip etc. */
    curl_easy_setopt(e, CURLOPT_BUFFERSIZE, 256L * 1024L);   /* bigger receive buffer -> fewer syscalls */
    curl_easy_setopt(e, CURLOPT_TCP_NODELAY, 1L);
    /* The 3DS decrypts TLS in software with no AES hardware; ChaCha20-Poly1305 is far faster there than
     * AES-GCM. Prefer it (server picks the best mutual cipher, so this safely falls back to AES). */
    curl_easy_setopt(e, CURLOPT_SSL_CIPHER_LIST,
        "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:"
        "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:AES128-GCM-SHA256");
    curl_easy_setopt(e, CURLOPT_TLS13_CIPHERS, "TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256");
    curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(e, CURLOPT_LOW_SPEED_LIMIT, 512L);   /* < 512 B/s for 30s -> abort, never hang */
    curl_easy_setopt(e, CURLOPT_LOW_SPEED_TIME, 30L);
}

/* ---- progress bridge ---- */
typedef struct { dl_progress_cb cb; void *user; curl_off_t resume_off; } Prog;
static int (*g_abort_cb)(void) = NULL;
void download_set_abort(int (*abort_cb)(void)) { g_abort_cb = abort_cb; }
static int xfer_cb(void *p, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ul, curl_off_t un) {
    (void)ul; (void)un;
    if (g_abort_cb && g_abort_cb()) return 1;   /* user wants to move on -> abort this transfer */
    Prog *pr = (Prog *)p;
    curl_off_t off = pr ? pr->resume_off : 0;   /* bytes already on disk (resumed transfers) */
    curl_off_t now = off + dlnow;
    curl_off_t tot = (dltotal > 0) ? off + dltotal : 0;
    if (pr && pr->cb && !pr->cb(pr->user, (uint32_t)now, (uint32_t)tot)) return 1; /* abort */
    return 0;
}

/* ---- to file (resumable) ---- */
typedef struct { FILE *f; curl_off_t resume_off; long status; int range_ignored; } DlFile;

/* Sniff the HTTP status so we can tell a real resume (206) from a server that ignored our
 * Range and is re-sending the whole file (200) -- appending that would corrupt the .part. */
static size_t hdr_cb(char *buf, size_t sz, size_t nm, void *ud) {
    size_t n = sz * nm;
    DlFile *d = (DlFile *)ud;
    if (n >= 12 && buf[0]=='H' && buf[1]=='T' && buf[2]=='T' && buf[3]=='P') {
        for (size_t i = 0; i + 3 < n; i++)
            if (buf[i] == ' ') { d->status = (buf[i+1]-'0')*100 + (buf[i+2]-'0')*10 + (buf[i+3]-'0'); break; }
        if (d->resume_off > 0 && d->status == 200) d->range_ignored = 1;   /* Range ignored */
    }
    return n;
}
static size_t file_cb(void *ptr, size_t sz, size_t nm, void *ud) {
    DlFile *d = (DlFile *)ud;
    if (d->range_ignored) return 0;   /* abort now rather than append a full body onto the partial */
    return fwrite(ptr, sz, nm, d->f);
}

/* Partial downloads live in ONE central dir, keyed by a hash of the URL (not the destination)
 * so a download resumes no matter which folder you send it to, and stubs never litter movie
 * folders. Completed files are moved (SD->SD rename) to the chosen destination. */
#define DL_TMP_DIR "sdmc:/moflex_player/downloads"
static void part_path_for_url(const char *url, char *out, size_t cap) {
    uint64_t h = 1469598103934665603ULL;                 /* FNV-1a 64 */
    for (const unsigned char *p = (const unsigned char *)url; *p; p++) { h ^= *p; h *= 1099511628211ULL; }
    mkdir("sdmc:/moflex_player", 0777);
    mkdir(DL_TMP_DIR, 0777);
    snprintf(out, cap, "%s/%016llx.part", DL_TMP_DIR, (unsigned long long)h);
}

long long download_partial_bytes(const char *url) {
    char part[512]; part_path_for_url(url, part, sizeof part);
    struct stat st;
    return (stat(part, &st) == 0) ? (long long)st.st_size : 0;
}
void download_discard_partial(const char *url) {
    char part[512]; part_path_for_url(url, part, sizeof part);
    remove(part);
}

bool download_to_file(const char *url, const char *dest, dl_progress_cb cb, void *user) {
    if (!downloader_init()) return false;
    char part[512]; part_path_for_url(url, part, sizeof part);

    const int MAX_TRIES = 5;
    for (int attempt = 0; attempt < MAX_TRIES; attempt++) {
        struct stat st;
        curl_off_t off = (stat(part, &st) == 0) ? (curl_off_t)st.st_size : 0;
        FILE *f = fopen(part, off > 0 ? "ab" : "wb");
        if (!f) return false;
        /* NOTE: deliberately NOT a big stdio buffer -- a large SD write holds the FAT lock long enough
         * to freeze the UI's SD reads on Old-3DS (menus hung for 1-2 min behind the download). Default
         * (small, frequent) writes let the main thread's reads interleave. */
        CURL *e = curl_easy_init();
        if (!e) { fclose(f); return false; }

        DlFile df = { f, off, 0, 0 };
        Prog pr = { cb, user, off };
        setup(e, url);
        curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, file_cb);
        curl_easy_setopt(e, CURLOPT_WRITEDATA, &df);
        curl_easy_setopt(e, CURLOPT_HEADERFUNCTION, hdr_cb);
        curl_easy_setopt(e, CURLOPT_HEADERDATA, &df);
        if (off > 0) curl_easy_setopt(e, CURLOPT_RESUME_FROM_LARGE, off);   /* resume via Range */
        if (cb) {
            curl_easy_setopt(e, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(e, CURLOPT_XFERINFOFUNCTION, xfer_cb);
            curl_easy_setopt(e, CURLOPT_XFERINFODATA, &pr);
        }
        CURLcode r = curl_easy_perform(e);
        long code = 0; curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &code);
        curl_easy_cleanup(e);
        fclose(f);

        if (r == CURLE_OK && (code == 200 || code == 206)) {
            remove(dest);                 /* replace any stale final */
            rename(part, dest);           /* .part -> dest only when complete */
            return true;
        }
        if (r == CURLE_ABORTED_BY_CALLBACK) return false;   /* user cancelled -> keep .part */
        if (df.range_ignored) remove(part);                 /* server can't resume -> start clean */
        svcSleepThread((s64)(attempt + 1) * 500000000LL);   /* backoff, then resume/retry */
    }
    return false;   /* give up for now; .part stays for a later Resume / Start-over */
}

/* ---- to memory ---- */
typedef struct { char *buf; size_t len, cap, max; int ok; } Mem;
static size_t mem_cb(void *ptr, size_t sz, size_t nm, void *ud) {
    Mem *m = (Mem *)ud;
    size_t add = sz * nm;
    if (m->len + add + 1 > m->max) { m->ok = 0; return 0; }
    if (m->len + add + 1 > m->cap) {
        while (m->len + add + 1 > m->cap) m->cap *= 2;
        char *n = (char *)realloc(m->buf, m->cap);
        if (!n) { m->ok = 0; return 0; }
        m->buf = n;
    }
    memcpy(m->buf + m->len, ptr, add);
    m->len += add;
    return add;
}

bool download_to_mem(const char *url, char **out, size_t *out_len, size_t max_bytes) {
    if (!downloader_init()) return false;
    CURL *e = curl_easy_init();
    if (!e) return false;

    Mem m = { (char *)malloc(256 * 1024), 0, 256 * 1024, max_bytes, 1 };
    if (!m.buf) { curl_easy_cleanup(e); return false; }

    setup(e, url);
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, mem_cb);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, &m);
    Prog pr = { NULL, NULL };   /* no % callback, but enable xfer so the global abort hook can fire */
    curl_easy_setopt(e, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(e, CURLOPT_XFERINFOFUNCTION, xfer_cb);
    curl_easy_setopt(e, CURLOPT_XFERINFODATA, &pr);
    CURLcode r = curl_easy_perform(e);
    curl_easy_cleanup(e);

    if (r != CURLE_OK || !m.ok) { free(m.buf); return false; }
    m.buf[m.len] = 0;
    *out = m.buf;
    if (out_len) *out_len = m.len;
    return true;
}
