/* Validate the INCREMENTAL bounded peek (device port): both candidates evaluated per block,
 * abort when both die or a verdict lands; caps g<1500 packets, nb<4 blocks. */
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
    int fingerprint = argc > 2;
    MfxPacket pkt;
    if (fingerprint) {
        while (fpn < MAXP && mfx_next_packet(&m, &pkt) == 1) {
            if (m.streams[pkt.stream_index].media_type != MFX_TYPE_VIDEO) continue;
            fp[fpn++] = fnv(pkt.data, pkt.size < 64 ? pkt.size : 64, pkt.size);
        }
    }
    int64_t seen[128]; int nseen = 0; int ok = 0, bad = 0, swaps = 0; long maxg = 0;
    for (int pc = 1; pc <= 99; pc += 2) {
        if (mfx_seek_time(&m, m.duration_us * pc / 100) != 0) continue;
        int64_t lts = m.ts; int dup = 0;
        for (int i = 0; i < nseen; i++) if (seen[i] == lts) dup = 1;
        if (dup || nseen >= 128) continue;
        seen[nseen++] = lts;
        uint64_t h0 = 0; int have0 = 0;
        /* THE INCREMENTAL DEVICE RULE */
        int eye_swap = 0;
        { int64_t bt = m.ts; long cnt = 0; int nb = 0;
          int okc[2] = {1, 1}, decided = 0; long g;
          for (g = 0; g < 1500 && nb < 4 && !decided && (okc[0] || okc[1]); g++) {
            if (mfx_next_packet(&m, &pkt) != 1) break;
            if (m.streams[pkt.stream_index].media_type != MFX_TYPE_VIDEO) continue;
            if (!have0) { h0 = fnv(pkt.data, pkt.size < 64 ? pkt.size : 64, pkt.size); have0 = 1; }
            if (m.ts != bt) {
                int64_t span = m.ts - bt; long c0 = cnt;
                for (int cand = 0; cand < 2 && !decided; cand++) {
                    if (!okc[cand]) continue;
                    double eye = cand ? (double)pair_dur * 0.5 : (double)pair_dur;
                    double fx = (double)span / eye;
                    long F = (long)(fx + 0.5);
                    double d = fx - (double)F; if (d < 0) d = -d;
                    if (d > 0.02) { okc[cand] = 0; continue; }
                    int step = (int)(c0 - F);
                    if (step == 1 || step == -1) { if ((step == 1 ? 0 : 1) == 1) eye_swap = 1; decided = 1; }
                    else if (step != 0) okc[cand] = 0;
                }
                nb++; bt = m.ts; cnt = 0;
            }
            cnt++;
          }
          if (g > maxg) maxg = g;
        }
        if (eye_swap) swaps++;
        if (fingerprint) {
            int gi = -1;
            for (int i = 0; i < fpn; i++) if (fp[i] == h0) { gi = i; break; }
            if (gi >= 0) { if ((gi & 1) == eye_swap) ok++; else { bad++;
                printf("  MISMATCH at %.2fs\n", lts / 1e6); } }
        }
    }
    printf("%s:\n  %d landings, swaps=%d, max packets peeked=%ld", argv[1], nseen, swaps, maxg);
    if (fingerprint) printf(", vs truth: correct=%d wrong=%d", ok, bad);
    printf("\n");
    return 0;
}
