/* Top-screen 3D branding shown while idle (no video playing). */
#ifndef BRANDING_H
#define BRANDING_H

/* Configure the top screen for RGB565 3D and draw the stereo logo. Idempotent;
 * call at startup and after returning from playback (which repurposes the top). */
void branding_show(void);

#endif
