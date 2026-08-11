/* Measure the seek PRIME's landing offset: target vs where the prime logic actually starts.
 * Replicates the device scan: skip 29 packets unless keyframe, even-parity gate for 3D, frame
 * must DECODE. Reports landing-target delta and P-frame decode success after flush. */
#include "moflex_demux.h"
#include "mobicompat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern int    mobi_init(AVCodecContext *);
extern int    mobi_decode(AVCodecContext *, AVFrame *, int *, AVPacket *);
extern void   mobi_flush(AVCodecContext *);
extern size_t mobi_ctx_size(void);
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    MfxDemux m;
    if (!f || mfx_open(&m, f) != 0) return 1;
    int vi = -1;
    for (int i = 0; i < m.nb_streams; i++)
        if (m.streams[i].media_type == MFX_TYPE_VIDEO) { vi = i; break; }
    int is3d = mfx_detect_stereo(&m);
    AVCodecContext ctx; memset(&ctx, 0, sizeof ctx);
    ctx.width = m.streams[vi].width; ctx.height = m.streams[vi].height;
    ctx.priv_data = calloc(1, mobi_ctx_size());
    mobi_init(&ctx);
    AVFrame *fr = av_frame_alloc();
    printf("is3d=%d\n", is3d);
    for (int pc = 20; pc <= 80; pc += 15) {
        int64_t tgt = m.duration_us * pc / 100;
        mfx_seek_time(&m, tgt);
        int64_t landed = m.ts;
        mobi_flush(&ctx);
        MfxPacket pk; int vseen = 0, pfail = 0, ptry = 0; int64_t lts = -1;
        for (int guard = 0; guard < 4000; guard++) {
            if (mfx_next_packet(&m, &pk) != 1) break;
            if (m.streams[pk.stream_index].media_type != MFX_TYPE_VIDEO) continue;
            vseen++;
            if (is3d && !(vseen & 1)) continue;
            if (!pk.keyframe && vseen < 30) continue;
            AVPacket ap; ap.data = pk.data; ap.size = pk.size; int got = 0;
            ptry++;
            if (!(mobi_decode(&ctx, fr, &got, &ap) >= 0 && got)) { pfail++; continue; }
            lts = m.ts;
            break;
        }
        printf("%2d%%: target %8.2fs  marker-landed %8.2fs (%+.2fs)  prime-landed %8.2fs (%+.2fs)  vseen %d, decode fails %d/%d\n",
               pc, tgt / 1e6, landed / 1e6, (landed - tgt) / 1e6,
               lts / 1e6, (lts - tgt) / 1e6, vseen, pfail, ptry);
    }
    return 0;
}
