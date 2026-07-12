/* MobiClip phase profiler: decode-only, base config, MOBI_PROFILE on -> reports where the decoder
 * spends time (motion comp / entropy / transform / intra) for the first TWO sdmc moflex. Absolute
 * ms is inflated by the profiling svcGetSystemTick calls (~15%), but the % split is what we want:
 * it tells us which phase to attack next (motion comp is the memory-bound one). */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include "moflex_demux.h"
#include "mobicompat.h"

extern int      mobi_init(AVCodecContext *);
extern int      mobi_decode(AVCodecContext *, AVFrame *, int *, AVPacket *);
extern void     mobi_flush(AVCodecContext *);
extern size_t   mobi_ctx_size(void);
extern int      mobi_opt;
extern uint64_t mobi_prof[8];
extern uint64_t mobi_stat[8];

#define NF 400
static MfxDemux m;
static AVCodecContext ctx;

static int find2(char paths[][300]) {
    DIR *d = opendir("sdmc:/"); if (!d) return 0;
    struct dirent *e; int n = 0;
    while ((e = readdir(d)) && n < 2) { size_t l = strlen(e->d_name);
        if (l > 7 && !strcasecmp(e->d_name + l - 7, ".moflex")) snprintf(paths[n++], 300, "sdmc:/%s", e->d_name); }
    closedir(d); return n;
}

static void profile_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f || mfx_open(&m, f) != 0) { printf("open failed\n"); if (f) fclose(f); return; }
    int vi = -1;
    for (int i = 0; i < m.nb_streams; i++) if (m.streams[i].media_type == MFX_TYPE_VIDEO && vi < 0) vi = i;
    if (vi < 0) { printf("no video\n"); mfx_close(&m); fclose(f); return; }
    memset(&ctx, 0, sizeof ctx);
    ctx.width = m.streams[vi].width; ctx.height = m.streams[vi].height;
    ctx.priv_data = calloc(1, mobi_ctx_size()); mobi_init(&ctx);
    AVFrame *fr = av_frame_alloc();

    mobi_opt = 0;
    mfx_seek_frac(&m, 0.35); mobi_flush(&ctx);
    memset(mobi_prof, 0, sizeof(uint64_t) * 8);
    memset(mobi_stat, 0, sizeof(uint64_t) * 8);
    MfxPacket pkt; int n = 0;
    while (n < NF) {
        if (mfx_next_packet(&m, &pkt) != 1) break;
        if (m.streams[pkt.stream_index].media_type != MFX_TYPE_VIDEO) continue;
        int got = 0; AVPacket ap; ap.data = pkt.data; ap.size = pkt.size;
        if (mobi_decode(&ctx, fr, &got, &ap) >= 0 && got) n++;
    }
    uint64_t intra = mobi_prof[0], motion = mobi_prof[3], transform = mobi_prof[4], entropy = mobi_prof[5];
    uint64_t sum = intra + motion + transform + entropy; if (!sum) sum = 1;
    const char *bn = strrchr(path, '/'); bn = bn ? bn + 1 : path;
    printf("\n%.28s (%d f)\n", bn, n);
    printf(" time: mo%4.1f%% en%4.1f%% tr%4.1f%% in%4.1f%%\n",
           100.0*motion/sum, 100.0*entropy/sum, 100.0*transform/sum, 100.0*intra/sum);
    /* block sparsity -> the ceiling for the sparse-IDCT / DC-only paths */
    uint64_t blk = mobi_stat[0]; if (!blk) blk = 1;
    printf(" blocks:%lu  DC-only%4.1f%%\n", (unsigned long)mobi_stat[0], 100.0*mobi_stat[1]/blk);
    printf(" avg hi-row %.2f/7  avg AC %.1f\n", (double)mobi_stat[2]/blk, (double)mobi_stat[3]/blk);

    av_frame_free(&fr); free(ctx.priv_data); mfx_close(&m); fclose(f);
}

int main(void) {
    osSetSpeedupEnable(true);
    gfxInitDefault(); consoleInit(GFX_TOP, NULL);
    printf("MobiClip phase profile (base decode)\n");
    char paths[2][300]; int np = find2(paths);
    if (!np) printf("\nPut a .moflex in sdmc:/ root.\n");
    else for (int i = 0; i < np; i++) profile_file(paths[i]);
    printf("\nmotion = memory-bound phase.\nSTART to exit.\n");
    while (aptMainLoop()) { hidScanInput(); if (hidKeysDown() & KEY_START) break; gspWaitForVBlank(); }
    gfxExit();
    return 0;
}
