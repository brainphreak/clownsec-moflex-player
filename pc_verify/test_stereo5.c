/* Does block ts map exactly to the global eye-frame index (idx = ts*60/1e6)? If yes everywhere,
 * the player can derive landing parity from ts alone. Prints every mismatch. */
#include "moflex_demux.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    MfxDemux m;
    if (!f || mfx_open(&m, f) != 0) { fprintf(stderr, "open failed\n"); return 1; }
    /* eye rate = 2x declared pair rate; movie.moflex declares 30fps pairs -> 60 eye fps.
     * Use the demuxed fps if available; else assume 60. */
    double eye_fps = 0;
    for (int i = 0; i < m.nb_streams; i++)
        if (m.streams[i].media_type == MFX_TYPE_VIDEO) {
            printf("stream fps num/den style fields unknown; assuming 59.94/60 via ts fit\n");
        }
    MfxPacket pkt;
    long vidx = 0, mism = 0, blocks = 0; int64_t last_ts = -1;
    while (mfx_next_packet(&m, &pkt) == 1) {
        if (m.streams[pkt.stream_index].media_type != MFX_TYPE_VIDEO) continue;
        if (m.ts != last_ts) {
            blocks++;
            /* expected index from ts at 60 eye-fps: idx = ts_us * 60 / 1e6, rounded */
            double fidx = (double)m.ts * 60.0 / 1e6;
            long ridx = (long)(fidx + 0.5);
            if (ridx != vidx) {
                if (mism < 20)
                    printf("block ts %8.1fms: expected idx %ld (%.2f), actual first idx %ld (delta %ld)\n",
                           m.ts / 1000.0, ridx, fidx, vidx, vidx - ridx);
                mism++;
            }
            last_ts = m.ts;
        }
        vidx++;
    }
    printf("\n%ld video packets, %ld blocks, %ld ts->idx mismatches\n", vidx, blocks, mism);
    printf(mism == 0 ? "TS MAPS EXACTLY -> parity from ts is reliable\n"
                     : "ts does NOT track the packet index -> parity from ts unreliable\n");
    return 0;
}
