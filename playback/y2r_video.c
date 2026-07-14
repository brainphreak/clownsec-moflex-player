#include "y2r_video.h"
#include <string.h>
#include <stdio.h>
#include <malloc.h>

#define SCR_H 240

static int    g_ready = 0;
static int    g_w = 0, g_h = 0;
static u8    *g_out  = NULL;    /* linear BGR8 (3 bytes/px), row-major (w x h) */
static Handle g_done = 0;

bool y2r_video_init(int w, int h) {
    g_ready = 0;
    /* Y2R + CPU transpose measured ~4x faster than the software 1-pass blit on Old 3DS
       (18.7ms vs 71.8ms per pair), so it stays on. */
    /* escape hatch: create sdmc:/moflex_player/sw_convert.txt to force the software blit */
    FILE *sw = fopen("sdmc:/moflex_player/sw_convert.txt", "rb");
    if (sw) { fclose(sw); return false; }
    if (R_FAILED(y2rInit())) return false;

    g_out = (u8 *)linearAlloc((size_t)w * h * 3);      /* 3 bytes/px, not 2 */
    if (!g_out) { y2rExit(); return false; }

    Y2RU_ConversionParams p;
    memset(&p, 0, sizeof(p));
    p.input_format         = INPUT_YUV420_INDIV_8;      /* planar YUV420p (mobiclip output) */
    /* 24-bit. RGB565 gave BLUE only 5 bits (32 levels), so smooth gradients could only land in
     * ~32 hard steps -- the banding users see against the official player. Y2R's RGB_24 output is
     * byte order B,G,R, which is exactly what a GSP_BGR8_OES framebuffer expects: no swizzle. */
    p.output_format        = OUTPUT_RGB_24;
    p.rotation             = ROTATION_NONE;              /* rotate on the CPU (BLOCK_LINE rotate is batched/scrambled) */
    p.block_alignment      = BLOCK_LINE;                 /* linear, row-major */
    p.input_line_width     = (s16)w;
    p.input_lines          = (s16)h;
    /* FULL range, matching the software LUT. The _SCALING variants expand 16..235 -> 0..255; this
     * content is full-range (luma 0..245, 38% of pixels below 16), so that stretch crushed the
     * shadows, blew the highlights, and amplified the RGB565 gradient banding. */
    p.standard_coefficient = COEFFICIENT_ITU_R_BT_601;
    p.alpha                = 0xFF;
    if (R_FAILED(Y2RU_SetConversionParams(&p))) { linearFree(g_out); g_out = NULL; y2rExit(); return false; }

    Y2RU_SetTransferEndInterrupt(true);
    if (R_FAILED(Y2RU_GetTransferEndEvent(&g_done))) { linearFree(g_out); g_out = NULL; y2rExit(); return false; }

    g_w = w; g_h = h; g_ready = 1;
    return true;
}

void y2r_video_exit(void) {
    if (!g_ready) return;
    if (g_out) { linearFree(g_out); g_out = NULL; }
    y2rExit();
    g_ready = 0;
}

/* Kick off the Y2R conversion (non-blocking) -- the hardware runs while the CPU can
   decode the next frame. The source frame must stay untouched until y2r_video_finish. */
bool y2r_video_start(AVFrame *f, int w, int h) {
    if (!g_ready || w != g_w || h != g_h) return false;
    int cw = w / 2, ch = h / 2;
    GSPGPU_FlushDataCache(f->data[0], (u32)w * h);
    GSPGPU_FlushDataCache(f->data[1], (u32)cw * ch);
    GSPGPU_FlushDataCache(f->data[2], (u32)cw * ch);
    Y2RU_SetSendingY(f->data[0], (u32)w * h,  (s16)w,  0);
    Y2RU_SetSendingU(f->data[1], (u32)cw * ch, (s16)cw, 0);
    Y2RU_SetSendingV(f->data[2], (u32)cw * ch, (s16)cw, 0);
    Y2RU_SetReceiving(g_out, (u32)w * h * 3, (s16)(w * 3 * 8), 0);
    svcClearEvent(g_done);
    return R_SUCCEEDED(Y2RU_StartConversion());
}

/* Wait for the started conversion and transpose the result into fb (cache-blocked). */
/* Transpose Y2R's row-major BGR8 output into the 3DS's column-major (rotated) framebuffer.
 * 3 bytes per pixel now instead of 2 -- same cache-blocked walk, just a wider element. */
void y2r_video_finish(u8 *fb) {
    int w = g_w, h = g_h;
    svcWaitSynchronization(g_done, 300000000LL);
    GSPGPU_InvalidateDataCache(g_out, (u32)w * h * 3);
    enum { TB = 16 };
    for (int xo = 0; xo < w; xo += TB) {
        int xe = xo + TB < w ? xo + TB : w;
        for (int yo = 0; yo < h; yo += TB) {
            int ye = yo + TB < h ? yo + TB : h;
            for (int x = xo; x < xe; x++) {
                u8 *col = fb + (size_t)x * SCR_H * 3;
                const u8 *src = g_out + (size_t)x * 3;
                for (int y = yo; y < ye; y++) {
                    const u8 *s3 = src + (size_t)y * w * 3;
                    u8 *d3 = col + (size_t)(SCR_H - 1 - y) * 3;
                    d3[0] = s3[0]; d3[1] = s3[1]; d3[2] = s3[2];
                }
            }
        }
    }
}

void y2r_video_drain(void) {   /* wait for an in-flight conversion (e.g. before a seek) */
    if (g_ready) svcWaitSynchronization(g_done, 300000000LL);
}

bool y2r_video_blit_fb(AVFrame *f, u8 *fb, int w, int h) {   /* synchronous (prime/fallback) */
    if (!y2r_video_start(f, w, h)) return false;
    y2r_video_finish(fb);
    return true;
}

bool y2r_video_blit(AVFrame *f, gfx3dSide_t side, int w, int h) {
    return y2r_video_blit_fb(f, (u8 *)gfxGetFramebuffer(GFX_TOP, side, NULL, NULL), w, h);
}
