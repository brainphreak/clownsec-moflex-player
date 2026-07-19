/* Validate step-back: if a landing is R-first, retarget to just before it (previous marker) and
 * re-peek, up to 3 steps. Report final parity + steps needed for every landing. */
#include "moflex_demux.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static long long pair_dur;
static int peek_rfirst(MfxDemux *m, int64_t *land_ts) {   /* 1 = R-first, 0 = L-first/unknown */
    MfxPacket pkt;
    int64_t bt = m->ts; long cnt = 0; int nb = 0, okc = 1, decided = 0, rfirst = 0;
    *land_ts = m->ts;
    double eye = (double)pair_dur * 0.5;
    for (long g = 0; g < 1500 && nb < 4 && !decided && okc; g++) {
        if (mfx_next_packet(m, &pkt) != 1) break;
        if (m->streams[pkt.stream_index].media_type != MFX_TYPE_VIDEO) continue;
        if (m->ts != bt) {
            double fx = (double)(m->ts - bt) / eye;
            long F = (long)(fx + 0.5);
            double d = fx - (double)F; if (d < 0) d = -d;
            if (d > 0.02) okc = 0;
            else { int step = (int)(cnt - F);
                   if (step == 1 || step == -1) { if (step == -1) rfirst = 1; decided = 1; }
                   else if (step != 0) okc = 0; }
            nb++; bt = m->ts; cnt = 0;
        }
        cnt++;
    }
    return rfirst;
}
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    MfxDemux m;
    if (!f || mfx_open(&m, f) != 0) return 1;
    int vi = -1;
    for (int i = 0; i < m.nb_streams; i++)
        if (m.streams[i].media_type == MFX_TYPE_VIDEO) { vi = i; break; }
    int is3d = mfx_detect_stereo(&m);
    pair_dur = 40000;
    if (m.streams[vi].tb_den > 0) {
        long long pd = (long long)m.streams[vi].tb_num * 1000000 / m.streams[vi].tb_den;
        if (pd >= 16000 && pd <= 100000) pair_dur = pd;
    }
    if (is3d && m.tb_is_eye) pair_dur *= 2;
    int64_t seen[128]; int nseen = 0; int still_r = 0; long tot_steps = 0; int max_steps = 0;
    for (int pc = 1; pc <= 99; pc += 2) {
        if (mfx_seek_time(&m, m.duration_us * pc / 100) != 0) continue;
        int64_t lts = m.ts; int dup = 0;
        for (int i = 0; i < nseen; i++) if (seen[i] == lts) dup = 1;
        if (dup || nseen >= 128) continue;
        seen[nseen++] = lts;
        int64_t tgt = m.duration_us * pc / 100, land = 0;
        int rfirst = 0, steps = 0;
        for (int attempt = 0; ; attempt++) {
            rfirst = peek_rfirst(&m, &land);
            if (!rfirst || attempt >= 3) break;
            steps++;
            tgt = land - 1;
            if (tgt < 0) tgt = 0;
            mfx_seek_time(&m, tgt);
        }
        tot_steps += steps; if (steps > max_steps) max_steps = steps;
        if (rfirst) still_r++;
        if (steps) printf("  %5.1fs: %d step(s) back -> %s (landed %.1fs)\n",
                          seen[nseen-1] / 1e6, steps, rfirst ? "STILL R-FIRST" : "L-first", land / 1e6);
    }
    printf("%d landings: %d unresolved-R after stepping, total steps %ld, max %d\n",
           nseen, still_r, tot_steps, max_steps);
    return 0;
}
