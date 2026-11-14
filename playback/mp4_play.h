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
    /* audio tracks for the SOUND menu (n_audio < 2: no choice). set_audio is called when the
     * viewer picks one; the stream then restarts where it is. */
    int         n_audio, audio_sel;
    const char  (*audio_lang)[8];
    const char  (*audio_name)[48];
    void        (*set_audio)(void *ctx, int idx);
    /* subtitles that arrive while the video plays: whole SRT blocks in movie time, 0 = none yet.
     * NULL = no such track. */
    int         (*subs_take)(void *ctx, long long now_us, char *buf, int cap);
    /* the server's subtitle tracks for the CC menu (n_subs < 2: no choice). set_sub is called
     * when the viewer picks one and answers with STREAM_SUB_* flags; path gets a file to load. */
    int         n_subs, sub_sel;
    const char  (*sub_name)[48];
    int         (*set_sub)(void *ctx, int idx, long long now_us, char *path, int cap);
} StreamSrc;
#define STREAM_SUB_FILE    1   /* load the file in path */
#define STREAM_SUB_ARRIVE  2   /* cues will come through subs_take: start empty */
#define STREAM_SUB_BURN    4   /* burned in by the server from now on */
#define STREAM_SUB_RESTART 8   /* the picture changes (burn on/off): restart the stream here */
/* why the last stream_play ended early ("" = it didn't): shown by the caller in its own UI */
const char *stream_last_error(void);
MoflexResult stream_play(const StreamSrc *src);

#endif
