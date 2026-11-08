/* MPEG-TS demuxer for live H.264 + AAC streams -- see ts_demux.h. */
#include "ts_demux.h"
#include <stdlib.h>
#include <string.h>

typedef struct { int kind, key; int64_t pts, dts; uint8_t *data; size_t len; } TsQ;

struct TsDemux {
    uint8_t  pkt[188]; int pkt_n;           /* a packet split across two pushes */
    int      pmt_pid, vpid, apid;
    /* PES being assembled, per stream */
    uint8_t *vbuf; size_t vlen, vcap; int64_t vpts, vdts; int vopen;
    uint8_t *abuf; size_t alen, acap; int64_t apts; int aopen;
    /* completed units, a growable ring */
    TsQ     *q; int qcap, qh, qn; size_t qbytes;
    uint8_t *popped;                         /* last unit handed out (freed on the next pop) */
    /* timestamps: 33-bit 90 kHz, unwrapped, relative to the first one seen */
    int64_t  base, last;
    /* parameter sets + derived config */
    uint8_t  sps[256], pps[128]; int sps_len, pps_len;
    uint8_t  avcc[512]; int avcc_len;
    int      w, h;
    uint8_t  asc[2]; int arate, ach, have_asc;
};

TsDemux *ts_new(void) {
    TsDemux *d = (TsDemux *)calloc(1, sizeof *d);
    if (!d) return NULL;
    d->qcap = 64; d->q = (TsQ *)calloc((size_t)d->qcap, sizeof(TsQ));
    if (!d->q) { free(d); return NULL; }
    ts_reset(d);
    return d;
}
static void q_clear(TsDemux *d) {
    for (int i = 0; i < d->qn; i++) free(d->q[(d->qh + i) % d->qcap].data);
    d->qh = d->qn = 0; d->qbytes = 0;
}
void ts_reset(TsDemux *d) {
    q_clear(d);
    free(d->popped); d->popped = NULL;
    d->pkt_n = 0; d->pmt_pid = -1; d->vpid = -1; d->apid = -1;
    d->vlen = 0; d->vopen = 0; d->alen = 0; d->aopen = 0;
    d->base = -1; d->last = 0;
    /* parameter sets survive a reset: a seek restarts the same stream */
}
void ts_free(TsDemux *d) {
    if (!d) return;
    q_clear(d); free(d->q); free(d->popped); free(d->vbuf); free(d->abuf); free(d);
}

static int q_add(TsDemux *d, int kind, int64_t pts, int64_t dts, int key, uint8_t *data, size_t len) {
    if (d->qn == d->qcap) {                  /* grow, keeping order */
        int nc = d->qcap * 2; TsQ *nq = (TsQ *)calloc((size_t)nc, sizeof(TsQ));
        if (!nq) { free(data); return -1; }
        for (int i = 0; i < d->qn; i++) nq[i] = d->q[(d->qh + i) % d->qcap];
        free(d->q); d->q = nq; d->qcap = nc; d->qh = 0;
    }
    TsQ *e = &d->q[(d->qh + d->qn) % d->qcap];
    e->kind = kind; e->pts = pts; e->dts = dts; e->key = key; e->data = data; e->len = len;
    d->qn++; d->qbytes += len;
    return 0;
}

/* 90 kHz -> microseconds since the first timestamp, across the 33-bit wrap */
static int64_t ts_time(TsDemux *d, int64_t t90) {
    if (d->base < 0) { d->base = t90; d->last = t90; }
    else {
        int64_t wrap = (int64_t)1 << 33;
        while (t90 - d->last < -(wrap / 2)) t90 += wrap;
        while (t90 - d->last >  (wrap / 2)) t90 -= wrap;
        d->last = t90;
    }
    return (t90 - d->base) * 1000 / 90;
}

/* ---- SPS: display size (exp-Golomb over the RBSP) ---- */
typedef struct { const uint8_t *p; int n, bit; } Bits;
static int b_u1(Bits *b) {
    if (b->bit >= b->n * 8) return 0;
    int v = (b->p[b->bit >> 3] >> (7 - (b->bit & 7))) & 1; b->bit++; return v;
}
static unsigned b_u(Bits *b, int k) { unsigned v = 0; while (k--) v = (v << 1) | (unsigned)b_u1(b); return v; }
static unsigned b_ue(Bits *b) {
    int z = 0; while (!b_u1(b) && z < 32) z++;
    return ((1u << z) - 1) + b_u(b, z);
}
static int b_se(Bits *b) { unsigned v = b_ue(b); return (v & 1) ? (int)((v + 1) / 2) : -(int)(v / 2); }
static void sps_dims(TsDemux *d) {
    uint8_t r[256]; int n = 0;
    for (int i = 1; i < d->sps_len && n < (int)sizeof r; i++) {   /* skip the NAL header; drop 03 */
        if (i >= 3 && d->sps[i] == 3 && d->sps[i - 1] == 0 && d->sps[i - 2] == 0) continue;
        r[n++] = d->sps[i];
    }
    Bits b = { r, n, 0 };
    int prof = (int)b_u(&b, 8); b_u(&b, 16); b_ue(&b);
    int chroma = 1;
    if (prof == 100 || prof == 110 || prof == 122 || prof == 244 || prof == 44 || prof == 83 ||
        prof == 86 || prof == 118 || prof == 128 || prof == 138 || prof == 139 || prof == 134) {
        chroma = (int)b_ue(&b);
        if (chroma == 3) b_u1(&b);
        b_ue(&b); b_ue(&b); b_u1(&b);
        if (b_u1(&b)) {                       /* scaling matrices: walk past them */
            for (int i = 0; i < (chroma != 3 ? 8 : 12); i++) if (b_u1(&b)) {
                int sz = i < 6 ? 16 : 64, last = 8, next = 8;
                for (int j = 0; j < sz; j++) {
                    if (next) next = (last + b_se(&b) + 256) % 256;
                    if (next) last = next;
                }
            }
        }
    }
    b_ue(&b);                                 /* log2_max_frame_num */
    unsigned poc = b_ue(&b);
    if (poc == 0) b_ue(&b);
    else if (poc == 1) { b_u1(&b); b_se(&b); b_se(&b); unsigned k = b_ue(&b); while (k--) b_se(&b); }
    b_ue(&b); b_u1(&b);
    unsigned wm = b_ue(&b), hm = b_ue(&b);
    int fmo = b_u1(&b);
    if (!fmo) b_u1(&b);
    b_u1(&b);
    int cl = 0, cr = 0, ct = 0, cb = 0;
    if (b_u1(&b)) { cl = (int)b_ue(&b); cr = (int)b_ue(&b); ct = (int)b_ue(&b); cb = (int)b_ue(&b); }
    int cux = (chroma == 0 || chroma == 3) ? 1 : 2;
    int cuy = ((chroma == 1) ? 2 : 1) * (2 - fmo);
    d->w = (int)(wm + 1) * 16 - (cl + cr) * cux;
    d->h = (2 - fmo) * (int)(hm + 1) * 16 - (ct + cb) * cuy;
}
static void build_avcc(TsDemux *d) {
    if (d->sps_len < 4 || !d->pps_len) return;
    uint8_t *a = d->avcc; int n = 0;
    a[n++] = 1; a[n++] = d->sps[1]; a[n++] = d->sps[2]; a[n++] = d->sps[3];
    a[n++] = 0xFF;                            /* 4-byte NAL lengths */
    a[n++] = 0xE1; a[n++] = (uint8_t)(d->sps_len >> 8); a[n++] = (uint8_t)d->sps_len;
    memcpy(a + n, d->sps, (size_t)d->sps_len); n += d->sps_len;
    a[n++] = 1; a[n++] = (uint8_t)(d->pps_len >> 8); a[n++] = (uint8_t)d->pps_len;
    memcpy(a + n, d->pps, (size_t)d->pps_len); n += d->pps_len;
    d->avcc_len = n;
}

/* A finished video PES: Annex-B -> 4-byte length prefixes, IDR flag, parameter sets kept. */
static void emit_video(TsDemux *d) {
    if (!d->vopen || !d->vlen) { d->vopen = 0; d->vlen = 0; return; }
    const uint8_t *s = d->vbuf; size_t n = d->vlen;
    uint8_t *out = (uint8_t *)malloc(n + 64);
    if (!out) { d->vopen = 0; d->vlen = 0; return; }
    size_t o = 0; int key = 0;
    size_t i = 0;
    while (i + 3 <= n && !(s[i] == 0 && s[i + 1] == 0 && (s[i + 2] == 1 || (i + 4 <= n && s[i + 2] == 0 && s[i + 3] == 1)))) i++;
    while (i < n) {
        i += (s[i + 2] == 1) ? 3 : 4;         /* past the start code */
        size_t st = i;
        while (i + 3 <= n && !(s[i] == 0 && s[i + 1] == 0 && (s[i + 2] == 1 || (i + 4 <= n && s[i + 2] == 0 && s[i + 3] == 1)))) i++;
        size_t en = (i + 3 <= n) ? i : n;
        while (en > st && s[en - 1] == 0) en--; /* trailing zero bytes belong to the next start code */
        if (en <= st) { if (i + 3 > n) break; continue; }
        int t = s[st] & 0x1F;
        size_t L = en - st;
        if (t == 7 && L <= sizeof d->sps) { if (L != (size_t)d->sps_len || memcmp(d->sps, s + st, L)) { memcpy(d->sps, s + st, L); d->sps_len = (int)L; sps_dims(d); build_avcc(d); } }
        if (t == 8 && L <= sizeof d->pps) { if (L != (size_t)d->pps_len || memcmp(d->pps, s + st, L)) { memcpy(d->pps, s + st, L); d->pps_len = (int)L; build_avcc(d); } }
        if (t == 5) key = 1;
        if (t != 9) {                         /* access unit delimiters carry nothing */
            if (o + 4 + L > n + 64) { uint8_t *g = (uint8_t *)realloc(out, o + 4 + L + 64); if (!g) break; out = g; }
            out[o++] = (uint8_t)(L >> 24); out[o++] = (uint8_t)(L >> 16); out[o++] = (uint8_t)(L >> 8); out[o++] = (uint8_t)L;
            memcpy(out + o, s + st, L); o += L;
        }
        if (i + 3 > n) break;
    }
    if (o) q_add(d, TS_AU_VIDEO, d->vpts, d->vdts, key, out, o); else free(out);
    d->vopen = 0; d->vlen = 0;
}

/* A finished audio PES: split its ADTS frames, one unit each, timed from the PES timestamp. */
static void emit_audio(TsDemux *d) {
    const uint8_t *s = d->abuf; size_t n = d->alen, i = 0; int k = 0;
    while (i + 7 <= n) {
        if (s[i] != 0xFF || (s[i + 1] & 0xF6) != 0xF0) { i++; continue; }   /* ADTS sync */
        int prot_absent = s[i + 1] & 1;
        int aot = ((s[i + 2] >> 6) & 3) + 1, sri = (s[i + 2] >> 2) & 15;
        int ch = ((s[i + 2] & 1) << 2) | ((s[i + 3] >> 6) & 3);
        size_t flen = ((size_t)(s[i + 3] & 3) << 11) | ((size_t)s[i + 4] << 3) | (s[i + 5] >> 5);
        size_t hl = prot_absent ? 7 : 9;
        if (flen < hl || i + flen > n) break;
        static const int RATES[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };
        if (sri > 12) { i += flen; continue; }
        if (!d->have_asc) {
            d->asc[0] = (uint8_t)((aot << 3) | (sri >> 1));
            d->asc[1] = (uint8_t)(((sri & 1) << 7) | (ch << 3));
            d->arate = RATES[sri]; d->ach = ch; d->have_asc = 1;
        }
        size_t pl = flen - hl;
        uint8_t *fr = (uint8_t *)malloc(pl ? pl : 1);
        if (fr) {
            memcpy(fr, s + i + hl, pl);
            int64_t t = d->apts + (int64_t)k * 1024 * 1000000 / RATES[sri];
            q_add(d, TS_AU_AUDIO, t, t, 1, fr, pl);
        }
        k++; i += flen;
    }
    d->aopen = 0; d->alen = 0;
}

static int grow(uint8_t **b, size_t *cap, size_t need) {
    if (need <= *cap) return 1;
    size_t nc = *cap ? *cap : 65536; while (nc < need) nc *= 2;
    uint8_t *g = (uint8_t *)realloc(*b, nc); if (!g) return 0;
    *b = g; *cap = nc; return 1;
}

static void on_pes_payload(TsDemux *d, int video, int pusi, const uint8_t *p, size_t n) {
    if (pusi) {
        if (video) emit_video(d); else if (d->aopen) emit_audio(d);
        if (n < 9 || p[0] || p[1] || p[2] != 1) return;   /* not a PES header */
        int flags = p[7], hl = p[8];
        if ((size_t)(9 + hl) > n) return;
        int64_t pts = -1, dts = -1;
        if (flags & 0x80) pts = ((int64_t)(p[9] & 0x0E) << 29) | ((int64_t)p[10] << 22) | ((int64_t)(p[11] & 0xFE) << 14) | ((int64_t)p[12] << 7) | (p[13] >> 1);
        if (flags & 0x40) dts = ((int64_t)(p[14] & 0x0E) << 29) | ((int64_t)p[15] << 22) | ((int64_t)(p[16] & 0xFE) << 14) | ((int64_t)p[17] << 7) | (p[18] >> 1);
        p += 9 + hl; n -= 9 + hl;
        int64_t pu = pts >= 0 ? ts_time(d, pts) : 0;
        int64_t du = dts >= 0 ? ts_time(d, dts) : pu;
        if (video) { d->vopen = 1; d->vlen = 0; d->vpts = pu; d->vdts = du; }
        else       { d->aopen = 1; d->alen = 0; d->apts = pu; }
    }
    if (video) { if (!d->vopen || !grow(&d->vbuf, &d->vcap, d->vlen + n)) return; memcpy(d->vbuf + d->vlen, p, n); d->vlen += n; }
    else       { if (!d->aopen || !grow(&d->abuf, &d->acap, d->alen + n)) return; memcpy(d->abuf + d->alen, p, n); d->alen += n; }
}

static void on_packet(TsDemux *d, const uint8_t *k) {
    int pid = ((k[1] & 0x1F) << 8) | k[2], pusi = (k[1] & 0x40) != 0, afc = (k[3] >> 4) & 3;
    if (!(afc & 1)) return;                   /* no payload */
    int off = 4 + ((afc & 2) ? 1 + k[4] : 0);
    if (off >= 188) return;
    const uint8_t *p = k + off; size_t n = (size_t)(188 - off);
    if (pid == 0 || pid == d->pmt_pid) {      /* PAT / PMT (one section, fits one packet here) */
        if (!pusi || n < 1) return;
        size_t ptr = p[0]; p += 1 + ptr; if (n < 1 + ptr + 12) return; n -= 1 + ptr;
        int slen = ((p[1] & 0x0F) << 8) | p[2];
        if ((size_t)(3 + slen) > n || slen < 9) return;
        if (pid == 0 && p[0] == 0x00) {
            for (int i = 8; i + 4 <= 3 + slen - 4; i += 4) {
                int prog = (p[i] << 8) | p[i + 1], ppid = ((p[i + 2] & 0x1F) << 8) | p[i + 3];
                if (prog) { d->pmt_pid = ppid; break; }
            }
        } else if (p[0] == 0x02) {
            int pil = ((p[10] & 0x0F) << 8) | p[11];
            for (int i = 12 + pil; i + 5 <= 3 + slen - 4; ) {
                int st = p[i], epid = ((p[i + 1] & 0x1F) << 8) | p[i + 2], esl = ((p[i + 3] & 0x0F) << 8) | p[i + 4];
                if (st == 0x1B && d->vpid < 0) d->vpid = epid;          /* H.264 */
                if (st == 0x0F && d->apid < 0) d->apid = epid;          /* AAC in ADTS */
                i += 5 + esl;
            }
        }
        return;
    }
    if (pid == d->vpid) on_pes_payload(d, 1, pusi, p, n);
    else if (pid == d->apid) on_pes_payload(d, 0, pusi, p, n);
}

int ts_push(TsDemux *d, const uint8_t *p, size_t n) {
    while (n) {
        if (d->pkt_n) {                       /* finish a packet split across pushes */
            size_t t = 188 - (size_t)d->pkt_n; if (t > n) t = n;
            memcpy(d->pkt + d->pkt_n, p, t); d->pkt_n += (int)t; p += t; n -= t;
            if (d->pkt_n == 188) { if (d->pkt[0] == 0x47) on_packet(d, d->pkt); d->pkt_n = 0; }
            continue;
        }
        if (p[0] != 0x47) { p++; n--; continue; }   /* resync */
        if (n < 188) { memcpy(d->pkt, p, n); d->pkt_n = (int)n; break; }
        on_packet(d, p); p += 188; n -= 188;
    }
    return 0;
}

void ts_flush(TsDemux *d) { emit_video(d); if (d->aopen) emit_audio(d); }

int ts_pop(TsDemux *d, TsAU *au) {
    free(d->popped); d->popped = NULL;
    if (!d->qn) return 0;
    TsQ *e = &d->q[d->qh];
    d->qh = (d->qh + 1) % d->qcap; d->qn--; d->qbytes -= e->len;
    d->popped = e->data;
    au->kind = e->kind; au->pts_us = e->pts; au->dts_us = e->dts; au->key = e->key;
    au->data = e->data; au->len = e->len;
    return 1;
}
int ts_queued(const TsDemux *d, size_t *bytes) { if (bytes) *bytes = d->qbytes; return d->qn; }
int ts_video_avcc(const TsDemux *d, const uint8_t **avcc, int *len) {
    if (!d->avcc_len) return 0; *avcc = d->avcc; *len = d->avcc_len; return 1;
}
int ts_video_dims(const TsDemux *d, int *w, int *h) { if (!d->w) return 0; *w = d->w; *h = d->h; return 1; }
int ts_audio_config(const TsDemux *d, uint8_t asc[2], int *rate, int *channels) {
    if (!d->have_asc) return 0;
    asc[0] = d->asc[0]; asc[1] = d->asc[1]; *rate = d->arate; *channels = d->ach; return 1;
}
