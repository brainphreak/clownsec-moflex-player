/* MP4/H.264 playback through MVD (New-3DS hardware video) + Helix AAC-LC audio.
 * demux -> feed video samples to MVD, decode AAC to ndsp -> present video slaved to the audio
 * clock (audio is the master, plays at natural 1.0x). 2D and side-by-side 3D. B to exit.
 * Still no seek/subtitles. */
#include "mp4_play.h"
#include "mp4_demux.h"
#include "mp4_mvd.h"
#include "mp4_aac.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NWB          16          /* audio wavebufs (~16 * 1024 samples ~ 370ms cushion @44.1k) */
#define AAC_MAXSAMP  2048         /* max PCM samples/channel a Helix frame can emit (LC=1024) */

/* quick message on the bottom-screen console (the caller left one active) */
static void mp4_msg(const char *m) {
    printf("\x1b[2J\x1b[H%s\n\nPress B.\n", m);
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & (KEY_A | KEY_B)) break;
        gspWaitForVBlank();
    }
}

/* ---- audio state (channel 0) ---- */
static Mp4Aac     g_aac;
static ndspWaveBuf g_wb[NWB];
static int16_t   *g_abuf[NWB];
static int        g_slot_ns[NWB];      /* samples currently queued in each slot */
static int        g_have_audio;
static int        g_a_next;            /* next audio sample index to decode */
static long long  g_a_played;          /* samples fully consumed by the DSP (the master clock) */
static int        g_a_started;

/* Refill any free/done wavebuf with the next decoded AAC access unit, and account finished ones. */
static void pump_audio(Mp4 *m, uint8_t *atmp) {
    if (!g_have_audio) return;
    for (int i = 0; i < NWB; i++) {
        ndspWaveBuf *wb = &g_wb[i];
        if (wb->status != NDSP_WBUF_FREE && wb->status != NDSP_WBUF_DONE) continue;
        if (wb->status == NDSP_WBUF_DONE) { g_a_played += g_slot_ns[i]; g_slot_ns[i] = 0; }
        if (g_a_next >= m->a_count) continue;                       /* no more audio to queue */
        Mp4Sample *as = &m->asamples[g_a_next];
        int n  = mp4_read_sample(m, as, atmp);
        int sp = (n == (int)as->size) ? mp4_aac_decode(&g_aac, atmp, n, g_abuf[i]) : 0;
        g_a_next++;
        if (sp <= 0) continue;                                     /* skip a frame that produced nothing */
        DSP_FlushDataCache(g_abuf[i], sp * g_aac.channels * 2);
        memset(wb, 0, sizeof *wb);
        wb->data_vaddr = g_abuf[i];
        wb->nsamples   = sp;
        g_slot_ns[i]   = sp;
        ndspChnWaveBufAdd(0, wb);
        g_a_started = 1;
    }
}

/* audible position in microseconds, or -1 before audio starts flowing */
static double audio_us(void) {
    if (!g_have_audio || !g_a_started) return -1.0;
    return (double)g_a_played * 1e6 / (double)g_aac.rate;
}

MoflexResult mp4_play(const char *path) {
    bool isnew = false; APT_CheckNew3DS(&isnew);
    if (!isnew) { mp4_msg("MP4 playback needs a New 3DS\n(it uses the hardware H.264 decoder)."); return MOFLEX_QUIT_BACK; }

    Mp4 m;
    if (!mp4_open(&m, path)) { mp4_msg("Could not open the MP4\n(no H.264 video track found)."); return MOFLEX_QUIT_BACK; }

    /* width >= two eyes wide -> side-by-side stereo 3D (each eye 400-wide); else flat 2D */
    int sbs = (m.width >= 800);

    /* top screen: RGB565, double-buffered so both eyes land in the back buffer before one atomic
     * swap (no left-before-right tearing on 3D); 3D on only for SBS content */
    gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
    gfxSet3D(sbs);
    gfxSetDoubleBuffering(GFX_TOP, true);

    if (!mp4_mvd_init(m.width, m.height, m.avcc, m.avcc_len, m.nal_length_size)) {
        mp4_msg("MVD init failed.\nH.264 profile/level may be unsupported\n(try Baseline/Main).");
        mp4_close(&m); return MOFLEX_QUIT_BACK;
    }

    /* video read buffer sized to the largest sample (keyframes are the biggest) */
    uint32_t maxsz = 1;
    for (int i = 0; i < m.v_count; i++) if (m.vsamples[i].size > maxsz) maxsz = m.vsamples[i].size;
    uint8_t *buf = (uint8_t *)malloc(maxsz + 16);
    if (!buf) { mp4_mvd_exit(); mp4_close(&m); return MOFLEX_ERROR; }

    /* ---- audio setup (AAC-LC via Helix -> ndsp channel 0) ---- */
    uint8_t *atmp = NULL;
    memset(g_wb, 0, sizeof g_wb); memset(g_abuf, 0, sizeof g_abuf); memset(g_slot_ns, 0, sizeof g_slot_ns);
    g_have_audio = 0; g_a_next = 0; g_a_played = 0; g_a_started = 0;
    if (m.has_audio && mp4_aac_open(&g_aac, m.asc, m.asc_len, m.a_rate, m.a_channels)) {
        uint32_t amax = 1;
        for (int i = 0; i < m.a_count; i++) if (m.asamples[i].size > amax) amax = m.asamples[i].size;
        atmp = (uint8_t *)malloc(amax + 16);
        int ok = (atmp != NULL);
        for (int i = 0; i < NWB && ok; i++) {
            g_abuf[i] = (int16_t *)linearAlloc(AAC_MAXSAMP * g_aac.channels * 2);
            if (!g_abuf[i]) ok = 0; else g_wb[i].status = NDSP_WBUF_DONE;
        }
        if (ok) {
            ndspChnReset(0);
            ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE);
            ndspChnSetRate(0, (float)g_aac.rate);
            ndspChnSetFormat(0, g_aac.channels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
            float mix[12]; memset(mix, 0, sizeof mix); mix[0] = mix[1] = 1.0f; ndspChnSetMix(0, mix);
            g_have_audio = 1;
        } else {
            for (int i = 0; i < NWB; i++) if (g_abuf[i]) { linearFree(g_abuf[i]); g_abuf[i] = NULL; }
            free(atmp); atmp = NULL; mp4_aac_close(&g_aac);
        }
    }

    printf("\x1b[2J\x1b[HMP4: %dx%d, %d frames%s%s\nB = back\n",
           m.width, m.height, m.v_count, sbs ? ", 3D" : "", g_have_audio ? ", AAC" : ", (no audio)");

    /* Video timeline. Audio is the master clock when present: present frame vi once the audible
     * position reaches vi/fps. Without audio, fall back to a vblank-counted 3:2 pulldown. Either
     * way we present only on vblank boundaries (double-buffered, swap then wait) for a clean cadence. */
    double dur_s = mp4_duration_s(&m);
    double fps   = (dur_s > 0 && m.v_count > 1) ? (m.v_count - 1) / dur_s : 30.0;
    if (fps < 1.0 || fps > 120.0) fps = 30.0;
    double frame_us = 1e6 / fps;
    double vpf = 59.83 / fps; if (vpf < 1.0) vpf = 1.0;   /* refreshes/frame (no-audio fallback) */
    u64  vb = 0; long hold_vb = 0;

    /* prime the audio buffers so the DSP has a cushion before the first frame shows */
    if (g_have_audio) pump_audio(&m, atmp);

    MoflexResult result = MOFLEX_QUIT_BACK;
    int quit = 0;
    for (int vi = 0; vi < m.v_count && aptMainLoop() && !quit; vi++) {
        hidScanInput();
        if (hidKeysDown() & KEY_B) { result = MOFLEX_QUIT_BACK; break; }

        Mp4Sample *s = &m.vsamples[vi];
        int n = mp4_read_sample(&m, s, buf);
        int got = (n == (int)s->size && mp4_mvd_decode(buf, n));   /* decode into the internal buffer */
        pump_audio(&m, atmp);

        /* hold until this frame is due (keep audio fed throughout the wait) */
        double due_us = vi * frame_us;
        for (;;) {
            pump_audio(&m, atmp);
            if (g_have_audio) {
                double a = audio_us();
                if (a < 0.0) break;                 /* audio not flowing yet -> show first frame now */
                if (a >= due_us) break;             /* audio has reached this frame */
            } else {
                if (vb >= (u64)hold_vb) break;      /* no-audio: vblank cadence */
            }
            if (!aptMainLoop()) { quit = 1; break; }
            gspWaitForVBlank(); vb++;
            hidScanInput();
            if (hidKeysDown() & KEY_B) { result = MOFLEX_QUIT_BACK; quit = 1; break; }
        }
        if (quit) break;

        if (got) { mp4_mvd_present(sbs); gfxFlushBuffers(); gfxSwapBuffers(); }  /* blit eye(s) -> back buffer */
        /* the swap flips at the next vblank; wait for it before the next iteration blits into the
         * new back buffer -- prevents two frames tearing together */
        gspWaitForVBlank(); vb++;
        hold_vb = (long)((vi + 1) * vpf + 0.5);
    }

    if (g_have_audio) {
        ndspChnWaveBufClear(0); ndspChnReset(0);
        for (int i = 0; i < NWB; i++) if (g_abuf[i]) { linearFree(g_abuf[i]); g_abuf[i] = NULL; }
        mp4_aac_close(&g_aac);
    }
    free(atmp);
    mp4_mvd_exit();
    free(buf);
    mp4_close(&m);
    return result;
}
