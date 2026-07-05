/* In-player fps validator: runs the real render pipeline (Y2R -> GPU texture -> citro2d, both
 * eyes, triple-buffered) on the motion segment for several decoder configs, and reports the
 * sustained fps of each. This catches optimizations that help PURE decode but regress in the
 * player (prefetch fights the Y2R DMA for the memory bus). No audio -- pure decode+render fps. */
#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include "moflex_demux.h"
#include "mobicompat.h"

extern int    mobi_init(AVCodecContext *);
extern int    mobi_decode(AVCodecContext *, AVFrame *, int *, AVPacket *);
extern void   mobi_flush(AVCodecContext *);
extern size_t mobi_ctx_size(void);
extern int    mobi_opt;

#define TEXW 512
#define TEXH 256
#define VW   400
#define VH   240
#define NBUF 3
#define SEG_FRAC  0.35
#define SEG_PAIRS 150

static C3D_Tex texL[NBUF], texR[NBUF];
static C2D_Image imgL[NBUF], imgR[NBUF];
static Tex3DS_SubTexture sub = { VW, VH, 0.0f, 1.0f, (float)VW / TEXW, 1.0f - (float)VH / TEXH };
static Handle y2r_ev;
static C3D_RenderTarget *topL, *topR;
static MfxDemux m;
static AVCodecContext ctx;

static void y2r_setup(void) {
    y2rInit();
    Y2RU_ConversionParams p; memset(&p, 0, sizeof p);
    p.input_format = INPUT_YUV420_INDIV_8; p.output_format = OUTPUT_RGB_16_565;
    p.rotation = ROTATION_NONE; p.block_alignment = BLOCK_8_BY_8;
    p.input_line_width = VW; p.input_lines = VH;
    p.standard_coefficient = COEFFICIENT_ITU_R_BT_601_SCALING; p.alpha = 0xFF;
    Y2RU_SetConversionParams(&p);
    Y2RU_SetTransferEndInterrupt(true);
    Y2RU_GetTransferEndEvent(&y2r_ev);
}
static void y2r_start(AVFrame *o, C3D_Tex *tex) {
    int cw = VW / 2, ch = VH / 2;
    GSPGPU_FlushDataCache(o->data[0], VW * VH);
    GSPGPU_FlushDataCache(o->data[1], cw * ch);
    GSPGPU_FlushDataCache(o->data[2], cw * ch);
    Y2RU_SetSendingY(o->data[0], VW * VH, VW, 0);
    Y2RU_SetSendingU(o->data[1], cw * ch, cw, 0);
    Y2RU_SetSendingV(o->data[2], cw * ch, cw, 0);
    Y2RU_SetReceiving(tex->data, VW * VH * 2, VW * 2 * 8, (TEXW - VW) * 2 * 8);
    svcClearEvent(y2r_ev); Y2RU_StartConversion();
}
static void y2r_wait(C3D_Tex *tex) { svcWaitSynchronization(y2r_ev, 300000000LL); C3D_TexFlush(tex); }
static void draw_present(int b) {
    C3D_FrameBegin(0);
    C2D_SceneBegin(topL); C2D_DrawImageAt(imgL[b], 0, 0, 0, NULL, 1, 1);
    C2D_SceneBegin(topR); C2D_DrawImageAt(imgR[b], 0, 0, 0, NULL, 1, 1);
    C3D_FrameEnd(0);
}

/* run the pipeline for SEG_PAIRS pairs with the given decoder config, return sustained fps */
static double run_cfg(AVFrame *fL, AVFrame *fR, int opt) {
    mobi_opt = opt;
    mfx_seek_frac(&m, SEG_FRAC); mobi_flush(&ctx);
    int vidx = 0, left_ok = 0, lv = -2, cur = 0, prev = -1; long pairs = 0; u64 t0 = 0;
    MfxPacket pkt;
    while (aptMainLoop() && pairs < SEG_PAIRS) {
        hidScanInput(); if (hidKeysDown() & KEY_START) break;
        if (mfx_next_packet(&m, &pkt) != 1) break;
        if (m.streams[pkt.stream_index].media_type != MFX_TYPE_VIDEO) continue;
        int eye = vidx & 1; AVFrame *dst = eye ? fR : fL; int got = 0;
        AVPacket ap; ap.data = pkt.data; ap.size = pkt.size;
        int ok = (mobi_decode(&ctx, dst, &got, &ap) >= 0 && got);
        if (eye == 0) {
            if (ok) {
                left_ok = 1; lv = vidx;
                if (prev >= 0) { y2r_wait(&texR[prev]); if (!t0) t0 = osGetTime(); draw_present(prev); pairs++; }
                y2r_start(fL, &texL[cur]);
            } else left_ok = 0;
        } else {
            if (ok && left_ok && vidx == lv + 1) { y2r_wait(&texL[cur]); y2r_start(fR, &texR[cur]); prev = cur; cur = (cur + 1) % NBUF; }
            left_ok = 0;
        }
        vidx++;
    }
    u64 dt = osGetTime() - t0; if (!dt) dt = 1;
    return pairs * 1000.0 / dt;
}

static int find_moflex(char *out, size_t cap) {
    DIR *d = opendir("sdmc:/"); if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) { size_t l = strlen(e->d_name);
        if (l > 7 && !strcasecmp(e->d_name + l - 7, ".moflex")) { snprintf(out, cap, "sdmc:/%s", e->d_name); closedir(d); return 1; } }
    closedir(d); return 0;
}

int main(void) {
    osSetSpeedupEnable(true);
    gfxInitDefault(); gfxSet3D(true);
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE); C2D_Init(C2D_DEFAULT_MAX_OBJECTS); C2D_Prepare();
    consoleInit(GFX_BOTTOM, NULL);
    topL = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    topR = C2D_CreateScreenTarget(GFX_TOP, GFX_RIGHT);
    for (int i = 0; i < NBUF; i++) {
        C3D_TexInit(&texL[i], TEXW, TEXH, GPU_RGB565); C3D_TexSetFilter(&texL[i], GPU_LINEAR, GPU_LINEAR);
        C3D_TexInit(&texR[i], TEXW, TEXH, GPU_RGB565); C3D_TexSetFilter(&texR[i], GPU_LINEAR, GPU_LINEAR);
        imgL[i] = (C2D_Image){ &texL[i], &sub }; imgR[i] = (C2D_Image){ &texR[i], &sub };
    }
    y2r_setup();

    char path[300];
    if (!find_moflex(path, sizeof path)) { printf("Put a .moflex in sdmc:/ root.\n"); goto wait; }
    FILE *f = fopen(path, "rb");
    if (!f || mfx_open(&m, f) != 0) { printf("open failed.\n"); goto wait; }
    int vi = -1;
    for (int i = 0; i < m.nb_streams; i++) if (m.streams[i].media_type == MFX_TYPE_VIDEO && vi < 0) vi = i;
    memset(&ctx, 0, sizeof ctx);
    ctx.width = m.streams[vi].width; ctx.height = m.streams[vi].height;
    ctx.priv_data = calloc(1, mobi_ctx_size()); mobi_init(&ctx);
    AVFrame *fL = av_frame_alloc(), *fR = av_frame_alloc();
    printf("in-player fps per config\n%.30s\n%d pairs each...\n\n", strrchr(path, '/') + 1, SEG_PAIRS);

    static const struct { const char *name; int opt; } C[6] = {
        { "0 base        ", 0 },        { "1 ASM-mc      ", 0x100 },
        { "2 ASM+pf+sk+dc", 0x10E },    { "3 pf+sk+dc    ", 0x0E },
        { "4 prefetch    ", 8 },        { "5 uhadd-mc(C) ", 16 },
    };
    int NC = sizeof(C) / sizeof(C[0]);
    double fps[6];
    for (int i = 0; i < NC; i++) { printf("  %s...\n", C[i].name); fps[i] = run_cfg(fL, fR, C[i].opt); }

    printf("\x1b[2J\x1b[Hin-player fps (higher=better)\n\n");
    for (int i = 0; i < NC; i++) printf("%s  %5.1f\n", C[i].name, fps[i]);
    printf("\nreal render loop, w/ Y2R contention.\ncompare to the decode-only matrix.\n");

wait:
    while (aptMainLoop()) { hidScanInput(); if (hidKeysDown() & KEY_START) break; gspWaitForVBlank(); }
    gfxExit();
    return 0;
}
