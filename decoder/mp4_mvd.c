/* MVD (New-3DS hardware H.264) wrapper -- see mp4_mvd.h.
 * Flow modeled on Core-2-Extreme/Video_player_for_3DS (proven in production):
 *   - feed NAL units in ANNEX-B form (00 00 01 prefix), all of a frame's NALs concatenated into
 *     ONE mvdstdProcessVideoFrame call (SPS/PPS from avcC are primed the same way at init),
 *   - the FIRST frame is fed twice (MVD needs it),
 *   - detect that a frame was actually produced by writing 0x11 sentinels to the output buffer's
 *     corners and checking whether MVD overwrote them (return status alone is unreliable),
 *   - render into a linear BGR565 buffer, then rotate-transpose it into the (column-major) top fb. */
#include "mp4_mvd.h"
#include <string.h>
#include <stdlib.h>
#include <malloc.h>

#define TOP_W 400
#define TOP_H 240

static int            g_ready = 0, g_first = 1;
static MVDSTD_Config  g_config;
static uint8_t       *g_pkt = NULL;       /* Annex-B packet for a frame */
static size_t         g_pkt_cap = 0;
static u16           *g_out = NULL;        /* MVD writes the decoded frame here (linear BGR565) */
static int            g_w = 0, g_h = 0, g_nal_len = 4;

static void set_corners(void) {
    uint8_t *d = (uint8_t *)g_out; int w = g_w, h = g_h;
    d[0] = 0x11; d[w*2 - 1] = 0x11; d[(w*h*2) - (w*2)] = 0x11; d[w*h*2 - 1] = 0x11;
}
static int corners_changed(void) {
    const uint8_t *d = (const uint8_t *)g_out; int w = g_w, h = g_h;
    return d[0] != 0x11 || d[w*2 - 1] != 0x11 || d[(w*h*2) - (w*2)] != 0x11 || d[w*h*2 - 1] != 0x11;
}

/* feed one Annex-B unit (00 00 01 + data) as its own ProcessVideoFrame -- used to prime SPS/PPS */
static void feed_annexb(const uint8_t *nal, int len) {
    if (len <= 0 || (size_t)(len + 3) > g_pkt_cap) return;
    g_pkt[0] = 0; g_pkt[1] = 0; g_pkt[2] = 1;
    memcpy(g_pkt + 3, nal, len);
    GSPGPU_FlushDataCache(g_pkt, len + 3);
    mvdstdProcessVideoFrame(g_pkt, len + 3, 0, NULL);
}

int mp4_mvd_init(int w, int h, const uint8_t *avcc, int avcc_len, int nal_len_size) {
    g_first = 1;
    g_nal_len = (nal_len_size == 1 || nal_len_size == 2 || nal_len_size == 4) ? nal_len_size : 4;
    g_w = w; g_h = h;

    g_pkt_cap = 2 * 1024 * 1024;
    g_pkt = (uint8_t *)linearAlloc(g_pkt_cap);
    g_out = (u16 *)linearAlloc((size_t)w * h * sizeof(u16));
    if (!g_pkt || !g_out) { mp4_mvd_exit(); return 0; }

    if (R_FAILED(mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264, MVD_OUTPUT_BGR565,
                            MVD_DEFAULT_WORKBUF_SIZE, NULL))) { mp4_mvd_exit(); return 0; }
    mvdstdGenerateDefaultConfig(&g_config, (u32)w, (u32)h, (u32)w, (u32)h, NULL, NULL, NULL);
    g_config.output_type = MVD_OUTPUT_BGR565;

    /* prime SPS/PPS from avcC as Annex-B units */
    if (avcc && avcc_len > 6) {
        int p = 5, nsps = avcc[p++] & 0x1F;
        for (int i = 0; i < nsps && p + 2 <= avcc_len; i++) { int l = (avcc[p]<<8)|avcc[p+1]; p+=2; if (p+l>avcc_len) break; feed_annexb(avcc+p, l); p+=l; }
        if (p < avcc_len) { int npps = avcc[p++];
            for (int i = 0; i < npps && p + 2 <= avcc_len; i++) { int l = (avcc[p]<<8)|avcc[p+1]; p+=2; if (p+l>avcc_len) break; feed_annexb(avcc+p, l); p+=l; } }
    }
    g_ready = 1;
    return 1;
}

void mp4_mvd_exit(void) {
    if (g_ready) mvdstdExit();
    if (g_pkt) { linearFree(g_pkt); g_pkt = NULL; }
    if (g_out) { linearFree(g_out); g_out = NULL; }
    g_ready = 0;
}

/* Blit one eye -- source columns [sx, sx+sw) of the decoded frame -- into a top-screen
 * framebuffer, aspect-fit (nearest-scaled, centered, letter/pillar-boxed) into 400x240 and
 * rotate-transposed into the column-major framebuffer. */
static void blit_eye(gfx3dSide_t side, int sx, int sw) {
    u16 *fb = (u16 *)gfxGetFramebuffer(GFX_TOP, side, NULL, NULL);
    int sh = g_h, dw, dh;
    /* fit sw x sh into TOP_W x TOP_H preserving aspect */
    if (sw * TOP_H <= sh * TOP_W) { dh = TOP_H; dw = sw ? sw * TOP_H / sh : TOP_W; }  /* height-limited */
    else                          { dw = TOP_W; dh = sh ? sh * TOP_W / sw : TOP_H; }  /* width-limited  */
    if (dw < 1) dw = 1; if (dw > TOP_W) dw = TOP_W;
    if (dh < 1) dh = 1; if (dh > TOP_H) dh = TOP_H;
    int dxoff = (TOP_W - dw) / 2, dyoff = (TOP_H - dh) / 2;

    /* precompute the source row for each destination row (avoids a divide per pixel) */
    static int srcrow[TOP_H];
    for (int dy = 0; dy < dh; dy++) srcrow[dy] = (dy * sh / dh) * g_w;

    for (int dx = 0; dx < TOP_W; dx++) {
        u16 *col = fb + dx * TOP_H;
        if (dx < dxoff || dx >= dxoff + dw) {            /* pillarbox column */
            for (int dy = 0; dy < TOP_H; dy++) col[dy] = 0;
            continue;
        }
        int srcx = sx + (dx - dxoff) * sw / dw;
        for (int dy = 0; dy < dyoff; dy++)               col[TOP_H - 1 - dy] = 0;                    /* top bar */
        for (int dy = 0; dy < dh; dy++)                  col[TOP_H - 1 - (dy + dyoff)] = g_out[srcrow[dy] + srcx];
        for (int dy = dyoff + dh; dy < TOP_H; dy++)      col[TOP_H - 1 - dy] = 0;                    /* bottom bar */
    }
}

void mp4_mvd_present(int sbs) {
    if (sbs) { int hw = g_w / 2; blit_eye(GFX_LEFT, 0, hw); blit_eye(GFX_RIGHT, hw, hw); }
    else     { blit_eye(GFX_LEFT, 0, g_w); }
}

int mp4_mvd_decode(const uint8_t *sample, int size) {
    if (!g_ready) return 0;

    /* build one Annex-B packet from all of this frame's length-prefixed NALs */
    int off = 0, sp = 0;
    while (sp + g_nal_len <= size) {
        uint32_t nlen = 0;
        for (int b = 0; b < g_nal_len; b++) nlen = (nlen << 8) | sample[sp + b];
        sp += g_nal_len;
        if (nlen == 0 || sp + (int)nlen > size) break;
        if ((size_t)(off + 3 + nlen) > g_pkt_cap) break;
        g_pkt[off] = 0; g_pkt[off+1] = 0; g_pkt[off+2] = 1; off += 3;
        memcpy(g_pkt + off, sample + sp, nlen); off += (int)nlen; sp += (int)nlen;
    }
    if (off == 0) return 0;

    g_config.physaddr_outdata0 = osConvertVirtToPhys(g_out);
    set_corners();
    GSPGPU_FlushDataCache(g_out, (u32)g_w * g_h * sizeof(u16));
    GSPGPU_FlushDataCache(g_pkt, off);

    Result r = mvdstdProcessVideoFrame(g_pkt, off, 0, NULL);
    if (g_first) { mvdstdProcessVideoFrame(g_pkt, off, 0, NULL); g_first = 0; }   /* MVD needs the first frame twice */
    if (!MVD_CHECKNALUPROC_SUCCESS(r)) return 0;

    /* render the decoded frame into g_out (the config points there) */
    mvdstdRenderVideoFrame(&g_config, true);
    GSPGPU_InvalidateDataCache(g_out, (u32)g_w * g_h * sizeof(u16));
    if (!corners_changed()) return 0;   /* MVD didn't actually write a frame yet */

    return 1;
}
