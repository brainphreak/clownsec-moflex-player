/* Minimal moflex A/V SYNC test -- no player, no UI, no controls. Just: decode video (0x1A0E) into a
 * decode-ahead ring, Y2R->GPU->citro2d present both eyes, a core-1 audio worker feeding ndsp, and the
 * simplest possible audio-master sync (present pair rd when audio-elapsed >= rd*pair_dur). Plays the
 * first sdmc:/ .moflex from the start. Goal: prove smooth, in-sync audio is achievable without any of
 * the player's per-frame UI / bolted-on-sync overhead. Bottom screen = a tiny text readout only.
 *   B/START = exit. */
#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include "moflex_demux.h"
#include "mobicompat.h"
#include "adpcm_moflex.h"

extern int    mobi_init(AVCodecContext *);
extern int    mobi_decode(AVCodecContext *, AVFrame *, int *, AVPacket *);
extern size_t mobi_ctx_size(void);
extern int    mobi_opt;

#define TEXW 512
#define TEXH 256
#define VW   400
#define VH   240
#define NBUF 12                 /* decode-ahead ring (pairs) */
#define AWB  16                 /* audio wavebufs */
#define ABUF (16 * 1024)        /* max samples/ch per audio packet */

/* ---- audio worker (core 1): publishes elapsed audible time (us), starts at 0 when gated on ---- */
static volatile int64_t g_apos = -1;     /* audible elapsed us since audio start, -1 = not started */
static volatile int     g_astop = 0;
static volatile int     g_agate = 0;      /* 0 = hold (video priming), 1 = feed audio */
static char             g_path[300];

static void audio_worker(void *arg) {
    (void)arg;
    FILE *f = fopen(g_path, "rb");
    if (!f) return;
    MfxDemux am;
    if (mfx_open(&am, f) != 0) { fclose(f); return; }
    am.audio_only = 1;
    int ai = -1;
    for (int i = 0; i < am.nb_streams; i++) if (am.streams[i].media_type == MFX_TYPE_AUDIO && ai < 0) ai = i;
    if (ai < 0) { mfx_close(&am); fclose(f); return; }
    int arate = am.streams[ai].sample_rate, chn = am.streams[ai].channels;

    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE);
    ndspChnSetRate(0, (float)arate);
    ndspChnSetFormat(0, chn == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
    float mix[12]; memset(mix, 0, sizeof mix); mix[0] = mix[1] = 1.0f; ndspChnSetMix(0, mix);

    ndspWaveBuf wb[AWB]; int16_t *ab[AWB]; memset(wb, 0, sizeof wb);
    for (int i = 0; i < AWB; i++) { ab[i] = (int16_t *)linearAlloc(ABUF * chn * 2); wb[i].data_vaddr = ab[i]; wb[i].status = NDSP_WBUF_DONE; }

    while (!g_astop && !g_agate) svcSleepThread(2000000);   /* hold until the video ring is primed */

    int wi = 0; long long played = 0; int slot_ns[AWB]; memset(slot_ns, 0, sizeof slot_ns);
    MfxPacket pkt;
    while (!g_astop) {
        if (mfx_next_packet(&am, &pkt) != 1) break;         /* EOF */
        if (am.streams[pkt.stream_index].media_type != MFX_TYPE_AUDIO) continue;
        ndspWaveBuf *w = &wb[wi];
        while (w->status != NDSP_WBUF_FREE && w->status != NDSP_WBUF_DONE) { if (g_astop) break; svcSleepThread(2000000); }
        if (g_astop) break;
        int fr = adpcm_moflex_decode(pkt.data, pkt.size, chn, ab[wi]);
        if (fr > 0 && fr <= ABUF) {
            played += slot_ns[wi]; slot_ns[wi] = fr;         /* the slot we're reusing has finished */
            g_apos = (int64_t)(played * 1000000LL / arate);  /* elapsed audible time */
            DSP_FlushDataCache(ab[wi], fr * chn * 2);
            memset(w, 0, sizeof *w); w->data_vaddr = ab[wi]; w->nsamples = fr;
            ndspChnWaveBufAdd(0, w); wi = (wi + 1) % AWB;
        }
    }
    ndspChnWaveBufClear(0); ndspChnReset(0);
    for (int i = 0; i < AWB; i++) if (ab[i]) linearFree(ab[i]);
    mfx_close(&am); fclose(f);
}

/* ---- video render (Y2R -> GPU texture -> citro2d), from asmtest ---- */
static C3D_Tex texL[NBUF], texR[NBUF];
static C2D_Image imgL[NBUF], imgR[NBUF];
static Tex3DS_SubTexture sub = { VW, VH, 0.0f, 1.0f, (float)VW / TEXW, 1.0f - (float)VH / TEXH };
static Handle y2r_ev;
static C3D_RenderTarget *topL, *topR;
static MfxDemux m;
static AVCodecContext ctx;
static AVFrame *fL, *fR;

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
    ndspInit();
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
    mobi_opt = 0x1A0E;

    printf("MoFlex AV sync test\n");
    if (!find_moflex(g_path, sizeof g_path)) { printf("Put a .moflex in sdmc:/ root.\n"); goto wait; }
    FILE *f = fopen(g_path, "rb");
    if (!f || mfx_open(&m, f) != 0) { printf("open failed\n"); goto wait; }
    int vi = -1;
    for (int i = 0; i < m.nb_streams; i++) if (m.streams[i].media_type == MFX_TYPE_VIDEO && vi < 0) vi = i;
    if (vi < 0) { printf("no video\n"); goto wait; }
    memset(&ctx, 0, sizeof ctx);
    ctx.width = m.streams[vi].width; ctx.height = m.streams[vi].height;
    ctx.priv_data = calloc(1, mobi_ctx_size()); mobi_init(&ctx);
    fL = av_frame_alloc(); fR = av_frame_alloc();
    printf("%.32s\n", strrchr(g_path, '/') + 1);

    /* per-pair display duration from the stream timebase (the per-displayed-frame period) */
    int64_t pair_dur = 40000;
    if (m.streams[vi].tb_den > 0) {
        int64_t pd = (int64_t)m.streams[vi].tb_num * 1000000 / m.streams[vi].tb_den;
        if (pd >= 16000 && pd <= 100000) pair_dur = pd;
    }

    /* start the audio worker (held at the gate until the video ring is primed) */
    g_astop = 0; g_agate = 0; g_apos = -1;
    s32 prio = 0x30; svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    APT_SetAppCpuTimeLimit(30);
    Thread awt = threadCreate(audio_worker, NULL, 16 * 1024, prio - 1, 1, false);

    /* decode-ahead ring state */
    int wr = 0, rd = 0, vidx = 0, left_ok = 0, lv = -2, cur = 0, prev = -1, ready[NBUF];
    memset(ready, 0, sizeof ready);
    int done = 0, gated = 0;
    u64 fps_t0 = osGetTime(); int shown = 0; double fps = 0;

    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & (KEY_B | KEY_START)) break;

        /* ---- decode into the ring whenever there's room ---- */
        if (!done && (wr - rd) < NBUF - 1) {
            MfxPacket pkt;
            if (mfx_next_packet(&m, &pkt) != 1) done = 1;
            else if (m.streams[pkt.stream_index].media_type == MFX_TYPE_VIDEO) {
                int eye = vidx & 1; AVFrame *dst = eye ? fR : fL; int got = 0, fill = wr % NBUF;
                AVPacket ap; ap.data = pkt.data; ap.size = pkt.size;
                int ok = (mobi_decode(&ctx, dst, &got, &ap) >= 0 && got);
                if (eye == 0) {
                    if (ok) { left_ok = 1; lv = vidx;
                        if (prev >= 0) { y2r_wait(&texR[prev]); ready[prev] = 1; prev = -1; }
                        y2r_start(fL, &texL[fill]);
                    } else left_ok = 0;
                } else {
                    if (ok && left_ok && vidx == lv + 1) {
                        y2r_wait(&texL[fill]); y2r_start(fR, &texR[fill]);
                        ready[fill] = 0; prev = fill; wr++;
                    }
                    left_ok = 0;
                }
                vidx++;
            }
        }

        /* ---- release audio once the ring is primed (so audio-0 == video pair-0) ---- */
        if (!gated && (wr - rd) >= NBUF - 3) { gated = 1; g_agate = 1; }

        /* ---- present the next pair when the audio clock reaches it ---- */
        if ((wr - rd) > 0 && ready[rd % NBUF]) {
            int64_t vtime = (int64_t)rd * pair_dur;      /* this pair's elapsed video time */
            int64_t apos  = g_apos;
            if (apos >= 0 ? (apos >= vtime) : (rd == 0)) {   /* audio due, or first frame pre-audio */
                draw_present(rd % NBUF);
                rd++; shown++;
            }
        } else if ((wr - rd) == 0 && done) {
            break;   /* ring drained + EOF */
        } else {
            gspWaitForVBlank();   /* nothing to do -> don't spin */
        }

        /* ---- tiny readout (~4x/sec): e = A/V error ms, q = ring depth, fps ---- */
        u64 now = osGetTime();
        if (now - fps_t0 >= 250) {
            fps = shown * 1000.0 / (now - fps_t0); shown = 0; fps_t0 = now;
            int64_t apos = g_apos;
            int e = (apos >= 0) ? (int)((int64_t)rd * pair_dur - apos) / 1000 : 0;
            printf("\x1b[5;0Hfps%5.1f  q%2d  e%5dms  apos%lldms   \n",
                   fps, wr - rd, e, (long long)(apos / 1000));
        }
    }

    g_astop = 1;
    if (awt) { threadJoin(awt, 2000000000LL); threadFree(awt); }

wait:
    while (aptMainLoop()) { hidScanInput(); if (hidKeysDown() & (KEY_B | KEY_START)) break; gspWaitForVBlank(); }
    ndspExit();
    gfxExit();
    return 0;
}
