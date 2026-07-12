/* Minimal MP4/H.264 playback through MVD (New-3DS hardware). See mp4_play.h.
 * This is the "does MVD decode + display?" milestone: demux -> feed samples to MVD -> render to the
 * top screen, paced to the video's frame rate, B to exit. No audio/seek/subtitles/3D yet. */
#include "mp4_play.h"
#include "mp4_demux.h"
#include "mp4_mvd.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* quick message on the bottom-screen console (the caller left one active) */
static void mp4_msg(const char *m) {
    printf("\x1b[2J\x1b[H%s\n\nPress B.\n", m);
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & (KEY_A | KEY_B)) break;
        gspWaitForVBlank();
    }
}

MoflexResult mp4_play(const char *path) {
    bool isnew = false; APT_CheckNew3DS(&isnew);
    if (!isnew) { mp4_msg("MP4 playback needs a New 3DS\n(it uses the hardware H.264 decoder)."); return MOFLEX_QUIT_BACK; }

    Mp4 m;
    if (!mp4_open(&m, path)) { mp4_msg("Could not open the MP4\n(no H.264 video track found)."); return MOFLEX_QUIT_BACK; }

    /* top screen: MVD outputs RGB565 straight to the framebuffer (it handles the rotation) */
    gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
    gfxSet3D(false);
    gfxSetDoubleBuffering(GFX_TOP, true);

    if (!mp4_mvd_init(m.width, m.height, m.avcc, m.avcc_len, m.nal_length_size)) {
        mp4_msg("MVD init failed.\nH.264 profile/level may be unsupported\n(try Baseline/Main).");
        mp4_close(&m); return MOFLEX_QUIT_BACK;
    }

    /* read buffer sized to the largest sample (keyframes are the biggest) */
    uint32_t maxsz = 1;
    for (int i = 0; i < m.v_count; i++) if (m.vsamples[i].size > maxsz) maxsz = m.vsamples[i].size;
    uint8_t *buf = (uint8_t *)malloc(maxsz + 16);
    if (!buf) { mp4_mvd_exit(); mp4_close(&m); return MOFLEX_ERROR; }

    printf("\x1b[2J\x1b[HMP4 (test): %dx%d, %d frames\nB = back\n", m.width, m.height, m.v_count);

    /* Pace by counting display refreshes so 24fps content gets a steady 3:2 pulldown (each frame
     * held 2 or 3 vblanks in an even pattern) -- a ms timer quantizes unevenly and judders. */
    double dur_s = mp4_duration_s(&m);
    double fps   = (dur_s > 0 && m.v_count > 1) ? (m.v_count - 1) / dur_s : 30.0;
    if (fps < 1.0 || fps > 120.0) fps = 30.0;
    double vpf = 59.83 / fps;                 /* display vblanks per video frame (3DS refresh ~59.83 Hz) */
    if (vpf < 1.0) vpf = 1.0;
    double acc = 0.0;

    MoflexResult result = MOFLEX_QUIT_BACK;
    for (int vi = 0; vi < m.v_count && aptMainLoop(); vi++) {
        hidScanInput();
        if (hidKeysDown() & KEY_B) { result = MOFLEX_QUIT_BACK; break; }

        Mp4Sample *s = &m.vsamples[vi];
        int n = mp4_read_sample(&m, s, buf);
        int got = (n == (int)s->size && mp4_mvd_decode(buf, n, GFX_LEFT));   /* decode into the back buffer */

        acc += vpf;                                              /* hold the current frame a steady 2/3 vblanks */
        int nv = (int)(acc + 1e-6); if (nv < 1) nv = 1;
        acc -= nv;
        for (int i = 0; i < nv; i++) gspWaitForVBlank();
        if (got) { gfxFlushBuffers(); gfxSwapBuffers(); }
    }

    mp4_mvd_exit();
    free(buf);
    mp4_close(&m);
    return result;
}
