/* Eye-independence probe for 3D moflex.
 *
 * 3D moflex is ONE video stream at 2x fps, interleaved L,R,L,R. The player decodes it through a
 * single decoder context. Question: can the two eyes be decoded on separate contexts (=> separate
 * CPU cores) and still produce identical pixels? They can iff neither eye's inter prediction ever
 * references a frame belonging to the other eye.
 *
 * Method: decode the first N video frames two ways and compare per-frame hashes.
 *   A) single context, all frames in decode order  (ground truth == what ships today)
 *   B) two contexts: even frame index -> ctxL, odd -> ctxR
 * If every frame's hash matches between A and B, the eyes are independent -> parallelizable.
 */
#include "moflex_demux.h"
#include "mobicompat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int    mobi_init(AVCodecContext *);
extern int    mobi_decode(AVCodecContext *, AVFrame *, int *, AVPacket *);
extern size_t mobi_ctx_size(void);

static unsigned long long fnv(const AVFrame *f) {
    unsigned long long h = 1469598103934665603ULL;
    for (int p = 0; p < 3; p++) {
        int w = p ? f->width / 2 : f->width, hh = p ? f->height / 2 : f->height;
        for (int y = 0; y < hh; y++) {
            const unsigned char *r = f->data[p] + (long)y * f->linesize[p];
            for (int x = 0; x < w; x++) { h ^= r[x]; h *= 1099511628211ULL; }
        }
    }
    return h;
}

static AVCodecContext *mkctx(int W, int H) {
    AVCodecContext *c = calloc(1, sizeof *c);
    c->width = W; c->height = H; c->priv_data = calloc(1, mobi_ctx_size());
    if (mobi_init(c) != 0) { fprintf(stderr, "mobi_init failed\n"); exit(1); }
    return c;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s in.moflex [nframes]\n", argv[0]); return 1; }
    int want = argc > 2 ? atoi(argv[2]) : 400;

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    MfxDemux m;
    if (mfx_open(&m, f) != 0) { fprintf(stderr, "mfx_open failed\n"); return 1; }
    int vi = -1;
    for (int i = 0; i < m.nb_streams; i++)
        if (m.streams[i].media_type == MFX_TYPE_VIDEO) { vi = i; break; }
    if (vi < 0) { fprintf(stderr, "no video stream\n"); return 1; }
    int W = m.streams[vi].width, H = m.streams[vi].height;

    int stereo = mfx_detect_stereo(&m);
    fprintf(stderr, "video %dx%d  stereo=%d  comparing %d frames\n", W, H, stereo, want);

    /* collect the first `want` video packets (own copies -- demux reuses its buffer) */
    unsigned char **pd = calloc(want, sizeof *pd);
    int *ps = calloc(want, sizeof *ps);
    int np = 0; MfxPacket pkt;
    while (np < want && mfx_next_packet(&m, &pkt) == 1) {
        if (m.streams[pkt.stream_index].media_type != MFX_TYPE_VIDEO) continue;
        pd[np] = malloc(pkt.size); memcpy(pd[np], pkt.data, pkt.size); ps[np] = pkt.size; np++;
    }
    fprintf(stderr, "captured %d video packets\n", np);

    unsigned long long *hA = calloc(np, sizeof *hA);
    unsigned long long *hB = calloc(np, sizeof *hB);
    int gotA = 0, gotB = 0;
    AVFrame *out = av_frame_alloc();

    /* A: single context */
    AVCodecContext *cs = mkctx(W, H);
    for (int i = 0; i < np; i++) {
        AVPacket ap; ap.data = pd[i]; ap.size = ps[i]; int got = 0;
        if (mobi_decode(cs, out, &got, &ap) >= 0 && got) hA[i] = fnv(out), gotA++;
        else hA[i] = 0;
    }

    /* B: two contexts, even->L odd->R */
    AVCodecContext *cl = mkctx(W, H), *cr = mkctx(W, H);
    for (int i = 0; i < np; i++) {
        AVCodecContext *c = (i & 1) ? cr : cl;
        AVPacket ap; ap.data = pd[i]; ap.size = ps[i]; int got = 0;
        if (mobi_decode(c, out, &got, &ap) >= 0 && got) hB[i] = fnv(out), gotB++;
        else hB[i] = 0;
    }

    int mism = 0, first = -1;
    for (int i = 0; i < np; i++) if (hA[i] != hB[i]) { mism++; if (first < 0) first = i; }
    fprintf(stderr, "gotA=%d gotB=%d  mismatches=%d", gotA, gotB, mism);
    if (mism) fprintf(stderr, "  (first at frame %d, eye %c)\n", first, (first & 1) ? 'R' : 'L');
    else      fprintf(stderr, "  -> EYES ARE INDEPENDENT: safe to decode L/R on separate cores\n");
    return mism ? 2 : 0;
}
