/* Minimal MP4 (H.264/AVC) playback via the New-3DS MVD hardware decoder.
 * VIDEO-ONLY test path for now (no audio/seek/panel) -- proves MVD decodes on hardware.
 * New-3DS only; on Old 3DS it shows a "needs New 3DS" notice and returns. */
#ifndef MP4_PLAY_H
#define MP4_PLAY_H

#include "moflex_playback.h"   /* MoflexResult */

MoflexResult mp4_play(const char *path);

/* ---- a network stream (Plex; later Jellyfin): MPEG-TS of H.264 + AAC, New-3DS only ----
 * The source opens the stream at a time (a fresh server session each call -- a seek is a new
 * stream) and is told where playback is. Everything on screen is the MP4 player's. */
#include "httpstream.h"
typedef struct {
    void       *ctx;
    const char *title;
    const char *key;          /* a stable id for per-video settings ("plex:12345") */
    long long   dur_us;
    long long   start_us;     /* resume point (0 = beginning) */
    int         fmt;          /* 0 = 2D, 1 = side-by-side, 2 = half side-by-side (the VIDEO menu's
                               * Format row overrides it per key) */
    HttpStream *(*open)(void *ctx, long long at_us, int fmt);   /* fmt != 0: ask for a 3D-sized frame */
    void        (*close)(void *ctx, HttpStream *s);
    void        (*report)(void *ctx, const char *state, long long pos_us);   /* may be NULL */
    const char *(*error)(void *ctx);                                        /* why open failed */
    const char *subfile;      /* a subtitle file to show (drawn by the player), or NULL */
    int         burnable;     /* the server can burn a subtitle track into the picture */
    int         subs_on;      /* the player's subtitle switch at start */
    void        (*set_burn)(void *ctx, int on);   /* burnable: the switch flipped (stream restarts) */
} StreamSrc;
/* why the last stream_play ended early ("" = it didn't): shown by the caller in its own UI */
const char *stream_last_error(void);
MoflexResult stream_play(const StreamSrc *src);

#endif
