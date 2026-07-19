/* Which temporal offset best matches L to R? Tests L(2k) against frames 2k-3..2k+3 (odd = R
 * frames at various temporal offsets). The winner reveals the encode's true eye alignment. */
#include "moflex_demux.h"
#include "mobicompat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int    mobi_init(AVCodecContext *);
extern int    mobi_decode(AVCodecContext *, AVFrame *, int *, AVPacket *);
extern void   mobi_flush(AVCodecContext *);
extern size_t mobi_ctx_size(void);

static int W, H;
static double madiff(const uint8_t *a, const uint8_t *b) {
    double s = 0;
    for (int i = 0; i < W * H; i++) s += a[i] > b[i] ? a[i] - b[i] : b[i] - a[i];
    return s / (W * H);
}

#define RING 9
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    MfxDemux m;
    if (!f || mfx_open(&m, f) != 0) { fprintf(stderr, "open failed\n"); return 1; }
    int vi = -1;
    for (int i = 0; i < m.nb_streams; i++)
        if (m.streams[i].media_type == MFX_TYPE_VIDEO) { vi = i; break; }
    W = m.streams[vi].width; H = m.streams[vi].height;
    AVCodecContext ctx; memset(&ctx, 0, sizeof ctx);
    ctx.width = W; ctx.height = H; ctx.priv_data = calloc(1, mobi_ctx_size());
    mobi_init(&ctx);
    AVFrame *fr = av_frame_alloc();
    double frac = argc > 2 ? atof(argv[2]) : 0.0;
    if (frac > 0) { mfx_seek_time(&m, (int64_t)(m.duration_us * frac)); mobi_flush(&ctx); }

    uint8_t *ring[RING]; for (int i = 0; i < RING; i++) ring[i] = malloc(W * H);
    long n = 0, used = 0;
    double sum[7] = {0}; long cnt = 0;   /* offsets -3..+3 relative: L(2k) vs frame 2k+off (odd offs = R frames) */
    MfxPacket pkt;
    while (used < 3000 && mfx_next_packet(&m, &pkt) == 1) {
        if (m.streams[pkt.stream_index].media_type != MFX_TYPE_VIDEO) continue;
        AVPacket ap; ap.data = pkt.data; ap.size = pkt.size; int got = 0;
        if (!(mobi_decode(&ctx, fr, &got, &ap) >= 0 && got)) { n++; used++; continue; }
        for (int y = 0; y < H; y++) memcpy(ring[n % RING] + y * W, fr->data[0] + y * fr->linesize[0], W);
        /* when frame n arrives, evaluate the L at center c = n-3 if c is even and window full */
        long c = n - 3;
        if (n >= 200 && c >= 3 && (c & 1) == 0) {
            /* motion gate: only score windows with real motion (L(c) vs L(c-2)) */
            double mo = madiff(ring[(c - 2) % RING], ring[c % RING]);
            if (mo > 2.0) {
                for (int off = -3; off <= 3; off += 2)   /* odd offsets = R frames */
                    sum[off + 3] += madiff(ring[c % RING], ring[(c + off) % RING]);
                cnt++;
            }
        }
        n++; used++;
    }
    printf("%ld motion samples (%s at %.0f%%)\n", cnt, argv[1], frac * 100);
    for (int off = -3; off <= 3; off += 2)
        printf("  L(2k) vs frame 2k%+d (R at pair %+d): %8.3f\n", off, (off - 1) / 2, sum[off + 3] / (cnt ? cnt : 1));
    return 0;
}
