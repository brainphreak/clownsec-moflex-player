/* Eye-dependency probe: decodes N video frames of a 3D moflex and reports the motion-comp
 * reference-distance histogram. Eyes alternate every frame, so an ODD reference distance means
 * a frame referenced the OTHER eye (cross-eye / inter-view prediction). If ANY odd distance is
 * used, single-eye decoding is impossible for this file. */
#include "moflex_demux.h"
#include "mobicompat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern int    mobi_init(AVCodecContext *);
extern int    mobi_decode(AVCodecContext *, AVFrame *, int *, AVPacket *);
extern size_t mobi_ctx_size(void);
extern unsigned long g_mobi_refdist[8];

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s in.moflex [nframes]\n", argv[0]); return 1; }
    int want = argc > 2 ? atoi(argv[2]) : 600;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    MfxDemux m;
    if (mfx_open(&m, f) != 0) { fprintf(stderr, "mfx_open failed\n"); return 1; }
    int vi = -1;
    for (int i = 0; i < m.nb_streams; i++)
        if (m.streams[i].media_type == MFX_TYPE_VIDEO) { vi = i; break; }
    if (vi < 0) { fprintf(stderr, "no video\n"); return 1; }
    AVCodecContext ctx; memset(&ctx, 0, sizeof ctx);
    ctx.width = m.streams[vi].width; ctx.height = m.streams[vi].height;
    ctx.priv_data = calloc(1, mobi_ctx_size());
    if (mobi_init(&ctx) != 0) { fprintf(stderr, "mobi_init failed\n"); return 1; }
    AVFrame *out = av_frame_alloc();
    MfxPacket pkt; int n = 0;
    while (n < want && mfx_next_packet(&m, &pkt) == 1) {
        if (m.streams[pkt.stream_index].media_type != MFX_TYPE_VIDEO) continue;
        AVPacket ap; ap.data = pkt.data; ap.size = pkt.size; int got = 0;
        if (mobi_decode(&ctx, out, &got, &ap) >= 0 && got) n++;
    }
    unsigned long odd = 0, even = 0, tot = 0;
    for (int d = 1; d < 8; d++) { tot += g_mobi_refdist[d]; if (d & 1) odd += g_mobi_refdist[d]; else even += g_mobi_refdist[d]; }
    printf("file: %s\n", argv[1]);
    printf("decoded %d frames, %dx%d\n", n, ctx.width, ctx.height);
    printf("reference-distance histogram (blocks referencing d frames back):\n");
    for (int d = 1; d < 8; d++) printf("  dist %d (%s): %lu\n", d, (d&1)?"CROSS-EYE":"same-eye", g_mobi_refdist[d]);
    printf("TOTAL motion refs: %lu   cross-eye(odd): %lu   same-eye(even): %lu\n", tot, odd, even);
    printf("VERDICT: %s\n", odd == 0 ? "SKIPPABLE (eyes are independent -- single-eye decode is safe)"
                                     : "NOT skippable (cross-eye references present -- must decode both eyes)");
    return 0;
}
