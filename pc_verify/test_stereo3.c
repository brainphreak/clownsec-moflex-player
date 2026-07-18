/* Dump the block/ts structure around video packets: does the ts (block) boundary group pairs?
 * Prints the first 40 video packets and packets around a landing point. */
#include "moflex_demux.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    MfxDemux m;
    if (!f || mfx_open(&m, f) != 0) { fprintf(stderr, "open failed\n"); return 1; }
    MfxPacket pkt;
    long vidx = 0; int64_t last_ts = -1;
    printf("start of file:\n idx | ts(ms) | new-block? | kf | size\n");
    while (vidx < 40 && mfx_next_packet(&m, &pkt) == 1) {
        if (m.streams[pkt.stream_index].media_type != MFX_TYPE_VIDEO) continue;
        printf(" %3ld | %7lld | %s | %d | %d\n", vidx, (long long)(m.ts / 1000),
               m.ts != last_ts ? "NEW" : "   ", pkt.keyframe, pkt.size);
        last_ts = m.ts;
        vidx++;
    }
    /* seek to 45% (a known ODD/right-eye landing) and dump the first 12 video packets */
    mfx_seek_time(&m, m.duration_us * 45 / 100);
    printf("\nafter seek to 45%% (landing was global idx 1525 = RIGHT eye):\n");
    last_ts = -1; vidx = 0;
    while (vidx < 12 && mfx_next_packet(&m, &pkt) == 1) {
        if (m.streams[pkt.stream_index].media_type != MFX_TYPE_VIDEO) continue;
        printf(" +%2ld | %7lld | %s | %d | %d\n", vidx, (long long)(m.ts / 1000),
               m.ts != last_ts ? "NEW" : "   ", pkt.keyframe, pkt.size);
        last_ts = m.ts;
        vidx++;
    }
    return 0;
}
