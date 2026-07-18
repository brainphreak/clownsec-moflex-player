/* Is the EYE recoverable from packet contents? For every bit of the first 4 bytes of pkt.data,
 * check correlation with global index parity (even=L, odd=R). A perfectly correlated bit = the
 * eye flag the official player uses after seeking. */
#include "moflex_demux.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    MfxDemux m;
    if (!f || mfx_open(&m, f) != 0) { fprintf(stderr, "open failed\n"); return 1; }
    MfxPacket pkt;
    long vidx = 0;
    long cnt[32][2][2] = {{{0}}};   /* [bit][parity][bitval] */
    while (mfx_next_packet(&m, &pkt) == 1) {
        if (m.streams[pkt.stream_index].media_type != MFX_TYPE_VIDEO) continue;
        int par = (int)(vidx & 1);
        for (int b = 0; b < 32 && b / 8 < pkt.size; b++) {
            int v = (pkt.data[b / 8] >> (7 - (b % 8))) & 1;
            cnt[b][par][v]++;
        }
        vidx++;
    }
    printf("%ld video packets. bits perfectly (or nearly) correlated with parity:\n", vidx);
    for (int b = 0; b < 32; b++) {
        long e0 = cnt[b][0][0], e1 = cnt[b][0][1], o0 = cnt[b][1][0], o1 = cnt[b][1][1];
        /* even mostly one value AND odd mostly the other */
        long agree_a = e0 + o1, agree_b = e1 + o0, tot = e0 + e1 + o0 + o1;
        long best = agree_a > agree_b ? agree_a : agree_b;
        if (best * 100 >= tot * 99)
            printf("  byte %d bit %d: %.2f%% correlated (even: %ld/%ld set, odd: %ld/%ld set)\n",
                   b / 8, 7 - (b % 8), 100.0 * best / tot, e1, e0 + e1, o1, o0 + o1);
    }
    printf("(nothing printed = no header bit encodes the eye)\n");
    return 0;
}
