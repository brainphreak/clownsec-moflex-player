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
#define NBUF 40                 /* decode-ahead ring (pairs) -- deeper = more cushion for hard scenes */
#define AWB  128                /* audio wavebufs -- banks audio ahead so hard scenes don't underrun */
#define ABUF (16 * 1024)        /* max samples/ch per audio packet */

/* ---- SINGLE-DEMUX audio: TWO demuxers reading the same file thrash the SD (each seeks to a
 * different position). So there is ONE demuxer (the video one); its interleaved audio packets are
 * fed to ndsp inline. g_apos = elapsed audible us. audio_feed_pkt returns 0 if no free wavebuf
 * (caller holds the packet as pending and retries) so nothing is dropped and read-ahead self-limits. */
static char        g_path[300];
static int64_t     g_apos = -1;           /* elapsed audible us, -1 = not started */
static ndspWaveBuf g_awb[AWB]; static int16_t *g_aab[AWB];
static int         g_awi = 0, g_arate = 44100, g_achn = 2, g_a_ok = 0;
static long long   g_aplayed = 0; static int g_acnt[AWB];   /* g_acnt[i]=1: buffer i's samples counted */

static void audio_setup(int arate, int chn) {
    g_arate = arate; g_achn = chn;
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE);
    ndspChnSetRate(0, (float)arate);
    ndspChnSetFormat(0, chn == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
    float mix[12]; memset(mix, 0, sizeof mix); mix[0] = mix[1] = 1.0f; ndspChnSetMix(0, mix);
    memset(g_awb, 0, sizeof g_awb); memset(g_acnt, 0, sizeof g_acnt);
    for (int i = 0; i < AWB; i++) { g_aab[i] = (int16_t *)linearAlloc(ABUF * chn * 2); g_awb[i].status = NDSP_WBUF_DONE; }
    g_a_ok = 1;
}
/* advance g_apos from ACTUAL DSP playback (count finished wavebufs) -- called every loop iteration, so
 * the audio clock keeps moving even when we're not feeding (holding a video packet on a full ring). */
static void audio_poll(void) {
    if (!g_a_ok) return;
    for (int i = 0; i < AWB; i++)
        if (g_awb[i].status == NDSP_WBUF_DONE && !g_acnt[i] && g_awb[i].nsamples > 0) { g_aplayed += g_awb[i].nsamples; g_acnt[i] = 1; }
    if (g_apos >= 0) g_apos = (int64_t)(g_aplayed * 1000000LL / g_arate);
}
/* buffer one audio packet. 1 = consumed, 0 = no free wavebuf (hold the packet and retry next iter). */
static int audio_feed_pkt(MfxPacket *pkt) {
    if (!g_a_ok) return 1;
    ndspWaveBuf *w = &g_awb[g_awi];
    if (w->status != NDSP_WBUF_FREE && w->status != NDSP_WBUF_DONE) return 0;   /* full -> hold */
    if (w->status == NDSP_WBUF_DONE && !g_acnt[g_awi] && w->nsamples > 0) { g_aplayed += w->nsamples; g_acnt[g_awi] = 1; }
    int fr = adpcm_moflex_decode(pkt->data, pkt->size, g_achn, g_aab[g_awi]);
    if (fr <= 0 || fr > ABUF) return 1;                                         /* bad -> drop */
    DSP_FlushDataCache(g_aab[g_awi], fr * g_achn * 2);
    memset(w, 0, sizeof *w); w->data_vaddr = g_aab[g_awi]; w->nsamples = fr;
    ndspChnWaveBufAdd(0, w); g_acnt[g_awi] = 0;   /* freshly queued: not yet played */
    if (g_apos < 0) g_apos = 0;                    /* audio has started */
    g_awi = (g_awi + 1) % AWB;
    return 1;
}
static void audio_close(void) {
    if (!g_a_ok) return;
    ndspChnWaveBufClear(0); ndspChnReset(0);
    for (int i = 0; i < AWB; i++) if (g_aab[i]) linearFree(g_aab[i]);
    g_a_ok = 0;
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

    /* audio: SINGLE demuxer (the video one) -> feed its interleaved audio packets to ndsp, playing
     * immediately (no pause/priming -> can't deadlock; buffers always free as they play). */
    g_apos = -1;
    { int ai = -1;
      for (int i = 0; i < m.nb_streams; i++) if (m.streams[i].media_type == MFX_TYPE_AUDIO && ai < 0) ai = i;
      if (ai >= 0) audio_setup(m.streams[ai].sample_rate, m.streams[ai].channels); }

    /* decode-ahead ring state + one pending packet (held when its target buffer is full) */
    int wr = 0, rd = 0, vidx = 0, left_ok = 0, lv = -2, prev = -1, ready[NBUF];
    memset(ready, 0, sizeof ready);
    int done = 0, has_pending = 0;
    MfxPacket pending;
    u64 fps_t0 = osGetTime(); int shown = 0; double fps = 0;
    u64 dec_ticks = 0; int dec_frames = 0;   /* rolling: raw decode capability (pairs/sec) */

    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & (KEY_B | KEY_START)) break;

        /* ---- ONE demuxer: read a packet (held as pending until its buffer has room), dispatch
         *      video -> ring, audio -> ndsp. No second demux -> no SD seek-thrash. ---- */
        if (!has_pending && !done) { if (mfx_next_packet(&m, &pending) != 1) done = 1; else has_pending = 1; }
        if (has_pending) {
            int mt = m.streams[pending.stream_index].media_type;
            if (mt == MFX_TYPE_VIDEO) {
                if ((wr - rd) < NBUF - 1) {
                    int eye = vidx & 1; AVFrame *dst = eye ? fR : fL; int got = 0, fill = wr % NBUF;
                    AVPacket ap; ap.data = pending.data; ap.size = pending.size;
                    u64 _dt = svcGetSystemTick();
                    int ok = (mobi_decode(&ctx, dst, &got, &ap) >= 0 && got);
                    dec_ticks += svcGetSystemTick() - _dt; dec_frames++;
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
                    has_pending = 0;
                }   /* else ring full -> hold this video packet, drain first */
            } else if (mt == MFX_TYPE_AUDIO) {
                if (audio_feed_pkt(&pending)) has_pending = 0;   /* else buffer full -> hold */
            } else {
                has_pending = 0;   /* data / other -> skip */
            }
        }

        audio_poll();   /* advance the audio clock from real DSP playback (not just when feeding) */

        /* ---- present, audio-master. Drop stale pairs (video slaved to the audio clock): if the NEXT
         *      pair is already due too, this one is stale -> skip it without drawing so video re-locks
         *      to audio instead of fast-forwarding. Audio stays smooth+in-sync; video sheds frames. ---- */
        while ((wr - rd) > 1 && ready[rd % NBUF] && (int64_t)(rd + 1) * pair_dur <= g_apos) rd++;
        if ((wr - rd) > 0 && ready[rd % NBUF] && g_apos >= (int64_t)rd * pair_dur) {
            draw_present(rd % NBUF); rd++; shown++;
        } else if ((wr - rd) == 0 && done) {
            break;   /* ring drained + EOF */
        }

        /* ---- tiny readout (~4x/sec): e = A/V error ms, q = ring depth, fps ---- */
        u64 now = osGetTime();
        if (now - fps_t0 >= 250) {
            fps = shown * 1000.0 / (now - fps_t0); shown = 0; fps_t0 = now;
            int dfps = (dec_ticks > 0) ? (int)((double)SYSCLOCK_ARM11 * dec_frames / (double)dec_ticks / 2.0) : 0;
            dec_ticks = 0; dec_frames = 0;   /* d = raw decode pairs/sec (>=24 => render/bank-bound; <24 => decode wall) */
            int64_t apos = g_apos;
            int e = (apos >= 0) ? (int)((int64_t)rd * pair_dur - apos) / 1000 : 0;
            printf("\x1b[4;0Hd%3d (>=24 ok)\x1b[5;0Hfps%5.1f  q%2d  e%5dms  apos%lldms   \n",
                   dfps,
                   fps, wr - rd, e, (long long)(apos / 1000));
        }
    }

    audio_close();

wait:
    while (aptMainLoop()) { hidScanInput(); if (hidKeysDown() & (KEY_B | KEY_START)) break; gspWaitForVBlank(); }
    ndspExit();
    gfxExit();
    return 0;
}
