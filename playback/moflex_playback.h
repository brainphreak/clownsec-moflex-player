/* Reusable native moflex (MobiClip 3D video + IMA-ADPCM audio) player.
 *
 * Drop-in: one call plays a .moflex to the top screen in stereoscopic 3D with
 * audio, returning when the file ends or the user backs out.
 *
 * Caller requirements (do these ONCE at app startup, not per file):
 *   gfxInitDefault();
 *   ndspInit();            // if you want audio; moflex_play uses channel 0
 * moflex_play() sets the top screen to RGB565 + enables 3D itself.
 */
#ifndef MOFLEX_PLAYBACK_H
#define MOFLEX_PLAYBACK_H

typedef enum {
    MOFLEX_QUIT_BACK = 0,   /* user pressed B/START -> return to your menu   */
    MOFLEX_QUIT_EXIT = 1,   /* aptMainLoop() ended -> your app should exit   */
    MOFLEX_EOF       = 2,   /* file played to the end                        */
    MOFLEX_QUIT_OPEN = 3,   /* user tapped OPEN -> host should open the browser */
    MOFLEX_ERROR     = -1   /* open/decode error                            */
} MoflexResult;

/* Plays path (e.g. "sdmc:/movie.moflex"). Blocking until done. Auto-resumes from
 * the saved position and checkpoints it as it plays. */
MoflexResult moflex_play(const char *path);

/* Saved resume position (microseconds) for a movie, or 0 if none. */
long long moflex_resume_get(const char *path);

/* Shared resume + volume stores, so the MP4 player keeps position/volume consistent with moflex. */
void  moflex_resume_save(const char *path, long long us);
void  moflex_resume_clear(const char *path);
float moflex_vol_get(void);          /* persisted playback volume (0.25..4.0) */
void  moflex_vol_set(float v);       /* clamp to 0.25..4.0 and persist */

/* Shared subtitle system, so the MP4 player reuses the same SRT loading/menu/overlay. */
void moflex_subs_autoload(const char *moviepath);   /* load a sidecar/moviedata .srt if present */
void moflex_sub_menu(const char *moviepath, int is3d);  /* the CC options menu (bottom screen) */
void moflex_sub_overlay(int is3d, long long us);    /* draw the active cue onto the top framebuffer */
int  moflex_subs_on(void);                          /* subtitles currently enabled? (for the CC button) */

#endif
