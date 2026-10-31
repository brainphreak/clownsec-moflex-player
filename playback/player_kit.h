/* The moflex player's controls, menus and GPU presentation, packaged for another decoder.
 *
 * The MP4 path used to carry its own copy of the panel, dark screen, touch handling and frame
 * blit, and every difference from the moflex player was a bug or a missing feature. This is the
 * moflex ring engine's own code -- panel, CC / AUDIO / VIDEO menus, dark screen, L+R lock, lid
 * watchdog, subtitle renderer, zoom / convergence / ghost draw, auto level -- exposed as calls, so
 * the MP4 player behaves exactly like it. The moflex engines themselves do not call this; it only
 * wraps their static helpers, so nothing about moflex playback changes.
 *
 * Frame flow for a caller: decode -> write the picture into a GT-sized RGB565 texture (the frame
 * occupies KIT_FRAME_W x KIT_FRAME_H at the top-left, like the ring's own textures) -> kit_draw. */
#ifndef PLAYER_KIT_H
#define PLAYER_KIT_H

#include <3ds.h>
#include <citro2d.h>
#include <stdint.h>

#define KIT_TEX_W   512   /* texture size, same as the ring's (GT_W x GT_H) */
#define KIT_TEX_H   256
#define KIT_FRAME_W 400   /* the picture area inside it: one top-screen eye */
#define KIT_FRAME_H 240

typedef struct { C3D_RenderTarget *topL, *topR, *bot; } KitGpu;

/* GPU on/off around a film: the same clean gfx hand-over the ring engine does. kit_gpu_begin
 * returns 0 (screens restored) if the GPU could not start. closing = the app is exiting. */
int  kit_gpu_begin(KitGpu *g, int is3d);
void kit_gpu_end(KitGpu *g, int closing);

/* Per-film state: panel services (battery, backlight), picture settings, menus closed, lock off,
 * letterbox + leveller reset, subtitle renderer. One audio track (no language row). */
void kit_film_begin(int is3d);
void kit_film_end(void);

/* 3D format: 0 = 2D, 1 = side-by-side (two full eyes), 2 = half side-by-side (two squeezed eyes).
 * The VIDEO menu shows a Format row for it; kit_fmt_take_change reports a choice made there.
 * kit_fmt_load/save keep a per-video choice (-1 = none saved). kit_set_view3d tells the menus
 * whether stereo-only rows apply. */
void kit_fmt_set(int fmt);
int  kit_fmt_take_change(int *fmt);
void kit_set_view3d(int is3d);
int  kit_fmt_load(const char *path);
void kit_fmt_save(const char *path, int fmt);

/* The menus' SD writes are held during kit playback (a write stalls the card mid-stream); write
 * them now. Call when playback pauses and before kit_film_end. */
void kit_flush_saves(const char *path);   /* only what differs from the baseline */
void kit_saves_baseline(void);             /* after the film's settings are loaded */

/* HOME / sleep: call from the caller's APT hook. kit_take_redraw() is 1 once after a restore. */
void kit_suspend(void);
void kit_restore(void);
int  kit_take_redraw(void);

/* One frame of the ring's pre-input rules: lid watchdog, L+R lock, dark-screen wake, lock toast,
 * backlight reconcile. May clear *kd / *kh; sets *dirty when the screen needs a repaint. */
void kit_input(u32 *kd, u32 *kh, int *dirty);

/* The CC / AUDIO / VIDEO menus. While one is open it owns input (the film plays on). */
int  kit_menu_open(void);
void kit_menu_input(u32 kd, u32 kh, touchPosition tp, int is3d, const char *path);
void kit_open_subs_menu(void);        /* SELECT */
void kit_toggle_subs(void);           /* Y */
void kit_toggle_dark(void);           /* X */

/* A touch-DOWN on the panel. Menu buttons and the moon are handled inside; transport actions
 * come back for the caller. */
typedef enum {
    KIT_T_NONE = 0, KIT_T_SCRUB, KIT_T_VOL, KIT_T_PLAY, KIT_T_RW, KIT_T_FF,
    KIT_T_OPEN, KIT_T_MANAGE, KIT_T_ADD, KIT_T_HANDLED
} KitTouch;
KitTouch kit_touch(int px, int py, int is3d);
int64_t  kit_scrub_us(int px, int64_t dur_us);   /* bar x -> time */
int      kit_vol_drag(int py);                   /* slider y -> volume; 1 when it changed */
void     kit_vol_step(int dir);                  /* Up / Down */
void     kit_vol_save(void);

/* PCM through the moflex gain stage: volume, auto level, soft knee. */
void kit_audio_gain(int16_t *s, int ns);

/* Picture settings (VIDEO menu). The caller applies them itself when it fills the texture. */
void kit_pq(int *bright, int *con, int *sat);
/* Letterbox measured on the presented picture, in KIT_FRAME coordinates (for Fill screen). */
void kit_lb_merge(int top, int bot, int left, int right);

/* Draw one frame: picture (zoom / convergence / ghost / edge mask), subtitles at disp_us, and the
 * bottom screen (panel, open menu or dark). R is ignored for 2D. */
void kit_draw(KitGpu *g, C2D_Image L, C2D_Image R, int is3d, const char *title,
              int64_t disp_us, int64_t dur_us, int playing);

#endif
