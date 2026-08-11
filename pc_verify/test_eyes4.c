/* Compare pairing rules on real content: GLOBAL (continuous L,R from start) vs BLOCK-LOCAL
 * (phase resets at every ts-block boundary). For each rule, in motion samples, count how often
 * the rule's L matches its assigned R better than the neighbouring R frames. */
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
    uint8_t *ring[RING]; for (int i = 0; i < RING; i++) ring[i] = malloc(W * H);
    int  bl[RING];   /* block-local index of each ring frame */
    long n = 0, used = 0;
    long g_cnt = 0, g_own = 0, b_cnt = 0, b_own = 0;
    int64_t bt = -1; int blidx = 0;
    MfxPacket pkt;
    long cap = argc > 2 ? atol(argv[2]) : 60000;
    while (used < cap && mfx_next_packet(&m, &pkt) == 1) {
        if (m.streams[pkt.stream_index].media_type != MFX_TYPE_VIDEO) continue;
        if (m.ts != bt) { bt = m.ts; blidx = 0; }
        AVPacket ap; ap.data = pkt.data; ap.size = pkt.size; int got = 0;
        int ok = mobi_decode(&ctx, fr, &got, &ap) >= 0 && got;
        if (ok) {
            for (int y = 0; y < H; y++) memcpy(ring[n % RING] + y * W, fr->data[0] + y * fr->linesize[0], W);
            bl[n % RING] = blidx;
        }
        long c = n - 3;
        if (ok && n >= 60 && c >= 3) {
            double mo = madiff(ring[(c - 2) % RING], ring[c % RING]);
            if (mo > 2.0) {
                double own  = madiff(ring[c % RING], ring[(c + 1) % RING]);
                double prev = madiff(ring[c % RING], ring[(c - 1) % RING]);
                double next = madiff(ring[c % RING], ring[(c + 3) % RING]);
                int best_own = own <= prev && own <= next;
                if ((c & 1) == 0)            { g_cnt++; if (best_own) g_own++; }   /* global rule L */
                if ((bl[c % RING] & 1) == 0) { b_cnt++; if (best_own) b_own++; }   /* block-local L */
            }
        }
        blidx++; n++; used++;
    }
    printf("%s\n", argv[1]);
    printf("  GLOBAL pairing:      L-samples %6ld, own-R best %6ld (%.1f%%)\n", g_cnt, g_own, 100.0 * g_own / (g_cnt ? g_cnt : 1));
    printf("  BLOCK-LOCAL pairing: L-samples %6ld, own-R best %6ld (%.1f%%)\n", b_cnt, b_own, 100.0 * b_own / (b_cnt ? b_cnt : 1));
    return 0;
}
