/* New-3DS hardware H.264 decoder (MVD service) wrapper for the MP4 path.
 * Feeds MP4/AVCC video samples to MVDSTD and renders decoded frames straight to a top-screen
 * framebuffer (MVD handles the rotation, so no Y2R/transpose). New-3DS ONLY. */
#ifndef MP4_MVD_H
#define MP4_MVD_H

#include <3ds.h>
#include <stdint.h>

/* Initialize MVD for a w x h H.264 stream and prime it with the SPS/PPS from the MP4 avcC box.
 * nal_len_size is the AVCC length-prefix width (usually 4). Returns 1 on success, -1 when the
 * linear heap cannot hold the decoder + frame buffers, 0 when the hardware refused the stream. */
int  mp4_mvd_init(int w, int h, const uint8_t *avcc, int avcc_len, int nal_len_size);
void mp4_mvd_exit(void);
/* Before init: the largest video sample in the file (sizes the packet buffer to fit, not 2 MB). */
void mp4_mvd_hint_max_sample(u32 bytes);
/* After init: decoded-frame slots allocated. The caller can hold at most slots-1 pictures queued
 * while the next one decodes. */
int  mp4_mvd_slots(void);

/* One decode-pump step (reference-player state machine). If pictures are pending inside the
 * decoder, drains one (sample ignored); otherwise feeds `sample` (*consumed set to 1).
 * Returns SLOT+1 when a picture was obtained (pass slot to mp4_mvd_present), 0 otherwise. */
int  mp4_mvd_pump(const uint8_t *sample, int size, int *consumed);

/* Nonzero while the decoder holds pictures that must be drained before feeding more. */
int  mp4_mvd_pending(void);

/* Re-prime the decoder after a seek so it resyncs on the next keyframe. Decode the keyframe and
 * any following frames up to the seek target (discarding output) before presenting again. */
void mp4_mvd_reset(void);

/* Blit the frame in `slot` to the top screen. If sbs is non-zero the frame is treated as
 * side-by-side stereo: left half -> GFX_LEFT, right half -> GFX_RIGHT (use with gfxSet3D(true)).
 * Otherwise the whole frame goes to GFX_LEFT. Each eye is aspect-fit (nearest-scaled + centered,
 * letterboxed) into the 400x240 top screen. */
void mp4_mvd_present(int slot, int sbs);

/* The decoded frame in `slot` itself: BGR565 (GPU_RGB565 bit layout), w x h DISPLAY pixels, rows
 * `stride` pixels apart (the coded width). For callers that present it themselves. */
const u16 *mp4_mvd_frame(int slot, int *w, int *h, int *stride);

#endif
