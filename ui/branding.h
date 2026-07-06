/* Top-screen 3D branding shown while idle (no video playing). */
#ifndef BRANDING_H
#define BRANDING_H

/* Configure the top screen for RGB565 3D and draw the stereo logo. Idempotent;
 * call at startup and after returning from playback (which repurposes the top). */
void branding_show(void);

/* Draw the logo flat (2D) on the top screen -- used by the file browser's info panel when a
 * highlighted movie has no metadata, so the top can stay 2D without a 3D toggle. */
void branding_show_2d(void);

#endif
