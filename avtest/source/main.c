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
extern void   mobi_flush(AVCodecContext *);
extern size_t mobi_ctx_size(void);
extern int    mobi_opt;

#define TEXW 512
#define TEXH 256
#define VW   400
#define VH   240
#define NBUF 32                 /* decode-ahead ring (pairs) -- backlog is the main buffer now */
#define AWB  256                /* audio wavebufs -- deep read-ahead so the next keyframe is buffered */
#define ABUF (8 * 1024)         /* max samples/ch per audio packet (real packets ~2k; 8k = safe + small) */

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
    ndspChnSetPaused(0, true);   /* start PAUSED: bank audio while the ring pre-rolls, then unpause aligned */
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
    g_awi = (g_awi + 1) % AWB;                     /* g_apos is armed at unpause (pre-roll), not here */
    return 1;
}
static void audio_close(void) {
    if (!g_a_ok) return;
    ndspChnWaveBufClear(0); ndspChnReset(0);
    for (int i = 0; i < AWB; i++) if (g_aab[i]) linearFree(g_aab[i]);
    g_a_ok = 0;
}

/* ---- compressed-video BACKLOG (+ keyframe flag). Phase 1 stashes video here while feeding audio
 * ahead; Phase 2 drains it, dropping whole GOPs to the next keyframe when it falls behind. ---- */
#define VQN 512
static uint8_t *g_vq[VQN]; static int g_vqsz[VQN]; static uint8_t g_vqkf[VQN];
static int g_vqh = 0, g_vqt = 0, g_vqn = 0, g_vqkfn = 0;   /* g_vqkfn = keyframe packets buffered */
/* buffered keyframe PAIR-positions (so we can skip to a keyframe at/before the audio clock, never past
 * it -- skipping past causes a freeze while present waits for audio to reach the future keyframe). */
#define KFPN 256
static int g_kfp[KFPN]; static int g_kfph = 0, g_kfpt = 0, g_kfpc = 0;
static int g_pushv = 0;   /* video packets pushed to the backlog (pair index = /2) */
static void kfp_enq(int pos) {
    if (g_kfpc >= KFPN) { g_kfph = (g_kfph + 1) % KFPN; g_kfpc--; }   /* drop oldest */
    g_kfp[g_kfpt] = pos; g_kfpt = (g_kfpt + 1) % KFPN; g_kfpc++;
}
static int vq_push(const uint8_t *d, int n, int kf) {
    if (g_vqn >= VQN) return 0;
    uint8_t *p = (uint8_t *)malloc(n); if (!p) return 0;
    memcpy(p, d, n); g_vq[g_vqt] = p; g_vqsz[g_vqt] = n; g_vqkf[g_vqt] = kf;
    g_vqt = (g_vqt + 1) % VQN; g_vqn++; if (kf) g_vqkfn++;
    if (kf && (g_pushv & 1) == 0) kfp_enq(g_pushv / 2);   /* keyframe on a left eye -> record its pair pos */
    g_pushv++;
    return 1;
}
static uint8_t *vq_pop(int *n) {
    if (g_vqn <= 0) return NULL;
    uint8_t *p = g_vq[g_vqh]; *n = g_vqsz[g_vqh];
    if (g_vqkf[g_vqh]) g_vqkfn--;
    g_vqh = (g_vqh + 1) % VQN; g_vqn--;
    return p;
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
    int wr = 0, rd = 0, ready[NBUF]; int64_t rts[NBUF], vpts = 0;   /* rts = each ring slot's content-time */
    memset(ready, 0, sizeof ready);
    int done = 0, has_pending = 0, skipping = 0, dropped = 0, gated = 0;
    int64_t dpair = 0;   /* decoder content position in pairs (advances on decode AND drop) */
    const int PRIME = 12;   /* pre-roll this many pairs before starting audio (kept small: pre-roll must
                             * finish before the paused audio bank fills and Phase 1 stops reading) */
    MfxPacket pending;
    u64 fps_t0 = osGetTime(); int shown = 0; double fps = 0;
    u64 dec_ticks = 0; int dec_frames = 0;   /* rolling: raw decode capability (pairs/sec) */

    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & (KEY_B | KEY_START)) break;

        /* ---- PHASE 1 (audio priority): read ahead, feed audio to the DSP bank (cheap: file+ADPCM),
         *      stash video packets (+ keyframe flag) into the backlog. This keeps g_apos advancing
         *      smoothly even while video decode is slow -> audio never stutters. ---- */
        while (!done && g_vqn < VQN) {
            if (!has_pending) { if (mfx_next_packet(&m, &pending) != 1) { done = 1; break; } has_pending = 1; }
            int mt = m.streams[pending.stream_index].media_type;
            if (mt == MFX_TYPE_AUDIO) { if (!audio_feed_pkt(&pending)) break; has_pending = 0; }
            else if (mt == MFX_TYPE_VIDEO) { if (!vq_push(pending.data, pending.size, pending.keyframe)) break; has_pending = 0; }
            else has_pending = 0;
        }

        /* ---- PHASE 2 (video, bounded): decode ONE pair from the backlog into the ring. CATCH-UP: when
         *      the decoder falls >500ms behind the audio clock, drop whole GOPs (their P-frames are
         *      undecodable once skipped) and resync at the next keyframe. Bounds the video lag so it
         *      can't drift; video jumps forward to stay locked to audio. rts[] carries each ringed
         *      pair's true content-time so present stays correct across the gaps left by drops. ---- */
        if (g_vqn >= 2 && (wr - rd) < NBUF - 1) {
            /* CATCH-UP: skip forward to the LATEST buffered keyframe that is at/before the audio clock,
             * so video re-locks without OVERSHOOTING (a keyframe past the audio clock would freeze the
             * picture until audio caught up to it). If the next keyframe is still in the future, don't
             * skip -- decode in order (show motion, slightly behind) until audio reaches it. */
            int apos_pair = (g_apos >= 0) ? (int)(g_apos / pair_dur) : -1;
            while (g_kfpc > 0 && g_kfp[g_kfph] < (int)dpair) { g_kfph = (g_kfph + 1) % KFPN; g_kfpc--; }
            while (g_kfpc >= 2 && apos_pair >= 0 && g_kfp[(g_kfph + 1) % KFPN] <= apos_pair) { g_kfph = (g_kfph + 1) % KFPN; g_kfpc--; }
            int target  = (g_kfpc > 0) ? g_kfp[g_kfph] : -1;
            int canskip = (apos_pair >= 0) && (target > (int)dpair) && (target <= apos_pair);
            (void)canskip; (void)target;
            /* SKIP DISABLED (diagnostic): decode strictly in order so the reference chain is always
             * intact -> decode always produces a frame -> present always has the latest to show ->
             * the picture can never freeze. Video just runs behind on heavy action (drift), no hangs. */
            if (0 && !skipping && canskip && target >= (int)dpair + 2) skipping = 1;
            if (skipping && (int)dpair >= target) { skipping = 0; mobi_flush(&ctx); }   /* reached keyframe -> decode */
            if (skipping) {
                int n; free(vq_pop(&n)); if (g_vqn > 0) free(vq_pop(&n));   /* drop toward the target keyframe */
                dpair++; dropped++;
            } else {
                int fill = wr % NBUF, n, got; uint8_t *p; AVPacket ap;
                u64 _dt = svcGetSystemTick();
                p = vq_pop(&n); ap.data = p; ap.size = n; got = 0;
                int okL = (mobi_decode(&ctx, fL, &got, &ap) >= 0 && got); free(p);
                if (okL) y2r_start(fL, &texL[fill]);                        /* overlap Y2R(L) w/ decode(R) */
                p = vq_pop(&n); ap.data = p; ap.size = n; got = 0;
                int okR = (mobi_decode(&ctx, fR, &got, &ap) >= 0 && got); free(p);
                dec_ticks += svcGetSystemTick() - _dt; dec_frames += 2;
                if (okL && okR) {
                    y2r_wait(&texL[fill]); y2r_start(fR, &texR[fill]); y2r_wait(&texR[fill]);
                    rts[fill] = dpair * pair_dur; ready[fill] = 1; wr++;   /* this pair's content-time */
                } else if (okL) { y2r_wait(&texL[fill]); }
                dpair++;
            }
        }

        /* ---- pre-roll: bank audio while priming the ring, then unpause aligned to content-0 ---- */
        if (!gated && wr >= PRIME) { gated = 1; ndspChnSetPaused(0, false); g_apos = 0; }

        audio_poll();   /* advance the audio clock from real DSP playback (not just when feeding) */

        /* ---- present, audio-master. Drop stale ringed pairs by their true content-time (rts) so video
         *      stays locked to the audio clock. g_apos = -1 until the pre-roll unpauses -> video waits. */
        while ((wr - rd) > 1 && ready[rd % NBUF] && rts[(rd + 1) % NBUF] <= g_apos) rd++;
        if ((wr - rd) > 0 && ready[rd % NBUF] && g_apos >= rts[rd % NBUF]) {
            vpts = rts[rd % NBUF]; draw_present(rd % NBUF); rd++; shown++;
        } else if ((wr - rd) == 0 && g_vqn < 2 && done) {
            break;   /* ring + backlog drained + EOF */
        }

        /* ---- tiny readout (~4x/sec): e = A/V error ms, q = ring depth, fps ---- */
        u64 now = osGetTime();
        if (now - fps_t0 >= 250) {
            fps = shown * 1000.0 / (now - fps_t0); shown = 0; fps_t0 = now;
            int dfps = (dec_ticks > 0) ? (int)((double)SYSCLOCK_ARM11 * dec_frames / (double)dec_ticks / 2.0) : 0;
            dec_ticks = 0; dec_frames = 0;   /* d = raw decode pairs/sec (>=24 => render/bank-bound; <24 => decode wall) */
            int64_t apos = g_apos;
            int e = (apos >= 0) ? (int)(vpts - apos) / 1000 : 0;   /* last-presented content-time vs audio */
            printf("\x1b[4;0Hd%3d drop%d bk%d kf%d   \x1b[5;0Hfps%5.1f  q%2d  e%5dms  apos%lldms   \n",
                   dfps, dropped, g_vqn, g_vqkfn,
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
