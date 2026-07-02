/* Implementations of the framework shim + tiny avutil stubs (for vlc.c). */
#include "mobicompat.h"
#include <stdio.h>
#include <stdarg.h>

#define PLANE_PAD 64

/* ---- bswapdsp ---- */
static void bswap16_buf_c(uint16_t *dst, const uint16_t *src, int len) {
    for (int i = 0; i < len; i++) { unsigned v = src[i]; dst[i] = (uint16_t)((v >> 8) | (v << 8)); }
}
void ff_bswapdsp_init(BswapDSPContext *c) { c->bswap16_buf = bswap16_buf_c; }

/* ---- frame pool (YUV420P, plane heights: Y=h, U/V=h/2) ---- */
static int plane_h(const AVFrame *f, int i) { return i ? (f->height + 1) / 2 : f->height; }

AVFrame *av_frame_alloc(void) { return (AVFrame *)calloc(1, sizeof(AVFrame)); }

void av_frame_unref(AVFrame *f) {
    if (!f) return;
    for (int i = 0; i < 4; i++) {
        if (f->_own[i]) free(f->data[i]);
        f->data[i] = NULL; f->linesize[i] = 0; f->_own[i] = 0;
    }
    f->width = f->height = 0; f->pict_type = 0; f->flags = 0;
}

void av_frame_free(AVFrame **pf) {
    if (pf && *pf) { av_frame_unref(*pf); free(*pf); *pf = NULL; }
}

int ff_reget_buffer(AVCodecContext *avctx, AVFrame *f, int flags) {
    (void)flags;
    int w = avctx->width, h = avctx->height, cw = w / 2, ch = h / 2;
    if (f->data[0] && f->width == w && f->height == h)
        return 0;                       /* reget preserves existing buffer */
    av_frame_unref(f);
    f->width = w; f->height = h;
    f->linesize[0] = w; f->linesize[1] = cw; f->linesize[2] = cw;
    f->data[0] = (uint8_t *)calloc(1, (size_t)w * h + PLANE_PAD);   f->_own[0] = 1;
    f->data[1] = (uint8_t *)calloc(1, (size_t)cw * ch + PLANE_PAD); f->_own[1] = 1;
    f->data[2] = (uint8_t *)calloc(1, (size_t)cw * ch + PLANE_PAD); f->_own[2] = 1;
    if (!f->data[0] || !f->data[1] || !f->data[2]) return AVERROR(ENOMEM);
    return 0;
}

int av_frame_ref(AVFrame *dst, const AVFrame *src) {
    /* shallow reference: the decoder owns the plane in its 6-frame pool and
     * won't reuse this slot for 6 frames, so it's valid to read until then.
     * Caller must consume (blit) before decoding the next frame. */
    av_frame_unref(dst);
    dst->width = src->width; dst->height = src->height;
    dst->pict_type = src->pict_type; dst->flags = src->flags;
    for (int i = 0; i < 3; i++) {
        dst->data[i]     = src->data[i];
        dst->linesize[i] = src->linesize[i];
        dst->_own[i]     = 0;              /* not owned -> unref won't free */
    }
    return 0;
}

void av_fast_padded_malloc(void *ptr, unsigned *size, size_t min_size) {
    uint8_t **p = (uint8_t **)ptr;
    size_t need = min_size + PLANE_PAD;
    if (*p && *size >= need) return;
    free(*p);
    *p = (uint8_t *)malloc(need);
    if (*p) memset(*p, 0, need);
    *size = (unsigned)(*p ? need : 0);
}

/* ---- tiny avutil stubs (satisfy vlc.c / mobiclip.c) ---- */
void *av_malloc(size_t s)            { return malloc(s); }
void *av_mallocz(size_t s)           { return calloc(1, s); }
void *av_calloc(size_t n, size_t s)  { return calloc(n, s); }
void *av_realloc(void *p, size_t s)  { return realloc(p, s); }
void  av_free(void *p)               { free(p); }
void  av_freep(void *p)              { void **pp = (void **)p; free(*pp); *pp = NULL; }
void  av_log(void *a, int l, const char *fmt, ...) { (void)a; (void)l; (void)fmt; }
void *av_malloc_array(size_t n, size_t s)          { return malloc(n * s); }
void *av_realloc_f(void *p, size_t n, size_t s)    { return realloc(p, n * s); }
void  avpriv_request_sample(void *a, const char *m, ...) { (void)a; (void)m; }
int   av_log2(unsigned v) { int n = 0; while (v >>= 1) n++; return n; }
