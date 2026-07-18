/* Which EYE does a seek landing start on? Fingerprint every video packet from the file start
 * (global index parity: even = LEFT under strict L,R alternation), then seek and match the first
 * landed packet back to its global index by content. Definitive, no decoding needed. */
#include "moflex_demux.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MAXP 200000
static uint64_t fp[MAXP]; static int fpn;
static int fsize[MAXP];

static uint64_t fnv(const uint8_t *d, int n, int size) {
    uint64_t h = 1469598103934665603ull ^ (uint64_t)size;
    for (int i = 0; i < n; i++) { h ^= d[i]; h *= 1099511628211ull; }
    return h;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s in.moflex\n", argv[0]); return 1; }
    FILE *f = fopen(argv[1], "rb");
    MfxDemux m;
    if (!f || mfx_open(&m, f) != 0) { fprintf(stderr, "open failed\n"); return 1; }

    MfxPacket pkt;
    while (fpn < MAXP && mfx_next_packet(&m, &pkt) == 1) {
        if (m.streams[pkt.stream_index].media_type != MFX_TYPE_VIDEO) continue;
        int n = pkt.size < 64 ? pkt.size : 64;
        fp[fpn] = fnv(pkt.data, n, pkt.size); fsize[fpn] = pkt.size; fpn++;
    }
    printf("%d video packets fingerprinted\n\n", fpn);

    for (int pc = 5; pc <= 95; pc += 10) {
        int64_t tgt = m.duration_us * pc / 100;
        if (mfx_seek_time(&m, tgt) != 0) { printf("%2d%%: seek failed\n", pc); continue; }
        /* first TWO video packets at the landing -> match both (guards against hash collisions) */
        uint64_t h0 = 0, h1 = 0; int got = 0;
        for (long g = 0; g < 40000 && got < 2; g++) {
            if (mfx_next_packet(&m, &pkt) != 1) break;
            if (m.streams[pkt.stream_index].media_type != MFX_TYPE_VIDEO) continue;
            int n = pkt.size < 64 ? pkt.size : 64;
            if (got == 0) h0 = fnv(pkt.data, n, pkt.size); else h1 = fnv(pkt.data, n, pkt.size);
            got++;
        }
        int gi = -1;
        for (int i = 0; i + 1 < fpn; i++) if (fp[i] == h0 && fp[i + 1] == h1) { gi = i; break; }
        printf("%2d%%: landing packet = global video index %d -> %s\n", pc, gi,
               gi < 0 ? "NO MATCH" : (gi & 1) ? "ODD  = RIGHT eye first (swapped!)" : "even = LEFT eye first (correct)");
    }
    fclose(f);
    return 0;
}
