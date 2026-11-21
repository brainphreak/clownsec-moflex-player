#include "ui_gfx.h"
#include "font8x8_basic.h"
#include "font8x8_ext.h"   /* Latin-1 + Greek + Turkish glyphs (foreign chars in UI text, e.g. "Pokemon") */
#include "font512.h"       /* fallback for Cyrillic/Hebrew/punctuation */
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <sys/stat.h>

/* decode one UTF-8 codepoint and advance; 8x8 glyph for any codepoint (falls back to '?') */
static u32 ui_u8_next(const char **ps) {
    const unsigned char *s = (const unsigned char *)*ps; u32 cp; int n;
    if (s[0] < 0x80) { cp = s[0]; n = 1; }
    else if ((s[0] & 0xE0) == 0xC0 && s[1]) { cp = ((u32)(s[0] & 0x1F) << 6) | (s[1] & 0x3F); n = 2; }
    else if ((s[0] & 0xF0) == 0xE0 && s[1] && s[2]) { cp = ((u32)(s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); n = 3; }
    else if ((s[0] & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) { cp = ((u32)(s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F); n = 4; }
    else { cp = '?'; n = 1; }
    *ps += n; return cp;
}
static const char *ui_glyph(u32 cp) {
    if (cp < 0x80)                   return font8x8_basic[cp];
    if (cp >= 0xA0 && cp <= 0xFF)    return font8x8_ext_latin[cp - 0xA0];
    if (cp >= 0x390 && cp <= 0x3C9)  return font8x8_greek[cp - 0x390];
    switch (cp) {                                                        /* Turkish Ext-A */
        case 0x011E: return font8x8_turk[0]; case 0x011F: return font8x8_turk[1];
        case 0x0130: return font8x8_turk[2]; case 0x0131: return font8x8_turk[3];
        case 0x015E: return font8x8_turk[4]; case 0x015F: return font8x8_turk[5];
    }
    int lo = 0, hi = FONT512_MAP_N - 1;                                 /* Cyrillic/quotes/dashes/etc */
    while (lo <= hi) { int m = (lo + hi) >> 1; unsigned c = font512_map[m].cp;
        if (c == cp) return (const char *)font512[font512_map[m].idx];
        if (c < cp) lo = m + 1; else hi = m - 1; }
    return font8x8_basic['?'];
}
int ui_u8_len(const char *s) { int n = 0; while (*s) { ui_u8_next(&s); n++; } return n; }   /* codepoints */
int ui_u8_bytes(const char *s, int ncp) {   /* byte length of the first ncp codepoints (safe truncation) */
    const char *p = s; int n = 0;
    while (*p && n < ncp) { ui_u8_next(&p); n++; }
    return (int)(p - s);
}

/* ======================= runtime theme ======================= */
UiTheme g_theme;   /* the live palette (all UI_* / TH_* macros read this) */

/* each preset gives the accents + base bg/text; the structural shades are derived. */
typedef struct { const char *name; u16 accent, accent2, accent3, red, hi, ink, dim, bg; } ThemePreset;
static const ThemePreset g_presets[] = {
    /* name        accent(1)          accent2            accent3            red               hi(bright text)     ink(text)          dim(muted)         bg(base)         */
    {"Clownsec", UI_RGB( 60,255,150), UI_RGB(180, 90,255), UI_RGB( 60,210,255), UI_RGB(255, 80, 60), UI_RGB(255,255,255), UI_RGB(205,214,235), UI_RGB( 90,100,130), UI_RGB(  9, 10, 18)},
    {"Matrix",   UI_RGB(  0,255, 65), UI_RGB(  0,200, 60), UI_RGB(120,255,140), UI_RGB(255, 70, 60), UI_RGB(190,255,190), UI_RGB(  0,225, 70), UI_RGB(  0,110, 40), UI_RGB(  0,  9,  2)},
    {"Nintendo", UI_RGB( 50,150,255), UI_RGB(195,200,210), UI_RGB(120,180,255), UI_RGB(228, 25, 35), UI_RGB(245,247,252), UI_RGB(220,224,232), UI_RGB(120,126,140), UI_RGB( 34, 37, 45)},
    {"Light",    UI_RGB( 60, 60, 58), UI_RGB(100, 98, 94), UI_RGB( 78, 78, 74), UI_RGB(176, 58, 50), UI_RGB( 14, 14, 16), UI_RGB( 34, 35, 38), UI_RGB( 92, 92, 88), UI_RGB(176,173,162)},
    {"Amber",    UI_RGB(255,176,  0), UI_RGB(255,120,  0), UI_RGB(255,210,120), UI_RGB(255, 70, 50), UI_RGB(255,232,175), UI_RGB(255,190, 90), UI_RGB(150,100, 20), UI_RGB( 16, 11,  3)},
    {"Ice",      UI_RGB(120,220,255), UI_RGB(160,180,255), UI_RGB(200,240,255), UI_RGB(255, 90,110), UI_RGB(240,250,255), UI_RGB(200,225,245), UI_RGB( 90,120,160), UI_RGB(  6, 12, 26)},
};
#define THEME_N ((int)(sizeof(g_presets)/sizeof(g_presets[0])))
#define CUSTOM_I THEME_N            /* the editable "Custom" theme sits after the presets */
static int g_theme_i = 0;

/* ---- editable Custom palette: 8 elements x RGB (0..255), the source of truth ---- */
#define ELEM_N 8
static const char *g_elem_name[ELEM_N] =
    { "Accent 1", "Accent 2", "Accent 3", "Danger", "Bright text", "Text", "Muted", "Background" };
static const u8 k_cust_default[ELEM_N][3] = {   /* defaults to the Clownsec palette */
    { 60,255,150}, {180, 90,255}, { 60,210,255}, {255, 80, 60},
    {255,255,255}, {205,214,235}, { 90,100,130}, {  9, 10, 18},
};
static u8  g_cust[ELEM_N][3];
static int g_cust_init = 0;
static void custom_reset(void) { memcpy(g_cust, k_cust_default, sizeof g_cust); g_cust_init = 1; }
static u16 cust565(int e) { return UI_RGB(g_cust[e][0], g_cust[e][1], g_cust[e][2]); }

/* blend a->b in RGB565, t=0..255 (0=a, 255=b) */
static u16 blend565(u16 a, u16 b, int t) {
    int ar=(a>>11)&0x1F, ag=(a>>5)&0x3F, ab=a&0x1F;
    int br=(b>>11)&0x1F, bg=(b>>5)&0x3F, bb=b&0x1F;
    int r=ar+((br-ar)*t)/255, g=ag+((bg-ag)*t)/255, l=ab+((bb-ab)*t)/255;
    return (u16)((r<<11)|(g<<5)|l);
}

/* set the live palette from 8 base colours + derive the structural shades */
static void theme_apply(u16 accent, u16 accent2, u16 accent3, u16 red,
                        u16 hi, u16 ink, u16 dim, u16 bg) {
    g_theme.accent=accent; g_theme.accent2=accent2; g_theme.accent3=accent3;
    g_theme.red=red; g_theme.hi=hi; g_theme.ink=ink; g_theme.dim=dim; g_theme.bg=bg;
    /* panels nudge toward the text colour, selection tints toward the accent -- so a
       light OR dark base bg both stay coherent. */
    g_theme.bg1   = blend565(bg, ink,    18);
    g_theme.bg2   = blend565(bg, ink,    40);
    g_theme.sel   = blend565(bg, accent, 55);
    g_theme.sello = blend565(bg, accent, 20);
    g_theme.line  = blend565(bg, ink,    70);
    g_theme.track = blend565(bg, ink,    45);
}

int  theme_count(void)       { return THEME_N + 1; }   /* presets + Custom */
const char *theme_name(int i) { return (i == CUSTOM_I) ? "Custom" : (i>=0 && i<THEME_N) ? g_presets[i].name : ""; }
int  theme_current(void)     { return g_theme_i; }

void theme_set(int i) {
    if (i < 0) i = 0; if (i > CUSTOM_I) i = CUSTOM_I;
    g_theme_i = i;
    if (i == CUSTOM_I) {
        if (!g_cust_init) custom_reset();
        theme_apply(cust565(0), cust565(1), cust565(2), cust565(3),
                    cust565(4), cust565(5), cust565(6), cust565(7));
    } else {
        const ThemePreset *p = &g_presets[i];
        theme_apply(p->accent, p->accent2, p->accent3, p->red, p->hi, p->ink, p->dim, p->bg);
    }
}

void theme_preview(int i, u16 *accent, u16 *accent2, u16 *accent3, u16 *bg) {
    if (i == CUSTOM_I) {
        if (!g_cust_init) custom_reset();
        if (accent) *accent=cust565(0); if (accent2) *accent2=cust565(1);
        if (accent3) *accent3=cust565(2); if (bg) *bg=cust565(7);
        return;
    }
    if (i < 0 || i >= THEME_N) i = 0;
    const ThemePreset *p = &g_presets[i];
    if (accent)  *accent  = p->accent;
    if (accent2) *accent2 = p->accent2;
    if (accent3) *accent3 = p->accent3;
    if (bg)      *bg      = p->bg;
}

/* ---- Custom-editor API ---- */
int theme_custom_index(void)      { return CUSTOM_I; }
int theme_elem_count(void)        { return ELEM_N; }
const char *theme_elem_name(int e){ return (e>=0 && e<ELEM_N) ? g_elem_name[e] : ""; }
int theme_custom_get(int e, int c){
    if (!g_cust_init) custom_reset();
    return (e>=0 && e<ELEM_N && c>=0 && c<3) ? g_cust[e][c] : 0;
}
void theme_custom_set(int e, int c, int v) {
    if (e<0 || e>=ELEM_N || c<0 || c>=3) return;
    if (v<0) v=0; if (v>255) v=255;
    if (!g_cust_init) custom_reset();
    g_cust[e][c] = (u8)v;
    if (g_theme_i == CUSTOM_I) theme_set(CUSTOM_I);   /* live re-apply */
}
void theme_custom_reset(void) {
    custom_reset();
    if (g_theme_i == CUSTOM_I) theme_set(CUSTOM_I);
}

#define THEME_CFG "sdmc:/moflex_player/theme.cfg"
#define CUST_CFG  "sdmc:/moflex_player/custom.cfg"
static void custom_load(void) {
    custom_reset();
    FILE *f = fopen(CUST_CFG, "rb");
    if (!f) return;
    for (int e=0; e<ELEM_N; e++) for (int c=0; c<3; c++) {
        int v; if (fscanf(f, "%d", &v) == 1) { if (v<0) v=0; if (v>255) v=255; g_cust[e][c]=(u8)v; }
    }
    fclose(f);
}
void theme_load(void) {
    custom_load();
    int i = 0;
    FILE *f = fopen(THEME_CFG, "rb");
    if (f) { if (fscanf(f, "%d", &i) != 1) i = 0; fclose(f); }
    theme_set(i);
}
void theme_save(void) {
    mkdir("sdmc:/moflex_player", 0777);   /* may not exist yet on a fresh card */
    FILE *f = fopen(THEME_CFG, "wb");
    if (f) { fprintf(f, "%d\n", g_theme_i); fclose(f); }
    FILE *g = fopen(CUST_CFG, "wb");
    if (g) { for (int e=0; e<ELEM_N; e++) fprintf(g, "%d %d %d\n", g_cust[e][0], g_cust[e][1], g_cust[e][2]); fclose(g); }
}
/* ============================================================= */

/* draw into an offscreen buffer (same rotated layout as the fb, height 240), then
 * copy in one shot in ui_present() -- avoids clear->draw flicker on the live fb.
 * Sized for the wider top screen (400x240); bottom uses the first 320 columns. */
static u16 g_buf[UI_TW * UI_H];
static int g_cy0 = 0, g_cy1 = UI_H;   /* vertical clip (ui_clip_y) */
#define CLIPPED_Y(yy) ((yy) < g_cy0 || (yy) >= g_cy1)
static u16 *g_fb = g_buf;
static int g_w = UI_W;
static gfxScreen_t g_screen = GFX_BOTTOM;

void ui_begin(gfxScreen_t screen) {
    g_screen = screen;
    g_w = (screen == GFX_TOP) ? UI_TW : UI_W;
    g_fb = g_buf;
}
int ui_width(void) { return g_w; }

static int g_capture = 0;
void ui_capture(int on) { g_capture = on; }
const u16 *ui_pixels(void) { return g_buf; }
/* columns are contiguous: (x, y) lives at [x*UI_H + (UI_H-1-y)], so rows y..y+h-1 of a column are
 * one run starting at UI_H-1-(y+h-1) */
void ui_rect_save(int x, int y, int w, int h, u16 *out) {
    for (int c = 0; c < w; c++) memcpy(out + c * h, g_buf + (x + c) * UI_H + (UI_H - y - h), (size_t)h * 2);
}
void ui_rect_load(int x, int y, int w, int h, const u16 *in) {
    for (int c = 0; c < w; c++) memcpy(g_buf + (x + c) * UI_H + (UI_H - y - h), in + c * h, (size_t)h * 2);
}

void ui_present(void) {
    if (g_capture) return;   /* caller lifts g_buf into a citro2d texture instead (no fb copy) */
    u16 *fb = (u16 *)gfxGetFramebuffer(g_screen, GFX_LEFT, NULL, NULL);
    memcpy(fb, g_buf, (size_t)g_w * UI_H * sizeof(u16));
    gfxFlushBuffers();
}

void ui_clip_y(int y0, int y1) { g_cy0 = y0 < 0 ? 0 : y0; g_cy1 = y1 > UI_H ? UI_H : y1; }
void ui_clip_reset(void) { g_cy0 = 0; g_cy1 = UI_H; }
void ui_px(int x, int y, u16 c) {
    if ((unsigned)x < (unsigned)g_w && (unsigned)y < UI_H && !CLIPPED_Y(y))
        g_fb[x * UI_H + (UI_H - 1 - y)] = c;      /* rotated framebuffer */
}

static void fill_rect_fast(int x, int y, int w, int h, u16 c);
void ui_clear(u16 c) {
    if (g_cy0 > 0 || g_cy1 < UI_H) { fill_rect_fast(0, 0, g_w, UI_H, c); return; }
    int n = g_w * UI_H;
    for (int i = 0; i < n; i++) g_fb[i] = c;
}

/* Solid rectangle straight into the rotated buffer, one contiguous run per column (the buffer is
 * column-major). Same pixels as ui_px per pixel -- clipped the same way -- at a fraction of the cost:
 * the per-pixel call, bounds test and row-wise walk against the layout made every software screen
 * (panel, in-film menus) cost tens of ms on Old 3DS. */
static void fill_rect_fast(int x, int y, int w, int h, u16 c) {
    int x0 = x < 0 ? 0 : x, x1 = x + w > g_w ? g_w : x + w;
    int y0 = y < g_cy0 ? g_cy0 : y, y1 = y + h > g_cy1 ? g_cy1 : y + h;
    if (x0 >= x1 || y0 >= y1) return;
    int n = y1 - y0;
    for (int i = x0; i < x1; i++) {
        u16 *p = g_fb + i * UI_H + (UI_H - y1);   /* rows y1-1 .. y0 are contiguous here */
        for (int k = 0; k < n; k++) p[k] = c;
    }
}
void ui_fill(int x, int y, int w, int h, u16 c) { fill_rect_fast(x, y, w, h, c); }

void ui_text(int x, int y, int scale, u16 c, const char *s) {
    while (*s) {
        const char *g = ui_glyph(ui_u8_next(&s));
        for (int row = 0; row < 8; row++) {
            if (y + row * scale + scale <= g_cy0 || y + row * scale >= g_cy1) continue;
            char bits = g[row];
            for (int col = 0; col < 8; col++)
                if (bits & (1 << col))
                    ui_fill(x + col * scale, y + row * scale, scale, scale, c);
        }
        x += 8 * scale;
    }
}

int ui_text_w(int scale, const char *s) { return ui_u8_len(s) * 8 * scale; }   /* by glyph, not byte */

void ui_text_center(int cx, int y, int scale, u16 c, const char *s) {
    ui_text(cx - ui_text_w(scale, s) / 2, y, scale, c, s);
}

void ui_text_clipped(int x, int y, int scale, u16 c, const char *s, int clipL, int clipR) {
    while (*s) {
        if (x >= clipR) break;
        const char *g = ui_glyph(ui_u8_next(&s));
        if (x + 8 * scale > clipL) {                 /* glyph at least partly inside the clip */
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
    if ((unsigned)x >= (unsigned)g_w || (unsigned)y >= UI_H || CLIPPED_Y(y)) return;
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

/* Rounded shapes, exact but fast. Along the straight middle stretch [r, w-r) a pixel's coverage depends
 * only on its row (the nearest centre is straight above/below), so it is computed once per row and the
 * run written in one loop; only the curved ends are per pixel. Same coverage, same writes, same order
 * of blends per pixel as the per-pixel originals -- verified identical on the host. */
static void row_put(int x0, int x1, int y, u16 c) {
    if ((unsigned)y >= UI_H || CLIPPED_Y(y)) return;
    if (x0 < 0) x0 = 0; if (x1 > g_w) x1 = g_w;
    u16 *p = g_fb + (UI_H - 1 - y);
    for (int i = x0; i < x1; i++) p[i * UI_H] = c;
}
static void row_blend(int x0, int x1, int y, u16 c, int a) {
    for (int i = x0; i < x1; i++) ui_blend_px(i, y, c, a);
}
static inline void cov_px(int x, int y, u16 c, int cov) {
    if (cov >= 255) ui_px(x, y, c); else if (cov > 0) ui_blend_px(x, y, c, cov);
}
void ui_fill_round(int x, int y, int w, int h, int r, u16 c) {
    if (r <= 0) { fill_rect_fast(x, y, w, h, c); return; }
    int m0 = r, m1 = w - r, e0 = r < w ? r : w, e1 = m1 > e0 ? m1 : e0;
    for (int j = 0; j < h; j++) {
        if (CLIPPED_Y(y + j)) continue;
        for (int i = 0; i < e0; i++) cov_px(x + i, y + j, c, round_cov(i, j, w, h, r));
        for (int i = e1; i < w; i++) cov_px(x + i, y + j, c, round_cov(i, j, w, h, r));
        if (m1 > m0) {
            int cov = round_cov(m0, j, w, h, r);
            if (cov >= 255) row_put(x + m0, x + m1, y + j, c);
            else if (cov > 0) row_blend(x + m0, x + m1, y + j, c, cov);
        }
    }
}

void ui_vgrad_round(int x, int y, int w, int h, int r, u16 top, u16 bot) {
    int rr = r > 0 ? r : 0;
    int m0 = rr, m1 = w - rr, e0 = rr < w ? rr : w, e1 = m1 > e0 ? m1 : e0;
    for (int j = 0; j < h; j++) {
        if (CLIPPED_Y(y + j)) continue;
        u16 c = lerp565(top, bot, j, h - 1);
        for (int i = 0; i < e0; i++) cov_px(x + i, y + j, c, round_cov(i, j, w, h, r));
        for (int i = e1; i < w; i++) cov_px(x + i, y + j, c, round_cov(i, j, w, h, r));
        if (m1 > m0) {
            int cov = round_cov(m0, j, w, h, r);
            if (cov >= 255) row_put(x + m0, x + m1, y + j, c);
            else if (cov > 0) row_blend(x + m0, x + m1, y + j, c, cov);
        }
    }
}

static inline int frame_a(int i, int j, int w, int h, int r, int thick, int iw, int ih, int ir) {
    int oc = round_cov(i, j, w, h, r);            /* coverage of the outer edge */
    if (oc <= 0) return 0;
    int ii = i - thick, jj = j - thick;           /* coverage of the inner edge */
    int ic = (ii >= 0 && jj >= 0 && ii < iw && jj < ih) ? round_cov(ii, jj, iw, ih, ir) : 0;
    return oc - ic;                               /* the ring = outside inner, inside outer (both AA) */
}
void ui_frame_round(int x, int y, int w, int h, int r, u16 c, int thick) {
    if (thick < 1) thick = 1;
    int iw = w - 2 * thick, ih = h - 2 * thick, ir = r - thick; if (ir < 0) ir = 0;
    /* columns where BOTH edges are column-independent: outer middle and inner middle overlap */
    int o0 = r > 0 ? r : 0, o1 = r > 0 ? w - r : w;
    int n0 = thick + (ir > 0 ? ir : 0), n1 = thick + iw - (ir > 0 ? ir : 0);
    int c0 = o0 > n0 ? o0 : n0, c1 = o1 < n1 ? o1 : n1;
    if (c1 < c0) c1 = c0;
    for (int j = 0; j < h; j++) {
        if (CLIPPED_Y(y + j)) continue;
        for (int i = 0; i < w; i++) {
            if (i == c0 && c1 > c0) {
                int a = frame_a(c0, j, w, h, r, thick, iw, ih, ir);
                if (a >= 255) row_put(x + c0, x + c1, y + j, c);
                else if (a > 0) row_blend(x + c0, x + c1, y + j, c, a);
                i = c1 - 1; continue;
            }
            int a = frame_a(i, j, w, h, r, thick, iw, ih, ir);
            if (a <= 0) continue;
            if (a >= 255) ui_px(x + i, y + j, c);
            else ui_blend_px(x + i, y + j, c, a);
        }
    }
}

/* skip rect (absolute, x0..x1 x y0..y1, may be empty): pixels the caller overwrites solidly afterwards */
static void glow_round_skip(int x, int y, int w, int h, int r, u16 c, int spread, int alpha,
                            int sx0, int sy0, int sx1, int sy1) {
    for (int g = spread; g >= 1; g--) {          /* stacked expanded rings build a soft halo */
        int gx = x - g, gy = y - g, gw = w + 2 * g, gh = h + 2 * g, gr = r + g;
        int m0 = gr > 0 ? gr : 0, m1 = gr > 0 ? gw - gr : gw;
        for (int j = 0; j < gh; j++) {
            int ay = gy + j, skiprow = (ay >= sy0 && ay < sy1 && sx1 > sx0);
            if (CLIPPED_Y(ay)) continue;
            for (int i = 0; i < gw; i++) {
                int ax = gx + i;
                if (i == m0 && m1 > m0) {               /* middle run: inside or not, decided by the row */
                    if (in_round(m0, j, gw, gh, gr))
                        for (int k = gx + m0; k < gx + m1; k++) {
                            if (skiprow && k >= sx0 && k < sx1) { k = sx1 - 1; continue; }
                            ui_blend_px(k, ay, c, alpha);
                        }
                    i = m1 - 1; continue;
                }
                if (skiprow && ax >= sx0 && ax < sx1) continue;
                if (in_round(i, j, gw, gh, gr)) ui_blend_px(ax, ay, c, alpha);
            }
        }
    }
}
void ui_glow_round(int x, int y, int w, int h, int r, u16 c, int spread, int alpha) {
    glow_round_skip(x, y, w, h, r, c, spread, alpha, 0, 0, 0, 0);
}

void ui_button(int x, int y, int w, int h, const char *label, int selected, u16 accent) {
    int r = h / 2 < 12 ? h / 2 : 12;
    /* the glow under the button's solid interior is painted over by the fill below: don't blend it */
    if (selected) {   /* rows whose middle run the gradient covers fully (it is written solid there) */
        int sy0 = -1, sy1 = -1;
        if (w > 2 * r)
            for (int j = 0; j < h; j++) if (round_cov(r, j, w, h, r) >= 255) { if (sy0 < 0) sy0 = j; sy1 = j + 1; }
        if (sy0 >= 0) glow_round_skip(x, y, w, h, r, accent, 5, 26, x + r, y + sy0, x + w - r, y + sy1);
        else          glow_round_skip(x, y, w, h, r, accent, 5, 26, 0, 0, 0, 0);
    }
    u16 top = selected ? TH_SEL : UI_BG2;
    u16 bot = selected ? TH_SELLO : UI_BG;
    ui_vgrad_round(x, y, w, h, r, top, bot);
    ui_frame_round(x, y, w, h, r, selected ? accent : UI_DIM, selected ? 2 : 1);
    u16 tc = selected ? accent : UI_INK;
    const char *lbl = label; char buf[64];   /* a too-long label must not spill the button */
    int maxw = w - 8;
    if (ui_text_w(1, label) > maxw) {
        /* make room before cutting words: first squeeze the wide gaps ("Zoom:  100%   (hint)"),
         * then drop a trailing "(hint)", and only then truncate with ".." */
        int o = 0;
        for (const char *p = label; *p && o < (int)sizeof buf - 1; p++)
            if (!(*p == ' ' && o > 0 && buf[o - 1] == ' ')) buf[o++] = *p;
        buf[o] = 0; lbl = buf;
        if (ui_text_w(1, buf) > maxw && o > 0 && buf[o - 1] == ')') {
            char *lp = strrchr(buf, '(');
            if (lp && lp > buf + 1 && lp[-1] == ' ') lp[-1] = 0;
        }
        if (ui_text_w(1, buf) > maxw) {
            char t[64]; snprintf(t, sizeof t, "%s", buf);
            int maxch = maxw / 8 - 2; if (maxch < 1) maxch = 1;
            snprintf(buf, sizeof buf, "%.*s..", maxch, t);
        }
    }
    ui_text_center(x + w / 2, y + (h - 8) / 2, 1, tc, lbl);
}

/* ---- hold-to-repeat (see ui_gfx.h): the lists' nav_repeat timing in milliseconds -- first repeat
 * after 267 ms, then every 83 ms, every 33 ms after another half second, every frame after 1.25 s ---- */
int ui_rep(UiRep *r, u32 kd, u32 kh, u32 mask) {
    if (!(kh & mask)) { r->t0 = 0; return 0; }
    u64 now = osGetTime();
    if (kd & mask) { r->t0 = now; r->next = now + 267; return 2; }
    if (!r->t0) { r->t0 = now; r->next = now + 267; return 0; }   /* held from before: no press seen */
    if (now < r->next) return 0;
    u64 held = now - r->t0;
    r->next = now + (held > 1517 ? 17 : held > 767 ? 33 : 83);
    return 1;
}
u32 ui_rep_held(const UiRep *r) { return r->t0 ? (u32)(osGetTime() - r->t0) : 0; }
int ui_list_step(int sel, int n, int dir, int how) {
    if (!how || n <= 0) return sel;
    int t = sel + dir;
    if (how == 2) return (t % n + n) % n;
    return t < 0 ? 0 : t >= n ? n - 1 : t;
}
int ui_list_nav(UiRep *up, UiRep *down, u32 kd, u32 kh, int sel, int n) {
    sel = ui_list_step(sel, n, 1, ui_rep(down, kd, kh, KEY_DOWN));
    return ui_list_step(sel, n, -1, ui_rep(up, kd, kh, KEY_UP));
}
