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

/* Decode one MP4 video sample (AVCC: length-prefixed NAL units) into the next rotating frame
 * buffer. Returns SLOT+1 if a frame was produced (pass slot to mp4_mvd_present), 0 otherwise.
 * Frames survive in their slot until ~4 further decodes -- enough for B-frame reordering. */
int  mp4_mvd_decode(const uint8_t *sample, int size);

/* Re-prime the decoder after a seek so it resyncs on the next keyframe. Decode the keyframe and
 * any following frames up to the seek target (discarding output) before presenting again. */
void mp4_mvd_reset(void);

/* Blit the frame in `slot` to the top screen. If sbs is non-zero the frame is treated as
 * side-by-side stereo: left half -> GFX_LEFT, right half -> GFX_RIGHT (use with gfxSet3D(true)).
 * Otherwise the whole frame goes to GFX_LEFT. Each eye is aspect-fit (nearest-scaled + centered,
 * letterboxed) into the 400x240 top screen. */
void mp4_mvd_present(int slot, int sbs);

#endif
