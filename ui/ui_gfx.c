#include "ui_gfx.h"
#include "font8x8_basic.h"
#include <string.h>

/* draw into an offscreen buffer (same rotated layout as the fb, height 240), then
 * copy in one shot in ui_present() -- avoids clear->draw flicker on the live fb.
 * Sized for the wider top screen (400x240); bottom uses the first 320 columns. */
static u16 g_buf[UI_TW * UI_H];
static u16 *g_fb = g_buf;
static int g_w = UI_W;
static gfxScreen_t g_screen = GFX_BOTTOM;

void ui_begin(gfxScreen_t screen) {
    g_screen = screen;
    g_w = (screen == GFX_TOP) ? UI_TW : UI_W;
    g_fb = g_buf;
}
int ui_width(void) { return g_w; }

void ui_present(void) {
    u16 *fb = (u16 *)gfxGetFramebuffer(g_screen, GFX_LEFT, NULL, NULL);
    memcpy(fb, g_buf, (size_t)g_w * UI_H * sizeof(u16));
    gfxFlushBuffers();
}

void ui_px(int x, int y, u16 c) {
    if ((unsigned)x < (unsigned)g_w && (unsigned)y < UI_H)
        g_fb[x * UI_H + (UI_H - 1 - y)] = c;      /* rotated framebuffer */
}

void ui_clear(u16 c) {
    int n = g_w * UI_H;
    for (int i = 0; i < n; i++) g_fb[i] = c;
}

void ui_fill(int x, int y, int w, int h, u16 c) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            ui_px(x + i, y + j, c);
}

void ui_text(int x, int y, int scale, u16 c, const char *s) {
    for (; *s; s++) {
        unsigned ch = (unsigned char)*s;
        if (ch > 127) ch = '?';
        const char *g = font8x8_basic[ch];
        for (int row = 0; row < 8; row++) {
            char bits = g[row];
            for (int col = 0; col < 8; col++)
                if (bits & (1 << col))
                    ui_fill(x + col * scale, y + row * scale, scale, scale, c);
        }
        x += 8 * scale;
    }
}

int ui_text_w(int scale, const char *s) { return (int)strlen(s) * 8 * scale; }

void ui_text_center(int cx, int y, int scale, u16 c, const char *s) {
    ui_text(cx - ui_text_w(scale, s) / 2, y, scale, c, s);
}

void ui_play(int cx, int cy, int size, u16 c) {
    int half = size / 2;
    for (int dx = 0; dx < size; dx++) {
        int hy = (size - dx) / 2;                 /* shrinks toward the apex */
        int x = cx - half + dx;
        for (int y = cy - hy; y <= cy + hy; y++) ui_px(x, y, c);
    }
}

void ui_play_l(int cx, int cy, int size, u16 c) {
    int half = size / 2;
    for (int dx = 0; dx < size; dx++) {
        int hy = dx / 2;                          /* grows toward the right base */
        int x = cx - half + dx;
        for (int y = cy - hy; y <= cy + hy; y++) ui_px(x, y, c);
    }
}

void ui_pause(int cx, int cy, int size, u16 c) {
    int bw = size / 3, gap = size / 4, half = size / 2;
    ui_fill(cx - gap - bw, cy - half, bw, size, c);
    ui_fill(cx + gap,      cy - half, bw, size, c);
}
