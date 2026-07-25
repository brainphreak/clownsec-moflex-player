/* AAC-LC audio decode for the MP4 path, via the Helix fixed-point decoder (thirdparty/helixaac).
 * Each MP4 audio sample is one raw AAC access unit (no ADTS) = up to 1024 PCM samples/channel. */
#ifndef MP4_AAC_H
#define MP4_AAC_H

#include <stdint.h>

typedef struct {
    void *h;            /* HAACDecoder */
    int   rate;         /* output sample rate (Hz) */
    int   channels;     /* output channels */
} Mp4Aac;

/* Init from the MP4 AudioSpecificConfig (esds). Falls back to fb_rate/fb_ch if the ASC is absent
 * or unparseable. Returns 1 on success (AAC-LC only). */
int  mp4_aac_open(Mp4Aac *a, const uint8_t *asc, int asc_len, int fb_rate, int fb_ch);
void mp4_aac_close(Mp4Aac *a);

/* Decode one raw AAC access unit into interleaved s16 PCM. pcm must hold >= 1024*channels shorts.
 * Returns samples-per-channel produced (e.g. 1024), or 0 on error. */
int  mp4_aac_decode(Mp4Aac *a, const uint8_t *data, int size, short *pcm);

#endif
