/* Per-sample pairing audit: for each L(2k) in motion, is its own R(2k+1) really the best match?
 * Counts wins/losses and reports the worst mismatch moments (timestamps) for visual inspection. */
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
    if (!f || mfx_open(&m, f) != 0) return 1;
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
    long n = 0, used = 0, cnt = 0, own_wins = 0, prev_wins = 0, next_wins = 0;
    double worst = 0; long worst_n = -1;
    MfxPacket pkt;
    while (used < 12000 && mfx_next_packet(&m, &pkt) == 1) {
        if (m.streams[pkt.stream_index].media_type != MFX_TYPE_VIDEO) continue;
        AVPacket ap; ap.data = pkt.data; ap.size = pkt.size; int got = 0;
        if (!(mobi_decode(&ctx, fr, &got, &ap) >= 0 && got)) { n++; used++; continue; }
        for (int y = 0; y < H; y++) memcpy(ring[n % RING] + y * W, fr->data[0] + y * fr->linesize[0], W);
        long c = n - 3;
        if (n >= 200 && c >= 3 && (c & 1) == 0) {
            double mo = madiff(ring[(c - 2) % RING], ring[c % RING]);
            if (mo > 2.0) {
                double own  = madiff(ring[c % RING], ring[(c + 1) % RING]);
                double prev = madiff(ring[c % RING], ring[(c - 1) % RING]);
                double next = madiff(ring[c % RING], ring[(c + 3) % RING]);
                cnt++;
                if (own <= prev && own <= next) own_wins++;
                else if (prev < own && prev <= next) { prev_wins++;
                    if (own - prev > worst) { worst = own - prev; worst_n = c; } }
                else next_wins++;
            }
        }
        n++; used++;
    }
    printf("%ld motion samples: own-pair best %ld (%.1f%%), prev-R best %ld, next-R best %ld\n",
           cnt, own_wins, 100.0 * own_wins / (cnt ? cnt : 1), prev_wins, next_wins);
    if (worst_n >= 0) printf("worst prev-R win at eye-frame %ld (~%.1fs into the sampled region)\n",
                             worst_n, worst_n / 2 * 0.0834);
    return 0;
}
