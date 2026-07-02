/* Minimal codec-framework shim so FFmpeg's mobiclip.c compiles standalone,
 * replacing avcodec.h / codec_internal.h / decode.h / thread.h / bswapdsp.h.
 * get_bits.h / golomb.h / mathops.h / libavutil/{mem,avassert}.h stay REAL. */
#ifndef MOBICOMPAT_H
#define MOBICOMPAT_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* normally from libavutil/internal.h (not pulled in) */
#ifndef SUINT
#define SUINT unsigned
#endif

/* constants from the real avutil small headers (no AVFrame there) */
#include "libavutil/avutil.h"    /* AV_PICTURE_TYPE_*, AVMEDIA_TYPE_* */
#include "libavutil/pixfmt.h"    /* AV_PIX_FMT_YUV420P */
#include "libavutil/log.h"       /* av_log, AV_LOG_* */
#include "libavutil/error.h"     /* AVERROR* */

#ifndef AVCOL_SPC_YCGCO
#define AVCOL_SPC_YCGCO 8
#endif
#ifndef AV_FRAME_FLAG_KEY
#define AV_FRAME_FLAG_KEY (1 << 1)
#endif
#define AV_CODEC_ID_MOBICLIP 0
#define AV_CODEC_CAP_DR1 0

/* ---- minimal frame / context / packet ---- */
typedef struct AVFrame {
    uint8_t *data[4];
    int      linesize[4];
    int      width, height;
    int      pict_type;
    int      flags;
    int      _own[4];   /* whether we malloc'd each plane */
} AVFrame;

typedef struct AVCodecContext {
    void *priv_data;
    int   width, height;
    int   pix_fmt;
    int   colorspace;
} AVCodecContext;

typedef struct AVPacket {
    uint8_t *data;
    int      size;
} AVPacket;

/* ---- bswapdsp shim ---- */
typedef struct BswapDSPContext {
    void (*bswap16_buf)(uint16_t *dst, const uint16_t *src, int len);
} BswapDSPContext;
void ff_bswapdsp_init(BswapDSPContext *c);

/* ---- one-time init (single-threaded) ---- */
typedef int AVOnce;
#define AV_ONCE_INIT 0
static inline void ff_thread_once(AVOnce *once, void (*fn)(void)) {
    if (!*once) { *once = 1; fn(); }
}

/* ---- frame / buffer management ---- */
AVFrame *av_frame_alloc(void);
void     av_frame_free(AVFrame **f);
int      av_frame_ref(AVFrame *dst, const AVFrame *src);   /* deep copy */
void     av_frame_unref(AVFrame *f);
int      ff_reget_buffer(AVCodecContext *avctx, AVFrame *f, int flags);
void     av_fast_padded_malloc(void *ptr, unsigned *size, size_t min_size);

#endif
