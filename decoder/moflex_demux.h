/* Standalone MOFLEX demuxer — derived from FFmpeg libavformat/moflex.c
 * (LGPL, Nouwt/Surani/Mahol). Reads from a plain FILE*, no libav.
 * Emits reassembled per-stream packets (one full frame each). */
#ifndef MOFLEX_DEMUX_H
#define MOFLEX_DEMUX_H

#include <stdio.h>
#include <stdint.h>

enum { MFX_TYPE_VIDEO = 0, MFX_TYPE_AUDIO = 1, MFX_TYPE_DATA = 2 };
enum { MFX_VCODEC_MOBICLIP = 0 };
enum { MFX_ACODEC_FASTAUDIO = 0, MFX_ACODEC_ADPCM_IMA = 1, MFX_ACODEC_PCM_S16LE = 2 };

typedef struct {
    int      media_type;   /* MFX_TYPE_* */
    int      codec_id;     /* per-media codec enum above */
    int      width, height;
    int      sample_rate, channels;
    int      tb_num, tb_den;

    /* frame reassembly accumulator */
    uint8_t *acc;
    int      acc_size, acc_cap;
} MfxStream;

typedef struct {
    int      stream_index;
    uint8_t *data;   /* borrowed: valid until next call */
    int      size;
    int64_t  pos;
    int      keyframe;
} MfxPacket;

#define MFX_RBUF (64 * 1024)

typedef struct {
    FILE   *f;
    int     eof;

    /* buffered reader (avoids per-byte stdio calls); heap-allocated so the
     * MfxDemux struct stays small enough to live on the 3DS's small stack. */
    uint8_t *rbuf;
    int     rn, rp;      /* bytes in buffer / current read pos */
    int64_t rbase;       /* file offset of rbuf[0] */

    MfxStream streams[16];
    int       nb_streams;

    /* sync/block state (mirrors FFmpeg MOFLEXDemuxContext) */
    unsigned size;
    int64_t  pos;
    int64_t  ts;            /* current block timestamp, MICROSECONDS */
    int      flags;
    int      in_block;

    int64_t  file_size;
    int64_t  duration_us;   /* estimated total duration (microseconds) */

    unsigned br_last, br_pos;  /* bit reader */
} MfxDemux;

/* Returns 0 on success. Parses the first sync so stream table is known. */
int  mfx_open(MfxDemux *m, FILE *f);
void mfx_close(MfxDemux *m);

/* Returns 1 and fills *pkt on a packet, 0 at EOF, <0 on error. */
int  mfx_next_packet(MfxDemux *m, MfxPacket *pkt);

/* Seek to ~fraction (0..1) of the file, realign to the next sync marker.
 * Approximate (nearest block); caller should flush the decoder and decode
 * until the first frame comes back. Returns 0 on success. */
int  mfx_seek_frac(MfxDemux *m, double frac);

/* Seek to a target timestamp (microseconds) via binary search — lands near the
 * target time despite the nonlinear byte<->time mapping. Returns 0 on success. */
int  mfx_seek_time(MfxDemux *m, int64_t target_us);

#endif
