/* Validate the dual-candidate + exact-grid decision rule against ground truth. */
#include "moflex_demux.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define MAXP 400000
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
    int vi = -1;
    for (int i = 0; i < m.nb_streams; i++)
        if (m.streams[i].media_type == MFX_TYPE_VIDEO) { vi = i; break; }
    long long pair_dur = 40000;
    if (m.streams[vi].tb_den > 0) {
        long long pd = (long long)m.streams[vi].tb_num * 1000000 / m.streams[vi].tb_den;
        if (pd >= 16000 && pd <= 100000) pair_dur = pd;
    }
    int fingerprint = argc > 2;   /* only for small files (ground truth) */
    MfxPacket pkt;
    if (fingerprint) {
        while (fpn < MAXP && mfx_next_packet(&m, &pkt) == 1) {
            if (m.streams[pkt.stream_index].media_type != MFX_TYPE_VIDEO) continue;
            fp[fpn++] = fnv(pkt.data, pkt.size < 64 ? pkt.size : 64, pkt.size);
        }
    }
    int64_t seen[128]; int nseen = 0; int ok = 0, bad = 0, noswap = 0, swaps = 0;
    for (int pc = 1; pc <= 99; pc += 2) {
        if (mfx_seek_time(&m, m.duration_us * pc / 100) != 0) continue;
        int64_t lts = m.ts; int dup = 0;
        for (int i = 0; i < nseen; i++) if (seen[i] == lts) dup = 1;
        if (dup || nseen >= 128) continue;
        seen[nseen++] = lts;
        uint64_t h0 = 0; int have0 = 0;
        int64_t spans[12]; long cnts[12]; int nb = 0;
        int64_t bt = m.ts; long cnt = 0;
        for (long g = 0; g < 20000 && nb < 12; g++) {
            if (mfx_next_packet(&m, &pkt) != 1) break;
            if (m.streams[pkt.stream_index].media_type != MFX_TYPE_VIDEO) continue;
            if (!have0) { h0 = fnv(pkt.data, pkt.size < 64 ? pkt.size : 64, pkt.size); have0 = 1; }
            if (m.ts != bt) { spans[nb] = m.ts - bt; cnts[nb] = cnt; nb++; bt = m.ts; cnt = 0; }
            cnt++;
        }
        /* THE DEVICE RULE */
        int eye_swap = 0;
        for (int cand = 0; cand < 2; cand++) {
            double eye = cand ? (double)pair_dur * 0.5 : (double)pair_dur;
            int okc = 1, cum = 0, verdict = -1;
            for (int k = 0; k < nb && okc; k++) {
                double fx = (double)spans[k] / eye;
                long F = (long)(fx + 0.5);
                double d = fx - (double)F; if (d < 0) d = -d;
                if (d > 0.02) { okc = 0; break; }
                int step = (int)(cnts[k] - F);
                if (step == 1 || step == -1) { verdict = (step == 1 ? 0 : 1) - cum; break; }
                if (step != 0) { okc = 0; break; }
            }
            if (okc && (verdict == 0 || verdict == 1)) { eye_swap = verdict; break; }
            if (okc && verdict < 0) break;
        }
        if (eye_swap) swaps++; else noswap++;
        if (fingerprint) {
            int gi = -1;
            for (int i = 0; i < fpn; i++) if (fp[i] == h0) { gi = i; break; }
            int truth = gi >= 0 ? (gi & 1) : -1;
            if (truth >= 0) { if (truth == eye_swap) ok++; else { bad++;
                printf("  MISMATCH at %.2fs: truth %s, verdict %s\n", lts / 1e6, truth ? "R" : "L", eye_swap ? "swap" : "no-swap"); } }
        }
    }
    printf("%s: %d landings, swap=%d no-swap=%d", argv[1], nseen, swaps, noswap);
    if (fingerprint) printf("  vs truth: correct=%d wrong=%d", ok, bad);
    printf("\n");
    return 0;
}
