/* Live HTTP body -> ring buffer, on a curl worker thread. See httpstream.h. */
#include "httpstream.h"
#include "downloader.h"
#include <3ds.h>
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

struct HttpStream {
    char *url; char **hdr; int nhdr;
    uint8_t *ring; size_t cap, head, fill;   /* head = next byte to read */
    LightLock lock;
    volatile int stop, done;                 /* done: 1 ok, -1 failed */
    volatile long http;
    volatile unsigned long long total;
    Thread th;
};

static size_t on_body(char *p, size_t sz, size_t nm, void *u) {
    HttpStream *s = (HttpStream *)u;
    size_t n = sz * nm, put = 0;
    while (put < n) {
        if (s->stop) return 0;               /* aborts the transfer */
        LightLock_Lock(&s->lock);
        size_t room = s->cap - s->fill;
        size_t k = n - put < room ? n - put : room;
        for (size_t i = 0; i < k; ) {        /* at most two spans around the ring's end */
            size_t at = (s->head + s->fill) % s->cap;
            size_t span = s->cap - at; if (span > k - i) span = k - i;
            memcpy(s->ring + at, p + put + i, span);
            s->fill += span; i += span;
        }
        LightLock_Unlock(&s->lock);
        put += k;
        if (put < n) svcSleepThread(4 * 1000 * 1000LL);   /* full: wait for the player */
    }
    s->total += n;
    return n;
}
static int on_progress(void *u, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
    (void)a; (void)b; (void)c; (void)d;
    return ((HttpStream *)u)->stop ? 1 : 0;
}
static void worker(void *arg) {
    HttpStream *s = (HttpStream *)arg;
    CURL *e = curl_easy_init();
    if (!e) { s->done = -1; return; }
    struct curl_slist *hl = NULL;
    for (int i = 0; i < s->nhdr; i++) hl = curl_slist_append(hl, s->hdr[i]);
    curl_easy_setopt(e, CURLOPT_URL, s->url);
    curl_easy_setopt(e, CURLOPT_HTTPHEADER, hl);
    curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(e, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(e, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(e, CURLOPT_USERAGENT, "moflex-player/1.0");
    curl_easy_setopt(e, CURLOPT_BUFFERSIZE, 64L * 1024L);
    curl_easy_setopt(e, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, s);
    curl_easy_setopt(e, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(e, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(e, CURLOPT_XFERINFODATA, s);
    CURLcode rc = curl_easy_perform(e);
    long code = 0; curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &code);
    s->http = code;
    s->done = (rc == CURLE_OK && code >= 200 && code < 300) ? 1 : -1;
    curl_slist_free_all(hl);
    curl_easy_cleanup(e);
}

HttpStream *hs_open(const char *url, const char *const *headers, size_t ring_bytes) {
    if (!downloader_init()) return NULL;     /* curl's one-time global init */
    HttpStream *s = (HttpStream *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->url = strdup(url);
    for (int i = 0; headers && headers[i]; i++) s->nhdr++;
    s->hdr = (char **)calloc((size_t)(s->nhdr ? s->nhdr : 1), sizeof(char *));
    for (int i = 0; i < s->nhdr; i++) s->hdr[i] = strdup(headers[i]);
    s->cap = ring_bytes; s->ring = (uint8_t *)malloc(ring_bytes);
    LightLock_Init(&s->lock);
    if (!s->url || !s->hdr || !s->ring) { hs_close(s); return NULL; }
    s32 prio = 0x30; svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    /* the downloads' placement: core 2 on New-3DS, else the system core, else the app core */
    s->th = threadCreate(worker, s, 64 * 1024, 0x3F, 2, false);
    if (!s->th) s->th = threadCreate(worker, s, 64 * 1024, 0x3F, 1, false);
    if (!s->th) s->th = threadCreate(worker, s, 64 * 1024, prio - 1, -2, false);
    if (!s->th) { hs_close(s); return NULL; }
    return s;
}
size_t hs_read(HttpStream *s, void *p, size_t n) {
    LightLock_Lock(&s->lock);
    size_t k = n < s->fill ? n : s->fill, got = 0;
    while (got < k) {
        size_t span = s->cap - s->head; if (span > k - got) span = k - got;
        memcpy((uint8_t *)p + got, s->ring + s->head, span);
        s->head = (s->head + span) % s->cap; s->fill -= span; got += span;
    }
    LightLock_Unlock(&s->lock);
    return got;
}
size_t hs_buffered(HttpStream *s) { LightLock_Lock(&s->lock); size_t f = s->fill; LightLock_Unlock(&s->lock); return f; }
int    hs_done(HttpStream *s) { return s->done; }
long   hs_http(HttpStream *s) { return s->http; }
unsigned long long hs_total(HttpStream *s) { return s->total; }
void hs_close(HttpStream *s) {
    if (!s) return;
    s->stop = 1;
    if (s->th) { threadJoin(s->th, U64_MAX); threadFree(s->th); }
    for (int i = 0; i < s->nhdr; i++) free(s->hdr[i]);
    free(s->hdr); free(s->ring); free(s->url); free(s);
}
