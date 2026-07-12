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

    /* read buffer sized to the largest sample (keyframes are the biggest) */
    uint32_t maxsz = 1;
    for (int i = 0; i < m.v_count; i++) if (m.vsamples[i].size > maxsz) maxsz = m.vsamples[i].size;
    uint8_t *buf = (uint8_t *)malloc(maxsz + 16);
    if (!buf) { mp4_mvd_exit(); mp4_close(&m); return MOFLEX_ERROR; }

    printf("\x1b[2J\x1b[HMP4 (test): %dx%d, %d frames\nB = back\n", m.width, m.height, m.v_count);

    /* Pace with the VBLANK as the clock, not wall-clock ms. The panel scans out only at vblank
     * (~59.83 Hz), so a frame can only ever change on a vblank boundary. Timing against osGetTime()
     * (1 ms integer, and drifting vs the vblanks) makes each frame round to an uneven number of
     * refreshes -- 24fps came out as holds of 3,3,2,3... instead of a clean 3,2,3,2, which reads as
     * juddery "not fluid" motion. Instead we count vblanks and hold each frame until its target
     * vblank round((vi+1) * refreshes_per_frame): that yields an exact, even 3:2 pulldown AND
     * absorbs decode jitter (a slow decode just eats into the hold instead of shifting the frame). */
    double dur_s = mp4_duration_s(&m);
    double fps   = (dur_s > 0 && m.v_count > 1) ? (m.v_count - 1) / dur_s : 30.0;
    if (fps < 1.0 || fps > 120.0) fps = 30.0;
    double vpf = 59.83 / fps;                 /* display refreshes per video frame */
    if (vpf < 1.0) vpf = 1.0;
    u64  vb = 0;                              /* vblanks elapsed since playback started */
    long hold_until = 0;                      /* keep the shown frame until this vblank count */

    MoflexResult result = MOFLEX_QUIT_BACK;
    int quit = 0;
    for (int vi = 0; vi < m.v_count && aptMainLoop() && !quit; vi++) {
        hidScanInput();
        if (hidKeysDown() & KEY_B) { result = MOFLEX_QUIT_BACK; break; }

        Mp4Sample *s = &m.vsamples[vi];
        int n = mp4_read_sample(&m, s, buf);
        int got = (n == (int)s->size && mp4_mvd_decode(buf, n));   /* decode into the internal buffer */

        /* hold the previous frame until its slot is up (decode above ran during this hold) */
        while (vb < (u64)hold_until && aptMainLoop()) {
            gspWaitForVBlank(); vb++;
            hidScanInput();
            if (hidKeysDown() & KEY_B) { result = MOFLEX_QUIT_BACK; quit = 1; break; }
        }
        if (quit) break;

        if (got) { mp4_mvd_present(sbs); gfxFlushBuffers(); gfxSwapBuffers(); }  /* blit eye(s) -> back buffer */
        /* the swap flips at the next vblank; this wait lands it (and makes this frame visible)
         * before the next iteration blits into the new back buffer -- prevents two-frame tearing */
        gspWaitForVBlank(); vb++;
        hold_until = (long)((vi + 1) * vpf + 0.5);   /* even 3:2 cadence: 2,3,2,3 refreshes */
    }

    mp4_mvd_exit();
    free(buf);
    mp4_close(&m);
    return result;
}
