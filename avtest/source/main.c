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
extern void   mobi_seed_refs(AVCodecContext *);
extern size_t mobi_ctx_size(void);
extern int    mobi_opt;
/* diagnostics were dropped from the shipped decoder; define locally so the HUD still links.
 * (They read as 0 now -- the substitution-tier counters are no longer populated. Fine for the
 * throughput/banking work this testbed is for.) */
int    mobi_err[16];
int    mobi_sub[3];   /* substitution tier taken: 0=forward-walk 1=any-frame 2=POOL EMPTY(black->green) */

/* Pin the regular heap so the LINEAR heap gets everything else (libctru __system_allocateHeaps:
 * heap != 0 && linear == 0  =>  linear = free - heap). Default is a 1:1 split, which was silently
 * capping the decoded-ahead ring at ~28MB while ~20MB of regular heap went unused. The regular heap
 * here only carries the decoder's 6-frame pool (~864KB), the packet backlog and stdio -- 10MB is
 * generous. Raise this (not lower it) if anything mallocs unexpectedly. */
u32 __ctru_heap_size = 10 * 1024 * 1024;

#define TEXW 512
#define TEXH 256
#define VW   400
#define VH   240
/* Decoded-ahead ring (pairs) = the REAL action cushion: filled when decode outpaces present in calm
 * scenes, drained during action. Each pair = 2 textures x 256KB = 512KB of LINEAR heap.
 *
 * The old NBUF=40 (21MB) + audio (8.4MB) = 29.4MB was hard against the linear heap and OOM'd past
 * that -- which looked like "the 3DS is out of RAM". It isn't. libctru's default __system_allocateHeaps
 * splits free app memory 1:1 between the regular heap and the LINEAR heap (disassembled: the both-zero
 * path does `linear = (free >> 1) & ~0xFFF`). We were being handed ~28MB of linear and ~28MB of regular
 * heap -- and the regular heap only ever holds the decoder's 6-frame pool (~864KB) + the packet backlog,
 * so ~20MB of it sat unused.
 *
 * Pinning __ctru_heap_size below hands ALL the remainder to the linear heap (libctru: if heap != 0 and
 * linear == 0, linear = free - heap), so the ring can roughly double with no SystemMode change, no
 * extended-memory mode, and no applet/HOME risk. NB is then sized AT RUNTIME from linearSpaceFree() so
 * it adapts to whatever the launcher actually gives us instead of us guessing a constant that OOMs. */
#define NBUF_MAX 128            /* array bound only -- the real depth NB is chosen at runtime */
/* Linear RAM the ring must NOT eat. The audio wavebufs are linearAlloc'd LAZILY inside audio_setup()
 * (first audio packet), i.e. AFTER the ring is built -- so if the ring spends everything, audio_setup
 * gets NULL back and we die. That is exactly what crashed the first build. Reserve it up front. */
#define AUDIO_LIN   (AWB * ABUF * 2 * 2)        /* 256 bufs x 8192 samples x stereo x 16-bit = 8MB */
#define LIN_RESERVE (AUDIO_LIN + (3 << 20))     /* + slack for Y2R / C3D / GPU cmdbuf scratch */
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
static int         g_nawb = 0;    /* wavebufs actually allocated (<= AWB) */
static long long   g_aplayed = 0; static int g_acnt[AWB];   /* g_acnt[i]=1: buffer i's samples counted */

static void audio_setup(int arate, int chn) {
    g_arate = arate; g_achn = chn;
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE);
    ndspChnSetRate(0, (float)arate);
    ndspChnSetFormat(0, chn == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
    float mix[12]; memset(mix, 0, sizeof mix); mix[0] = mix[1] = 1.0f; ndspChnSetMix(0, mix);
    memset(g_awb, 0, sizeof g_awb); memset(g_acnt, 0, sizeof g_acnt);
    /* NAWB = how many wavebufs we actually got. Never assume the alloc succeeds: the ring is built
     * first and could have starved the linear heap (that crash is why LIN_RESERVE now covers AUDIO_LIN). */
    for (g_nawb = 0; g_nawb < AWB; g_nawb++) {
        g_aab[g_nawb] = (int16_t *)linearAlloc(ABUF * chn * 2);
        if (!g_aab[g_nawb]) break;
        g_awb[g_nawb].status = NDSP_WBUF_DONE;
    }
    if (g_nawb < 8) { printf("AUDIO ALLOC FAILED (%d bufs)\n", g_nawb); g_a_ok = 0; return; }
    ndspChnSetPaused(0, true);   /* start PAUSED: bank audio while the ring pre-rolls, then unpause aligned */
    g_a_ok = 1;
}
/* advance g_apos from ACTUAL DSP playback (count finished wavebufs) -- called every loop iteration, so
 * the audio clock keeps moving even when we're not feeding (holding a video packet on a full ring). */
static void audio_poll(void) {
    if (!g_a_ok) return;
    for (int i = 0; i < g_nawb; i++)
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
    g_awi = (g_awi + 1) % g_nawb;                     /* g_apos is armed at unpause (pre-roll), not here */
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
/* is the packet at the head of the backlog an INTRA frame? The flush is only safe if the very next
 * frame needs no references at all. */
static int vq_head_is_kf(void) { return g_vqn > 0 && g_vqkf[g_vqh]; }
static uint8_t *vq_pop(int *n) {
    if (g_vqn <= 0) return NULL;
    uint8_t *p = g_vq[g_vqh]; *n = g_vqsz[g_vqh];
    if (g_vqkf[g_vqh]) g_vqkfn--;
    g_vqh = (g_vqh + 1) % VQN; g_vqn--;
    return p;
}

/* ---- video render (Y2R -> GPU texture -> citro2d), from asmtest ---- */
static C3D_Tex texL[NBUF_MAX], texR[NBUF_MAX];
static C2D_Image imgL[NBUF_MAX], imgR[NBUF_MAX];
static int NB;                  /* ring depth actually allocated (see main) */
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
    /* Grow the ring until the linear heap is (nearly) spent, instead of trusting a hardcoded NBUF.
     * LIN_RESERVE holds back the audio wavebufs (allocated LATER, lazily, in audio_setup) plus GPU
     * slack -- spending that here is what crashed the first build. C3D_TexInit also returns false on
     * failure, so a miscalculation costs a smaller ring rather than a crash. */
    size_t lin0 = linearSpaceFree();
    for (NB = 0; NB < NBUF_MAX; NB++) {
        if (linearSpaceFree() < (size_t)(2 * TEXW * TEXH * 2) + LIN_RESERVE) break;
        if (!C3D_TexInit(&texL[NB], TEXW, TEXH, GPU_RGB565)) break;
        if (!C3D_TexInit(&texR[NB], TEXW, TEXH, GPU_RGB565)) { C3D_TexDelete(&texL[NB]); break; }
        C3D_TexSetFilter(&texL[NB], GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetFilter(&texR[NB], GPU_LINEAR, GPU_LINEAR);
        imgL[NB] = (C2D_Image){ &texL[NB], &sub }; imgR[NB] = (C2D_Image){ &texR[NB], &sub };
    }
    y2r_setup();
    mobi_opt = 0x1BDA5E | 0x08000000;  /* shipped fast path + FUSED transpose-free IDCT (0x08000000, ~3% faster, bit-exact) */

    printf("linear %uK -> ring NB=%d pairs (~%.1fs cushion)\n",
           (unsigned)(lin0 >> 10), NB, NB / 23.976);
    if (NB < 8) { printf("LINEAR HEAP TOO SMALL -- lower __ctru_heap_size\n"); goto wait; }

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
    int wr = 0, rd = 0, ready[NBUF_MAX]; int64_t rts[NBUF_MAX], vpts = 0;   /* rts = each ring slot's content-time */
    memset(ready, 0, sizeof ready);
    int done = 0, has_pending = 0, skipping = 0, dropped = 0, gated = 0, just_resynced = 0;
    int64_t dpair = 0;   /* decoder content position in pairs (advances on decode AND drop) */
    const int PRIME = 12;   /* pre-roll this many pairs before starting audio (kept small: pre-roll must
                             * finish before the paused audio bank fills and Phase 1 stops reading) */
    MfxPacket pending;
    u64 fps_t0 = osGetTime(); int shown = 0; double fps = 0;
    u64 dec_ticks = 0; int dec_frames = 0;   /* rolling: raw decode capability (pairs/sec) */
    u64 loop_t0 = svcGetSystemTick(); int loop_pairs = 0;   /* SUSTAINED loop throughput (what actually matters) */
    double dps = 0, lps = 0;
    /* freeze attribution */
    int nfail = 0, hold_empty = 0, hold_gate = 0, elided = 0;
    /* CATCH-UP MODE (Y cycles). All three are now understood; pick by eye on real content.
     *  0 NOSKIP  : never skip. Picture is ALWAYS clean (no reference is ever corrupted). Video falls
     *              behind audio during heavy action, then catches up in calm scenes -- the present loop
     *              already discards stale ringed pairs, and those pairs were still DECODED, so refs stay
     *              perfect. Cost: lip-sync slips during long action. Watch 'e'.
     *  1 SKIP    : drop to a keyframe, no flush. No freeze, but the ~6 corrupted frames get baked in and
     *              every later P-frame references them -> ghosting persists until the next NATURAL
     *              keyframe, which this content barely has. (What you just saw: artifacts everywhere.)
     *  2 SKIP+FL : the original. Flush nulls the pool -> decode cascade -> content clock runs away ->
     *              multi-second "freeze". Kept only to reproduce the old behaviour. */
    int mode = 2;   /* default = SKIP+FLUSH: the one that looked best (clean picture, rare freezes) */
    u64 last_present = osGetTime(), maxhold = 0;
    /* FREEZE SNAPSHOTS -- the live numbers move too fast to read, so latch the state automatically.
     * A = the FIRST freeze over 1s (the one that shows up ~10-15s in). Latched once, never overwritten.
     * B = the WORST freeze so far (re-latched whenever a hold beats the previous record, at its peak).
     * Both survive on screen after the freeze ends. START/B exits; X re-arms both. */
    typedef struct { int live, d, q, e, drop, bk, kf, fail, empty, gate, skip, ahead; u64 held; long long apos;
                     int err[13], sub[3]; } Snap;
    static Snap snapA, snapB;
    int dfps_live = 0;

    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & (KEY_B | KEY_START)) break;
        if (hidKeysDown() & KEY_X) { memset(&snapA, 0, sizeof snapA); memset(&snapB, 0, sizeof snapB);
                                     maxhold = 0; memset(mobi_err, 0, sizeof mobi_err); }
        if (hidKeysDown() & KEY_Y) { mode = (mode + 1) % 3; dropped = nfail = 0;
                                     memset(mobi_err, 0, sizeof mobi_err); }
        /* A = missing-ref tolerance on/off, LIVE. On paper it should turn the multi-second freeze into
         * ~5 soft frames; in practice it went green, which means tier 2 (POOL COMPLETELY EMPTY -> we
         * referenced the black frame being built) is firing. sub0/1/2 below says which tier actually
         * runs, so we can see it instead of guessing. */
        if (hidKeysDown() & KEY_A) { mobi_opt ^= 0x8000; memset(mobi_sub, 0, sizeof mobi_sub);
                                     memset(mobi_err, 0, sizeof mobi_err); nfail = 0; }

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

        int apos_pair_g = (g_apos >= 0) ? (int)(g_apos / pair_dur) : -1;
        /* ---- PHASE 2 (video, bounded): decode ONE pair from the backlog into the ring. CATCH-UP: when
         *      the decoder falls >500ms behind the audio clock, drop whole GOPs (their P-frames are
         *      undecodable once skipped) and resync at the next keyframe. Bounds the video lag so it
         *      can't drift; video jumps forward to stay locked to audio. rts[] carries each ringed
         *      pair's true content-time so present stays correct across the gaps left by drops. ---- */
        /* Retire the previous pair's RIGHT-eye Y2R here, at the TOP. The old code waited for it
         * immediately after starting it, so the conversion had NOTHING to overlap with -- pure dead
         * time on every pair, and it sat OUTSIDE the 'd' measurement, which is why decode looked like
         * it had margin (d=26-33 vs 24 needed) while the loop still fell behind forever. Now it
         * overlaps Phase 1's file reads, the present and the HUD. Single Y2R engine + single event, so
         * this wait MUST happen before any new y2r_start -- it does. */
        int ahead = (apos_pair_g >= 0) ? (int)dpair - apos_pair_g : 0;
        if (g_vqn >= 2 && (wr - rd) < NB - 1 && ahead < NB) {
            /* CATCH-UP: skip forward to the LATEST buffered keyframe that is at/before the audio clock,
             * so video re-locks without OVERSHOOTING (a keyframe past the audio clock would freeze the
             * picture until audio caught up to it). If the next keyframe is still in the future, don't
             * skip -- decode in order (show motion, slightly behind) until audio reaches it. */
            int apos_pair = (g_apos >= 0) ? (int)(g_apos / pair_dur) : -1;
            while (g_kfpc > 0 && g_kfp[g_kfph] < (int)dpair) { g_kfph = (g_kfph + 1) % KFPN; g_kfpc--; }
            /* target = NEAREST keyframe ahead (small catch-up -> short freeze), not the farthest. */
            int target  = (g_kfpc > 0) ? g_kfp[g_kfph] : -1;
            int canskip = (apos_pair >= 0) && (target > (int)dpair) && (target <= apos_pair);
            /* FLUSH-based resync (clean picture, brief freeze -- the trade you preferred). Skip-with-flush
             * gives a CLEAN frozen frame (not corruption): flush nulls the pool so post-resync P-frames
             * error until a natural keyframe refills it -> the picture holds a clean frame, then recovers.
             * To make freezes RARER, only resync when we've actually drifted past DRIFT_MAX; between
             * resyncs the picture just plays clean, a little behind. Higher DRIFT_MAX = fewer freezes but
             * looser sync. */
            (void)just_resynced;
            int drift = (apos_pair >= 0) ? apos_pair - (int)dpair : 0;   /* pairs the video is behind audio */
            const int DRIFT_MAX = 6;                                     /* ~0.25s: resync early+often -> short freezes */
            if (mode > 0 && !skipping && canskip && drift > DRIFT_MAX) skipping = 1;
            /* NO mobi_flush ON RESYNC. The flush was the root of everything: it NULLs the whole 6-frame
             * pool, so the P-frames after the resync keyframe (which still reference 2..5 frames back --
             * a mobiclip keyframe is the intra bit, NOT an IDR) hit NULL refs and error out. And because
             * s->current_pic only advances on a SUCCESSFUL decode, a failed frame never fills its own
             * slot, so the pool can NEVER refill: every P-frame fails until the next natural keyframe.
             * Measured: drop=4 skips produced fail=250 / err9=500. Each failure still ran dpair++, so the
             * content clock raced ~5s ahead of audio in a fraction of a second, and the recovered frames
             * were stamped in the FUTURE -- present then correctly refused to show them. THAT was the
             * multi-second "freeze" (gate 83861 vs empty 4543), and it was never a speed problem.
             *
             * Without the flush the pool keeps the pre-skip frames: stale, but VALID. The resync keyframe
             * is intra so it decodes clean; only blocks referencing 2..5 frames back get stale content,
             * and index=1 (the previous frame, = the clean keyframe) is the common case. Result: brief
             * ghosting that decays at the next natural keyframe, instead of a cascade. mobi_opt 0x8000
             * stays on as a safety net but should now never fire -- nothing is NULL. */
            /* END OF SKIP. Two guards that were missing, and together they are the green:
             *  (a) target == -1 (no keyframe buffered) made `dpair >= target` ALWAYS TRUE, so we could
             *      flush at an arbitrary position;
             *  (b) nothing verified the next frame was actually intra.
             * Flush + a P-frame = every reference NULL = the decoder falls back to the black frame it is
             * building = GREEN, which then propagates through every later P-frame. So: only flush when
             * the packet at the head of the backlog really IS a keyframe. Then the pool always gets a
             * clean intra frame first, and the 0x8000 tolerance only ever has to cover the 2..5-frames-
             * back references -- which is what it was designed for. */
            if (skipping && target >= 0 && (int)dpair >= target) {
                skipping = 0;
                if (mode == 2 && vq_head_is_kf()) mobi_flush(&ctx);
            } else if (skipping && target < 0) {
                skipping = 0;                    /* lost the target -- abort the skip, never flush blind */
            }
            if (skipping) {
                int n; free(vq_pop(&n)); if (g_vqn > 0) free(vq_pop(&n));   /* drop toward the target keyframe */
                dpair++; dropped++;
            } else {
                int fill = wr % NB, n, got; uint8_t *p; AVPacket ap;

                /* CATCH-UP ELISION. When we are behind, every pair we decode is already past the audio
                 * clock, and present will THROW IT AWAY unseen (the stale-drop loop below) -- but only
                 * after we paid to Y2R both eyes (2 conversions + 6 GSPGPU_FlushDataCache) and burn a
                 * ring slot. That is pure waste, and it is the reason catching up is so slow: the calm
                 * scenes have to fund display work for frames nobody ever sees.
                 *
                 * So: if this pair is already stale, DECODE it (references must stay perfect -- we skip
                 * NOTHING in the decoder) but skip all the display work. The visible result is identical,
                 * because present was going to discard it anyway.
                 *
                 * The one thing we must not do is go dark: an earlier attempt elided EVERY stale pair,
                 * and since being behind means ALL pairs are stale, nothing ever reached the ring and the
                 * picture froze. So force a real display if nothing has been shown for LIVE_MS -- the
                 * picture keeps moving (~10fps) while catch-up runs at full decode speed. */
                #define LIVE_MS 100
                int64_t this_ts = dpair * pair_dur;
                int stale    = (g_apos >= 0) && (this_ts + pair_dur < g_apos);
                int starving = (osGetTime() - last_present) >= LIVE_MS;
                int elide    = stale && !starving;

                u64 _dt = svcGetSystemTick();
                p = vq_pop(&n); ap.data = p; ap.size = n; got = 0;
                int okL = (mobi_decode(&ctx, fL, &got, &ap) >= 0 && got); free(p);
                if (okL && !elide) y2r_start(fL, &texL[fill]);              /* overlap Y2R(L) w/ decode(R) */
                p = vq_pop(&n); ap.data = p; ap.size = n; got = 0;
                int okR = (mobi_decode(&ctx, fR, &got, &ap) >= 0 && got); free(p);
                dec_ticks += svcGetSystemTick() - _dt; dec_frames += 2;
                if (elide) { elided++; dpair++; goto phase2_done; }
                if (okL && okR) {
                    /* Blocking R-eye wait, ON PURPOSE. Deferring it (overlapping Y2R with the next
                     * decode) LOOKED like free throughput but measured WORSE: in-player decode fell
                     * 26-33 -> 22 pairs/s, because the Y2R DMA then contends with the decoder for the
                     * memory bus while it runs. Same effect the old notes saw with prefetch. The clock
                     * time we "save" waiting is paid back with interest in slower decode. Leave it. */
                    y2r_wait(&texL[fill]); y2r_start(fR, &texR[fill]); y2r_wait(&texR[fill]);
                    rts[fill] = dpair * pair_dur; ready[fill] = 1; wr++; loop_pairs++;
                } else if (okL) { y2r_wait(&texL[fill]); }
                if (!okL || !okR) nfail++;   /* pair dropped by a DECODE ERROR (not by catch-up) */
                dpair++;
            }
        }
        phase2_done:;

        /* ---- pre-roll: bank audio while priming the ring, then unpause aligned to content-0 ---- */
        if (!gated && wr >= PRIME) { gated = 1; ndspChnSetPaused(0, false); g_apos = 0; }

        audio_poll();   /* advance the audio clock from real DSP playback (not just when feeding) */

        /* ---- present, audio-master. Drop stale ringed pairs by their true content-time (rts) so video
         *      stays locked to the audio clock. g_apos = -1 until the pre-roll unpauses -> video waits. */
        while ((wr - rd) > 1 && ready[rd % NB] && rts[(rd + 1) % NB] <= g_apos) rd++;
        if ((wr - rd) > 0 && ready[rd % NB] && g_apos >= rts[rd % NB]) {
            vpts = rts[rd % NB]; draw_present(rd % NB); rd++; shown++;
            last_present = osGetTime();                  /* picture actually changed */
        } else if ((wr - rd) == 0 && g_vqn < 2 && done) {
            break;   /* ring + backlog drained + EOF */
        } else if (gated) {
            /* PICTURE IS HELD. Attribute WHY -- these need completely different fixes and we cannot
             * tell them apart from the outside:
             *   EMPTY  = ring starved: decode genuinely slower than real time
             *   GATE   = ring HAS a pair but its content-time is in the FUTURE vs the audio clock,
             *            so we refuse to show it (a timeline/rts bug, not a speed problem)
             * plus nfail = pairs lost to DECODE ERRORS, and skipping = we're dropping to a keyframe. */
            if ((wr - rd) == 0) hold_empty++; else hold_gate++;
            u64 held = osGetTime() - last_present;
            /* latch the state AT THE PEAK of a freeze, so it can be read after the fact */
            if (held > 1000 || held > maxhold) {
                Snap sn; sn.live = 1; sn.d = dfps_live; sn.q = wr - rd;
                sn.e = (g_apos >= 0) ? (int)(vpts - g_apos) / 1000 : 0;
                sn.drop = dropped; sn.bk = g_vqn; sn.kf = g_vqkfn; sn.fail = nfail;
                sn.empty = hold_empty; sn.gate = hold_gate; sn.skip = skipping;
                sn.held = held; sn.apos = (long long)(g_apos / 1000);
                for (int k = 0; k < 13; k++) sn.err[k] = mobi_err[k];
                for (int k = 0; k < 3; k++) sn.sub[k] = mobi_sub[k];
                sn.ahead = ahead;
                if (!snapA.live && held > 1000) snapA = sn;      /* FIRST freeze > 1s: latch once */
                if (held > maxhold) snapB = sn;                  /* WORST freeze: re-latch at its peak */
            }
            if (held > maxhold) maxhold = held;           /* longest single freeze this run */
        }

        /* ---- tiny readout (~4x/sec): e = A/V error ms, q = ring depth, fps ---- */
        u64 now = osGetTime();
        if (now - fps_t0 >= 500) {
            fps = shown * 1000.0 / (now - fps_t0); shown = 0; fps_t0 = now;
            int dfps = (dec_ticks > 0) ? (int)((double)SYSCLOCK_ARM11 * dec_frames / (double)dec_ticks / 2.0) : 0;
            dfps_live = dfps;
            u64 lt = svcGetSystemTick() - loop_t0;
            if (lt > 0) { dps = dfps; lps = (double)SYSCLOCK_ARM11 * loop_pairs / (double)lt; }
            loop_t0 = svcGetSystemTick(); loop_pairs = 0;
            dec_ticks = 0; dec_frames = 0;   /* d = raw decode pairs/sec (>=24 => render/bank-bound; <24 => decode wall) */
            int64_t apos = g_apos;
            int e = (apos >= 0) ? (int)(vpts - apos) / 1000 : 0;   /* last-presented content-time vs audio */
            u64 held = osGetTime() - last_present;
            printf("\x1b[3;0HLOOP%5.1f (need 24.0) dec%5.1f el%d\n"
                   "\x1b[9;0Hsub: walk%d any%d EMPTY%d      \n",
                   lps, dps, elided,
                   mobi_sub[0], mobi_sub[1], mobi_sub[2]);
            printf("\x1b[4;0Hd%3d drop%d bk%d kf%d fail%d   "
                   "\x1b[5;0Hfps%5.1f q%2d e%6dms apos%llds   "
                   "\x1b[6;0HHELD now%4llums  max%4llums   "
                   "\x1b[7;0Hwhy: empty%d gate%d %s      \n",
                   dfps, dropped, g_vqn, g_vqkfn, nfail,
                   fps, wr - rd, e, (long long)(apos / 1000000),
                   (unsigned long long)held, (unsigned long long)maxhold,
                   hold_empty, hold_gate, skipping ? "SKIPPING" : "");
            /* WHICH decoder error path is firing -- 9=null-ref 10=mv-out-of-bounds 1=bad-quantizer
             * 6/7/12=coef-table 3/4=coef-pos 11=pkt-size. Only nonzero ones are shown. */
            printf("\x1b[8;0Herr:");
            for (int k = 0; k < 13; k++) if (mobi_err[k]) printf(" %d=%d", k, mobi_err[k]);
            printf("        \n");

            /* ---- LATCHED FREEZE SNAPSHOTS (these lines stay put -- read them at your leisure) ---- */
            for (int w = 0; w < 2; w++) {
                Snap *sn = w ? &snapB : &snapA;
                int row = 10 + w * 4;
                if (!sn->live) { printf("\x1b[%d;0H%s: (none yet)                    \n", row, w ? "WORST" : "FIRST>1s"); continue; }
                printf("\x1b[%d;0H%s  held%llums  %s     ", row, w ? "WORST   " : "FIRST>1s",
                       (unsigned long long)sn->held, sn->skip ? "SKIPPING" : "        ");
                printf("\x1b[%d;0H  d%3d q%2d e%6dms apos%llds fail%d    ", row + 1,
                       sn->d, sn->q, sn->e, sn->apos / 1000, sn->fail);
                printf("\x1b[%d;0H  why empty%d gate%d  drop%d bk%d kf%d   ", row + 2,
                       sn->empty, sn->gate, sn->drop, sn->bk, sn->kf);
                printf("\x1b[%d;0H  err:", row + 3);
                for (int k = 0; k < 13; k++) if (sn->err[k]) printf(" %d=%d", k, sn->err[k]);
                printf(" | sub %d/%d/E%d ahead%d      \n", sn->sub[0], sn->sub[1], sn->sub[2], sn->ahead);
            }
            printf("\x1b[19;0HY=mode[%s] A=tol X=re-arm START=exit\n",
                   mode == 0 ? "NOSKIP-clean" : mode == 1 ? "SKIP-noflush" : "SKIP+FLUSH");
        }
    }

    audio_close();

wait:
    while (aptMainLoop()) { hidScanInput(); if (hidKeysDown() & (KEY_B | KEY_START)) break; gspWaitForVBlank(); }
    ndspExit();
    gfxExit();
    return 0;
}
