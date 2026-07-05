/* moflex_bench -- runs the video pipeline through several blit methods on the same
 * clip (seeked into the MOTION part), shows each on the top screen so you can judge
 * correctness, and prints a comparison report (decode/blit ms, fps). */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <malloc.h>

#include "moflex_demux.h"
#include "mobicompat.h"

extern int    mobi_init(AVCodecContext *);
extern int    mobi_decode(AVCodecContext *, AVFrame *, int *, AVPacket *);
extern void   mobi_flush(AVCodecContext *);
extern size_t mobi_ctx_size(void);

#define SCR_W 400
#define SCR_H 240
#define PAIRS 100

/* ---- YUV->RGB565 ---- */
static int yl[256], rv[256], gu[256], gv[256], bu[256];
static u8  clamp8[1024];
static void init_luts(void) {
    for (int i = 0; i < 256; i++) {
        yl[i] = 298 * (i - 16) + 128;
        rv[i] = 409 * (i - 128); gu[i] = -100 * (i - 128);
        gv[i] = -208 * (i - 128); bu[i] = 516 * (i - 128);
    }
    for (int i = 0; i < 1024; i++) { int v = i - 256; clamp8[i] = (u8)(v < 0 ? 0 : (v > 255 ? 255 : v)); }
}
static inline u16 yuv2rgb565(int Y, int U, int V) {
    int y = yl[Y];
    int r = clamp8[((y + rv[V]) >> 8) + 256];
    int g = clamp8[((y + gu[U] + gv[V]) >> 8) + 256];
    int b = clamp8[((y + bu[U]) >> 8) + 256];
    return (u16)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* ---- Y2R ---- */
static u16   *y2r_buf = NULL;
static Handle y2r_ev = 0;
static void y2r_config(int W, int H, int rotation, int block) {
    Y2RU_ConversionParams p; memset(&p, 0, sizeof p);
    p.input_format = INPUT_YUV420_INDIV_8; p.output_format = OUTPUT_RGB_16_565;
    p.rotation = rotation; p.block_alignment = block;
    p.input_line_width = W; p.input_lines = H;
    p.standard_coefficient = COEFFICIENT_ITU_R_BT_601_SCALING; p.alpha = 0xFF;
    Y2RU_SetConversionParams(&p);
    Y2RU_SetTransferEndInterrupt(true);
    Y2RU_GetTransferEndEvent(&y2r_ev);
}
static void y2r_run(AVFrame *o, int W, int H) {
    int cw = W / 2, ch = H / 2;
    GSPGPU_FlushDataCache(o->data[0], W * H);
    GSPGPU_FlushDataCache(o->data[1], cw * ch);
    GSPGPU_FlushDataCache(o->data[2], cw * ch);
    Y2RU_SetSendingY(o->data[0], W * H, W, 0);
    Y2RU_SetSendingU(o->data[1], cw * ch, cw, 0);
    Y2RU_SetSendingV(o->data[2], cw * ch, cw, 0);
    Y2RU_SetReceiving(y2r_buf, W * H * 2, W * 2 * 8, 0);
    svcClearEvent(y2r_ev);
    Y2RU_StartConversion();
    svcWaitSynchronization(y2r_ev, 300000000LL);
}
static void blit_y2r_cpu(AVFrame *o, u16 *fb, int W, int H) {
    y2r_run(o, W, H);
    GSPGPU_InvalidateDataCache(y2r_buf, W * H * 2);
    enum { TB = 16 };
    for (int xo = 0; xo < W; xo += TB) { int xe = xo + TB < W ? xo + TB : W;
        for (int yo = 0; yo < H; yo += TB) { int ye = yo + TB < H ? yo + TB : H;
            for (int x = xo; x < xe; x++) { u16 *col = fb + x * SCR_H; const u16 *src = y2r_buf + x;
                for (int y = yo; y < ye; y++) col[SCR_H - 1 - y] = src[y * W]; } } }
}
static void blit_y2r_gpu(gfx3dSide_t side, int flip) {   /* rotation set via y2r_config */
    GSPGPU_FlushDataCache(y2r_buf, SCR_W * SCR_H * 2);
    u16 fw = 0, fh = 0;
    u16 *fb = (u16 *)gfxGetFramebuffer(GFX_TOP, side, &fw, &fh);
    GX_DisplayTransfer((u32 *)y2r_buf, GX_BUFFER_DIM(fw, fh), (u32 *)fb, GX_BUFFER_DIM(fw, fh),
        GX_TRANSFER_FLIP_VERT(flip) |
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565));
    gspWaitForPPF();
}

/* ---- methods ---- */
enum { M_DEC, M_Y2R_CPU, M_GPU_90, M_GPU_90F, M_GPU_270, M_GPU_270F, M_COUNT };
static const char *M_NAME[M_COUNT] = {
    "decode only", "Y2R+CPU (correct ref)", "GPU CW90", "GPU CW90 flip", "GPU CW270", "GPU CW270 flip",
};
typedef struct { double dec_ms, blit_ms, fps; int pairs; } Res;

static void do_blit(int method, AVFrame *o, gfx3dSide_t side, int W, int H) {
    switch (method) {
        case M_Y2R_CPU:  blit_y2r_cpu(o, (u16 *)gfxGetFramebuffer(GFX_TOP, side, NULL, NULL), W, H); break;
        case M_GPU_90:   y2r_run(o, W, H); blit_y2r_gpu(side, 0); break;
        case M_GPU_90F:  y2r_run(o, W, H); blit_y2r_gpu(side, 1); break;
        case M_GPU_270:  y2r_run(o, W, H); blit_y2r_gpu(side, 0); break;
        case M_GPU_270F: y2r_run(o, W, H); blit_y2r_gpu(side, 1); break;
    }
}

static void run_method(int method, MfxDemux *m, AVCodecContext *ctx, AVFrame *out, int W, int H, Res *r) {
    if (method == M_Y2R_CPU)                          y2r_config(W, H, ROTATION_NONE, BLOCK_LINE);
    else if (method == M_GPU_90 || method == M_GPU_90F)  y2r_config(W, H, ROTATION_CLOCKWISE_90, BLOCK_8_BY_8);
    else if (method == M_GPU_270 || method == M_GPU_270F) y2r_config(W, H, ROTATION_CLOCKWISE_270, BLOCK_8_BY_8);

    /* seek into the motion part of the movie (1/3 in), not the static intro */
    if (m->duration_us > 0) mfx_seek_time(m, m->duration_us / 3); else mfx_seek_frac(m, 0.0);
    mobi_flush(ctx);

    int vidx = 0, left_ok = 0, left_vidx = -2, pairs = 0, warm = 16, started = 0;
    u64 dec = 0, blit = 0, t0 = 0;
    MfxPacket pkt;
    while (pairs < PAIRS && mfx_next_packet(m, &pkt) == 1) {
        if (m->streams[pkt.stream_index].media_type != MFX_TYPE_VIDEO) continue;
        AVPacket ap; ap.data = pkt.data; ap.size = pkt.size; int got = 0;
        u64 td = svcGetSystemTick();
        int ok = (mobi_decode(ctx, out, &got, &ap) >= 0 && got);
        u64 dd = svcGetSystemTick() - td, bb = 0;
        if ((vidx & 1) == 0) {
            if (ok) { if (method != M_DEC) { u64 tb = svcGetSystemTick(); do_blit(method, out, GFX_LEFT, W, H); bb = svcGetSystemTick() - tb; } left_ok = 1; left_vidx = vidx; }
            else left_ok = 0;
        } else {
            if (ok && left_ok && vidx == left_vidx + 1) {
                if (method != M_DEC) { u64 tb = svcGetSystemTick(); do_blit(method, out, GFX_RIGHT, W, H); bb = svcGetSystemTick() - tb; }
                gfxFlushBuffers(); gfxSwapBuffers();
                if (!started) { if (--warm <= 0) { started = 1; t0 = osGetTime(); } }  /* skip keyframe warm-up */
                else pairs++;
            }
            left_ok = 0;
        }
        if (started) { dec += dd; blit += bb; }
        vidx++;
    }
    u64 wall = osGetTime() - t0;
    double tpms = SYSCLOCK_ARM11 / 1000.0;
    r->pairs = pairs;
    r->dec_ms  = pairs ? dec  / tpms / (pairs * 2) : 0;
    r->blit_ms = pairs ? blit / tpms / (pairs * 2) : 0;
    r->fps     = wall  ? pairs * 1000.0 / wall : 0;
}

static int find_moflex(char *out, size_t cap) {
    DIR *d = opendir("sdmc:/");
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l > 7 && !strcasecmp(e->d_name + l - 7, ".moflex")) { snprintf(out, cap, "sdmc:/%s", e->d_name); closedir(d); return 1; }
    }
    closedir(d); return 0;
}

int main(void) {
    osSetSpeedupEnable(true);
    gfxInitDefault();
    gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
    gfxSetDoubleBuffering(GFX_TOP, true);
    gfxSet3D(false);
    consoleInit(GFX_BOTTOM, NULL);
    init_luts();
    y2rInit();
    y2r_buf = (u16 *)linearAlloc(SCR_W * SCR_H * sizeof(u16));

    char path[300];
    if (!find_moflex(path, sizeof(path))) { printf("Put a .moflex in the SD root\n(sdmc:/), then relaunch.\nSTART to exit.\n"); goto wait; }
    printf("Bench (motion, 1/3 in):\n%.40s\n\n", strrchr(path, '/') + 1);

    FILE *f = fopen(path, "rb");
    MfxDemux m;
    if (!f || mfx_open(&m, f) != 0) { printf("open failed\nSTART to exit\n"); goto wait; }
    int vi = -1; for (int i = 0; i < m.nb_streams; i++) if (m.streams[i].media_type == MFX_TYPE_VIDEO) { vi = i; break; }
    int W = m.streams[vi].width, H = m.streams[vi].height;

    AVCodecContext ctx; memset(&ctx, 0, sizeof ctx);
    ctx.width = W; ctx.height = H; ctx.priv_data = calloc(1, mobi_ctx_size()); mobi_init(&ctx);
    AVFrame *out = av_frame_alloc();

    Res res[M_COUNT];
    for (int method = 0; method < M_COUNT; method++) {
        printf("[%d/%d] %-22s ", method + 1, M_COUNT, M_NAME[method]);
        gfxFlushBuffers(); gfxSwapBuffers();
        run_method(method, &m, &ctx, out, W, H, &res[method]);
        printf("fps%.1f\n", res[method].fps);
    }

    printf("\n==== REPORT (ms per pair) ====\n");
    for (int i = 0; i < M_COUNT; i++)
        printf("%-22s %4.1ffps d%4.1f b%4.1f\n", M_NAME[i], res[i].fps, res[i].dec_ms * 2, res[i].blit_ms * 2);
    printf("\nWhich GPU variant looked CORRECT?\n24fps needs dec+blit <= 41ms.\nSTART to exit.\n");

wait:
    while (aptMainLoop()) { hidScanInput(); if (hidKeysDown() & KEY_START) break; gspWaitForVBlank(); }
    gfxExit();
    return 0;
}
