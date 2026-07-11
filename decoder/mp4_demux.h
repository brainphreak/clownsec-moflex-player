/* Minimal MP4 (ISO-BMFF) demuxer: extracts a single H.264 video track and a single AAC audio
 * track, with per-sample offset/size/timestamp/keyframe tables. Pure C (stdio), no 3DS deps, so
 * it builds + tests on a PC too. Feeds the New-3DS MVD hardware H.264 decoder on the mp4 path;
 * the moflex path is untouched. */
#ifndef MP4_DEMUX_H
#define MP4_DEMUX_H

#include <stdio.h>
#include <stdint.h>

typedef struct {
    int64_t offset;    /* absolute file offset of the sample */
    uint32_t size;     /* sample size in bytes */
    int64_t dts;       /* decode timestamp, in the track's timescale units */
    int keyframe;      /* 1 = sync sample (IDR); seek targets */
} Mp4Sample;

typedef struct {
    FILE *f;

    /* ---- video (H.264 / avc1) ---- */
    int      has_video;
    int      width, height;         /* coded dimensions */
    uint32_t v_timescale;           /* ticks per second for v-samples' dts */
    uint8_t  avcc[512];             /* raw avcC payload (holds SPS/PPS) */
    int      avcc_len;
    int      nal_length_size;       /* 1/2/4: length prefix of NAL units in samples */
    Mp4Sample *vsamples;
    int      v_count;

    /* ---- audio (AAC / mp4a) ---- */
    int      has_audio;
    uint32_t a_timescale;
    int      a_rate, a_channels;
    uint8_t  asc[64];               /* AudioSpecificConfig (from esds) */
    int      asc_len;
    Mp4Sample *asamples;
    int      a_count;
} Mp4;

/* Open + fully parse an MP4. Returns 1 on success (at least a video track found). */
int  mp4_open(Mp4 *m, const char *path);
void mp4_close(Mp4 *m);

/* Read a sample's bytes into buf (must be >= sample->size). Returns bytes read, 0 on error. */
int  mp4_read_sample(Mp4 *m, const Mp4Sample *s, uint8_t *buf);

/* Index of the last video keyframe at or before dts (for seeking). */
int  mp4_keyframe_before(const Mp4 *m, int64_t dts);

/* Duration of the video track in seconds (0 if unknown). */
double mp4_duration_s(const Mp4 *m);

#endif
