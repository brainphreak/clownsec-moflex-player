#include "y2r_video.h"
#include <string.h>
#include <stdio.h>
#include <malloc.h>

#define SCR_H 240

static int    g_ready = 0;
static int    g_w = 0, g_h = 0;
static u16   *g_out  = NULL;    /* linear RGB565, row-major (w x h) */
static Handle g_done = 0;

bool y2r_video_init(int w, int h) {
    g_ready = 0;
    /* escape hatch: create sdmc:/moflex_player/sw_convert.txt to force the software blit */
    FILE *sw = fopen("sdmc:/moflex_player/sw_convert.txt", "rb");
    if (sw) { fclose(sw); return false; }
    if (R_FAILED(y2rInit())) return false;

    g_out = (u16 *)linearAlloc((size_t)w * h * sizeof(u16));
    if (!g_out) { y2rExit(); return false; }

    Y2RU_ConversionParams p;
    memset(&p, 0, sizeof(p));
    p.input_format         = INPUT_YUV420_INDIV_8;      /* planar YUV420p (mobiclip output) */
    p.output_format        = OUTPUT_RGB_16_565;         /* matches the top framebuffer */
    p.rotation             = ROTATION_NONE;
    p.block_alignment      = BLOCK_LINE;                 /* linear, row-major */
    p.input_line_width     = (s16)w;
    p.input_lines          = (s16)h;
    p.standard_coefficient = COEFFICIENT_ITU_R_BT_601_SCALING;  /* TV range, like the SW blit */
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

bool y2r_video_blit(AVFrame *f, gfx3dSide_t side, int w, int h) {
    if (!g_ready || w != g_w || h != g_h) return false;
    int cw = w / 2, ch = h / 2;

    /* flush the decoded planes so Y2R's DMA reads what the CPU just wrote */
    GSPGPU_FlushDataCache(f->data[0], (u32)w * h);
    GSPGPU_FlushDataCache(f->data[1], (u32)cw * ch);
    GSPGPU_FlushDataCache(f->data[2], (u32)cw * ch);

    Y2RU_SetSendingY(f->data[0], (u32)w * h,  (s16)w,  0);
    Y2RU_SetSendingU(f->data[1], (u32)cw * ch, (s16)cw, 0);
    Y2RU_SetSendingV(f->data[2], (u32)cw * ch, (s16)cw, 0);
    Y2RU_SetReceiving(g_out, (u32)w * h * 2, (s16)(w * 2 * 8), 0);   /* 8 lines/unit (per Y2R note) */

    svcClearEvent(g_done);
    if (R_FAILED(Y2RU_StartConversion())) return false;
    svcWaitSynchronization(g_done, 300000000LL);   /* 300ms safety timeout */

    /* CPU will read g_out; invalidate so we don't see stale cache lines */
    GSPGPU_InvalidateDataCache(g_out, (u32)w * h * 2);

    /* transpose row-major RGB565 into the rotated top framebuffer (sequential writes) */
    u16 *fb = (u16 *)gfxGetFramebuffer(GFX_TOP, side, NULL, NULL);
    for (int x = 0; x < w; x++) {
        u16 *col = fb + x * SCR_H;
        for (int y = 0; y < h; y++)
            col[SCR_H - 1 - y] = g_out[y * w + x];
    }
    return true;
}
