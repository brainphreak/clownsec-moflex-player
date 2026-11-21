/* Stereo branding image for the top screen (RGB565, rotated framebuffer layout), as two layers
 * so the right eye can be rebuilt at any depth (ui/branding.c).
 * Generated from clownsec-banner-3d-parallel.png by tools/gen_logo.py. */
#ifndef CLOWNSEC_LOGO_H_INCLUDED
#define CLOWNSEC_LOGO_H_INCLUDED

#define CLOWNSEC_LOGO_W     400
#define CLOWNSEC_LOGO_H     240
#define CLOWNSEC_LOGO_BYTES (400 * 240 * 2)   /* one eye, RGB565 */
#define CLOWNSEC_LOGO_RUNS  396
/* full depth: right-eye x = x + BG_A * x + BG_B (background), x + FG_D (the logo) */
#define CLOWNSEC_LOGO_BG_A  0.018244f
#define CLOWNSEC_LOGO_BG_B  5.9533f
#define CLOWNSEC_LOGO_FG_D  (-12)

extern const unsigned char clownsec_logo_left[CLOWNSEC_LOGO_BYTES];
extern const unsigned char clownsec_logo_plate[CLOWNSEC_LOGO_BYTES];
extern const unsigned short clownsec_logo_runs[CLOWNSEC_LOGO_RUNS][3];   /* column, first index, length */

#endif
