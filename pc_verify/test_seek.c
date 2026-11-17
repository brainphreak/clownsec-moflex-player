#include <stdio.h>
#include <stdlib.h>
#include "moflex_demux.h"
extern long g_seek_probes;
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb"); MfxDemux m;
    if (mfx_open(&m, f)) { printf("open fail\n"); return 1; }
    int64_t dur = m.duration_us; printf("duration %.1f s\n", dur / 1e6);
    int bad = 0; double maxlag = 0, sumlag = 0; int n = 0;
    srand(7);
    for (int k = 0; k < 200; k++) {
        int64_t t = (int64_t)((double)rand() / RAND_MAX * (dur - 2000000)) + 1000000;
        if (k % 3 == 1) t += 400000;              /* nearby retargets, like a drag */
        if (mfx_seek_time(&m, t)) { printf("seek fail %lld\n", (long long)t); bad++; continue; }
        int64_t land = m.ts; double lag = (t - land) / 1e6;
        if (land > t) bad++;
        if (lag > maxlag) maxlag = lag; sumlag += lag; n++;
        MfxPacket p; if (mfx_next_packet(&m, &p) != 1) bad++;
        int64_t land2;   /* same target again must land identically */
        mfx_seek_time(&m, t); land2 = m.ts; if (land2 != land) bad++;
    }
    printf("probes per seek %.1f\n", g_seek_probes / 400.0); printf("seeks %d bad %d  avg landing %.2f s before target, max %.2f s, known points %d\n", n, bad, sumlag / n, maxlag, m.kn);
    return bad != 0;
}
