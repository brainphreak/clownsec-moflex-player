/* Tiny immediate-mode drawing for the 3DS bottom screen (320x240 RGB565),
 * written straight to the framebuffer (no GPU) so it coexists with the
 * direct-framebuffer video path. */
#ifndef UI_GFX_H
#define UI_GFX_H
#include <3ds.h>

#define UI_W 320
#define UI_TW 400          /* top screen width */
#define UI_H 240
#define UI_RGB(r,g,b) ((u16)((((r)&0xF8)<<8)|(((g)&0xFC)<<3)|((b)>>3)))
#define UI_BLACK UI_RGB(0,0,0)

/* ---- runtime theme: the whole UI palette is a struct so it can be changed at
 * runtime (Themes screen). The accents + base bg/text come from a preset; the
 * structural shades (panels/rows/borders) are derived so any base bg stays coherent. */
typedef struct {
    u16 accent, accent2, accent3;  /* the three accents (green/purple/cyan on the default) */
    u16 red;                        /* danger / exit */
    u16 hi;                         /* brightest text (selected rows, knobs) */
    u16 ink;                        /* normal text */
    u16 dim;                        /* muted text / borders */
    u16 bg;                         /* screen base (darkest) */
    u16 bg1, bg2, sel, sello, line, track;   /* derived by theme_set() */
} UiTheme;
extern UiTheme g_theme;
int  theme_count(void);
const char *theme_name(int i);
int  theme_current(void);
void theme_set(int i);      /* apply preset i (also derives the structural shades) */
void theme_preview(int i, u16 *accent, u16 *accent2, u16 *accent3, u16 *bg);  /* colors for a swatch */
/* editable "Custom" theme (index == theme_custom_index(), the last theme) */
int  theme_custom_index(void);
int  theme_elem_count(void);            /* editable palette entries (accents/text/bg...) */
const char *theme_elem_name(int elem);
int  theme_custom_get(int elem, int ch);        /* ch: 0=R 1=G 2=B, returns 0..255 */
void theme_custom_set(int elem, int ch, int val); /* live-applies if Custom is active */
void theme_custom_reset(void);
void theme_load(void);      /* load saved index from SD (call once at startup) */
void theme_save(void);

/* palette macros now read the runtime theme (all ~200 existing usages just work) */
#define UI_NEON   (g_theme.accent)
#define UI_NEONP  (g_theme.accent2)
#define UI_NEONC  (g_theme.accent3)
#define UI_RED    (g_theme.red)
#define UI_WHITE  (g_theme.hi)
#define UI_GRAY   (g_theme.dim)
#define UI_INK    (g_theme.ink)
#define UI_DIM    (g_theme.dim)
#define UI_BG     (g_theme.bg)
#define UI_BG2    (g_theme.bg2)
/* structural tiers (the hardcoded dark shades map to these) */
#define TH_BG1    (g_theme.bg1)
#define TH_SEL    (g_theme.sel)
#define TH_SELLO  (g_theme.sello)
#define TH_LINE   (g_theme.line)
#define TH_TRACK  (g_theme.track)

void ui_begin(gfxScreen_t screen);         /* draw to offscreen for GFX_TOP or GFX_BOTTOM */
void ui_present(void);                     /* copy offscreen -> that screen's framebuffer */
void ui_capture(int on);                   /* when on, ui_present() is a no-op: the caller reads ui_pixels()
                                              itself (used to draw the UI as a citro2d texture instead of the
                                              framebuffer, which deadlocks against citro2d owning the GPU) */
const u16 *ui_pixels(void);                /* offscreen buffer; pixel (x,y) is at [x*UI_H + (UI_H-1-y)] */
int  ui_width(void);                       /* current target width (320 or 400) */
void ui_clear(u16 c);
void ui_px(int x, int y, u16 c);
void ui_fill(int x, int y, int w, int h, u16 c);
void ui_text(int x, int y, int scale, u16 c, const char *s);
int  ui_text_w(int scale, const char *s);
int  ui_u8_len(const char *s);              /* number of UTF-8 codepoints (for layout/centering) */
int  ui_u8_bytes(const char *s, int ncp);   /* byte length of the first ncp codepoints (safe truncation) */
void ui_text_center(int cx, int y, int scale, u16 c, const char *s);
/* draw text but only the pixels within [clipL, clipR) horizontally (for marquee/scrolling titles) */
void ui_text_clipped(int x, int y, int scale, u16 c, const char *s, int clipL, int clipR);
void ui_play(int cx, int cy, int size, u16 c);    /* right-pointing triangle */
void ui_play_l(int cx, int cy, int size, u16 c);  /* left-pointing triangle */
void ui_pause(int cx, int cy, int size, u16 c);   /* two bars */

/* ---- widget kit (rounded / gradient / glow, for the graphical UI) ---- */
void ui_blend_px(int x, int y, u16 c, int a);                         /* alpha 0..255 */
void ui_fill_round(int x, int y, int w, int h, int r, u16 c);
void ui_vgrad_round(int x, int y, int w, int h, int r, u16 top, u16 bot);   /* vertical gradient */
void ui_frame_round(int x, int y, int w, int h, int r, u16 c, int thick);   /* rounded border */
void ui_glow_round(int x, int y, int w, int h, int r, u16 c, int spread, int alpha);
/* a themed button: rounded gradient fill + border + centered label, brighter when selected */
void ui_button(int x, int y, int w, int h, const char *label, int selected, u16 accent);

#endif
