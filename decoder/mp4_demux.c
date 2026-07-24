/* Minimal MP4/ISO-BMFF demuxer -- see mp4_demux.h. Handles one H.264 (avc1) video track and one
 * AAC (mp4a) audio track. To stay reliable on the 3DS SD/FS, it reads the whole 'moov' index into
 * RAM once (a handful of seeks + one big read) and parses everything from memory -- rather than
 * thousands of tiny seeks/reads across the card. Sample DATA is still read on demand from mdat. */
#include "mp4_demux.h"
#include <stdlib.h>
#include <string.h>

/* ---- big-endian readers over a memory buffer ---- */
static uint32_t mu32(const uint8_t *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static uint16_t mu16(const uint8_t *p) { return (uint16_t)((p[0]<<8)|p[1]); }
static uint64_t mu64(const uint8_t *p) { return ((uint64_t)mu32(p)<<32)|mu32(p+4); }

#define BOX(a,b,c,d) (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(d))

/* per-track scratch while parsing */
typedef struct {
    int is_video, is_audio;
    int width, height;
    uint32_t timescale;
    uint8_t avcc[512]; int avcc_len; int nal_len_size;
    uint32_t vfourcc;               /* video sample-entry fourcc (0 = none) */
    uint8_t asc[64]; int asc_len; int a_rate, a_channels;
    uint32_t *stts_n, *stts_d; int stts_cnt;
    uint32_t *stsc_first, *stsc_spc; int stsc_cnt;
    uint32_t *stsz; int stsz_cnt; uint32_t stsz_uniform;
    uint64_t *stco; int stco_cnt;
    uint32_t *stss; int stss_cnt;
} Trak;

/* ---- memory box iterator: fills type/body/bend for one child box in b[pos,end) ---- */
static int mbox(const uint8_t *b, int64_t pos, int64_t end, uint32_t *type, int64_t *body, int64_t *bend) {
    if (pos + 8 > end) return 0;
    uint64_t size = mu32(b + pos);
    *type = mu32(b + pos + 4);
    int64_t hdr = 8;
    if (size == 1) { if (pos + 16 > end) return 0; size = mu64(b + pos + 8); hdr = 16; }
    else if (size == 0) size = (uint64_t)(end - pos);
    if ((int64_t)size < hdr || pos + (int64_t)size > end) return 0;
    *body = pos + hdr; *bend = pos + (int64_t)size;
    return 1;
}

/* MPEG-4 descriptor length (7-bit continuation) */
static int mem_desclen(const uint8_t *b, int64_t *p, int64_t end) {
    int n = 0, c, cnt = 0;
    do { if (*p >= end) break; c = b[(*p)++]; n = (n << 7) | (c & 0x7f); } while ((c & 0x80) && ++cnt < 4);
    return n;
}
static void parse_esds_mem(const uint8_t *b, int64_t body, int64_t bend, Trak *t) {
    int64_t p = body + 4;                                   /* skip version+flags */
    if (p >= bend || b[p++] != 0x03) return;                /* ES_Descriptor */
    mem_desclen(b, &p, bend); p += 3;                        /* ES_ID(2)+flags(1) */
    if (p >= bend || b[p++] != 0x04) return;                /* DecoderConfigDescriptor */
    mem_desclen(b, &p, bend); p += 13;                       /* objtype(1)+streamtype/bufsize(4)+br(8) */
    if (p >= bend || b[p++] != 0x05) return;                /* DecoderSpecificInfo = AudioSpecificConfig */
    int n = mem_desclen(b, &p, bend);
    if (n > (int)sizeof t->asc) n = sizeof t->asc;
    if (n > 0 && p + n <= bend) { memcpy(t->asc, b + p, n); t->asc_len = n; }
}

static void parse_stsd_mem(const uint8_t *b, int64_t body, int64_t bend, Trak *t) {
    if (body + 8 > bend) return;
    uint32_t nent = mu32(b + body + 4);
    int64_t pos = body + 8;
    for (uint32_t i = 0; i < nent && pos + 8 <= bend; i++) {
        uint32_t esz = mu32(b + pos), fmt = mu32(b + pos + 4);
        int64_t eend = pos + (esz ? esz : 8); if (eend > bend) eend = bend;
        /* Record the video codec fourcc even for formats we CANNOT decode, so the player can say
         * WHY instead of feeding a 0x0 frame with no SPS/PPS straight into the MVD hardware.
         * MVD does H.264 only: an HEVC file (hvc1/hev1) used to match nothing here, leaving
         * width=height=avcc_len=0, which then went into mvdstdInit() and crashed. */
        if (fmt == BOX('h','v','c','1') || fmt == BOX('h','e','v','1') ||
            fmt == BOX('a','v','0','1') || fmt == BOX('m','p','4','v') ||
            fmt == BOX('v','p','0','9') || fmt == BOX('V','P','9','0')) {
            t->is_video = 1; t->vfourcc = fmt;              /* known video, unsupported codec */
            if (pos + 8 + 28 <= bend) { t->width = mu16(b + pos + 8 + 24); t->height = mu16(b + pos + 8 + 26); }
        }
        if (fmt == BOX('a','v','c','1') || fmt == BOX('a','v','c','C')) {
            t->is_video = 1; t->vfourcc = BOX('a','v','c','1');
            if (pos + 8 + 28 <= bend) { t->width = mu16(b + pos + 8 + 24); t->height = mu16(b + pos + 8 + 26); }
            int64_t cp = pos + 8 + 78;                       /* scan avc1's child boxes for 'avcC' */
            while (cp + 8 <= eend) {
                uint32_t csz = mu32(b + cp), ct = mu32(b + cp + 4);
                if (csz < 8 || cp + csz > eend) break;
                if (ct == BOX('a','v','c','C')) {
                    int n = (int)csz - 8; if (n > (int)sizeof t->avcc) n = sizeof t->avcc;
                    memcpy(t->avcc, b + cp + 8, n); t->avcc_len = n;
                    if (n >= 5) t->nal_len_size = (t->avcc[4] & 3) + 1;
                    break;
                }
                cp += csz;
            }
        } else if (fmt == BOX('m','p','4','a')) {
            t->is_audio = 1;
            if (pos + 8 + 28 <= bend) { t->a_channels = mu16(b + pos + 8 + 16); t->a_rate = (int)(mu32(b + pos + 8 + 24) >> 16); }
            int64_t cp = pos + 8 + 28;
            while (cp + 8 <= eend) {
                uint32_t csz = mu32(b + cp), ct = mu32(b + cp + 4);
                if (csz < 8 || cp + csz > eend) break;
                if (ct == BOX('e','s','d','s')) { parse_esds_mem(b, cp + 8, cp + csz, t); break; }
                cp += csz;
            }
        }
        pos = eend;
    }
}

/* clamp an entry count so count*stride stays inside the box */
static uint32_t clamp_cnt(uint32_t n, int64_t avail, int stride) {
    if (stride <= 0 || avail < 0) return 0;
    uint32_t max = (uint32_t)(avail / stride);
    return n > max ? max : n;
}
static void parse_stbl_leaf_mem(const uint8_t *b, uint32_t type, int64_t body, int64_t bend, Trak *t) {
    if (body + 8 > bend) return;
    const uint8_t *p = b + body;
    int64_t avail = bend - body - 8;
    if (type == BOX('s','t','t','s')) {
        uint32_t n = clamp_cnt(mu32(p + 4), avail, 8);
        t->stts_n = malloc((size_t)n * 4 + 4); t->stts_d = malloc((size_t)n * 4 + 4); t->stts_cnt = n;
        for (uint32_t i = 0; i < n; i++) { t->stts_n[i] = mu32(p + 8 + i*8); t->stts_d[i] = mu32(p + 8 + i*8 + 4); }
    } else if (type == BOX('s','t','s','c')) {
        uint32_t n = clamp_cnt(mu32(p + 4), avail, 12);
        t->stsc_first = malloc((size_t)n * 4 + 4); t->stsc_spc = malloc((size_t)n * 4 + 4); t->stsc_cnt = n;
        for (uint32_t i = 0; i < n; i++) { t->stsc_first[i] = mu32(p + 8 + i*12); t->stsc_spc[i] = mu32(p + 8 + i*12 + 4); }
    } else if (type == BOX('s','t','s','z')) {
        t->stsz_uniform = mu32(p + 4);
        uint32_t n = mu32(p + 8);
        if (t->stsz_uniform == 0) n = clamp_cnt(n, avail - 4, 4);
        t->stsz_cnt = n;
        if (t->stsz_uniform == 0) { t->stsz = malloc((size_t)n * 4 + 4); for (uint32_t i = 0; i < n; i++) t->stsz[i] = mu32(p + 12 + i*4); }
    } else if (type == BOX('s','t','c','o')) {
        uint32_t n = clamp_cnt(mu32(p + 4), avail, 4);
        t->stco = malloc((size_t)n * 8 + 8); t->stco_cnt = n;
        for (uint32_t i = 0; i < n; i++) t->stco[i] = mu32(p + 8 + i*4);
    } else if (type == BOX('c','o','6','4')) {
        uint32_t n = clamp_cnt(mu32(p + 4), avail, 8);
        t->stco = malloc((size_t)n * 8 + 8); t->stco_cnt = n;
        for (uint32_t i = 0; i < n; i++) t->stco[i] = mu64(p + 8 + i*8);
    } else if (type == BOX('s','t','s','s')) {
        uint32_t n = clamp_cnt(mu32(p + 4), avail, 4);
        t->stss = malloc((size_t)n * 4 + 4); t->stss_cnt = n;
        for (uint32_t i = 0; i < n; i++) t->stss[i] = mu32(p + 8 + i*4);
    }
}

static Mp4Sample *assemble(Trak *t, int *count) {
    int n = (int)t->stsz_cnt; if (n <= 0) { *count = 0; return NULL; }
    Mp4Sample *s = (Mp4Sample *)malloc((size_t)n * sizeof(Mp4Sample));
    if (!s) { *count = 0; return NULL; }
    for (int i = 0; i < n; i++) s[i].size = t->stsz_uniform ? t->stsz_uniform : t->stsz[i];
    int si = 0;
    for (int c = 0; c < t->stco_cnt && si < n; c++) {
        int spc = 1;
        for (int r = 0; r < t->stsc_cnt; r++) { if ((int)t->stsc_first[r] - 1 <= c) spc = (int)t->stsc_spc[r]; else break; }
        int64_t off = (int64_t)t->stco[c];
        for (int k = 0; k < spc && si < n; k++) { s[si].offset = off; off += s[si].size; si++; }
    }
    for (; si < n; si++) s[si].offset = 0;
    int64_t dts = 0; int idx = 0;
    for (int e = 0; e < t->stts_cnt && idx < n; e++)
        for (uint32_t k = 0; k < t->stts_n[e] && idx < n; k++) { s[idx].dts = dts; dts += t->stts_d[e]; idx++; }
    for (; idx < n; idx++) s[idx].dts = dts;
    if (t->stss_cnt > 0) {
        for (int i = 0; i < n; i++) s[i].keyframe = 0;
        for (int i = 0; i < t->stss_cnt; i++) { int k = (int)t->stss[i] - 1; if (k >= 0 && k < n) s[k].keyframe = 1; }
    } else for (int i = 0; i < n; i++) s[i].keyframe = 1;
    *count = n;
    return s;
}

static void trak_free_tables(Trak *t) {
    free(t->stts_n); free(t->stts_d); free(t->stsc_first); free(t->stsc_spc);
    free(t->stsz); free(t->stco); free(t->stss);
}
static void finalize_trak(Mp4 *m, Trak *t) {
    if (t->is_video && !m->has_video) {
        m->has_video = 1; m->width = t->width; m->height = t->height; m->v_timescale = t->timescale ? t->timescale : 1;
        m->vfourcc = t->vfourcc;
        memcpy(m->avcc, t->avcc, sizeof m->avcc); m->avcc_len = t->avcc_len; m->nal_length_size = t->nal_len_size ? t->nal_len_size : 4;
        m->vsamples = assemble(t, &m->v_count);
    } else if (t->is_audio && !m->has_audio) {
        m->has_audio = 1; m->a_timescale = t->timescale ? t->timescale : 1; m->a_rate = t->a_rate; m->a_channels = t->a_channels;
        memcpy(m->asc, t->asc, sizeof m->asc); m->asc_len = t->asc_len;
        m->asamples = assemble(t, &m->a_count);
    }
    trak_free_tables(t);
}

static void walk_mem(Mp4 *m, Trak *t, const uint8_t *b, int64_t pos, int64_t end) {
    uint32_t type; int64_t body, bend;
    while (mbox(b, pos, end, &type, &body, &bend)) {
        switch (type) {
            case BOX('m','d','i','a'): case BOX('m','i','n','f'): case BOX('s','t','b','l'):
                walk_mem(m, t, b, body, bend); break;
            case BOX('t','r','a','k'): { Trak nt; memset(&nt, 0, sizeof nt); walk_mem(m, &nt, b, body, bend); finalize_trak(m, &nt); break; }
            case BOX('m','d','h','d'): { if (body + 24 <= bend) { uint32_t vf = mu32(b + body);
                    t->timescale = ((vf >> 24) == 1) ? mu32(b + body + 4 + 16) : mu32(b + body + 4 + 8); } break; }
            case BOX('h','d','l','r'): { if (body + 12 <= bend) { uint32_t h = mu32(b + body + 8);
                    if (h == BOX('v','i','d','e')) t->is_video = 1; else if (h == BOX('s','o','u','n')) t->is_audio = 1; } break; }
            case BOX('s','t','s','d'): parse_stsd_mem(b, body, bend, t); break;
            case BOX('s','t','t','s'): case BOX('s','t','s','c'): case BOX('s','t','s','z'):
            case BOX('s','t','c','o'): case BOX('c','o','6','4'): case BOX('s','t','s','s'):
                parse_stbl_leaf_mem(b, type, body, bend, t); break;
            default: break;
        }
        pos = bend;
    }
}

/* ---- file-level box header read (only used to locate 'moov') ---- */
static int fbox(FILE *f, int64_t pos, int64_t end, uint32_t *type, int64_t *body, int64_t *bend) {
    unsigned char h[16];
    if (pos + 8 > end || fseeko(f, pos, SEEK_SET)) return 0;
    if (fread(h, 1, 8, f) != 8) return 0;
    uint64_t size = mu32(h); *type = mu32(h + 4);
    int64_t hdr = 8;
    if (size == 1) { if (fread(h + 8, 1, 8, f) != 8) return 0; size = mu64(h + 8); hdr = 16; }
    else if (size == 0) size = (uint64_t)(end - pos);
    if ((int64_t)size < hdr || pos + (int64_t)size > end) return 0;
    *body = pos + hdr; *bend = pos + (int64_t)size;
    return 1;
}

int mp4_open(Mp4 *m, const char *path) {
    memset(m, 0, sizeof *m);
    m->f = fopen(path, "rb");
    if (!m->f) return 0;
    setvbuf(m->f, NULL, _IOFBF, 128 * 1024);   /* SD reads are the frame budget: buffer big */
    m->fa = fopen(path, "rb");                 /* audio's own handle -> two sequential streams
                                                * instead of one seek-thrashing between them */
    if (m->fa) setvbuf(m->fa, NULL, _IOFBF, 64 * 1024);
    fseeko(m->f, 0, SEEK_END);
    int64_t fsz = ftello(m->f);
    if (fsz <= 8) { mp4_close(m); return 0; }

    /* locate 'moov' among the top-level boxes (handles moov at the front OR end of the file) */
    int64_t pos = 0, moov_body = -1, moov_end = 0;
    for (;;) {
        uint32_t type; int64_t body, bend;
        if (!fbox(m->f, pos, fsz, &type, &body, &bend)) break;
        if (type == BOX('m','o','o','v')) { moov_body = body; moov_end = bend; break; }
        pos = bend;
    }
    if (moov_body < 0) { mp4_close(m); return 0; }

    int64_t msz = moov_end - moov_body;
    if (msz <= 0 || msz > 64 * 1024 * 1024) { mp4_close(m); return 0; }
    uint8_t *buf = (uint8_t *)malloc((size_t)msz);
    if (!buf) { mp4_close(m); return 0; }
    if (fseeko(m->f, moov_body, SEEK_SET) || (int64_t)fread(buf, 1, (size_t)msz, m->f) != msz) {
        free(buf); mp4_close(m); return 0;
    }

    walk_mem(m, NULL, buf, 0, msz);
    free(buf);
    if (!m->has_video) { mp4_close(m); return 0; }
    return 1;
}

void mp4_close(Mp4 *m) {
    if (m->f) fclose(m->f);
    if (m->fa) fclose(m->fa);
    free(m->vsamples); free(m->asamples);
    memset(m, 0, sizeof *m);
}

int mp4_read_sample(Mp4 *m, const Mp4Sample *s, uint8_t *buf) {
    if (fseeko(m->f, s->offset, SEEK_SET)) return 0;
    return (int)fread(buf, 1, s->size, m->f);
}
int mp4_read_sample_audio(Mp4 *m, const Mp4Sample *s, uint8_t *buf) {
    FILE *f = m->fa ? m->fa : m->f;
    if (fseeko(f, s->offset, SEEK_SET)) return 0;
    return (int)fread(buf, 1, s->size, f);
}

int mp4_keyframe_before(const Mp4 *m, int64_t dts) {
    int best = 0;
    for (int i = 0; i < m->v_count; i++) {
        if (m->vsamples[i].dts > dts) break;
        if (m->vsamples[i].keyframe) best = i;
    }
    return best;
}

double mp4_duration_s(const Mp4 *m) {
    if (!m->has_video || m->v_count == 0 || m->v_timescale == 0) return 0;
    int64_t last = m->vsamples[m->v_count - 1].dts;
    return (double)last / (double)m->v_timescale;
}
