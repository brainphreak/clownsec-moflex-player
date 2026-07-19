/* Is the L/R pairing in the encode off-by-one? Decode a motion section and compare mean abs luma
 * diffs:  pair   = |L(k) - R(k)|        (frames 2k, 2k+1: parallax only if paired right)
 *         prevR  = |R(k-1) - L(k)|      (frames 2k-1, 2k: parallax only if L lags one frame)
 * Whichever is consistently smaller is the true pairing. */
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

    double frac = argc > 2 ? atof(argv[2]) : 0.3;
    mfx_seek_time(&m, (int64_t)(m.duration_us * frac));
    mobi_flush(&ctx);

    uint8_t *prev2 = malloc(W * H), *prev1 = malloc(W * H), *cur = malloc(W * H);
    long n = 0, used = 0;
    double sum_pair = 0, sum_prevR = 0, sum_nextpair = 0; long cnt = 0;
    MfxPacket pkt;
    while (used < 1200 && mfx_next_packet(&m, &pkt) == 1) {
        if (m.streams[pkt.stream_index].media_type != MFX_TYPE_VIDEO) continue;
        AVPacket ap; ap.data = pkt.data; ap.size = pkt.size; int got = 0;
        if (!(mobi_decode(&ctx, fr, &got, &ap) >= 0 && got)) { n++; continue; }
        for (int y = 0; y < H; y++) memcpy(cur + y * W, fr->data[0] + y * fr->linesize[0], W);
        if (n >= 120 && (n & 1) == 0 && n >= 2) {   /* frame 2k with 2k-1 and (after next) 2k+1 */
            /* skip the first 120 frames after the seek (reference artifacts) */
            /* compute when we ALSO have 2k+1: defer -- handle below on odd frames instead */
        }
        if (n >= 120 && (n & 1) == 1) {   /* odd frame R(k)=2k+1: prev1 = L(k)=2k, prev2 = R(k-1)=2k-1 */
            double pair  = madiff(prev1, cur);    /* L(k)  vs R(k)   */
            double prevR = madiff(prev2, prev1);  /* R(k-1) vs L(k)  */
            sum_pair += pair; sum_prevR += prevR; cnt++;
        }
        uint8_t *t = prev2; prev2 = prev1; prev1 = cur; cur = t;
        n++; used++;
    }
    printf("%ld sample pairs from %.0f%% in\n", cnt, frac * 100);
    printf("  mean |L(k)-R(k)|   (current pairing)   = %8.3f\n", sum_pair / cnt);
    printf("  mean |R(k-1)-L(k)| (L lags one frame)  = %8.3f\n", sum_prevR / cnt);
    printf(sum_prevR < sum_pair * 0.9 ? "=> L MATCHES THE PREVIOUS R: encode paired off-by-one\n"
         : sum_pair < sum_prevR * 0.9 ? "=> current pairing is correct (no off-by-one)\n"
         : "=> inconclusive (not enough motion; try another position)\n");
    return 0;
}
