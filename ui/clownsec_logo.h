/* Stereo branding image for the top screen (RGB565, rotated framebuffer layout).
 * Generated from clownsec-banner-3d-parallel.png by tools/gen_logo.py. */
#ifndef CLOWNSEC_LOGO_H
#define CLOWNSEC_LOGO_H

#define CLOWNSEC_LOGO_W     400
#define CLOWNSEC_LOGO_H     240
#define CLOWNSEC_LOGO_BYTES (400 * 240 * 2)   /* one eye, RGB565 */

extern const unsigned char clownsec_logo_left[CLOWNSEC_LOGO_BYTES];
extern const unsigned char clownsec_logo_right[CLOWNSEC_LOGO_BYTES];

#endif
