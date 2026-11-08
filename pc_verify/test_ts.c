/* Native check of decoder/ts_demux.c against a captured Plex stream:
 *   test_ts in.ts out.h264 out_audio.aac
 * Feeds the file in random-sized chunks (as the network delivers it), prints counts, dims,
 * the first timestamps, and rebuilds an Annex-B H.264 + ADTS AAC for ffmpeg to decode. */
#include "../decoder/ts_demux.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    if (argc < 4) return 2;
    FILE *f = fopen(argv[1], "rb"), *ov = fopen(argv[2], "wb"), *oa = fopen(argv[3], "wb");
    if (!f || !ov || !oa) return 1;
    TsDemux *d = ts_new();
    static unsigned char buf[70000];
    long nv = 0, na = 0, keys = 0; srand(7);
    int64_t first_v[8]; int fv = 0; int64_t last_a = 0;
    for (;;) {
        size_t want = 1 + (size_t)(rand() % 65000);
        size_t got = fread(buf, 1, want, f);
        if (got) ts_push(d, buf, got); else ts_flush(d);
        TsAU au;
        while (ts_pop(d, &au)) {
            if (au.kind == TS_AU_VIDEO) {
                nv++; keys += au.key; if (fv < 8) first_v[fv++] = au.pts_us;
                for (size_t i = 0; i + 4 <= au.len; ) {   /* back to Annex-B for ffmpeg */
                    size_t L = ((size_t)au.data[i] << 24) | (au.data[i+1] << 16) | (au.data[i+2] << 8) | au.data[i+3];
                    fwrite("\0\0\0\1", 1, 4, ov); fwrite(au.data + i + 4, 1, L, ov); i += 4 + L;
                }
            } else {
                na++; last_a = au.pts_us;
                uint8_t asc[2]; int rate, ch; ts_audio_config(d, asc, &rate, &ch);
                int sri = ((asc[0] & 7) << 1) | (asc[1] >> 7); size_t fl = au.len + 7;
                unsigned char h[7] = { 0xFF, 0xF1, (unsigned char)((1 << 6) | (sri << 2) | (ch >> 2)),
                    (unsigned char)(((ch & 3) << 6) | (fl >> 11)), (unsigned char)((fl >> 3) & 0xFF),
                    (unsigned char)(((fl & 7) << 5) | 0x1F), 0xFC };
                fwrite(h, 1, 7, oa); fwrite(au.data, 1, au.len, oa);
            }
        }
        if (!got) break;
    }
    int w = 0, h = 0; ts_video_dims(d, &w, &h);
    const unsigned char *av; int al = 0; ts_video_avcc(d, &av, &al);
    uint8_t asc[2]; int rate = 0, ch = 0; ts_audio_config(d, asc, &rate, &ch);
    printf("video %ld (keys %ld) dims %dx%d avcc %d bytes prof %d level %d\n", nv, keys, w, h, al, al ? av[1] : 0, al ? av[3] : 0);
    printf("audio %ld frames %d Hz %d ch, last pts %.3f s\n", na, rate, ch, last_a / 1e6);
    printf("first video pts (s):"); for (int i = 0; i < fv; i++) printf(" %.3f", first_v[i] / 1e6); printf("\n");
    ts_free(d); fclose(f); fclose(ov); fclose(oa);
    return 0;
}
