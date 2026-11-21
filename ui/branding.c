#include <3ds.h>
#include <string.h>
#include "branding.h"
#include "clownsec_logo.h"

/* The logo is pre-baked in the top screen's RGB565 rotated framebuffer layout (columns of 240
 * pixels, contiguous), as the left eye plus two layers -- the background alone and where the logo
 * is -- so the right eye can be rebuilt at whatever depth the 3D slider asks for (tools/gen_logo.py).
 * Double-buffering is disabled so the single image persists across the menu loop's buffer swaps
 * without needing a redraw. */
#define LW CLOWNSEC_LOGO_W
#define LH CLOWNSEC_LOGO_H
#define LVLS 32                     /* slider steps the right eye is rebuilt at */
static int g_brand_lvl = -1;        /* depth the right eye was last built at, -1 = logo not shown in 3D */

/* the right eye at depth s (0 = flat .. 1 = the banner's own): the background plate stretched along
 * its tilted plane, then the logo's runs from the left eye, shifted by its depth */
static void build_right(u8 *r, float s) {
    const u8 *plate = clownsec_logo_plate, *left = clownsec_logo_left;
    float a = 1.0f + s * CLOWNSEC_LOGO_BG_A, b = s * CLOWNSEC_LOGO_BG_B;
    for (int x = 0; x < LW; x++) {
        float fx = (x - b) / a;
        int sx = fx <= 0.0f ? 0 : (int)(fx + 0.5f);
        if (sx > LW - 1) sx = LW - 1;
        memcpy(r + x * LH * 2, plate + sx * LH * 2, LH * 2);
    }
    float fd = s * CLOWNSEC_LOGO_FG_D;
    int d = (int)(fd < 0 ? fd - 0.5f : fd + 0.5f);
    for (int i = 0; i < CLOWNSEC_LOGO_RUNS; i++) {
        int x = clownsec_logo_runs[i][0], y0 = clownsec_logo_runs[i][1], n = clownsec_logo_runs[i][2];
        int dx = x + d;
        if (dx < 0 || dx >= LW) continue;
        memcpy(r + (dx * LH + y0) * 2, left + (x * LH + y0) * 2, n * 2);
    }
}

static int slider_lvl(void) { return (int)(osGet3DSliderState() * LVLS + 0.5f); }

void branding_show(void) {
    gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
    gfxSetDoubleBuffering(GFX_TOP, false);
    gfxSet3D(true);
    u8 *l = gfxGetFramebuffer(GFX_TOP, GFX_LEFT,  NULL, NULL);
    u8 *r = gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, NULL, NULL);
    memcpy(l, clownsec_logo_left, CLOWNSEC_LOGO_BYTES);
    g_brand_lvl = slider_lvl();
    build_right(r, (float)g_brand_lvl / LVLS);
    gfxFlushBuffers();
    gfxSwapBuffers();
}

/* Follow the 3D slider: rebuild the right eye when it moves, while the logo is still what the top
 * screen shows (anything drawn over it -- an info panel, a film -- stops this until the next
 * branding_show). A slider read and a compare per call; the rebuild (~1 ms) only on a change. */
void branding_tick(void) {
    if (g_brand_lvl < 0) return;
    int lvl = slider_lvl();
    if (lvl == g_brand_lvl) return;
    if (!gfxIs3D()) { g_brand_lvl = -1; return; }
    const u8 *l = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    static const int spot[3] = { 0, CLOWNSEC_LOGO_BYTES / 2, CLOWNSEC_LOGO_BYTES - 64 };
    for (int i = 0; i < 3; i++)
        if (memcmp(l + spot[i], clownsec_logo_left + spot[i], 64)) { g_brand_lvl = -1; return; }
    build_right(gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, NULL, NULL), (float)lvl / LVLS);
    gfxFlushBuffers();
    g_brand_lvl = lvl;
}

/* Flat 2D logo: the browser keeps the top screen in 2D while showing info panels, so the
 * "no metadata" fallback shows the same logo image without switching stereo mode. */
void branding_show_2d(void) {
    g_brand_lvl = -1;
    gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
    gfxSetDoubleBuffering(GFX_TOP, false);
    gfxSet3D(false);
    u16 *fb = (u16 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    memcpy(fb, clownsec_logo_left, CLOWNSEC_LOGO_BYTES);
    gfxFlushBuffers();
    gfxSwapBuffers();
}
