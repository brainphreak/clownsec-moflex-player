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

/* ---- double-buffered SD committer (Universal-Updater's technique): the transfer thread only
 * appends into a RAM buffer while this thread flushes the OTHER buffer to SD, so the network
 * never idles waiting on an SD write (every fwrite is a blocking FS IPC -- serial write-then-read
 * was costing ~half the achievable throughput). Unlike UU we commit in 64KB slices, so the FAT
 * lock is never held long enough to freeze the UI's SD reads (the 256KB single-write lesson). */
#define WB_SZ    (256 * 1024)
#define WB_SLICE (64 * 1024)
typedef struct {
    FILE *f;
    u8 *buf[2];
    u32 len;                  /* fill of buf[widx] (transfer thread only) */
    int widx;                 /* buffer being filled */
    volatile int cidx;        /* buffer being committed, -1 = none */
    volatile u32 clen;
    volatile int err, quit;
    LightEvent ready, done;
    Thread th;
} WBuf;

static void wb_thread(void *arg) {
    WBuf *w = (WBuf *)arg;
    for (;;) {
        LightEvent_Wait(&w->ready);
        if (w->cidx >= 0) {
            const u8 *p = w->buf[w->cidx];
            for (u32 o = 0; o < w->clen && !w->err; o += WB_SLICE) {
                u32 n = w->clen - o; if (n > WB_SLICE) n = WB_SLICE;
                if (fwrite(p + o, 1, n, w->f) != n) w->err = 1;
            }
            w->cidx = -1;
            LightEvent_Signal(&w->done);
        }
        if (w->quit) break;
    }
}
static int wb_flip(WBuf *w) {          /* hand the filled buffer to the committer */
    while (w->cidx >= 0) LightEvent_Wait(&w->done);
    if (w->err) return 0;
    w->clen = w->len; w->cidx = w->widx;
    w->widx ^= 1; w->len = 0;
    LightEvent_Signal(&w->ready);
    return 1;
}
static size_t wb_write(WBuf *w, const void *p, size_t n) {
    if (w->err) return 0;
    const u8 *s = (const u8 *)p; size_t left = n;
    while (left) {
        size_t room = WB_SZ - w->len, take = left < room ? left : room;
        memcpy(w->buf[w->widx] + w->len, s, take);
        w->len += (u32)take; s += take; left -= take;
        if (w->len == WB_SZ && !wb_flip(w)) return 0;
    }
    return n;
}
static WBuf *wb_open(FILE *f) {
    WBuf *w = (WBuf *)calloc(1, sizeof *w);
    if (!w) return NULL;
    w->f = f; w->cidx = -1;
    w->buf[0] = (u8 *)malloc(WB_SZ); w->buf[1] = (u8 *)malloc(WB_SZ);
    LightEvent_Init(&w->ready, RESET_ONESHOT);
    LightEvent_Init(&w->done, RESET_ONESHOT);
    if (w->buf[0] && w->buf[1])
        /* 0x2E = just above the main thread (0x30): commits preempt UI redraw for the microseconds
         * between FS IPCs, so a busy screen can never back the buffers up and stall the transfer */
        w->th = threadCreate(wb_thread, w, 8 * 1024, 0x2E, -2, false);
    if (!w->th) { free(w->buf[0]); free(w->buf[1]); free(w); return NULL; }   /* -> direct writes */
    return w;
}
static int wb_close(WBuf *w) {         /* drain remaining bytes, stop the thread; 0 = write error */
    if (!w) return 1;
    if (w->len && !w->err) wb_flip(w);
    while (w->cidx >= 0) LightEvent_Wait(&w->done);
    w->quit = 1;
    LightEvent_Signal(&w->ready);
    threadJoin(w->th, U64_MAX); threadFree(w->th);
    int ok = !w->err;
    free(w->buf[0]); free(w->buf[1]); free(w);
    return ok;
}

/* ---- to file (resumable) ---- */
typedef struct { FILE *f; WBuf *wb; curl_off_t resume_off; long status; int range_ignored; } DlFile;
static size_t dl_sink(DlFile *d, const void *p, size_t n) {
    return d->wb ? wb_write(d->wb, p, n) : fwrite(p, 1, n, d->f);
}

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
    return dl_sink(d, ptr, sz * nm) / (sz ? sz : 1);
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

/* ---- httpc path: HTTP(S) through the http:C sysmodule. TLS runs inside the system module
 * (hardware-assisted), NOT in-app mbedTLS burning our worker core -- this is how FBI and the
 * fast downloaders get their speed. We read in 64KB chunks (vs curl's 16KB TLS records), which
 * also means fewer, larger SD writes without approaching the 256KB size that froze the Old-3DS
 * UI behind the FAT lock. curl remains the fallback for anything httpc can't do. */
#define HF_OK        1     /* body complete */
#define HF_FAIL      0     /* transient failure -> retry (resume picks up) */
#define HF_ABORT    -1     /* user cancelled */
#define HF_RANGE    -2     /* server ignored Range -> caller starts clean */
#define HF_USE_CURL -3     /* httpc can't handle this transfer -> curl this attempt */
#define HF_CHUNK   (64 * 1024)
#define HF_STALL_NS (30LL * 1000 * 1000 * 1000)   /* 30s no-progress abort, like curl LOW_SPEED */

static bool g_httpc_ready;
static bool httpc_ready(void) {
    static bool tried;
    if (!tried) {
        if (g_lock_ready) LightLock_Lock(&g_init_lock);
        if (!tried) { g_httpc_ready = R_SUCCEEDED(httpcInit(0)); tried = true; }
        if (g_lock_ready) LightLock_Unlock(&g_init_lock);
    }
    return g_httpc_ready;
}

/* The http sysmodule REJECTS URLs with raw spaces / non-ASCII (curl passes them through and
 * the servers tolerate it) -- and our movie URLs are full of "Name (2025) (3D).moflex". Encode
 * anything unsafe; '%' is left alone so an already-encoded URL isn't double-encoded. */
static void url_enc(const char *in, char *out, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < cap; p++) {
        if (*p <= 0x20 || *p >= 0x7f || strchr("\"<>\\^`{|}", *p)) {
            out[o++] = '%'; out[o++] = hex[*p >> 4]; out[o++] = hex[*p & 15];
        } else out[o++] = (char)*p;
    }
    out[o] = 0;
}

static int httpc_fetch(const char *url, DlFile *df, Prog *pr) {
    char cur[1024]; url_enc(url, cur, sizeof cur);
    for (int redir = 0; redir < 8; redir++) {
        httpcContext c;
        /* use_defaultproxy=0: the curl engine never used the console's proxy setting, so the
         * httpc engine must not either (a configured proxy = choppy/stalling downloads) */
        if (R_FAILED(httpcOpenContext(&c, HTTPC_METHOD_GET, cur, 0))) return HF_USE_CURL;
        httpcSetSSLOpt(&c, SSLCOPT_DisableVerify);   /* 3DS has no cert store (same as curl setup) */
        httpcAddRequestHeaderField(&c, "User-Agent", "moflex-player/1.0");
        if (df->resume_off > 0) {
            char rg[48]; snprintf(rg, sizeof rg, "bytes=%lld-", (long long)df->resume_off);
            httpcAddRequestHeaderField(&c, "Range", rg);
        }
        /* ANY failure before body bytes hit the sink falls back to curl for this attempt --
         * httpc must never make a download fail that curl could have completed. */
        if (R_FAILED(httpcBeginRequest(&c))) { httpcCloseContext(&c); return HF_USE_CURL; }
        u32 status = 0;
        if (R_FAILED(httpcGetResponseStatusCodeTimeout(&c, &status, HF_STALL_NS))) {
            httpcCloseContext(&c); return HF_USE_CURL;
        }
        if (status >= 300 && status < 400) {
            char loc[1024] = "";
            Result lr = httpcGetResponseHeader(&c, "Location", loc, sizeof loc);
            if (R_FAILED(lr)) lr = httpcGetResponseHeader(&c, "location", loc, sizeof loc);
            httpcCloseContext(&c);
            if (R_FAILED(lr) || !strstr(loc, "://")) return HF_USE_CURL;   /* odd redirect -> curl */
            url_enc(loc, cur, sizeof cur);
            continue;
        }
        df->status = (long)status;
        if (df->resume_off > 0 && status == 200) {   /* Range ignored: full body incoming */
            httpcCloseContext(&c); df->range_ignored = 1; return HF_RANGE;
        }
        if (status != 200 && status != 206) { httpcCloseContext(&c); return HF_USE_CURL; }

        u32 want = 0; httpcGetDownloadSizeState(&c, NULL, &want);   /* bytes THIS response will send */
        u8 *buf = (u8 *)malloc(HF_CHUNK);
        if (!buf) { httpcCloseContext(&c); return HF_USE_CURL; }
        u32 got_total = 0;
        int ret = HF_OK;
        Result r;
        do {
            r = httpcReceiveDataTimeout(&c, buf, HF_CHUNK, HF_STALL_NS);
            int pending = (r == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING);
            if (R_FAILED(r) && !pending) { ret = HF_FAIL; break; }
            u32 now = 0; httpcGetDownloadSizeState(&c, &now, NULL);
            u32 chunk = now - got_total;                       /* bytes valid in buf this round */
            if (chunk && dl_sink(df, buf, chunk) != chunk) { ret = HF_FAIL; break; }
            got_total = now;
            if (g_abort_cb && g_abort_cb()) { ret = HF_ABORT; break; }
            if (pr && pr->cb && !pr->cb(pr->user, (uint32_t)(pr->resume_off + got_total),
                                        (uint32_t)(want ? pr->resume_off + want : 0))) { ret = HF_ABORT; break; }
            if (!pending) break;
        } while (1);
        free(buf);
        httpcCloseContext(&c);
        if (ret == HF_OK && want && got_total < want) ret = HF_FAIL;      /* connection died mid-body */
        if (ret == HF_FAIL && got_total == 0) ret = HF_USE_CURL;          /* got nothing -> let curl try */
        return ret;
    }
    return HF_FAIL;   /* redirect loop */
}

bool download_to_file(const char *url, const char *dest, dl_progress_cb cb, void *user) {
    if (!downloader_init()) return false;
    char part[512]; part_path_for_url(url, part, sizeof part);

    const int MAX_TRIES = 5;
    for (int attempt = 0; attempt < MAX_TRIES; attempt++) {
        struct stat st;
        curl_off_t off = (stat(part, &st) == 0) ? (curl_off_t)st.st_size : 0;
        /* RESUME: open read+write and seek to the end ONCE, NOT append mode ("ab"). On the 3DS FAT
         * driver, append does an lseek(END) before every write -- O(file-size) per write -- so a resumed
         * download crawls and slows further as the .part grows. r+b seeks once, then writes sequentially. */
        FILE *f = fopen(part, off > 0 ? "r+b" : "wb");
        if (!f) return false;
        if (off > 0) fseeko(f, 0, SEEK_END);
        /* NOTE: no big stdio buffer here -- writes go through the double-buffered committer (wb),
         * which flushes in 64KB slices so the FAT lock never freezes the UI's SD reads on Old-3DS
         * (menus hung 1-2 min behind the download when a single write was 256KB). */
        WBuf *wb = wb_open(f);        /* NULL (alloc/thread fail) -> plain direct writes */
        DlFile df = { f, wb, off, 0, 0 };
        Prog pr = { cb, user, off };

        /* Fast path: the http:C sysmodule (TLS off our core). Falls through to curl if it can't. */
        int hr = HF_USE_CURL;
        if (httpc_ready()) hr = httpc_fetch(url, &df, &pr);
        if (hr != HF_USE_CURL) {
            int wok = wb_close(wb);
            fclose(f);
            if (hr == HF_OK && wok) {
                remove(dest);
                rename(part, dest);
                return true;
            }
            if (hr == HF_ABORT) return false;            /* user cancelled -> keep .part */
            if (df.range_ignored) remove(part);          /* server can't resume -> start clean */
            svcSleepThread((s64)(attempt + 1) * 500000000LL);
            continue;                                    /* backoff, then resume/retry */
        }

        CURL *e = curl_easy_init();
        if (!e) { wb_close(wb); fclose(f); return false; }

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
        int wok = wb_close(wb);
        fclose(f);

        if (r == CURLE_OK && wok && (code == 200 || code == 206)) {
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
