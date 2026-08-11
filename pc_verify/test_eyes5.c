/* Is it truly stereo? A real L/R pair differs by a HORIZONTAL shift (parallax): shifting L a few
 * px should collapse the difference. A temporal neighbour (2D motion / animation) won't collapse.
 * Reports, for pairs (2k,2k+1): raw diff, best diff over x-shifts -8..8, and identical-frame %. */
#include "moflex_demux.h"
#include "mobicompat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern int    mobi_init(AVCodecContext *);
extern int    mobi_decode(AVCodecContext *, AVFrame *, int *, AVPacket *);
extern size_t mobi_ctx_size(void);
static int W, H;
static double madiff_sh(const uint8_t *a, const uint8_t *b, int sh) {
    double s = 0; long n = 0;
    for (int y = 0; y < H; y++)
        for (int x = 8; x < W - 8; x++) { int d = a[y*W + x + sh] - b[y*W + x]; s += d < 0 ? -d : d; n++; }
    return s / n;
}
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    MfxDemux m;
    if (!f || mfx_open(&m, f) != 0) return 1;
    int vi = -1;
    for (int i = 0; i < m.nb_streams; i++)
        if (m.streams[i].media_type == MFX_TYPE_VIDEO) { vi = i; break; }
    W = m.streams[vi].width; H = m.streams[vi].height;
    AVCodecContext ctx; memset(&ctx, 0, sizeof ctx);
    ctx.width = W; ctx.height = H; ctx.priv_data = calloc(1, mobi_ctx_size());
    mobi_init(&ctx);
    AVFrame *fr = av_frame_alloc();
    uint8_t *A = malloc(W * H), *B = malloc(W * H);
    long n = 0, cnt = 0, ident = 0; double sraw = 0, sbest = 0; long shift_hist[17] = {0};
    MfxPacket pkt;
    while (n < 4000 && mfx_next_packet(&m, &pkt) == 1) {
        if (m.streams[pkt.stream_index].media_type != MFX_TYPE_VIDEO) continue;
        AVPacket ap; ap.data = pkt.data; ap.size = pkt.size; int got = 0;
        if (!(mobi_decode(&ctx, fr, &got, &ap) >= 0 && got)) { n++; continue; }
        uint8_t *dst = (n & 1) ? B : A;
        for (int y = 0; y < H; y++) memcpy(dst + y * W, fr->data[0] + y * fr->linesize[0], W);
        if ((n & 1) && n >= 200) {
            double raw = madiff_sh(A, B, 0);
            if (raw < 0.15) ident++;
            else {
                double best = raw; int bs = 0;
                for (int sh = -8; sh <= 8; sh++) { double d = madiff_sh(A, B, sh); if (d < best) { best = d; bs = sh; } }
                sraw += raw; sbest += best; shift_hist[bs + 8]++; cnt++;
            }
        }
        n++;
    }
    printf("%s\n  pairs: %ld differing, %ld identical (%.1f%% identical)\n",
           argv[1], cnt, ident, 100.0 * ident / (cnt + ident ? cnt + ident : 1));
    printf("  mean raw diff %.3f -> best-shift diff %.3f (collapse ratio %.2f)\n",
           sraw / cnt, sbest / cnt, (sraw / cnt) / (sbest / cnt));
    printf("  best-shift histogram (-8..+8): ");
    for (int i = 0; i < 17; i++) printf("%ld ", shift_hist[i]);
    printf("\n");
    return 0;
}
