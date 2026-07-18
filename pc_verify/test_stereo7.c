/* Validate decode-based eye detection: at each seek landing, flush the decoder (device-like),
 * decode the landing keyframe A and next frame B, and measure how well B "fits" A (mean abs
 * luma diff). Ground truth from packet fingerprints. If L-first and R-first landings separate,
 * the player can auto-detect the eye after every seek. */
#include "moflex_demux.h"
#include "mobicompat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

extern int    mobi_init(AVCodecContext *);
extern int    mobi_decode(AVCodecContext *, AVFrame *, int *, AVPacket *);
extern void   mobi_flush(AVCodecContext *);
extern size_t mobi_ctx_size(void);

#define MAXP 200000
static uint64_t fp[MAXP]; static int fpn;
static uint64_t fnv(const uint8_t *d, int n, int size) {
    uint64_t h = 1469598103934665603ull ^ (uint64_t)size;
    for (int i = 0; i < n; i++) { h ^= d[i]; h *= 1099511628211ull; }
    return h;
}
static double madiff(const AVFrame *a, const AVFrame *b, int W, int H) {
    double s = 0;
    for (int y = 0; y < H; y++) {
        const uint8_t *pa = a->data[0] + y * a->linesize[0], *pb = b->data[0] + y * b->linesize[0];
        for (int x = 0; x < W; x++) s += pa[x] > pb[x] ? pa[x] - pb[x] : pb[x] - pa[x];
    }
    return s / (W * H);
}

int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    MfxDemux m;
    if (!f || mfx_open(&m, f) != 0) { fprintf(stderr, "open failed\n"); return 1; }
    int vi = -1;
    for (int i = 0; i < m.nb_streams; i++)
        if (m.streams[i].media_type == MFX_TYPE_VIDEO) { vi = i; break; }
    int W = m.streams[vi].width, H = m.streams[vi].height;

    MfxPacket pkt;
    while (fpn < MAXP && mfx_next_packet(&m, &pkt) == 1) {
        if (m.streams[pkt.stream_index].media_type != MFX_TYPE_VIDEO) continue;
        int n = pkt.size < 64 ? pkt.size : 64;
        fp[fpn++] = fnv(pkt.data, n, pkt.size);
    }

    AVCodecContext ctx; memset(&ctx, 0, sizeof ctx);
    ctx.width = W; ctx.height = H; ctx.priv_data = calloc(1, mobi_ctx_size());
    mobi_init(&ctx);
    AVFrame *fA = av_frame_alloc(), *fB = av_frame_alloc();

    int64_t seen[128]; int nseen = 0;
    printf("landing_ts  truth   madiff(A,B)\n");
    for (int pc = 1; pc <= 99; pc += 2) {
        if (mfx_seek_time(&m, m.duration_us * pc / 100) != 0) continue;
        int64_t lts = m.ts; int dup = 0;
        for (int i = 0; i < nseen; i++) if (seen[i] == lts) dup = 1;
        if (dup || nseen >= 128) continue;
        seen[nseen++] = lts;

        /* device-like: flush, then decode the first two video packets at the landing */
        mobi_flush(&ctx);
        uint64_t h0 = 0; int got = 0, okA = 0, okB = 0;
        while (got < 2) {
            if (mfx_next_packet(&m, &pkt) != 1) break;
            if (m.streams[pkt.stream_index].media_type != MFX_TYPE_VIDEO) continue;
            AVPacket ap; ap.data = pkt.data; ap.size = pkt.size; int g = 0;
            if (got == 0) { h0 = fnv(pkt.data, pkt.size < 64 ? pkt.size : 64, pkt.size);
                            okA = mobi_decode(&ctx, fA, &g, &ap) >= 0 && g; }
            else okB = mobi_decode(&ctx, fB, &g, &ap) >= 0 && g;
            got++;
        }
        int gi = -1;
        for (int i = 0; i < fpn; i++) if (fp[i] == h0) { gi = i; break; }
        if (!okA || !okB || gi < 0) { printf("%9.2fs  ?      decode/match failed\n", lts / 1e6); continue; }
        printf("%9.2fs  %s  %8.2f\n", lts / 1e6, (gi & 1) ? "R-1st" : "L-1st", madiff(fA, fB, W, H));
    }
    return 0;
}
