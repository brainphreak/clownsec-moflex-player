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
    MOFLEX_QUIT_MAIN = 4,   /* (unused) kept for ABI stability                 */
    MOFLEX_QUIT_MANAGE = 5, /* user tapped MANAGE -> the manage browser        */
    MOFLEX_QUIT_ADD  = 6,   /* user tapped ADD VIDEO -> the add-video menu     */
    MOFLEX_ERROR     = -1   /* open/decode error                            */
} MoflexResult;

/* Plays path (e.g. "sdmc:/movie.moflex"). Blocking until done. Auto-resumes from
 * the saved position and checkpoints it as it plays. */
MoflexResult moflex_play(const char *path);

/* Saved resume position (microseconds) for a movie, or 0 if none. */
long long moflex_resume_get(const char *path);

/* Same, but for a specific embedded video inside a CIA (by its byte offset). */
long long moflex_resume_at(const char *path, long long off);

/* Forget the saved position (used by the "start from beginning" choice). */
void moflex_resume_clear(const char *path);

/* ---- shared with the MP4 player: one resume/volume/subtitle system across both formats ---- */
void  moflex_resume_save(const char *path, long long us);
float moflex_vol_get(void);          /* persisted playback volume (0.25..4.0) */
void  moflex_vol_set(float v);       /* clamp to 0.25..4.0 and persist */
void  moflex_subs_autoload(const char *moviepath);      /* load a sidecar/moviedata .srt if present */
void  moflex_sub_menu(const char *moviepath, int is3d); /* the CC options menu (bottom screen) */
void  moflex_sub_overlay(int is3d, long long us);       /* draw the active cue onto the top framebuffer */
int   moflex_subs_on(void);                             /* subtitles currently enabled? (CC button) */

/* Playback defaults: what a video that has NO saved settings of its own starts with. Once the
 * viewer changes something in a video's menus, that video keeps its own settings as before. */
#define MOFLEX_DEF_SUBS_AS_FOUND 0   /* the old behaviour: a sidecar .srt shows, built-in waits */
#define MOFLEX_DEF_SUBS_ON       1
#define MOFLEX_DEF_SUBS_OFF      2
#define MOFLEX_DEF_SUBS_FOREIGN  3   /* on when the audio is not in the subtitle language */
#define MOFLEX_DEF_SUBS_N        4
typedef struct {
    int alang;   /* preferred audio language, index into moflex_lang_*; 0 = file's first track */
    int subs;    /* MOFLEX_DEF_SUBS_* */
    int slang;   /* preferred built-in subtitle language; 0 = file's first track */
    int size;    /* 1..moflex_sub_size_max() */
    int pos;     /* px up from the bottom, 0..moflex_sub_pos_max() */
    int font;    /* 0 = system, 1 = pixel */
    int depth;   /* 3D depth -16..16 */
} MoflexDefaults;
void moflex_defaults_get(MoflexDefaults *d);
void moflex_defaults_set(const MoflexDefaults *d);         /* saves to the SD card */
void moflex_defaults_reset(MoflexDefaults *d);             /* the built-in values (not saved) */
int  moflex_defaults_from_movie(const char *path, MoflexDefaults *d);   /* 0 = no saved settings */
int  moflex_lang_count(void);
const char *moflex_lang_code(int i);                       /* "ENG", "JPN" ... ("" for 0) */
const char *moflex_lang_name(int i);                       /* "English", "Japanese" ... */
int  moflex_sub_size_max(void);
int  moflex_sub_pos_max(void);
void moflex_defaults_apply_subs(const char *alang);        /* MP4 path: after autoload */
/* Run the defaults screen with a live caption on the top screen, drawn by the player's own
 * renderer. frame() handles one frame of input and draws the bottom screen with ui_* as usual
 * (setting *drew when it repainted); a nonzero return ends the preview and is returned. Returns
 * -1 when the app is closing, -2 when the GPU could not start (run the screen without it).
 * The screens come back like after moflex_play(): re-init the console and the top branding. */
int  moflex_defaults_preview(const MoflexDefaults *d,
                             int (*frame)(void *ctx, unsigned kd, unsigned kh, unsigned ku,
                                          int tx, int ty, int *drew), void *ctx);

/* Watched registry: videos finished to the end. */
/* Auto-advance hook. The player normally STOPS on the last frame when a movie ends (A replays,
 * B leaves) and only returns once B is pressed -- so the host never sees MOFLEX_EOF and cannot
 * tell "finished" from "quit". Register a predicate and, for paths it accepts (a series episode
 * with another episode to follow), reaching the end returns MOFLEX_EOF instead of parking.
 * Films are untouched: with no callback, or a path it rejects, the stop-in-place stays. */
void moflex_set_autonext(int (*has_next)(const char *path));

int  moflex_watched(const char *path);
void moflex_watched_set(const char *path, int on);
/* Per-embedded-video (CIA) variants, keyed by the video's byte offset. */
int  moflex_watched_at(const char *path, long long off);
void moflex_watched_set_at(const char *path, long long off, int on);

#endif
