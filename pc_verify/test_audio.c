/* demux + decode moflex audio -> interleaved s16le, for compare with ffmpeg. */
#include "moflex_demux.h"
#include "adpcm_moflex.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s in.moflex out.pcm [max_bytes]\n", argv[0]); return 1; }
    long maxb = argc > 3 ? atol(argv[3]) : 0;

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    MfxDemux m;
    if (mfx_open(&m, f) != 0) { fprintf(stderr, "mfx_open failed\n"); return 1; }

    int ai = -1;
    for (int i = 0; i < m.nb_streams; i++)
        if (m.streams[i].media_type == MFX_TYPE_AUDIO) { ai = i; break; }
    if (ai < 0) { fprintf(stderr, "no audio stream\n"); return 1; }
    int chn = m.streams[ai].channels;
    fprintf(stderr, "audio: %d ch, %d Hz, codec=%d\n", chn, m.streams[ai].sample_rate, m.streams[ai].codec_id);

    FILE *out = fopen(argv[2], "wb");
    int16_t pcm[64 * 1024];
    MfxPacket pkt;
    long written = 0;
    while (mfx_next_packet(&m, &pkt) == 1) {
        if (m.streams[pkt.stream_index].media_type != MFX_TYPE_AUDIO) continue;
        int frames = adpcm_moflex_decode(pkt.data, pkt.size, chn, pcm);
        if (frames < 0) { fprintf(stderr, "decode err\n"); break; }
        int bytes = frames * chn * 2;
        fwrite(pcm, 1, bytes, out);
        written += bytes;
        if (maxb && written >= maxb) break;
    }
    fprintf(stderr, "wrote %ld bytes\n", written);
    fclose(out);
    mfx_close(&m);
    fclose(f);
    return 0;
}
