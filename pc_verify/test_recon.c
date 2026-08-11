/* Bit-exact reference for the dual-core reconstruction refactor.
 * Decodes the first N video frames of a moflex file and prints one FNV-1a hash per decoded frame.
 * Capture this from the ORIGINAL decoder (baseline), then after every refactor step re-run and `diff`
 * -- the parse/reconstruct split (and later the threading) must reproduce these hashes EXACTLY. */
#include "moflex_demux.h"
#include "mobicompat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int    mobi_init(AVCodecContext *);
extern int    mobi_decode(AVCodecContext *, AVFrame *, int *, AVPacket *);
extern size_t mobi_ctx_size(void);
extern int    mobi_opt;

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

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s in.moflex [nframes] [opt]\n", argv[0]); return 1; }
    int want = argc > 2 ? atoi(argv[2]) : 300;
    if (argc > 3) mobi_opt = (int)strtol(argv[3], NULL, 0);

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    MfxDemux m;
    if (mfx_open(&m, f) != 0) { fprintf(stderr, "mfx_open failed\n"); return 1; }
    int vi = -1;
    for (int i = 0; i < m.nb_streams; i++)
        if (m.streams[i].media_type == MFX_TYPE_VIDEO) { vi = i; break; }
    if (vi < 0) { fprintf(stderr, "no video\n"); return 1; }

    AVCodecContext ctx; memset(&ctx, 0, sizeof ctx);
    ctx.width = m.streams[vi].width; ctx.height = m.streams[vi].height;
    ctx.priv_data = calloc(1, mobi_ctx_size());
    if (mobi_init(&ctx) != 0) { fprintf(stderr, "mobi_init failed\n"); return 1; }
    fprintf(stderr, "%dx%d  opt=0x%x  %d frames\n", ctx.width, ctx.height, mobi_opt, want);

    AVFrame *out = av_frame_alloc();
    MfxPacket pkt; int n = 0;
    while (n < want && mfx_next_packet(&m, &pkt) == 1) {
        if (m.streams[pkt.stream_index].media_type != MFX_TYPE_VIDEO) continue;
        AVPacket ap; ap.data = pkt.data; ap.size = pkt.size; int got = 0;
        if (mobi_decode(&ctx, out, &got, &ap) >= 0 && got) { printf("%016llx\n", fnv(out)); n++; }
    }
    fprintf(stderr, "hashed %d frames\n", n);
    return 0;
}
