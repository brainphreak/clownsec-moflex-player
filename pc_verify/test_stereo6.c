/* Classify LEFT-eye vs RIGHT-eye KEYFRAMES: dump the first 6 bytes of every keyframe packet with
 * its global parity, and test every bit of the first 6 bytes for perfect L/R separation. */
#include "moflex_demux.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    MfxDemux m;
    if (!f || mfx_open(&m, f) != 0) { fprintf(stderr, "open failed\n"); return 1; }
    MfxPacket pkt;
    long vidx = 0;
    long cnt[48][2][2] = {{{0}}};
    printf(" idx  par | first 6 bytes\n");
    int shown = 0;
    while (mfx_next_packet(&m, &pkt) == 1) {
        if (m.streams[pkt.stream_index].media_type != MFX_TYPE_VIDEO) continue;
        if (pkt.keyframe) {
            int par = (int)(vidx & 1);
            if (shown < 24) {
                printf("%5ld  %s | %02X %02X %02X %02X %02X %02X\n", vidx, par ? "R" : "L",
                       pkt.data[0], pkt.data[1], pkt.data[2], pkt.data[3], pkt.data[4], pkt.data[5]);
                shown++;
            }
            for (int b = 0; b < 48 && b / 8 < pkt.size; b++) {
                int v = (pkt.data[b / 8] >> (7 - (b % 8))) & 1;
                cnt[b][par][v]++;
            }
        }
        vidx++;
    }
    printf("\nbits separating L-keyframes from R-keyframes (>=95%%):\n");
    for (int b = 0; b < 48; b++) {
        long e0 = cnt[b][0][0], e1 = cnt[b][0][1], o0 = cnt[b][1][0], o1 = cnt[b][1][1];
        long agree_a = e0 + o1, agree_b = e1 + o0, tot = e0 + e1 + o0 + o1;
        long best = agree_a > agree_b ? agree_a : agree_b;
        if (tot && best * 100 >= tot * 95)
            printf("  byte %d bit %d: %.1f%% (L set %ld/%ld, R set %ld/%ld)\n",
                   b / 8, 7 - (b % 8), 100.0 * best / tot, e1, e0 + e1, o1, o0 + o1);
    }
    return 0;
}
