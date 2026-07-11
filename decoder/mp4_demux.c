/* Minimal MP4/ISO-BMFF demuxer -- see mp4_demux.h. Handles one H.264 (avc1) video track and one
 * AAC (mp4a) audio track: reads avcC (SPS/PPS) + esds (AudioSpecificConfig) and assembles the
 * per-sample offset/size/dts/keyframe tables from stsc/stco/stsz/stts/stss. */
#include "mp4_demux.h"
#include <stdlib.h>
#include <string.h>

/* ---- big-endian readers ---- */
static uint32_t rd32(FILE *f) { unsigned char b[4]; if (fread(b,1,4,f)!=4) return 0; return ((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|((uint32_t)b[2]<<8)|b[3]; }
static uint16_t rd16(FILE *f) { unsigned char b[2]; if (fread(b,1,2,f)!=2) return 0; return (uint16_t)((b[0]<<8)|b[1]); }
static uint64_t rd64(FILE *f) { return ((uint64_t)rd32(f)<<32)|rd32(f); }
static uint32_t u32be(const uint8_t *b) { return ((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|((uint32_t)b[2]<<8)|b[3]; }

/* per-track scratch while parsing */
typedef struct {
    int is_video, is_audio;
    int width, height;
    uint32_t timescale;
    /* video */
    uint8_t avcc[512]; int avcc_len; int nal_len_size;
    /* audio */
    uint8_t asc[64]; int asc_len; int a_rate, a_channels;
    /* raw sample tables */
    uint32_t *stts_n, *stts_d; int stts_cnt;                 /* time-to-sample: count/delta pairs */
    uint32_t *stsc_first, *stsc_spc; int stsc_cnt;           /* sample-to-chunk: first_chunk/samples_per_chunk */
    uint32_t *stsz; int stsz_cnt; uint32_t stsz_uniform;     /* sample sizes */
    uint64_t *stco; int stco_cnt;                            /* chunk offsets */
    uint32_t *stss; int stss_cnt;                            /* sync (keyframe) sample indices, 1-based */
} Trak;

/* ---- box iterator: returns 0 at end; fills type/body/bend for one child box ---- */
static int next_box(FILE *f, int64_t pos, int64_t end, uint32_t *type, int64_t *body, int64_t *bend) {
    if (pos + 8 > end) return 0;
    fseeko(f, pos, SEEK_SET);
    uint64_t size = rd32(f);
    *type = rd32(f);
    int64_t hdr = 8;
    if (size == 1) { size = rd64(f); hdr = 16; }
    else if (size == 0) size = (uint64_t)(end - pos);
    if ((int64_t)size < hdr || pos + (int64_t)size > end) return 0;
    *body = pos + hdr;
    *bend = pos + (int64_t)size;
    return 1;
}

#define BOX(a,b,c,d) (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(d))

/* ---- MPEG-4 descriptor length (variable, 7-bit continuation) ---- */
static int desc_len(FILE *f) { int n = 0, c, v; do { c = fgetc(f); if (c < 0) return n; v = c & 0x7f; n = (n << 7) | v; } while (c & 0x80); return n; }

/* parse an esds box body -> AudioSpecificConfig into t->asc */
static void parse_esds(FILE *f, int64_t body, int64_t bend, Trak *t) {
    fseeko(f, body + 4, SEEK_SET);                 /* skip version+flags */
    if (fgetc(f) != 0x03) return;                  /* ES_Descriptor */
    desc_len(f); rd16(f); fgetc(f);                /* ES_ID + flags */
    if (fgetc(f) != 0x04) return;                  /* DecoderConfigDescriptor */
    desc_len(f);
    fseeko(f, 13, SEEK_CUR);                        /* objtype(1)+streamtype/bufsize(4)+max/avg br(8) */
    if (fgetc(f) != 0x05) return;                  /* DecoderSpecificInfo = AudioSpecificConfig */
    int n = desc_len(f);
    if (n > (int)sizeof t->asc) n = sizeof t->asc;
    if (ftello(f) + n <= bend) t->asc_len = (int)fread(t->asc, 1, n, f);
}

/* parse stsd: find avc1/avcC (video) or mp4a/esds (audio) */
static void parse_stsd(FILE *f, int64_t body, int64_t bend, Trak *t) {
    fseeko(f, body + 4, SEEK_SET);                 /* version+flags */
    uint32_t nent = rd32(f);
    int64_t pos = ftello(f);
    for (uint32_t i = 0; i < nent && pos + 8 <= bend; i++) {
        fseeko(f, pos, SEEK_SET);
        uint32_t esz = rd32(f), fmt = rd32(f);
        int64_t eend = pos + (esz ? esz : 8);
        if (fmt == BOX('a','v','c','1') || fmt == BOX('a','v','c','C')) {
            t->is_video = 1;
            fseeko(f, pos + 8 + 24, SEEK_SET);     /* into the visual sample entry: width/height */
            t->width = rd16(f); t->height = rd16(f);
            /* scan child boxes of avc1 for 'avcC' */
            int64_t cp = pos + 8 + 78;
            while (cp + 8 <= eend) {
                fseeko(f, cp, SEEK_SET);
                uint32_t csz = rd32(f), ct = rd32(f);
                if (csz < 8 || cp + csz > eend) break;
                if (ct == BOX('a','v','c','C')) {
                    int n = (int)csz - 8; if (n > (int)sizeof t->avcc) n = sizeof t->avcc;
                    t->avcc_len = (int)fread(t->avcc, 1, n, f);
                    if (t->avcc_len >= 5) t->nal_len_size = (t->avcc[4] & 3) + 1;
                    break;
                }
                cp += csz;
            }
        } else if (fmt == BOX('m','p','4','a')) {
            t->is_audio = 1;
            fseeko(f, pos + 8 + 16, SEEK_SET);     /* channelcount, samplesize */
            t->a_channels = rd16(f); rd16(f); rd16(f); rd16(f);
            t->a_rate = (int)(rd32(f) >> 16);      /* 16.16 fixed -> integer Hz */
            int64_t cp = pos + 8 + 28;
            while (cp + 8 <= eend) {
                fseeko(f, cp, SEEK_SET);
                uint32_t csz = rd32(f), ct = rd32(f);
                if (csz < 8 || cp + csz > eend) break;
                if (ct == BOX('e','s','d','s')) { parse_esds(f, cp + 8, cp + csz, t); break; }
                cp += csz;
            }
        }
        pos = eend;
    }
}

static uint32_t *read_u32_table(FILE *f, int64_t body, int stride_words, int *count_out, int pick) {
    fseeko(f, body + 4, SEEK_SET);
    uint32_t n = rd32(f);
    if (n == 0 || n > 20000000) { *count_out = 0; return NULL; }
    uint32_t *out = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
    if (!out) { *count_out = 0; return NULL; }
    for (uint32_t i = 0; i < n; i++) { uint32_t v = 0; for (int w = 0; w < stride_words; w++) { uint32_t x = rd32(f); if (w == pick) v = x; } out[i] = v; }
    *count_out = (int)n;
    return out;
}

static void parse_stbl_leaf(FILE *f, uint32_t type, int64_t body, int64_t bend, Trak *t) {
    (void)bend;
    if (type == BOX('s','t','t','s')) {
        fseeko(f, body + 4, SEEK_SET); uint32_t n = rd32(f);
        t->stts_n = malloc((size_t)n*4); t->stts_d = malloc((size_t)n*4); t->stts_cnt = n;
        for (uint32_t i=0;i<n;i++){ t->stts_n[i]=rd32(f); t->stts_d[i]=rd32(f); }
    } else if (type == BOX('s','t','s','c')) {
        fseeko(f, body + 4, SEEK_SET); uint32_t n = rd32(f);
        t->stsc_first = malloc((size_t)n*4); t->stsc_spc = malloc((size_t)n*4); t->stsc_cnt = n;
        for (uint32_t i=0;i<n;i++){ t->stsc_first[i]=rd32(f); t->stsc_spc[i]=rd32(f); rd32(f); }
    } else if (type == BOX('s','t','s','z')) {
        fseeko(f, body + 4, SEEK_SET); t->stsz_uniform = rd32(f); uint32_t n = rd32(f); t->stsz_cnt = n;
        if (t->stsz_uniform == 0) { t->stsz = malloc((size_t)n*4); for (uint32_t i=0;i<n;i++) t->stsz[i]=rd32(f); }
    } else if (type == BOX('s','t','c','o')) {
        int c; uint32_t *o = read_u32_table(f, body, 1, &c, 0);
        t->stco = malloc((size_t)c*8); t->stco_cnt = c; for (int i=0;i<c;i++) t->stco[i]=o?o[i]:0; free(o);
    } else if (type == BOX('c','o','6','4')) {
        fseeko(f, body + 4, SEEK_SET); uint32_t n = rd32(f); t->stco = malloc((size_t)n*8); t->stco_cnt = n;
        for (uint32_t i=0;i<n;i++) t->stco[i]=rd64(f);
    } else if (type == BOX('s','t','s','s')) {
        int c; t->stss = read_u32_table(f, body, 1, &c, 0); t->stss_cnt = c;
    }
}

/* assemble per-sample table from the raw tables, into out[] (malloc'd), returns count */
static Mp4Sample *assemble(Trak *t, int *count) {
    int n = (int)t->stsz_cnt; if (n <= 0) { *count = 0; return NULL; }
    Mp4Sample *s = (Mp4Sample *)malloc((size_t)n * sizeof(Mp4Sample));
    if (!s) { *count = 0; return NULL; }
    /* sizes */
    for (int i = 0; i < n; i++) s[i].size = t->stsz_uniform ? t->stsz_uniform : t->stsz[i];
    /* offsets: walk chunks per stsc, laying samples consecutively within each chunk */
    int si = 0;
    for (int c = 0; c < t->stco_cnt && si < n; c++) {
        /* samples-per-chunk for chunk index c (0-based) from stsc runs */
        int spc = 1;
        for (int r = 0; r < t->stsc_cnt; r++) {
            if ((int)t->stsc_first[r] - 1 <= c) spc = (int)t->stsc_spc[r]; else break;
        }
        int64_t off = (int64_t)t->stco[c];
        for (int k = 0; k < spc && si < n; k++) { s[si].offset = off; off += s[si].size; si++; }
    }
    for (; si < n; si++) { s[si].offset = 0; }
    /* dts from stts deltas */
    int64_t dts = 0; int idx = 0;
    for (int e = 0; e < t->stts_cnt && idx < n; e++) {
        for (uint32_t k = 0; k < t->stts_n[e] && idx < n; k++) { s[idx].dts = dts; dts += t->stts_d[e]; idx++; }
    }
    for (; idx < n; idx++) s[idx].dts = dts;
    /* keyframes */
    if (t->stss_cnt > 0) {
        for (int i = 0; i < n; i++) s[i].keyframe = 0;
        for (int i = 0; i < t->stss_cnt; i++) { int k = (int)t->stss[i] - 1; if (k >= 0 && k < n) s[k].keyframe = 1; }
    } else for (int i = 0; i < n; i++) s[i].keyframe = 1;   /* no stss -> every sample is a sync sample */
    *count = n;
    return s;
}

static void trak_free_tables(Trak *t) {
    free(t->stts_n); free(t->stts_d); free(t->stsc_first); free(t->stsc_spc);
    free(t->stsz); free(t->stco); free(t->stss);
}

static void walk(Mp4 *m, Trak *t, int64_t pos, int64_t end);

static void finalize_trak(Mp4 *m, Trak *t) {
    if (t->is_video && !m->has_video) {
        m->has_video = 1; m->width = t->width; m->height = t->height; m->v_timescale = t->timescale ? t->timescale : 1;
        memcpy(m->avcc, t->avcc, sizeof m->avcc); m->avcc_len = t->avcc_len; m->nal_length_size = t->nal_len_size ? t->nal_len_size : 4;
        m->vsamples = assemble(t, &m->v_count);
    } else if (t->is_audio && !m->has_audio) {
        m->has_audio = 1; m->a_timescale = t->timescale ? t->timescale : 1; m->a_rate = t->a_rate; m->a_channels = t->a_channels;
        memcpy(m->asc, t->asc, sizeof m->asc); m->asc_len = t->asc_len;
        m->asamples = assemble(t, &m->a_count);
    }
    trak_free_tables(t);
}

static void walk(Mp4 *m, Trak *t, int64_t pos, int64_t end) {
    uint32_t type; int64_t body, bend;
    while (next_box(m->f, pos, end, &type, &body, &bend)) {
        switch (type) {
            case BOX('m','o','o','v'): case BOX('m','d','i','a'):
            case BOX('m','i','n','f'): case BOX('s','t','b','l'):
                walk(m, t, body, bend); break;
            case BOX('t','r','a','k'): {
                Trak nt; memset(&nt, 0, sizeof nt);
                walk(m, &nt, body, bend); finalize_trak(m, &nt); break;
            }
            case BOX('m','d','h','d'): {
                fseeko(m->f, body, SEEK_SET); uint32_t vf = rd32(m->f);
                if ((vf >> 24) == 1) { rd64(m->f); rd64(m->f); t->timescale = rd32(m->f); }
                else { rd32(m->f); rd32(m->f); t->timescale = rd32(m->f); }
                break;
            }
            case BOX('h','d','l','r'): {
                fseeko(m->f, body + 8, SEEK_SET); uint32_t h = rd32(m->f);
                if (h == BOX('v','i','d','e')) t->is_video = 1; else if (h == BOX('s','o','u','n')) t->is_audio = 1;
                break;
            }
            case BOX('s','t','s','d'): parse_stsd(m->f, body, bend, t); break;
            case BOX('s','t','t','s'): case BOX('s','t','s','c'): case BOX('s','t','s','z'):
            case BOX('s','t','c','o'): case BOX('c','o','6','4'): case BOX('s','t','s','s'):
                parse_stbl_leaf(m->f, type, body, bend, t); break;
            default: break;
        }
        pos = bend;
    }
}

int mp4_open(Mp4 *m, const char *path) {
    memset(m, 0, sizeof *m);
    m->f = fopen(path, "rb");
    if (!m->f) return 0;
    fseeko(m->f, 0, SEEK_END); int64_t sz = ftello(m->f);
    walk(m, NULL, 0, sz);           /* top level: only 'moov' recursion allocates Trak contexts */
    if (!m->has_video) { mp4_close(m); return 0; }
    return 1;
}

void mp4_close(Mp4 *m) {
    if (m->f) fclose(m->f);
    free(m->vsamples); free(m->asamples);
    memset(m, 0, sizeof *m);
}

int mp4_read_sample(Mp4 *m, const Mp4Sample *s, uint8_t *buf) {
    if (fseeko(m->f, s->offset, SEEK_SET)) return 0;
    return (int)fread(buf, 1, s->size, m->f);
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
