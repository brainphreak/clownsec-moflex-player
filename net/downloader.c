#include "downloader.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* soc service is initialized once by the app (net_soc_init in main); curl uses it. */

static bool g_ready;

bool downloader_init(void) {
    if (g_ready) return true;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) return false;
    g_ready = true;
    return true;
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
    curl_easy_setopt(e, CURLOPT_BUFFERSIZE, 64L * 1024L);
    curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, 20L);
}

/* ---- progress bridge ---- */
typedef struct { dl_progress_cb cb; void *user; } Prog;
static int xfer_cb(void *p, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ul, curl_off_t un) {
    (void)ul; (void)un;
    Prog *pr = (Prog *)p;
    if (pr && pr->cb && !pr->cb(pr->user, (uint32_t)dlnow, (uint32_t)dltotal)) return 1; /* abort */
    return 0;
}

/* ---- to file ---- */
static size_t file_cb(void *ptr, size_t sz, size_t nm, void *ud) {
    return fwrite(ptr, sz, nm, (FILE *)ud);
}

bool download_to_file(const char *url, const char *dest, dl_progress_cb cb, void *user) {
    if (!downloader_init()) return false;
    CURL *e = curl_easy_init();
    if (!e) return false;
    FILE *f = fopen(dest, "wb");
    if (!f) { curl_easy_cleanup(e); return false; }

    Prog pr = { cb, user };
    setup(e, url);
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, file_cb);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, f);
    if (cb) {
        curl_easy_setopt(e, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(e, CURLOPT_XFERINFOFUNCTION, xfer_cb);
        curl_easy_setopt(e, CURLOPT_XFERINFODATA, &pr);
    }
    CURLcode r = curl_easy_perform(e);
    fclose(f);
    curl_easy_cleanup(e);
    if (r != CURLE_OK) { remove(dest); return false; }
    return true;
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
    CURLcode r = curl_easy_perform(e);
    curl_easy_cleanup(e);

    if (r != CURLE_OK || !m.ok) { free(m.buf); return false; }
    m.buf[m.len] = 0;
    *out = m.buf;
    if (out_len) *out_len = m.len;
    return true;
}
