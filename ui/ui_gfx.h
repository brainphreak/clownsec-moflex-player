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
#define UI_WHITE UI_RGB(255,255,255)
#define UI_BLACK UI_RGB(0,0,0)
#define UI_GRAY  UI_RGB(140,140,140)
#define UI_RED   UI_RGB(255,80,60)
/* neon / cyberpunk palette */
#define UI_NEON  UI_RGB(60,255,150)    /* CLOWNSEC green */
#define UI_NEONP UI_RGB(180,90,255)    /* purple accent */
#define UI_NEONC UI_RGB(60,210,255)    /* cyan accent */
#define UI_BG    UI_RGB(9,10,18)       /* near-black background */
#define UI_BG2   UI_RGB(20,22,36)      /* panel background */
#define UI_INK   UI_RGB(205,214,235)   /* light text */
#define UI_DIM   UI_RGB(90,100,130)    /* muted text / borders */

void ui_begin(gfxScreen_t screen);         /* draw to offscreen for GFX_TOP or GFX_BOTTOM */
void ui_present(void);                     /* copy offscreen -> that screen's framebuffer */
int  ui_width(void);                       /* current target width (320 or 400) */
void ui_clear(u16 c);
void ui_px(int x, int y, u16 c);
void ui_fill(int x, int y, int w, int h, u16 c);
void ui_text(int x, int y, int scale, u16 c, const char *s);
int  ui_text_w(int scale, const char *s);
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
