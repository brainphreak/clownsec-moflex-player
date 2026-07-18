/* Device-accurate peek simulation: uses the stream timebase (like the player) and reports, per
 * landing, each block's exact span fraction + step, and what the device peek would decide. */
#include "moflex_demux.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

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
    printf("pair_dur %lld us (%.3f pairs/s)\n", pair_dur, 1e6 / pair_dur);
    MfxPacket pkt;
    for (int pc = 20; pc <= 80; pc += 20) {
        mfx_seek_time(&m, m.duration_us * pc / 100);
        long long bt = m.ts; long cnt = 0; int nb = 0, verdict = -99, cum = 0;
        printf("\n%d%%: landing ts %.2fs\n", pc, m.ts / 1e6);
        for (long g = 0; g < 20000 && nb < 8; g++) {
            if (mfx_next_packet(&m, &pkt) != 1) break;
            if (m.streams[pkt.stream_index].media_type != MFX_TYPE_VIDEO) continue;
            if (m.ts != bt) {
                double fexact = (double)(m.ts - bt) * 2.0 / (double)pair_dur;
                long F = lround(fexact);
                int step = (int)(cnt - F);
                printf("  block %d: span %.3f frames (count %ld, step %+d)%s\n",
                       nb, fexact, cnt, step, fabs(fexact - F) > 0.02 ? "  NON-INTEGRAL" : "");
                if (verdict == -99 && (step == 1 || step == -1)) verdict = (step == 1 ? 0 : 1) - cum;
                else if (verdict == -99) cum += step;
                bt = m.ts; cnt = 0; nb++;
            }
            cnt++;
        }
        printf("  device verdict: %s\n", verdict == -99 ? "unresolved (no swap)" :
               verdict == 1 ? "SWAP (R-first)" : verdict == 0 ? "no swap (L-first)" : "out-of-range");
    }
    return 0;
}
