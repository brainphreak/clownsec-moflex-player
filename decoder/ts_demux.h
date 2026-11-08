/* MPEG-TS demuxer for a live stream (Plex's universal transcoder, later Jellyfin): H.264 + AAC.
 *
 * Bytes go in as they arrive from the network (any chunk size); complete access units come out
 * with their timestamps, already in the shape the MP4 engine consumes:
 *   video: the picture's NAL units LENGTH-PREFIXED (4 bytes, AVCC) -- what mp4_mvd_pump takes;
 *          SPS/PPS are kept and offered as an avcC record for mp4_mvd_init.
 *   audio: one raw AAC frame (ADTS header stripped) per unit; ts_audio_config gives the
 *          AudioSpecificConfig for mp4_aac_open.
 * No platform code: it is verified on a PC against ffprobe (pc_verify/test_ts.c). */
#ifndef TS_DEMUX_H
#define TS_DEMUX_H

#include <stdint.h>
#include <stddef.h>

enum { TS_AU_VIDEO = 1, TS_AU_AUDIO = 2 };

typedef struct {
    int      kind;        /* TS_AU_VIDEO / TS_AU_AUDIO */
    int64_t  pts_us;      /* presentation time, microseconds from the stream's first timestamp */
    int64_t  dts_us;      /* decode time (video; == pts for audio) */
    int      key;         /* video: contains an IDR picture */
    uint8_t *data;        /* owned by the demuxer until the next ts_pop / ts_free */
    size_t   len;
} TsAU;

typedef struct TsDemux TsDemux;

TsDemux *ts_new(void);
void     ts_free(TsDemux *d);
void     ts_reset(TsDemux *d);              /* drop everything (a new stream follows) */

/* Feed stream bytes. Returns 0 on success, -1 when the internal queue is full (pop first). */
int  ts_push(TsDemux *d, const uint8_t *p, size_t n);
/* Next complete unit in arrival order. 1 = *au filled (valid until the next ts_pop), 0 = none. */
int  ts_pop(TsDemux *d, TsAU *au);
/* Units waiting, and their payload bytes (to bound the buffer). */
int  ts_queued(const TsDemux *d, size_t *bytes);
/* End of stream: complete the last open video unit so it can be popped. */
void ts_flush(TsDemux *d);

/* Once seen in the stream (0 until then): */
int  ts_video_avcc(const TsDemux *d, const uint8_t **avcc, int *len);   /* avcC from SPS/PPS */
int  ts_video_dims(const TsDemux *d, int *w, int *h);                   /* display size from the SPS */
int  ts_audio_config(const TsDemux *d, uint8_t asc[2], int *rate, int *channels);

#endif
