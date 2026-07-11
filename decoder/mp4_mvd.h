/* New-3DS hardware H.264 decoder (MVD service) wrapper for the MP4 path.
 * Feeds MP4/AVCC video samples to MVDSTD and renders decoded frames straight to a top-screen
 * framebuffer (MVD handles the rotation, so no Y2R/transpose). New-3DS ONLY. */
#ifndef MP4_MVD_H
#define MP4_MVD_H

#include <3ds.h>
#include <stdint.h>

/* Initialize MVD for a w x h H.264 stream and prime it with the SPS/PPS from the MP4 avcC box.
 * nal_len_size is the AVCC length-prefix width (usually 4). Returns 1 on success. */
int  mp4_mvd_init(int w, int h, const uint8_t *avcc, int avcc_len, int nal_len_size);
void mp4_mvd_exit(void);

/* Decode one MP4 video sample (AVCC: length-prefixed NAL units) and render the resulting frame
 * to the given top-screen eye. Returns 1 if a frame was rendered, 0 otherwise. */
int  mp4_mvd_decode(const uint8_t *sample, int size, gfx3dSide_t side);

#endif
