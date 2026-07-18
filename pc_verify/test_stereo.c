/* 3D eye-parity analyzer: answers, from the real stream,
 *   1) do keyframes sit on even (L) or odd (R) global video-packet indices? singles or pairs?
 *   2) after mfx_seek_time to a mid-file point, what parity does the landing start on?
 * This decides how the player's seek prime must pick the LEFT eye. */
#include "moflex_demux.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s in.moflex\n", argv[0]); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    MfxDemux m;
    if (mfx_open(&m, f) != 0) { fprintf(stderr, "mfx_open failed\n"); return 1; }
    printf("duration %lld us, streams %d\n", (long long)m.duration_us, m.nb_streams);

    /* pass 1: keyframe geography from the file start (ground truth for parity) */
    MfxPacket pkt;
    long vidx = 0, nkf = 0, kf_even = 0, kf_odd = 0, kf_pairs = 0;
    int prev_kf = 0; long prev_idx = -10;
    long first_kf[16]; int nfirst = 0;
    while (mfx_next_packet(&m, &pkt) == 1) {
        if (m.streams[pkt.stream_index].media_type != MFX_TYPE_VIDEO) continue;
        if (pkt.keyframe) {
            nkf++;
            if (vidx & 1) kf_odd++; else kf_even++;
            if (prev_kf && vidx == prev_idx + 1) kf_pairs++;   /* consecutive keyframes = L+R pair */
            if (nfirst < 16) first_kf[nfirst++] = vidx;
            prev_kf = 1; prev_idx = vidx;
        } else prev_kf = 0;
        vidx++;
    }
    printf("video packets %ld, keyframes %ld (even-idx %ld, odd-idx %ld, consecutive-pairs %ld)\n",
           vidx, nkf, kf_even, kf_odd, kf_pairs);
    printf("first kf indices:");
    for (int i = 0; i < nfirst; i++) printf(" %ld", first_kf[i]);
    printf("\n\n");

    /* pass 2: seek landings -- parity of the first keyframe after each landing.
     * If keyframes are always LEFT eyes (pass 1), an ODD offset here means the landing
     * started on a RIGHT eye (marker does NOT reset eye phase). */
    for (int pc = 10; pc <= 90; pc += 10) {
        int64_t tgt = m.duration_us * pc / 100;
        if (mfx_seek_time(&m, tgt) != 0) { printf("%2d%%: seek failed\n", pc); continue; }
        long off = 0, kfoff = -1; int64_t lts = m.ts;
        for (long g = 0; g < 20000; g++) {
            if (mfx_next_packet(&m, &pkt) != 1) break;
            if (m.streams[pkt.stream_index].media_type != MFX_TYPE_VIDEO) continue;
            if (pkt.keyframe) { kfoff = off; break; }
            off++;
        }
        printf("%2d%%: landed ts %8.2fs, first kf at video-offset %ld (%s)\n",
               pc, lts / 1e6, kfoff,
               kfoff < 0 ? "none found" : (kfoff & 1) ? "ODD -> landing started on RIGHT eye"
                                                      : "even -> landing started on LEFT eye");
    }
    fclose(f);
    return 0;
}
