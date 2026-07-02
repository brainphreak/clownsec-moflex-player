/* Hardware YUV420p -> RGB565 color conversion via the 3DS Y2R block, as a drop-in
 * for the software blit. Offloads the per-pixel color conversion from the CPU. */
#ifndef Y2R_VIDEO_H
#define Y2R_VIDEO_H

#include <3ds.h>
#include "mobicompat.h"   /* AVFrame */

/* Set up Y2R for w x h YUV420p input. Returns true if the hardware is available. */
bool y2r_video_init(int w, int h);
void y2r_video_exit(void);

/* Convert an AVFrame and write it to the given top-screen eye. Returns false if
 * Y2R is not ready (caller should fall back to the software blit). */
bool y2r_video_blit(AVFrame *f, gfx3dSide_t side, int w, int h);

#endif
