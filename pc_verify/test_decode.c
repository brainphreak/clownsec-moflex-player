/* demux + decode N video frames -> raw YUV420P, for comparison with ffmpeg. */
#include "moflex_demux.h"
#include "mobicompat.h"
#include <stdio.h>
#include <stdlib.h>

extern int    mobi_init(AVCodecContext *);
extern int    mobi_decode(AVCodecContext *, AVFrame *, int *, AVPacket *);
extern size_t mobi_ctx_size(void);

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s in.moflex out.yuv [nframes]\n", argv[0]); return 1; }
    int want = argc > 3 ? atoi(argv[3]) : 10;

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    MfxDemux m;
    if (mfx_open(&m, f) != 0) { fprintf(stderr, "mfx_open failed\n"); return 1; }

    int vi = -1;
    for (int i = 0; i < m.nb_streams; i++)
        if (m.streams[i].media_type == MFX_TYPE_VIDEO) { vi = i; break; }
    if (vi < 0) { fprintf(stderr, "no video stream\n"); return 1; }
    int W = m.streams[vi].width, H = m.streams[vi].height;
    fprintf(stderr, "video %dx%d\n", W, H);

    AVCodecContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.width = W; ctx.height = H;
    ctx.priv_data = calloc(1, mobi_ctx_size());
    if (mobi_init(&ctx) != 0) { fprintf(stderr, "mobi_init failed\n"); return 1; }

    AVFrame *out = av_frame_alloc();
    FILE *yuv = fopen(argv[2], "wb");
    MfxPacket pkt;
    int n = 0, r;
    while (n < want && mfx_next_packet(&m, &pkt) == 1) {
        if (m.streams[pkt.stream_index].media_type != MFX_TYPE_VIDEO) continue;
        AVPacket ap; ap.data = pkt.data; ap.size = pkt.size;
        int got = 0;
        r = mobi_decode(&ctx, out, &got, &ap);
        if (r < 0) { fprintf(stderr, "decode error %d at frame %d (pkt size %d)\n", r, n, pkt.size); break; }
        if (got) {
            for (int y = 0; y < out->height;   y++) fwrite(out->data[0] + y * out->linesize[0], 1, out->width,   yuv);
            for (int y = 0; y < out->height/2; y++) fwrite(out->data[1] + y * out->linesize[1], 1, out->width/2, yuv);
            for (int y = 0; y < out->height/2; y++) fwrite(out->data[2] + y * out->linesize[2], 1, out->width/2, yuv);
            n++;
        }
    }
    fprintf(stderr, "decoded %d frames\n", n);
    fclose(yuv);
    av_frame_free(&out);
    mfx_close(&m);
    fclose(f);
    return 0;
}
