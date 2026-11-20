#include "livemoflex.h"
#include "httpstream.h"
#include <3ds.h>
#include <stdlib.h>
#include <string.h>
#include "moflex_demux.h"

#define LIVE_WINDOW   (6 * 1024 * 1024)   /* kept stream bytes: rewinds land inside this */
#define LIVE_RING     (1024 * 1024)       /* httpstream ring */
#define LIVE_STALL_MS 20000               /* no data for this long: the read gives up (EOF) */

struct LiveMoflex {
    char url[512];
    HttpStream *hs;
    uint8_t *win;            /* stream bytes [base, base + len) */
    int64_t base;
    size_t len;
    int64_t pos;             /* read position, in the current stream */
    int64_t t0;              /* the time the current stream starts at */
    int64_t dur;             /* the title's length (0 = unknown) */
    FILE *f;
    unsigned long long bytes;
    unsigned wait_max, reopens;
};

static int start(LiveMoflex *lm, int64_t t_us) {
    if (lm->hs) { hs_close(lm->hs); lm->hs = NULL; }
    char u[600];
    snprintf(u, sizeof u, "%s%cat=%lld", lm->url, strchr(lm->url, '?') ? '&' : '?', (long long)(t_us / 1000));
    /* the system core, like the Big Top file reader: the decoder owns the app core */
    lm->hs = hs_open_core(u, NULL, LIVE_RING, 1);
    lm->base = 0; lm->len = 0; lm->pos = 0; lm->t0 = t_us;
    return lm->hs != NULL;
}

/* pull whatever has arrived into the window; 1 = got some, 0 = none yet, -1 = stream over */
static int pull(LiveMoflex *lm) {
    if (lm->len == LIVE_WINDOW) {                 /* full: drop the older half */
        size_t drop = LIVE_WINDOW / 2;
        memmove(lm->win, lm->win + drop, lm->len - drop);
        lm->len -= drop; lm->base += drop;
    }
    size_t got = hs_read(lm->hs, lm->win + lm->len, LIVE_WINDOW - lm->len);
    if (got) { lm->len += got; lm->bytes += got; return 1; }
    return hs_done(lm->hs) ? -1 : 0;
}

static ssize_t lm_read(void *c, char *buf, size_t n) {
    LiveMoflex *lm = (LiveMoflex *)c;
    if (!lm->hs || lm->pos < lm->base) return -1;           /* before the kept window: lost */
    u64 t0 = osGetTime();
    while (lm->pos >= lm->base + (int64_t)lm->len) {
        int r = pull(lm);
        if (r < 0) return 0;                                  /* the stream ended: EOF */
        if (r == 0) {
            u64 w = osGetTime() - t0;
            if (w > LIVE_STALL_MS) return 0;
            svcSleepThread(2 * 1000 * 1000LL);
        }
    }
    { u32 w = (u32)(osGetTime() - t0); if (w > lm->wait_max) lm->wait_max = w; }
    size_t avail = (size_t)(lm->base + (int64_t)lm->len - lm->pos);
    size_t k = n < avail ? n : avail;
    memcpy(buf, lm->win + (lm->pos - lm->base), k);
    lm->pos += (int64_t)k;
    return (ssize_t)k;
}
static int lm_seek(void *c, _off64_t *off, int whence) {
    LiveMoflex *lm = (LiveMoflex *)c;
    int64_t np = whence == SEEK_SET ? *off : whence == SEEK_CUR ? lm->pos + *off : -1;
    if (np < lm->base) return -1;                             /* SEEK_END, or older than the window */
    lm->pos = np; *off = np;
    return 0;
}
/* the open live FILEs, so mfx_open can tell one from a card file */
#define LIVE_MAX 4
static LiveMoflex *g_open[LIVE_MAX];
static LightLock g_lk = 1;   /* LightLock: 1 = unlocked */
static void reg(LiveMoflex *lm, int add) {
    LightLock_Lock(&g_lk);
    for (int i = 0; i < LIVE_MAX; i++)
        if (add ? !g_open[i] : g_open[i] == lm) { g_open[i] = add ? lm : NULL; break; }
    LightLock_Unlock(&g_lk);
}
static int live_hook(MfxDemux *m, FILE *f) {
    LiveMoflex *lm = NULL;
    LightLock_Lock(&g_lk);
    for (int i = 0; i < LIVE_MAX; i++) if (g_open[i] && g_open[i]->f == f) lm = g_open[i];
    LightLock_Unlock(&g_lk);
    if (!lm) return -2;
    if (lm->base != 0 && live_seek(lm, lm->t0) < 0) return -1;   /* its start is gone: from the top */
    return mfx_open_live(m, f, live_seek, lm, lm->t0, lm->dur);
}
void live_install_hook(void) { mfx_open_hook = live_hook; }

static int lm_close(void *c) {
    LiveMoflex *lm = (LiveMoflex *)c;
    reg(lm, 0);
    if (lm->hs) hs_close(lm->hs);
    free(lm->win); free(lm);
    return 0;
}

LiveMoflex *live_open(const char *url, int64_t t_us, FILE **fp) { return live_open_dur(url, t_us, 0, fp); }
LiveMoflex *live_open_dur(const char *url, int64_t t_us, int64_t dur_us, FILE **fp) {
    LiveMoflex *lm = (LiveMoflex *)calloc(1, sizeof *lm);
    if (!lm) return NULL;
    lm->dur = dur_us;
    snprintf(lm->url, sizeof lm->url, "%s", url);
    lm->win = (uint8_t *)malloc(LIVE_WINDOW);
    if (!lm->win || !start(lm, t_us)) { free(lm->win); free(lm); return NULL; }
    cookie_io_functions_t fn = { lm_read, NULL, lm_seek, lm_close };
    FILE *f = fopencookie(lm, "rb", fn);
    if (!f) { hs_close(lm->hs); free(lm->win); free(lm); return NULL; }
    setvbuf(f, NULL, _IONBF, 0);                              /* the demuxer reads in big blocks itself */
    lm->f = f; reg(lm, 1);
    *fp = f;
    return lm;
}

int64_t live_seek(void *c, int64_t t_us) {
    LiveMoflex *lm = (LiveMoflex *)c;
    if (t_us < 0) t_us = 0;
    /* already starting there, start still kept: rewind instead of restarting the server's encoder */
    if (llabs(t_us - lm->t0) < 1000 && lm->base == 0) { lm->pos = 0; return lm->t0; }
    lm->reopens++;
    return start(lm, t_us) ? t_us : -1;
}

void live_stats(LiveMoflex *lm, unsigned long long *bytes, unsigned *wait_max_ms, unsigned *reopens) {
    if (bytes) *bytes = lm->bytes;
    if (wait_max_ms) *wait_max_ms = lm->wait_max;
    if (reopens) *reopens = lm->reopens;
}

void live_status(LiveMoflex *lm, char *buf, int n) {
    if (!lm->hs) { snprintf(buf, n, "no stream"); return; }
    snprintf(buf, n, "http=%ld curl=%d done=%d got=%llu", hs_http(lm->hs), hs_curl_rc(lm->hs),
             hs_done(lm->hs), lm->bytes);
}
