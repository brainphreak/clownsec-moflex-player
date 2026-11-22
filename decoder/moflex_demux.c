/* Standalone MOFLEX demuxer — see moflex_demux.h. Faithful translation of
 * FFmpeg libavformat/moflex.c to a plain FILE*-based reader. */
#include "moflex_demux.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

int mfx_rbuf_size = MFX_RBUF;

/* ---- buffered byte reader (mirrors avio_* used by moflex.c) ---- */
static void io_fill(MfxDemux *m) {
    if (m->io_err) { m->rn = m->rp = 0; m->eof = 1; return; }   /* never read from where a failed seek left us */
    m->rbase = (int64_t)ftello(m->f) - m->base;      /* logical pos within [base, base+file_size) */
    int toread = mfx_rbuf_size;
    int64_t remain = m->file_size - m->rbase;         /* never read past the window (CIA-embedded moflex) */
    if (remain < mfx_rbuf_size) toread = remain > 0 ? (int)remain : 0;
    size_t got = toread ? fread(m->rbuf, 1, (size_t)toread, m->f) : 0;
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
        m->io_err = fseeko(m->f, (off_t)(m->base + target), SEEK_SET) != 0;   /* physical = base + logical */
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
    m->ts   = (int64_t)io_rb64(m) + m->ts_off;
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
            /* Unknown descriptor type: SKIP its payload (previously the bytes were re-parsed
             * as descriptor entries -- derailed). If it declares a stream, register it as DATA
             * so its packets accumulate (used by the embedded-subtitle experiment: an unknown
             * type is invisible to the official player but carries our SRT). */
            if (ssize >= 1) {
                media_type = MFX_TYPE_DATA;
                stream_index = io_r8(m);
                if (ssize > 1) io_seek(m, (int64_t)(ssize - 1), SEEK_CUR);
            }
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

int mfx_open_window(MfxDemux *m, FILE *f, int64_t base, int64_t size) {
    memset(m, 0, sizeof(*m));
    m->f = f;
    m->base = base;
    m->rbuf = (uint8_t *)malloc(mfx_rbuf_size);
    if (!m->rbuf) return -1;

    /* window size (whole file, or the embedded region) + duration probe over the window's tail */
    if (size >= 0) m->file_size = size;
    else { fseeko(f, 0, SEEK_END); m->file_size = (int64_t)ftello(f) - base; }
    {
        /* The last sync block is normally in the final 2 MB. But when the audio outlasts the video,
         * the encoder writes the leftover audio as blocks with no sync markers -- minutes of it
         * (To Love Ru OVA S03e01: 3 min, 8.5 MB) -- so step back 2 MB at a time, up to 64 MB. */
        const int CH = 2 * 1024 * 1024;
        uint8_t *pb = (uint8_t *)malloc(CH + 12);
        if (pb) {
            int64_t last = 0;
            const int64_t MAX_US = 24LL * 3600 * 1000000;   /* 24h sanity cap */
            for (int64_t end = m->file_size; end > 0 && last == 0 && m->file_size - end < 64LL * 1024 * 1024;
                 end -= CH) {
                int64_t probe = end - CH;
                if (probe < 0) probe = 0;
                int want = (int)(end - probe);
                if (end + 12 <= m->file_size) want += 12;   /* overlap: a header split across chunks */
                fseeko(f, base + probe, SEEK_SET);
                int got = (int)fread(pb, 1, want, f);
                for (int i = 0; i + 12 < got; i++)
                    if (pb[i] == 0x4C && pb[i + 1] == 0x32) {
                        int64_t t = 0;
                        for (int j = 0; j < 8; j++) t = (t << 8) | pb[i + 4 + j];
                        /* accept only sane, monotonically-increasing timestamps -- the 0x4C32 byte
                         * pattern also appears inside media data, so reject garbage (negative, absurd,
                         * or out-of-order) to avoid a bogus duration that breaks the bar and seeking. */
                        if (t > last && t < MAX_US) last = t;
                        i += 11;
                    }
                if (probe == 0) break;
            }
            m->duration_us = last;
            free(pb);
        }
    }
    fseeko(f, base, SEEK_SET);
    m->rn = m->rp = 0; m->rbase = 0; m->eof = 0;   /* reset buffered reader */

    int ret = moflex_read_sync(m);
    if (ret != 0) return -1;   /* a moflex always begins with the 0x4C32 sync -- anything else is not one */
    io_seek(m, 0, SEEK_SET);   /* AVFMTCTX_NOHEADER: restart for packets */
    return 0;
}

int (*mfx_open_hook)(MfxDemux *m, FILE *f);
int mfx_open(MfxDemux *m, FILE *f) {
    if (mfx_open_hook) { int r = mfx_open_hook(m, f); if (r != -2) return r; }
    return mfx_open_window(m, f, 0, -1);
}

static int seek_off(MfxDemux *m, int64_t off, int64_t *landed_off);

#define MFX_LIVE_SIZE (1LL << 50)   /* a live stream has no end we know of: reads run until EOF */
int mfx_open_live(MfxDemux *m, FILE *f, int64_t (*live_seek)(void *, int64_t), void *ctx,
                  int64_t t0_us, int64_t duration_us) {
    memset(m, 0, sizeof(*m));
    m->f = f;
    m->rbuf = (uint8_t *)malloc(mfx_rbuf_size);
    if (!m->rbuf) return -1;
    m->file_size = MFX_LIVE_SIZE;
    m->duration_us = duration_us;
    m->live_seek = live_seek; m->live_ctx = ctx; m->ts_off = t0_us;
    m->rn = m->rp = 0; m->rbase = 0; m->eof = 0;
    m->io_err = fseeko(f, 0, SEEK_SET) != 0;   /* opened again on the same stream (after a probe): its start */
    if (moflex_read_sync(m) != 0) return -1;
    io_seek(m, 0, SEEK_SET);
    return 0;
}
/* live: a new stream from the source starting at target_us, read from its first block */
static int live_seek_time(MfxDemux *m, int64_t target_us) {
    if (target_us < 0) target_us = 0;
    if (m->duration_us > 0 && target_us > m->duration_us - 1000000) target_us = m->duration_us - 1000000;
    if (target_us < 0) target_us = 0;
    int64_t t0 = m->live_seek(m->live_ctx, target_us);
    if (t0 < 0) { m->eof = 1; return -1; }
    m->ts_off = t0;
    m->rn = m->rp = 0; m->rbase = 0;      /* the stream restarted at byte 0: drop what the reader held */
    m->io_err = fseeko(m->f, 0, SEEK_SET) != 0;
    m->eof = 0; m->in_block = 0;
    for (int i = 0; i < m->nb_streams; i++) m->streams[i].acc_size = 0;
    return seek_off(m, 0, NULL);
}
int mfx_seek_frac(MfxDemux *m, double frac) {
    if (m->live_seek) return live_seek_time(m, (int64_t)(frac * m->duration_us));
    if (m->file_size <= 0) return -1;
    if (frac < 0) frac = 0;
    if (frac > 0.999) frac = 0.999;
    return seek_off(m, (int64_t)(frac * m->file_size), NULL);
}
/* go to byte offset `off` (window-relative) and realign to the next real sync marker; *landed_off =
 * where that block starts */
#ifdef MFX_COUNT
long g_seek_probes;
#endif
static int seek_off(MfxDemux *m, int64_t off, int64_t *landed_off) {
#ifdef MFX_COUNT
    g_seek_probes++;
#endif
    if (m->file_size <= 0) return -1;
    if (off < 0) off = 0;
    io_seek(m, off, SEEK_SET);
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
            if (ok) { m->ts = ts + m->ts_off; io_seek(m, after - 2, SEEK_SET); found = 1;
                      if (landed_off) *landed_off = after - 2; break; }  /* real sync */
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
static void seek_learn(MfxDemux *m, int64_t off, int64_t ts) {
    int i = 0;
    while (i < m->kn && m->kofs[i] < off) i++;
    if (i < m->kn && m->kofs[i] == off) return;
    if (m->kn >= MFX_KN) return;
    for (int j = m->kn; j > i; j--) { m->kofs[j] = m->kofs[j - 1]; m->kts[j] = m->kts[j - 1]; }
    m->kofs[i] = off; m->kts[i] = ts; m->kn++;
}
/* Seek to a target TIMESTAMP (microseconds): lands on the last block at/before it.
 * Interpolates between the closest known blocks (every probe is remembered), so a first seek takes a
 * few probes and later ones -- preview stills while dragging, the landing -- usually one; stops once
 * the bracket is under 64 KB (a fraction of a second). The previous version halved the whole file 12
 * times per seek, each probe a resync scan, and landed up to ~0.5 MB early. */
int mfx_seek_time(MfxDemux *m, int64_t target_us) {
    if (m->live_seek) return live_seek_time(m, target_us);
    if (m->duration_us <= 0 || m->file_size <= 0) return mfx_seek_frac(m, 0);
    if (target_us <= 0) return mfx_seek_frac(m, 0);   /* seek-to-start: no search needed */
    if (target_us > m->duration_us) target_us = m->duration_us;
    if (m->ls_valid && m->ls_t == target_us) return seek_off(m, m->ls_off, NULL);   /* same answer again */

    int64_t ao = 0, at = 0, bo = m->file_size, bt = m->duration_us;
    for (int i = 0; i < m->kn; i++) {
        if (m->kts[i] <= target_us) { if (m->kofs[i] > ao) { ao = m->kofs[i]; at = m->kts[i]; } }
        else if (m->kofs[i] < bo) { bo = m->kofs[i]; bt = m->kts[i]; }
    }
    int have_a = (ao > 0);
    for (int it = 0; it < 12 && bo - ao > 65536; it++) {
        int64_t span = bo - ao, g;
        if (bt > at) g = ao + (int64_t)((double)(target_us - at) / (double)(bt - at) * (double)span);
        else g = ao + span / 2;
        int64_t lo = ao + span / 16, hi = bo - span / 16;        /* always shrink the bracket */
        if (g < lo) g = lo; if (g > hi) g = hi;
        int64_t lofs = -1;
        if (seek_off(m, g, &lofs) != 0 || lofs < 0) { bo = g; continue; }
        seek_learn(m, lofs, m->ts);
        if (m->ts <= target_us) { if (lofs > ao) { ao = lofs; at = m->ts; have_a = 1; } else { bo = g; } }
        else { bo = g; bt = m->ts; }
    }
    int64_t lofs = -1;
    int r = have_a ? seek_off(m, ao, &lofs) : mfx_seek_frac(m, 0);
    if (r == 0) { m->ls_valid = 1; m->ls_t = target_us; m->ls_off = have_a ? ao : 0; }
    return r;
}

/* Detect frame-interleaved 3D vs 2D: a 3D moflex delivers ~2x the timebase frame rate (an L and an R
 * frame per displayed instant); a 2D one delivers ~1x. Pre-scans ~2s of packets (demux only, no
 * decode) then rewinds to the start so playback is unaffected. Returns 1 = 3D, 0 = 2D. */
int mfx_detect_stereo(MfxDemux *m) {
    int vi = -1;
    for (int i = 0; i < m->nb_streams; i++)
        if (m->streams[i].media_type == MFX_TYPE_VIDEO && vi < 0) vi = i;
    if (vi < 0 || m->streams[vi].tb_den <= 0 || m->streams[vi].tb_num <= 0) return 1;   /* default 3D */

    double tbfps = (double)m->streams[vi].tb_den / (double)m->streams[vi].tb_num;
    /* A/V interleave: a block's timestamp is (about) its first video frame's time, but muxers put
     * the audio a little ahead or behind -- Nintendo's by 25-65 ms, varying block to block. From the
     * file start both are known EXACTLY (frames and samples counted from 0), so measure each block's
     * audio-vs-video offset here; a seek or resume, which starts mid-file where nothing can be
     * counted, then places its audio with the average instead of trusting the timestamp (which left
     * the sound up to ~60 ms early after a seek -- "audio a moment sooner" on resumed 3D films). */
    int ai = -1;
    for (int i = 0; i < m->nb_streams; i++)
        if (m->streams[i].media_type == MFX_TYPE_AUDIO) { ai = i; break; }
    int afr_ok = ai >= 0 && m->streams[ai].sample_rate > 0 && m->streams[ai].channels > 0 &&
                 (m->streams[ai].codec_id == MFX_ACODEC_ADPCM_IMA || m->streams[ai].codec_id == MFX_ACODEC_PCM_S16LE);
    /* which muxer: Nintendo's header carries 0x07 at byte 12 (ours 0x0f, which clocks audio exactly) */
    io_seek(m, 12, SEEK_SET); int hdr12 = io_r8(m);
    io_seek(m, 0, SEEK_SET); m->in_block = 0; m->eof = 0;
    for (int i = 0; i < m->nb_streams; i++) m->streams[i].acc_size = 0;
    double trf = 0, tdu = 0;   /* microsecond truncation: lost fractions / packet durations */
#define AVB 8
    int64_t bts_[AVB]; long bv_[AVB]; long long ba_[AVB]; int bvs_[AVB], bas_[AVB]; int nbk = 0;
    long long asmp = 0; int64_t blk = -1;
    long vc = 0; int64_t t0 = -1; MfxPacket p;
    while (mfx_next_packet(m, &p) == 1) {
        if (m->ts != blk) { blk = m->ts; if (nbk < AVB) { bts_[nbk] = blk; bvs_[nbk] = bas_[nbk] = 0; } nbk++; }
        int mt = m->streams[p.stream_index].media_type;
        if (afr_ok && p.stream_index == ai) {
            if (nbk <= AVB && !bas_[nbk - 1]) { ba_[nbk - 1] = asmp; bas_[nbk - 1] = 1; }
            int ch = m->streams[ai].channels;
            long ns = m->streams[ai].codec_id == MFX_ACODEC_ADPCM_IMA ? (p.size > 4 * ch ? (long)(p.size - 4 * ch) * 2 / ch : 0)
                                                                      : p.size / (2 * ch);
            asmp += ns;
            { double d = (double)ns * 1e6 / m->streams[ai].sample_rate; trf += d - (double)(long long)d; tdu += d; }
        }
        if (mt != MFX_TYPE_VIDEO) continue;
        if (nbk <= AVB && !bvs_[nbk - 1]) { bv_[nbk - 1] = vc; bvs_[nbk - 1] = 1; }
        if (t0 < 0) t0 = m->ts;
        vc++;
        if (m->ts - t0 >= 2000000) break;   /* 2s of content is plenty to see the 1x vs 2x ratio */
    }
    int stereo = 1;
    m->tb_is_eye = 0;
    if (t0 >= 0 && m->ts > t0 && vc > 4) {
        double dfps = (double)vc / ((double)(m->ts - t0) / 1e6);
        /* Interleaved 3D shows up two ways depending on how the frame rate was declared:
         *  - our transcodes declare the DISPLAY rate (24-30) -> actual packet rate is ~2x it.
         *  - Nintendo's files declare the PACKET rate itself (~48-60 = L+R for 24-30fps) -> ratio ~1x,
         *    but the declared rate is implausibly high for a real display rate on this hardware.
         *    In that case the timebase is the EYE period: a display pair lasts 2x it (tb_is_eye,
         *    else the player paces video at double speed and audio falls behind). */
        if ((dfps / tbfps) > 1.5) stereo = 1;
        else if (tbfps >= 47.0 && dfps / tbfps > 0.75) { stereo = 1; m->tb_is_eye = 1; }
        else stereo = 0;
    }
    /* the A/V offset, averaged over the blocks after the first (block 0 is exact by definition) */
    m->av_off = 0; m->av_drift = 0; m->av_tref = 0;
    if (afr_ok && !m->live_seek && nbk >= 2) {
        if (hdr12 == 0x07 && tdu > 0) m->av_drift = trf / tdu;
        double tsum = 0;
        double vper = (double)m->streams[vi].tb_num / m->streams[vi].tb_den * 1e6;   /* one video packet */
        if (stereo && !m->tb_is_eye) vper /= 2;                                     /* (pair-rate timebase) */
        double sum = 0; int n = 0;
        for (int k = 1; k < nbk && k < AVB; k++) {
            if (!bvs_[k] || !bas_[k]) continue;
            double vt = bv_[k] * vper, at = ba_[k] * 1e6 / m->streams[ai].sample_rate;
            sum += (double)(bts_[k] - at) - (double)(bts_[k] - vt); tsum += (double)bts_[k]; n++;
        }
        if (n) { double o = sum / n; if (o > -150000 && o < 150000) { m->av_off = (int64_t)o; m->av_tref = (int64_t)(tsum / n); } }
        if (!m->av_off && !m->av_tref) m->av_drift = 0;
    }
#undef AVB
    /* rewind demux to the start for playback */
    io_seek(m, 0, SEEK_SET);
    m->in_block = 0;
    for (int i = 0; i < m->nb_streams; i++) m->streams[i].acc_size = 0;
    m->eof = 0;
    return stereo;
}

int64_t mfx_audio_start(const MfxDemux *m, int64_t ts) {
    if (ts <= 1000) return ts;                         /* the first block: exact by definition */
    double off = (double)m->av_off - m->av_drift * (double)(ts - m->av_tref);
    return ts - (int64_t)off;
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
            if (m->audio_only && st->media_type != MFX_TYPE_AUDIO) {
                io_seek(m, (int64_t)pkt_size, SEEK_CUR);   /* audio-only reader: skip video data (no memcpy/read) */
                continue;
            }
            if (acc_append(st, m, pkt_size) < 0) return -1;

            if (endframe && st->acc_size > 0) {
                pkt->stream_index = stream_index;
                pkt->data = st->acc;
                pkt->size = st->acc_size;
                pkt->pos  = m->pos;
                if (st->media_type == MFX_TYPE_VIDEO)
                    pkt->keyframe = (st->acc[1] & 0x80) ? 1 : 0;
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
