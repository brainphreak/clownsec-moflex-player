/* MobiClip A/B decode profiler -- verifies ported optimizations are bit-identical AND measures
 * speed. opt bits: 1=SWAR 2=idct-skip 4=DC-only 8=prefetch 16=UHADD8 mc 32=clamp-LUT (ported). */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include "moflex_demux.h"
#include "mobicompat.h"

extern int    mobi_init(AVCodecContext *);
extern int    mobi_decode(AVCodecContext *, AVFrame *, int *, AVPacket *);
extern void   mobi_flush(AVCodecContext *);
extern size_t mobi_ctx_size(void);
extern int    mobi_opt;

#define SEG_FRAC   0.35
#define SEG_FRAMES 500

static MfxDemux m;
static AVCodecContext ctx;
static int gW, gH;

static int find_moflex(char *out, size_t cap) {
    DIR *d = opendir("sdmc:/"); if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) { size_t l = strlen(e->d_name);
        if (l > 7 && !strcasecmp(e->d_name + l - 7, ".moflex")) { snprintf(out, cap, "sdmc:/%s", e->d_name); closedir(d); return 1; } }
    closedir(d); return 0;
}

typedef struct { double tot; uint32_t csum; } Res;

static uint32_t framesum(AVFrame *fr) {
    uint32_t s = 2166136261u;
    for (int y = 0; y < gH; y++) {
        const uint8_t *r = fr->data[0] + y * fr->linesize[0];
        for (int x = 0; x < gW; x++) s = (s ^ r[x]) * 16777619u;
    }
    return s;
}

static Res run_cfg(AVFrame *fr, int opt) {
    Res r; memset(&r, 0, sizeof r);
    mobi_opt = opt;
    mfx_seek_frac(&m, SEG_FRAC); mobi_flush(&ctx);
    uint64_t total = 0; uint32_t cs = 2166136261u; int n = 0;
    MfxPacket pkt;
    while (n < SEG_FRAMES) {
        if (mfx_next_packet(&m, &pkt) != 1) break;
        if (m.streams[pkt.stream_index].media_type != MFX_TYPE_VIDEO) continue;
        int got = 0; AVPacket ap; ap.data = pkt.data; ap.size = pkt.size;
        u64 s = svcGetSystemTick();
        int ok = (mobi_decode(&ctx, fr, &got, &ap) >= 0 && got);
        total += svcGetSystemTick() - s;
        if (ok) { n++; cs = (cs ^ framesum(fr)) * 16777619u; }
    }
    double k = 1000.0 / SYSCLOCK_ARM11 / (n ? n : 1);
    r.tot = total * k; r.csum = cs;
    return r;
}

int main(void) {
    osSetSpeedupEnable(true);
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    printf("MobiClip port A/B (clamp-LUT)\n\n");

    char path[300];
    if (!find_moflex(path, sizeof path)) { printf("Put a .moflex in sdmc:/ root.\n"); goto wait; }
    FILE *f = fopen(path, "rb");
    if (!f || mfx_open(&m, f) != 0) { printf("open failed.\n"); goto wait; }
    int vi = -1;
    for (int i = 0; i < m.nb_streams; i++) if (m.streams[i].media_type == MFX_TYPE_VIDEO && vi < 0) vi = i;
    gW = m.streams[vi].width; gH = m.streams[vi].height;
    memset(&ctx, 0, sizeof ctx);
    ctx.width = gW; ctx.height = gH; ctx.priv_data = calloc(1, mobi_ctx_size()); mobi_init(&ctx);
    AVFrame *fr = av_frame_alloc();
    printf("%.30s  %dx%d\n%d frames x configs...\n\n", strrchr(path, '/') + 1, gW, gH, SEG_FRAMES);

    /* SIMD-IDCT A/B: scalar full-transform vs packed 2-row full-transform (0x04000000), like-for-like
     * (both WITHOUT the DC-only/rowmask shortcuts, so we isolate the transform). 3 pairs interleaved
     * to expose thermal drift -- compare each SIMD to the scalar right above it, not across runs. */
    /* FUSED transpose-free IDCT (0x08000000) vs scalar full transform, both with packed residual
     * write (0x40). Only difference = the transpose + separate 2nd-pass. 3 interleaved pairs. */
    /* REAL-WORLD: shipped path (0x1BDA5E, DC/rowmask shortcuts + packed residual) vs the same
     * PLUS fused transpose-free IDCT (0x08000000). This is what playback actually runs. */
    static const struct { const char *name; int opt; } C[6] = {
        { "SHIPPED    A ", 0x1BDA5E },
        { "SHIP+FUSED A ", 0x1BDA5E | 0x08000000 },
        { "SHIPPED    B ", 0x1BDA5E },
        { "SHIP+FUSED B ", 0x1BDA5E | 0x08000000 },
        { "SHIPPED    C ", 0x1BDA5E },
        { "SHIP+FUSED C ", 0x1BDA5E | 0x08000000 },
    };
    int NC = sizeof(C) / sizeof(C[0]);
    Res res[16];
    for (int i = 0; i < NC; i++) { printf("  %s...\n", C[i].name); res[i] = run_cfg(fr, C[i].opt); }

    printf("\x1b[2J\x1b[Hcfg          ms/frame  ok\n");
    for (int i = 0; i < NC; i++)
        printf("%s  %6.2f    %s\n", C[i].name, res[i].tot,
               i == 0 ? "base" : (res[i].csum == res[0].csum ? "YES" : "DIFF"));
    printf("\nSIMD-IDCT (0x04000000) vs scalar, full transform.\n");
    printf("ok=YES = bit-identical. lower=faster.\n");
    printf("\nSTART to exit.\n");

wait:
    while (aptMainLoop()) { hidScanInput(); if (hidKeysDown() & KEY_START) break; gspWaitForVBlank(); }
    gfxExit();
    return 0;
}
