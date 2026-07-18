/* Validate parity-by-block-arithmetic: at a landing, walk forward block by block. Each block i has
 * packet count C[i] and ts span F[i] (in eye frames). delta[i] (0 = starts on L, 1 = starts on R)
 * obeys delta[i+1] = delta[i] + (C[i] - F[i]). Since delta is always 0/1, the first nonzero step
 * resolves the whole chain, including the landing. Check prediction vs fingerprint ground truth. */
#include "moflex_demux.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define MAXP 200000
static uint64_t fp[MAXP]; static int fpn;
static uint64_t fnv(const uint8_t *d, int n, int size) {
    uint64_t h = 1469598103934665603ull ^ (uint64_t)size;
    for (int i = 0; i < n; i++) { h ^= d[i]; h *= 1099511628211ull; }
    return h;
}

int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    MfxDemux m;
    if (!f || mfx_open(&m, f) != 0) { fprintf(stderr, "open failed\n"); return 1; }
    MfxPacket pkt;
    while (fpn < MAXP && mfx_next_packet(&m, &pkt) == 1) {
        if (m.streams[pkt.stream_index].media_type != MFX_TYPE_VIDEO) continue;
        int n = pkt.size < 64 ? pkt.size : 64;
        fp[fpn++] = fnv(pkt.data, n, pkt.size);
    }
    /* eye frame duration in us: movie declares 30fps pairs -> 60 eye fps */
    double eye_us = 1e6 / 60.0;

    int64_t seen[128]; int nseen = 0; int ok = 0, bad = 0, unres = 0;
    printf("landing    truth  predicted  blocks-needed\n");
    for (int pc = 1; pc <= 99; pc += 2) {
        if (mfx_seek_time(&m, m.duration_us * pc / 100) != 0) continue;
        int64_t lts = m.ts; int dup = 0;
        for (int i = 0; i < nseen; i++) if (seen[i] == lts) dup = 1;
        if (dup || nseen >= 128) continue;
        seen[nseen++] = lts;

        /* fingerprint the first packet for ground truth, then walk blocks */
        uint64_t h0 = 0; int have0 = 0;
        int64_t bt0 = lts; long cnt = 0; int steps[16]; int nb = 0;
        long g;
        for (g = 0; g < 40000 && nb < 12; g++) {
            if (mfx_next_packet(&m, &pkt) != 1) break;
            if (m.streams[pkt.stream_index].media_type != MFX_TYPE_VIDEO) continue;
            if (!have0) { h0 = fnv(pkt.data, pkt.size < 64 ? pkt.size : 64, pkt.size); have0 = 1; }
            if (m.ts != bt0) {   /* block boundary: close out the previous block */
                long F = (long)(((double)(m.ts - bt0)) / eye_us + 0.5);
                steps[nb++] = (int)(cnt - F);
                bt0 = m.ts; cnt = 0;
            }
            cnt++;
        }
        int gi = -1;
        for (int i = 0; i < fpn; i++) if (fp[i] == h0) { gi = i; break; }
        /* resolve: delta[k+1] = delta[k] + step[k]; delta in {0,1}. first nonzero step pins it. */
        int pred = -1, need = -1;
        int cum = 0;   /* delta[k] - delta[0] */
        for (int k = 0; k < nb; k++) {
            if (steps[k] == 1)  { pred = 0 - cum; need = k + 1; break; }   /* delta[k]=0 -> delta0 = -cum */
            if (steps[k] == -1) { pred = 1 - cum; need = k + 1; break; }   /* delta[k]=1 */
            cum += steps[k];   /* step 0 -> unchanged */
        }
        if (pred < 0 || pred > 1) { unres++; printf("%7.2fs  %s  UNRESOLVED (%d blocks)\n",
                                                    lts / 1e6, gi < 0 ? "?" : (gi & 1) ? "R" : "L", nb); continue; }
        const char *tr = gi < 0 ? "?" : (gi & 1) ? "R" : "L";
        const char *pr = pred ? "R" : "L";
        int match = gi >= 0 && ((gi & 1) == pred);
        if (gi >= 0) { if (match) ok++; else bad++; }
        printf("%7.2fs  %s      %s          %d   %s\n", lts / 1e6, tr, pr, need, match ? "OK" : "WRONG");
    }
    printf("\nresolved correct %d, wrong %d, unresolved %d\n", ok, bad, unres);
    return 0;
}
