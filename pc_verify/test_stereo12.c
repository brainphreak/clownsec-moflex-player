/* Final validation: tb_is_eye pacing correction + single-candidate trust-gated peek. */
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
    int is3d = mfx_detect_stereo(&m);
    long long pair_dur = 40000;
    if (m.streams[vi].tb_den > 0) {
        long long pd = (long long)m.streams[vi].tb_num * 1000000 / m.streams[vi].tb_den;
        if (pd >= 16000 && pd <= 100000) pair_dur = pd;
    }
    if (is3d && m.tb_is_eye) pair_dur *= 2;
    printf("%s\n  is3d=%d tb_is_eye=%d -> pair_dur %lld us (%.3f pairs/s)\n",
           argv[1], is3d, m.tb_is_eye, pair_dur, 1e6 / pair_dur);
    int fingerprint = argc > 2;
    MfxPacket pkt;
    if (fingerprint) {
        while (fpn < MAXP && mfx_next_packet(&m, &pkt) == 1) {
            if (m.streams[pkt.stream_index].media_type != MFX_TYPE_VIDEO) continue;
            fp[fpn++] = fnv(pkt.data, pkt.size < 64 ? pkt.size : 64, pkt.size);
        }
    }
    int64_t seen[128]; int nseen = 0; int ok = 0, bad = 0, swaps = 0;
    for (int pc = 1; pc <= 99; pc += 2) {
        if (mfx_seek_time(&m, m.duration_us * pc / 100) != 0) continue;
        int64_t lts = m.ts; int dup = 0;
        for (int i = 0; i < nseen; i++) if (seen[i] == lts) dup = 1;
        if (dup || nseen >= 128) continue;
        seen[nseen++] = lts;
        uint64_t h0 = 0; int have0 = 0;
        int eye_swap = 0;
        { int64_t bt = m.ts; long cnt = 0; int nb = 0, okc = 1, decided = 0;
          double eye = (double)pair_dur * 0.5;
          for (long g = 0; g < 1500 && nb < 4 && !decided && okc; g++) {
            if (mfx_next_packet(&m, &pkt) != 1) break;
            if (m.streams[pkt.stream_index].media_type != MFX_TYPE_VIDEO) continue;
            if (!have0) { h0 = fnv(pkt.data, pkt.size < 64 ? pkt.size : 64, pkt.size); have0 = 1; }
            if (m.ts != bt) {
                double fx = (double)(m.ts - bt) / eye;
                long F = (long)(fx + 0.5);
                double d = fx - (double)F; if (d < 0) d = -d;
                if (d > 0.02) okc = 0;
                else { int step = (int)(cnt - F);
                       if (step == 1 || step == -1) { if (step == -1) eye_swap = 1; decided = 1; }
                       else if (step != 0) okc = 0; }
                nb++; bt = m.ts; cnt = 0;
            }
            cnt++;
          }
        }
        if (eye_swap) swaps++;
        if (fingerprint) {
            int gi = -1;
            for (int i = 0; i < fpn; i++) if (fp[i] == h0) { gi = i; break; }
            if (gi >= 0) { if ((gi & 1) == eye_swap) ok++; else bad++; }
        }
    }
    printf("  %d landings, swaps=%d", nseen, swaps);
    if (fingerprint) printf(", vs truth: correct=%d wrong=%d", ok, bad);
    printf("\n");
    return 0;
}
