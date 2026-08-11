/* Validate backward-keyframe landing: check the direct landing first (Nintendo lands ON kfs);
 * else scan a 20s back-window for the last keyframe <= target. Report landing deltas. */
#include "moflex_demux.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    MfxDemux m;
    if (!f || mfx_open(&m, f) != 0) return 1;
    mfx_detect_stereo(&m);
    for (int pc = 20; pc <= 80; pc += 15) {
        int64_t tgt = m.duration_us * pc / 100;
        /* direct landing keyframe? */
        mfx_seek_time(&m, tgt);
        int64_t direct = m.ts; int direct_kf = 0;
        MfxPacket pk;
        for (int g = 0; g < 200; g++) {
            if (mfx_next_packet(&m, &pk) != 1) break;
            if (m.streams[pk.stream_index].media_type != MFX_TYPE_VIDEO) continue;
            direct_kf = pk.keyframe; break;
        }
        if (direct_kf) { printf("%2d%%: direct landing IS a keyframe (%+.2fs)\n", pc, (direct - tgt) / 1e6); continue; }
        /* back-window search */
        int64_t back = tgt - 20000000; if (back < 0) back = 0;
        mfx_seek_time(&m, back);
        int64_t kf_marker = -1; long pkts = 0;
        int64_t cur_marker = m.ts;
        for (long g = 0; g < 60000; g++) {
            if (mfx_next_packet(&m, &pk) != 1) break;
            pkts++;
            if (m.ts != cur_marker) { if (m.ts > tgt) break; cur_marker = m.ts; }
            if (m.streams[pk.stream_index].media_type != MFX_TYPE_VIDEO) continue;
            if (pk.keyframe) kf_marker = cur_marker;
        }
        if (kf_marker >= 0)
            printf("%2d%%: back-window kf landing %+.2fs from target (%ld pkts scanned)\n",
                   pc, (kf_marker - tgt) / 1e6, pkts);
        else
            printf("%2d%%: NO kf in 20s window (%ld pkts) -> old forward behavior\n", pc, pkts);
    }
    return 0;
}
