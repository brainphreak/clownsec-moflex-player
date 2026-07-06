#include <3ds.h>
#include <string.h>
#include "branding.h"
#include "clownsec_logo.h"

/* The logo is pre-baked in the top screen's RGB565 rotated framebuffer layout,
 * so each eye is a straight memcpy. Double-buffering is disabled so the single
 * image persists across the menu loop's buffer swaps without needing a redraw. */
void branding_show(void) {
    gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
    gfxSetDoubleBuffering(GFX_TOP, false);
    gfxSet3D(true);
    u16 *l = (u16 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT,  NULL, NULL);
    u16 *r = (u16 *)gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, NULL, NULL);
    memcpy(l, clownsec_logo_left,  CLOWNSEC_LOGO_BYTES);
    memcpy(r, clownsec_logo_right, CLOWNSEC_LOGO_BYTES);
    gfxFlushBuffers();
    gfxSwapBuffers();
}

/* Flat 2D logo: the browser keeps the top screen in 2D while showing info panels, so the
 * "no metadata" fallback shows the same logo image without switching stereo mode. */
void branding_show_2d(void) {
    gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
    gfxSetDoubleBuffering(GFX_TOP, false);
    gfxSet3D(false);
    u16 *fb = (u16 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    memcpy(fb, clownsec_logo_left, CLOWNSEC_LOGO_BYTES);
    gfxFlushBuffers();
    gfxSwapBuffers();
}
