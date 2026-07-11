#include "ui_gfx.h"
#include "font8x8_basic.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <sys/stat.h>

/* ======================= runtime theme ======================= */
UiTheme g_theme;   /* the live palette (all UI_* / TH_* macros read this) */

/* each preset gives the accents + base bg/text; the structural shades are derived. */
typedef struct { const char *name; u16 accent, accent2, accent3, red, hi, ink, dim, bg; } ThemePreset;
static const ThemePreset g_presets[] = {
    /* name        accent(1)          accent2            accent3            red               hi(bright text)     ink(text)          dim(muted)         bg(base)         */
    {"Clownsec", UI_RGB( 60,255,150), UI_RGB(180, 90,255), UI_RGB( 60,210,255), UI_RGB(255, 80, 60), UI_RGB(255,255,255), UI_RGB(205,214,235), UI_RGB( 90,100,130), UI_RGB(  9, 10, 18)},
    {"Matrix",   UI_RGB(  0,255, 65), UI_RGB(  0,200, 60), UI_RGB(120,255,140), UI_RGB(255, 70, 60), UI_RGB(190,255,190), UI_RGB(  0,225, 70), UI_RGB(  0,110, 40), UI_RGB(  0,  9,  2)},
    {"Game Boy", UI_RGB( 48, 98, 48), UI_RGB( 15, 56, 15), UI_RGB(107,140, 15), UI_RGB( 15, 56, 15), UI_RGB( 15, 56, 15), UI_RGB( 15, 56, 15), UI_RGB( 48, 98, 48), UI_RGB(155,188, 15)},
    {"Nintendo", UI_RGB( 50,150,255), UI_RGB(195,200,210), UI_RGB(120,180,255), UI_RGB(228, 25, 35), UI_RGB(245,247,252), UI_RGB(220,224,232), UI_RGB(120,126,140), UI_RGB( 34, 37, 45)},
    {"Light",    UI_RGB( 30,170,110), UI_RGB(140, 80,210), UI_RGB( 40,140,210), UI_RGB(210, 55, 45), UI_RGB( 15, 18, 26), UI_RGB( 38, 44, 60), UI_RGB(120,128,145), UI_RGB(232,235,242)},
    {"Amber",    UI_RGB(255,176,  0), UI_RGB(255,120,  0), UI_RGB(255,210,120), UI_RGB(255, 70, 50), UI_RGB(255,232,175), UI_RGB(255,190, 90), UI_RGB(150,100, 20), UI_RGB( 16, 11,  3)},
    {"Ice",      UI_RGB(120,220,255), UI_RGB(160,180,255), UI_RGB(200,240,255), UI_RGB(255, 90,110), UI_RGB(240,250,255), UI_RGB(200,225,245), UI_RGB( 90,120,160), UI_RGB(  6, 12, 26)},
};
#define THEME_N ((int)(sizeof(g_presets)/sizeof(g_presets[0])))
static int g_theme_i = 0;

/* blend a->b in RGB565, t=0..255 (0=a, 255=b) */
static u16 blend565(u16 a, u16 b, int t) {
    int ar=(a>>11)&0x1F, ag=(a>>5)&0x3F, ab=a&0x1F;
    int br=(b>>11)&0x1F, bg=(b>>5)&0x3F, bb=b&0x1F;
    int r=ar+((br-ar)*t)/255, g=ag+((bg-ag)*t)/255, l=ab+((bb-ab)*t)/255;
    return (u16)((r<<11)|(g<<5)|l);
}

int  theme_count(void)       { return THEME_N; }
const char *theme_name(int i) { return (i>=0 && i<THEME_N) ? g_presets[i].name : ""; }
int  theme_current(void)     { return g_theme_i; }

void theme_set(int i) {
    if (i < 0) i = 0; if (i >= THEME_N) i = THEME_N - 1;
    g_theme_i = i;
    const ThemePreset *p = &g_presets[i];
    g_theme.accent=p->accent; g_theme.accent2=p->accent2; g_theme.accent3=p->accent3;
    g_theme.red=p->red; g_theme.hi=p->hi; g_theme.ink=p->ink; g_theme.dim=p->dim; g_theme.bg=p->bg;
    /* structural shades derived from the base so any bg (dark OR light) stays coherent:
       panels nudge toward the text colour, selection tints toward the accent. */
    g_theme.bg1   = blend565(p->bg, p->ink,    18);
    g_theme.bg2   = blend565(p->bg, p->ink,    40);
    g_theme.sel   = blend565(p->bg, p->accent, 55);
    g_theme.sello = blend565(p->bg, p->accent, 20);
    g_theme.line  = blend565(p->bg, p->ink,    70);
    g_theme.track = blend565(p->bg, p->ink,    45);
}

void theme_preview(int i, u16 *accent, u16 *accent2, u16 *accent3, u16 *bg) {
    if (i < 0 || i >= THEME_N) i = 0;
    const ThemePreset *p = &g_presets[i];
    if (accent)  *accent  = p->accent;
    if (accent2) *accent2 = p->accent2;
    if (accent3) *accent3 = p->accent3;
    if (bg)      *bg      = p->bg;
}

#define THEME_CFG "sdmc:/moflex_player/theme.cfg"
void theme_load(void) {
    int i = 0;
    FILE *f = fopen(THEME_CFG, "rb");
    if (f) { if (fscanf(f, "%d", &i) != 1) i = 0; fclose(f); }
    theme_set(i);
}
void theme_save(void) {
    mkdir("sdmc:/moflex_player", 0777);   /* may not exist yet on a fresh card */
    FILE *f = fopen(THEME_CFG, "wb");
    if (f) { fprintf(f, "%d\n", g_theme_i); fclose(f); }
}
/* ============================================================= */

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

void ui_text_clipped(int x, int y, int scale, u16 c, const char *s, int clipL, int clipR) {
    for (; *s; s++) {
        if (x >= clipR) break;
        unsigned ch = (unsigned char)*s;
        if (ch > 127) ch = '?';
        if (x + 8 * scale > clipL) {                 /* glyph at least partly inside the clip */
            const char *g = font8x8_basic[ch];
            for (int row = 0; row < 8; row++) {
                char bits = g[row];
                for (int col = 0; col < 8; col++)
                    if (bits & (1 << col)) {
                        int bx = x + col * scale;
                        for (int sx = 0; sx < scale; sx++) {
                            int xx = bx + sx;
                            if (xx < clipL || xx >= clipR) continue;
                            for (int sy = 0; sy < scale; sy++) ui_px(xx, y + row * scale + sy, c);
                        }
                    }
            }
        }
        x += 8 * scale;
    }
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

/* ---- widget kit ---- */
void ui_blend_px(int x, int y, u16 c, int a) {
    if ((unsigned)x >= (unsigned)g_w || (unsigned)y >= UI_H) return;
    if (a <= 0) return;
    if (a >= 255) { ui_px(x, y, c); return; }
    u16 *p = &g_fb[x * UI_H + (UI_H - 1 - y)];
    u16 bg = *p;
    int br = (bg >> 11) & 0x1F, bgc = (bg >> 5) & 0x3F, bb = bg & 0x1F;
    int cr = (c  >> 11) & 0x1F, cgc = (c  >> 5) & 0x3F, cb = c  & 0x1F;
    int r = br + ((cr - br) * a) / 255;
    int g = bgc + ((cgc - bgc) * a) / 255;
    int b = bb + ((cb - bb) * a) / 255;
    *p = (u16)((r << 11) | (g << 5) | b);
}

/* is pixel (i,j) inside a w x h rounded rect of corner radius r? */
static int in_round(int i, int j, int w, int h, int r) {
    if (r <= 0) return 1;
    int cx = i < r ? r : (i >= w - r ? w - 1 - r : i);
    int cy = j < r ? r : (j >= h - r ? h - 1 - r : j);
    int dx = i - cx, dy = j - cy;
    return dx * dx + dy * dy <= r * r;
}

/* anti-aliased coverage (0..255) of pixel (i,j) in a rounded rect -- soft 1px edge on the curves */
static int round_cov(int i, int j, int w, int h, int r) {
    if (r <= 0) return 255;
    if (i >= r && i < w - r && j >= r && j < h - r) return 255;   /* straight interior, no sqrt */
    int cx = i < r ? r : (i >= w - r ? w - 1 - r : i);
    int cy = j < r ? r : (j >= h - r ? h - 1 - r : j);
    float dx = (float)(i - cx), dy = (float)(j - cy);
    float cov = (float)r + 0.5f - sqrtf(dx * dx + dy * dy);
    if (cov <= 0.0f) return 0;
    if (cov >= 1.0f) return 255;
    return (int)(cov * 255.0f);
}
static u16 lerp565(u16 a, u16 b, int t, int tmax) {
    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    if (tmax <= 0) tmax = 1;
    int r = ar + ((br - ar) * t) / tmax, g = ag + ((bg - ag) * t) / tmax, bl = ab + ((bb - ab) * t) / tmax;
    return (u16)((r << 11) | (g << 5) | bl);
}

void ui_fill_round(int x, int y, int w, int h, int r, u16 c) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int cov = round_cov(i, j, w, h, r);
            if (cov >= 255) ui_px(x + i, y + j, c);
            else if (cov > 0) ui_blend_px(x + i, y + j, c, cov);
        }
}

void ui_vgrad_round(int x, int y, int w, int h, int r, u16 top, u16 bot) {
    for (int j = 0; j < h; j++) {
        u16 c = lerp565(top, bot, j, h - 1);
        for (int i = 0; i < w; i++) {
            int cov = round_cov(i, j, w, h, r);
            if (cov >= 255) ui_px(x + i, y + j, c);
            else if (cov > 0) ui_blend_px(x + i, y + j, c, cov);
        }
    }
}

void ui_frame_round(int x, int y, int w, int h, int r, u16 c, int thick) {
    if (thick < 1) thick = 1;
    int iw = w - 2 * thick, ih = h - 2 * thick, ir = r - thick; if (ir < 0) ir = 0;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int oc = round_cov(i, j, w, h, r);            /* coverage of the outer edge */
            if (oc <= 0) continue;
            int ii = i - thick, jj = j - thick;           /* coverage of the inner edge */
            int ic = (ii >= 0 && jj >= 0 && ii < iw && jj < ih) ? round_cov(ii, jj, iw, ih, ir) : 0;
            int a = oc - ic;                              /* the ring = outside inner, inside outer (both AA) */
            if (a <= 0) continue;
            if (a >= 255) ui_px(x + i, y + j, c);
            else ui_blend_px(x + i, y + j, c, a);
        }
}

void ui_glow_round(int x, int y, int w, int h, int r, u16 c, int spread, int alpha) {
    for (int g = spread; g >= 1; g--) {          /* stacked expanded rings build a soft halo */
        int gx = x - g, gy = y - g, gw = w + 2 * g, gh = h + 2 * g, gr = r + g;
        for (int j = 0; j < gh; j++)
            for (int i = 0; i < gw; i++)
                if (in_round(i, j, gw, gh, gr)) ui_blend_px(gx + i, gy + j, c, alpha);
    }
}

void ui_button(int x, int y, int w, int h, const char *label, int selected, u16 accent) {
    int r = h / 2 < 12 ? h / 2 : 12;
    if (selected) ui_glow_round(x, y, w, h, r, accent, 5, 26);
    u16 top = selected ? TH_SEL : UI_BG2;
    u16 bot = selected ? TH_SELLO : UI_BG;
    ui_vgrad_round(x, y, w, h, r, top, bot);
    ui_frame_round(x, y, w, h, r, selected ? accent : UI_DIM, selected ? 2 : 1);
    u16 tc = selected ? accent : UI_INK;
    const char *lbl = label; char buf[64];   /* truncate a too-long label so it can't spill the button */
    int maxw = w - 8;
    if (ui_text_w(1, label) > maxw) {
        int maxch = maxw / 8 - 2; if (maxch < 1) maxch = 1;
        snprintf(buf, sizeof buf, "%.*s..", maxch, label); lbl = buf;
    }
    ui_text_center(x + w / 2, y + (h - 8) / 2, 1, tc, lbl);
}
