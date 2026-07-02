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

void ui_begin(gfxScreen_t screen);         /* draw to offscreen for GFX_TOP or GFX_BOTTOM */
void ui_present(void);                     /* copy offscreen -> that screen's framebuffer */
int  ui_width(void);                       /* current target width (320 or 400) */
void ui_clear(u16 c);
void ui_px(int x, int y, u16 c);
void ui_fill(int x, int y, int w, int h, u16 c);
void ui_text(int x, int y, int scale, u16 c, const char *s);
int  ui_text_w(int scale, const char *s);
void ui_text_center(int cx, int y, int scale, u16 c, const char *s);
void ui_play(int cx, int cy, int size, u16 c);    /* right-pointing triangle */
void ui_play_l(int cx, int cy, int size, u16 c);  /* left-pointing triangle */
void ui_pause(int cx, int cy, int size, u16 c);   /* two bars */

#endif
