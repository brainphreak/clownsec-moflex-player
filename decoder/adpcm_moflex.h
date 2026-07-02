/* Standalone adpcm_ima_moflex decoder (from FFmpeg adpcm.c, LGPL).
 * Each packet is self-contained. Output: interleaved S16. */
#ifndef ADPCM_MOFLEX_H
#define ADPCM_MOFLEX_H
#include <stdint.h>

/* Decode one moflex audio packet into interleaved int16.
 * out must hold at least ((size - 4*channels)*2/channels)*channels samples.
 * Returns frames (samples per channel), or <0 on error. */
int adpcm_moflex_decode(const uint8_t *buf, int size, int channels, int16_t *out);

#endif
