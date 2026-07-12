/* Standalone moflex PROOF-OF-CONCEPT + assembly-decoder test harness (no GUI/player).
 *
 * Purpose (assembly optimization campaign, Step 0):
 *   - prove we can decode + display a moflex on its own, and
 *   - measure the real 3D throughput on Old 3DS for several decoder configs, including the
 *     hand-written ARM assembly path -- packing multiple tests into ONE transfer.
 *
 * It plays the FIRST .moflex in sdmc:/ root, in stereoscopic 3D, looping. Controls:
 *   LEFT / RIGHT : switch the live decoder config (watch the fps + smoothness change)
 *   Y            : run a precise fixed-segment benchmark of ALL configs -> fps table
 *   START        : exit
 *
 * The top screen shows the video (both eyes via the same Y2R -> GPU texture -> citro2d pipeline the
 * real player uses); the bottom screen shows the live config + fps + a benchmark table.
 *
 * Decoder configs are `mobi_opt` bitflags (see mobiclip.c): 2=idct-skip 4=DC-only 8=prefetch
 * 0x100=ASM motion-comp. When the new entropy/IDCT assembly lands it slots in as another config. */
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

/* the decoder configs under test (name + mobi_opt). ASM = uses decoder/mc_asm.s. */
static const struct { const char *name; int opt; } CFG[] = {
    { "C  base            ", 0 },
    { "C  best pf+sk+dc   ", 0x0E },
    { "C  entropy         ", 0x200 },         /* inlined lazy-refill bit reader (host bit-exact)  */
    { "ASM entropy        ", 0x400 },         /* hand ARM asm of the entropy loop                */
    { "ASM entropy + best ", 0x40E },
};
#define NCFG ((int)(sizeof(CFG)/sizeof(CFG[0])))

static C3D_Tex texL[NBUF], texR[NBUF];
static C2D_Image imgL[NBUF], imgR[NBUF];
static Tex3DS_SubTexture sub = { VW, VH, 0.0f, 1.0f, (float)VW / TEXW, 1.0f - (float)VH / TEXH };
static Handle y2r_ev;
static C3D_RenderTarget *topL, *topR;
static MfxDemux m;
static AVCodecContext ctx;
static AVFrame *fL, *fR;

/* continuous-playback decode state */
static int vidx, left_ok, lv, cur, prev;

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

static void restart(void) {
    mfx_seek_frac(&m, 0.0); mobi_flush(&ctx);
    vidx = 0; left_ok = 0; lv = -2; cur = 0; prev = -1;
}

/* decode + render one video packet of the interleaved 3D stream; returns 1 if a stereo PAIR was
 * presented this call (i.e. one displayed frame). Loops the file at EOF. */
static int step(void) {
    MfxPacket pkt;
    if (mfx_next_packet(&m, &pkt) != 1) { restart(); return 0; }
    if (m.streams[pkt.stream_index].media_type != MFX_TYPE_VIDEO) return 0;
    int eye = vidx & 1; AVFrame *dst = eye ? fR : fL; int got = 0;
    AVPacket ap; ap.data = pkt.data; ap.size = pkt.size;
    int ok = (mobi_decode(&ctx, dst, &got, &ap) >= 0 && got);
    int presented = 0;
    if (eye == 0) {
        if (ok) {
            left_ok = 1; lv = vidx;
            if (prev >= 0) { y2r_wait(&texR[prev]); draw_present(prev); presented = 1; }
            y2r_start(fL, &texL[cur]);
        } else left_ok = 0;
    } else {
        if (ok && left_ok && vidx == lv + 1) { y2r_wait(&texL[cur]); y2r_start(fR, &texR[cur]); prev = cur; cur = (cur + 1) % NBUF; }
        left_ok = 0;
    }
    vidx++;
    return presented;
}

/* precise benchmark: sustained pairs/sec for one config over SEG_PAIRS pairs from SEG_FRAC */
static double bench_cfg(int opt) {
    mobi_opt = opt;
    restart(); mfx_seek_frac(&m, SEG_FRAC); mobi_flush(&ctx);
    vidx = 0; left_ok = 0; lv = -2; cur = 0; prev = -1;
    long pairs = 0; u64 t0 = 0;
    while (aptMainLoop() && pairs < SEG_PAIRS) {
        hidScanInput(); if (hidKeysDown() & KEY_START) break;
        if (step()) { if (!t0) t0 = osGetTime(); pairs++; }
    }
    u64 dt = osGetTime() - t0; if (!dt) dt = 1;
    return pairs * 1000.0 / dt;
}

static char g_paths[2][300]; static int g_npaths = 0, g_cur = 0;
static FILE *g_f = NULL;
static char g_name[64];

/* collect the first two .moflex in sdmc:/ root (2 samples per transfer) */
static int find_moflex_list(void) {
    DIR *d = opendir("sdmc:/"); if (!d) return 0;
    struct dirent *e; g_npaths = 0;
    while ((e = readdir(d)) && g_npaths < 2) { size_t l = strlen(e->d_name);
        if (l > 7 && !strcasecmp(e->d_name + l - 7, ".moflex"))
            snprintf(g_paths[g_npaths++], 300, "sdmc:/%s", e->d_name); }
    closedir(d); return g_npaths;
}

/* switch the open demuxer to file idx (both moflex are 400x240, so ctx is reused + flushed) */
static int switch_file(int idx) {
    if (idx >= g_npaths) return 0;
    if (g_f) { mfx_close(&m); fclose(g_f); g_f = NULL; }
    g_f = fopen(g_paths[idx], "rb");
    if (!g_f || mfx_open(&m, g_f) != 0) { if (g_f) { fclose(g_f); g_f = NULL; } return 0; }
    g_cur = idx;
    const char *bn = strrchr(g_paths[idx], '/'); snprintf(g_name, sizeof g_name, "%s", bn ? bn + 1 : g_paths[idx]);
    mobi_flush(&ctx); restart();
    return 1;
}

static void print_live(int cfg, double fps) {
    printf("\x1b[H\x1b[2J");
    printf("MoFlex ASM test  (standalone)\n");
    printf("file %d/%d: %.32s\n\n", g_cur + 1, g_npaths, g_name);
    printf("config: %s\n", CFG[cfg].name);
    printf("opt   : 0x%X\n\n", CFG[cfg].opt);
    printf("LIVE fps: %5.1f  (stereo pairs/sec)\n\n", fps);
    printf("LEFT/RIGHT = change config\n");
    printf("X          = switch file (2 samples)\n");
    printf("Y          = benchmark table (this file)\n");
    printf("START      = exit\n");
}

int main(void) {
    osSetSpeedupEnable(true);   /* New 3DS 804MHz; no-op on Old 3DS */
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

    if (!find_moflex_list()) { printf("Put a .moflex in sdmc:/ root.\n"); goto wait; }
    /* open the first file to size the decoder context (all moflex are 400x240) */
    g_f = fopen(g_paths[0], "rb");
    if (!g_f || mfx_open(&m, g_f) != 0) { printf("open failed: %s\n", g_paths[0]); goto wait; }
    int vi = -1;
    for (int i = 0; i < m.nb_streams; i++) if (m.streams[i].media_type == MFX_TYPE_VIDEO && vi < 0) vi = i;
    if (vi < 0) { printf("no video stream.\n"); goto wait; }
    memset(&ctx, 0, sizeof ctx);
    ctx.width = m.streams[vi].width; ctx.height = m.streams[vi].height;
    ctx.priv_data = calloc(1, mobi_ctx_size()); mobi_init(&ctx);
    fL = av_frame_alloc(); fR = av_frame_alloc();
    g_cur = 0;
    { const char *bn = strrchr(g_paths[0], '/'); snprintf(g_name, sizeof g_name, "%s", bn ? bn + 1 : g_paths[0]); }

    int cfg = 0; mobi_opt = CFG[cfg].opt;
    restart();
    u64 fps_t0 = osGetTime(); int fps_pairs = 0; double live = 0;
    print_live(cfg, 0.0);

    while (aptMainLoop()) {
        hidScanInput();
        u32 kd = hidKeysDown();
        if (kd & KEY_START) break;
        if (kd & (KEY_RIGHT | KEY_LEFT)) {
            cfg = (kd & KEY_RIGHT) ? (cfg + 1) % NCFG : (cfg + NCFG - 1) % NCFG;
            mobi_opt = CFG[cfg].opt; restart();
            fps_t0 = osGetTime(); fps_pairs = 0; live = 0; print_live(cfg, live);
        }
        if ((kd & KEY_X) && g_npaths > 1) {   /* switch to the other sample file */
            switch_file((g_cur + 1) % g_npaths);
            mobi_opt = CFG[cfg].opt;
            fps_t0 = osGetTime(); fps_pairs = 0; live = 0; print_live(cfg, live);
        }
        if (kd & KEY_Y) {   /* precise benchmark of all configs */
            printf("\x1b[H\x1b[2J benchmarking (%d pairs each)...\n\n", SEG_PAIRS);
            double r[NCFG];
            for (int i = 0; i < NCFG; i++) { printf("  %s ...\n", CFG[i].name); r[i] = bench_cfg(CFG[i].opt); }
            printf("\x1b[H\x1b[2Jbenchmark  (pairs/sec, higher=better)\n%.36s\n\n", g_name);
            for (int i = 0; i < NCFG; i++) printf("%s %5.1f\n", CFG[i].name, r[i]);
            printf("\nlibs on real render loop (Y2R + GPU).\n");
            printf("A/START continues playback...\n");
            while (aptMainLoop()) { hidScanInput(); if (hidKeysDown() & (KEY_A | KEY_START)) break; gspWaitForVBlank(); }
            mobi_opt = CFG[cfg].opt; restart();
            fps_t0 = osGetTime(); fps_pairs = 0; live = 0; print_live(cfg, live);
        }

        if (step()) fps_pairs++;

        u64 now = osGetTime();
        if (now - fps_t0 >= 500) { live = fps_pairs * 1000.0 / (now - fps_t0); fps_t0 = now; fps_pairs = 0; print_live(cfg, live); }
    }

wait:
    while (aptMainLoop()) { hidScanInput(); if (hidKeysDown() & KEY_START) break; gspWaitForVBlank(); }
    gfxExit();
    return 0;
}
