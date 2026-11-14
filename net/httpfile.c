/* FILE* over HTTP Range requests. See httpfile.h. */
#define _GNU_SOURCE
#include "httpfile.h"
#include "httpstream.h"
#include "downloader.h"
#include <3ds.h>
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define HF_RING      (2u << 20)
#define HF_STALL_MS  20000          /* no byte for this long: the read fails */
#define HF_SKIP_MAX  (512u << 10)   /* a forward seek within this much reads through instead */

typedef struct {
    char *url;
    long long size;      /* whole file */
    long long pos;       /* where the next fread reads (the FILE's position) */
    long long hs_pos;    /* the file offset of the next byte the download hands over */
    HttpStream *hs;
    size_t ring;
} HFile;

/* the file's size: a HEAD request (cached for the last few URLs -- the player opens one file
 * several times: probe, trailer, playback) */
static struct { char url[256]; long long size; } s_sizes[4];
static int s_sizes_next;

static long long hf_size(const char *url) {
    for (int i = 0; i < 4; i++) if (s_sizes[i].url[0] && !strcmp(s_sizes[i].url, url)) return s_sizes[i].size;
    if (!downloader_init()) return -1;
    CURL *e = curl_easy_init(); if (!e) return -1;
    curl_easy_setopt(e, CURLOPT_URL, url);
    curl_easy_setopt(e, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(e, CURLOPT_TIMEOUT, 20L);
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

static int hf_restart(HFile *h) {
    if (h->hs) { hs_close(h->hs); h->hs = NULL; }
    char range[48]; snprintf(range, sizeof range, "Range: bytes=%lld-", h->pos);
    const char *hdr[] = { range, NULL };
    h->hs = hs_open_core(h->url, hdr, h->ring, 1);   /* off core 2: the moflex decoder lives there */
    h->hs_pos = h->pos;
    return h->hs != NULL;
}

static ssize_t hf_read(void *c, char *buf, size_t n) {
    HFile *h = (HFile *)c;
    if (h->pos >= h->size) return 0;
    if ((long long)n > h->size - h->pos) n = (size_t)(h->size - h->pos);
    /* a seek moved us: skip forward through what is already coming, else start over there */
    if (!h->hs || h->pos < h->hs_pos || h->pos - h->hs_pos > (long long)HF_SKIP_MAX)
        if (!hf_restart(h)) return -1;
    u64 last = osGetTime();
    int retried = 0;
    static char skipbuf[16 * 1024];
    for (;;) {
        if (h->hs_pos < h->pos) {                          /* discarding up to the seek target */
            size_t want = (size_t)(h->pos - h->hs_pos);
            if (want > sizeof skipbuf) want = sizeof skipbuf;
            size_t got = hs_read(h->hs, skipbuf, want);
            if (got) { h->hs_pos += (long long)got; last = osGetTime(); continue; }
        } else {
            size_t got = hs_read(h->hs, buf, n);
            if (got) { h->pos += (long long)got; h->hs_pos += (long long)got; return (ssize_t)got; }
        }
        if (hs_done(h->hs)) {                              /* the download ended short of what we want */
            if (retried++ < 2) { if (!hf_restart(h)) return -1; last = osGetTime(); continue; }
            return -1;
        }
        if (osGetTime() - last > HF_STALL_MS) return -1;
        svcSleepThread(2 * 1000 * 1000LL);
    }
}

static int hf_seek(void *c, off_t *off, int whence) {
    HFile *h = (HFile *)c;
    long long np = whence == SEEK_SET ? (long long)*off
                 : whence == SEEK_CUR ? h->pos + (long long)*off
                 : h->size + (long long)*off;
    if (np < 0) return -1;
    h->pos = np;                                           /* the next read decides how to get there */
    *off = (off_t)np;
    return 0;
}

static int hf_close(void *c) {
    HFile *h = (HFile *)c;
    if (h->hs) hs_close(h->hs);
    free(h->url); free(h);
    return 0;
}

FILE *httpfile_open(const char *url, long long size, size_t ring_bytes) {
    APT_SetAppCpuTimeLimit(30);             /* the download thread runs on the system core */
    if (size < 0) size = hf_size(url);
    if (size < 0) return NULL;
    HFile *h = (HFile *)calloc(1, sizeof *h);
    if (!h) return NULL;
    h->url = strdup(url); h->size = size; h->ring = ring_bytes ? ring_bytes : HF_RING;
    if (!h->url) { free(h); return NULL; }
    cookie_io_functions_t io = { hf_read, NULL, hf_seek, hf_close };
    FILE *f = fopencookie(h, "rb", io);
    if (!f) { free(h->url); free(h); return NULL; }
    setvbuf(f, NULL, _IOFBF, 32 * 1024);                  /* stdio's own buffer over ours */
    return f;
}
