/* Standalone MOFLEX demuxer — see moflex_demux.h. Faithful translation of
 * FFmpeg libavformat/moflex.c to a plain FILE*-based reader. */
#include "moflex_demux.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* ---- buffered byte reader (mirrors avio_* used by moflex.c) ---- */
static void io_fill(MfxDemux *m) {
    m->rbase = (int64_t)ftello(m->f);
    size_t got = fread(m->rbuf, 1, MFX_RBUF, m->f);
    m->rn = (int)got;
    m->rp = 0;
    if (m->rn <= 0) { m->rn = 0; m->eof = 1; }
}
static int io_r8(MfxDemux *m) {
    if (m->rp >= m->rn) { io_fill(m); if (m->rn == 0) return 0; }
    return m->rbuf[m->rp++];
}
static unsigned io_rb16(MfxDemux *m) { unsigned a = io_r8(m); return (a << 8) | io_r8(m); }
static unsigned io_rb24(MfxDemux *m) { unsigned a = io_rb16(m); return (a << 8) | io_r8(m); }
static uint64_t io_rb64(MfxDemux *m) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | (unsigned)io_r8(m);
    return v;
}
static int64_t io_tell(MfxDemux *m) { return m->rbase + m->rp; }
static void    io_seek(MfxDemux *m, int64_t off, int whence) {
    int64_t target = (whence == SEEK_CUR) ? io_tell(m) + off : off;   /* SEEK_SET/CUR only used here */
    if (target >= m->rbase && target <= m->rbase + m->rn) {
        m->rp = (int)(target - m->rbase);
    } else {
        fseeko(m->f, (off_t)target, SEEK_SET);
        m->rn = m->rp = 0; m->rbase = target;
    }
    m->eof = 0;
}
static int io_feof(MfxDemux *m) {
    if (m->rp < m->rn) return 0;
    io_fill(m);
    return m->rn == 0;
}
static int io_read(MfxDemux *m, uint8_t *dst, int n) {
    int got = 0;
    while (got < n) {
        if (m->rp >= m->rn) { io_fill(m); if (m->rn == 0) break; }
        int c = m->rn - m->rp; if (c > n - got) c = n - got;
        memcpy(dst + got, m->rbuf + m->rp, c);
        m->rp += c; got += c;
    }
    return got;
}

/* ---- bit reader (BitReader in moflex.c) ---- */
static int pop(MfxDemux *m) {
    if (io_feof(m)) return -1;
    if ((m->br_pos & 7) == 0) m->br_last = (unsigned)io_r8(m) << 24U;
    else                      m->br_last <<= 1;
    m->br_pos++;
    return !!(m->br_last & 0x80000000U);
}
static int pop_int(MfxDemux *m, int n) {
    int value = 0;
    for (int i = 0; i < n; i++) {
        int ret = pop(m);
        if (ret < 0) return ret;
        if (ret > INT_MAX - value - value) return -2;
        value = 2 * value + ret;
    }
    return value;
}
static int pop_length(MfxDemux *m) {
    int ret, n = 1;
    while ((ret = pop(m)) == 0) n++;
    if (ret < 0) return ret;
    return n;
}

static int read_var_byte(MfxDemux *m, unsigned *out) {
    unsigned value = 0, data;
    data = io_r8(m);
    if (!(data & 0x80)) { *out = data; return 0; }
    value = (data & 0x7F) << 7;
    data = io_r8(m);
    if (!(data & 0x80)) { *out = value | data; return 0; }
    value = (((data & 0x7F) | value) << 7);
    data = io_r8(m);
    if (!(data & 0x80)) { *out = value | data; return 0; }
    value = (((data & 0x7F) | value) << 7) | io_r8(m);
    *out = value;
    return 0;
}

/* Parse a sync marker + stream descriptors. Returns 0 on a real sync,
 * 1 if the 2 bytes were not a sync (rewound), <0 on EOF. */
static int moflex_read_sync(MfxDemux *m) {
    if (io_rb16(m) != 0x4C32) {
        if (io_feof(m)) return -1;
        io_seek(m, -2, SEEK_CUR);
        return 1;
    }
    io_seek(m, 2, SEEK_CUR);
    m->ts   = (int64_t)io_rb64(m);
    m->size = io_rb16(m) + 1;

    while (!io_feof(m)) {
        unsigned type, ssize, codec_id = 0;
        int media_type = -1, width = 0, height = 0, sample_rate = 0, channels = 0;
        int stream_index = -1, tb_num = 0, tb_den = 0;

        read_var_byte(m, &type);
        read_var_byte(m, &ssize);

        switch (type) {
        case 0:
            if (ssize > 0) io_seek(m, (int64_t)ssize, SEEK_CUR);
            return 0;
        case 2:
            media_type = MFX_TYPE_AUDIO;
            stream_index = io_r8(m);
            codec_id = io_r8(m);
            sample_rate = io_rb24(m) + 1;
            tb_num = 1; tb_den = sample_rate;
            channels = io_r8(m) + 1;
            break;
        case 1:
        case 3:
            media_type = MFX_TYPE_VIDEO;
            stream_index = io_r8(m);
            codec_id = io_r8(m);           /* 0 = MobiClip */
            tb_den = io_rb16(m);
            tb_num = io_rb16(m);
            width  = io_rb16(m);
            height = io_rb16(m);
            io_seek(m, type == 3 ? 3 : 2, SEEK_CUR);
            break;
        case 4:
            media_type = MFX_TYPE_DATA;
            stream_index = io_r8(m);
            io_seek(m, 1, SEEK_CUR);
            break;
        default:
            break;
        }

        if (stream_index == m->nb_streams && stream_index < 16) {
            MfxStream *st = &m->streams[m->nb_streams++];
            memset(st, 0, sizeof(*st));
            st->media_type  = media_type;
            st->codec_id    = codec_id;
            st->width       = width;
            st->height      = height;
            st->sample_rate = sample_rate;
            st->channels    = channels;
            st->tb_num      = tb_num;
            st->tb_den      = tb_den;
        }
    }
    return 0;
}

static int acc_append(MfxStream *st, MfxDemux *m, int n) {
    if (st->acc_size + n > st->acc_cap) {
        int cap = st->acc_size + n + 4096;
        uint8_t *p = (uint8_t *)realloc(st->acc, cap);
        if (!p) return -1;
        st->acc = p; st->acc_cap = cap;
    }
    if (io_read(m, st->acc + st->acc_size, n) != n) { m->eof = 1; return -1; }
    st->acc_size += n;
    return 0;
}

int mfx_open(MfxDemux *m, FILE *f) {
    memset(m, 0, sizeof(*m));
    m->f = f;
    m->rbuf = (uint8_t *)malloc(MFX_RBUF);
    if (!m->rbuf) return -1;

    /* probe file size + duration: last sync timestamp (microseconds) in the tail */
    fseeko(f, 0, SEEK_END);
    m->file_size = (int64_t)ftello(f);
    {
        int plen = 2 * 1024 * 1024;
        if ((int64_t)plen > m->file_size) plen = (int)m->file_size;
        int64_t probe = m->file_size - plen;
        uint8_t *pb = (uint8_t *)malloc(plen);
        if (pb) {
            fseeko(f, probe, SEEK_SET);
            int got = (int)fread(pb, 1, plen, f);
            int64_t last = 0;
            for (int i = 0; i + 12 < got; i++)
                if (pb[i] == 0x4C && pb[i + 1] == 0x32) {
                    int64_t t = 0;
                    for (int j = 0; j < 8; j++) t = (t << 8) | pb[i + 4 + j];
                    last = t; i += 11;
                }
            m->duration_us = last;
            free(pb);
        }
    }
    fseeko(f, 0, SEEK_SET);
    m->rn = m->rp = 0; m->rbase = 0; m->eof = 0;   /* reset buffered reader */

    int ret = moflex_read_sync(m);
    if (ret < 0) return ret;
    io_seek(m, 0, SEEK_SET);   /* AVFMTCTX_NOHEADER: restart for packets */
    return 0;
}

int mfx_seek_frac(MfxDemux *m, double frac) {
    if (m->file_size <= 0) return -1;
    if (frac < 0) frac = 0;
    if (frac > 0.999) frac = 0.999;
    io_seek(m, (int64_t)(frac * m->file_size), SEEK_SET);
    m->eof = 0;
    /* realign to the next REAL sync marker: 0x4C32 whose timestamp is in range
     * (filters false 0x4C32 matches inside packet data, which caused hangs). */
    int prev = -1, found = 0;
    long scan = 0;
    while (scan < (8 * 1024 * 1024) && !io_feof(m)) {
        int b = io_r8(m); scan++;
        if (prev == 0x4C && b == 0x32) {
            int64_t after = io_tell(m);              /* just past 0x32 */
            io_seek(m, 2, SEEK_CUR);                 /* skip 2 */
            int64_t ts = 0;
            for (int j = 0; j < 8; j++) ts = (ts << 8) | (unsigned)io_r8(m);
            int ok;
            if (m->duration_us > 0) ok = (ts >= 0 && ts <= m->duration_us + 2000000);
            else                    ok = (ts >= 0 && ts < 100LL * 3600 * 1000000);
            if (ok) { m->ts = ts; io_seek(m, after - 2, SEEK_SET); found = 1; break; }  /* real sync */
            io_seek(m, after, SEEK_SET);             /* false match; keep scanning */
            prev = -1;
            continue;
        }
        prev = b;
    }
    m->in_block = 0;
    for (int i = 0; i < m->nb_streams; i++) m->streams[i].acc_size = 0;
    return found ? 0 : -1;
}

/* Seek to a target TIMESTAMP (microseconds). Binary-searches byte offsets so it
 * lands near the target regardless of the nonlinear byte<->time mapping. */
int mfx_seek_time(MfxDemux *m, int64_t target_us) {
    if (m->duration_us <= 0 || m->file_size <= 0) return mfx_seek_frac(m, 0);
    if (target_us < 0) target_us = 0;
    if (target_us > m->duration_us) target_us = m->duration_us;

    double lo = 0.0, hi = 0.999, best = 0.0;
    for (int it = 0; it < 12; it++) {
        double mid = (lo + hi) * 0.5;
        if (mfx_seek_frac(m, mid) != 0) { hi = mid; continue; }
        int64_t landed = m->ts;                 /* set by mfx_seek_frac */
        if (landed <= target_us) { lo = mid; best = mid; }
        else                     { hi = mid; }
    }
    return mfx_seek_frac(m, best);              /* land at/just before target */
}

void mfx_close(MfxDemux *m) {
    for (int i = 0; i < m->nb_streams; i++) free(m->streams[i].acc);
    free(m->rbuf);
    m->rbuf = NULL;
}

int mfx_next_packet(MfxDemux *m, MfxPacket *pkt) {
    while (!io_feof(m)) {
        if (!m->in_block) {
            m->pos = io_tell(m);
            int ret = moflex_read_sync(m);
            if (ret < 0) return 0;      /* EOF */
            m->flags = io_r8(m);
            if (m->flags & 2) io_seek(m, 2, SEEK_CUR);
        }

        while ((io_tell(m) < m->pos + (int64_t)m->size) && !io_feof(m) && io_r8(m)) {
            int stream_index, bits, pkt_size, endframe;
            m->in_block = 1;

            io_seek(m, -1, SEEK_CUR);
            m->br_pos = m->br_last = 0;

            bits = pop_length(m);
            if (bits < 0) return -1;
            stream_index = pop_int(m, bits);
            if (stream_index < 0) return -1;
            if (stream_index >= m->nb_streams) return -1;

            endframe = pop(m);
            if (endframe < 0) return -1;
            if (endframe) {
                bits = pop_length(m); if (bits < 0) return -1;
                pop_int(m, bits);
                pop(m);
                bits = pop_length(m); if (bits < 0) return -1;
                pop_int(m, bits * 2 + 26);
            }

            pkt_size = pop_int(m, 13) + 1;
            if ((unsigned)pkt_size > m->size) return -1;

            MfxStream *st = &m->streams[stream_index];
            if (acc_append(st, m, pkt_size) < 0) return -1;

            if (endframe && st->acc_size > 0) {
                pkt->stream_index = stream_index;
                pkt->data = st->acc;
                pkt->size = st->acc_size;
                pkt->pos  = m->pos;
                if (st->media_type == MFX_TYPE_VIDEO)
                    pkt->keyframe = (st->acc[0] & 0x80) ? 1 : 0;
                else
                    pkt->keyframe = 1;
                st->acc_size = 0;       /* consumed; reuse buffer */
                return 1;
            }
        }

        m->in_block = 0;
        if (m->flags % 2 == 0) {
            if (m->size <= 0) return -1;
            io_seek(m, m->pos + (int64_t)m->size, SEEK_SET);
        }
    }
    return 0;
}
