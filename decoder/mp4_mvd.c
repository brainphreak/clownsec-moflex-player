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
#include <stdio.h>
#include <stdarg.h>

/* crash forensics: every MVD step appends a line to sdmc:/mvd_log.txt (opened+closed per line
 * so it survives a sysmodule crash + reboot). The LAST line names the failing call. */
void mvd_log(const char *fmt, ...) {
    FILE *f = fopen("sdmc:/mvd_log.txt", "ab");
    if (!f) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

#define TOP_W 400
#define TOP_H 240

static int            g_ready = 0, g_first = 1;
static MVDSTD_Config  g_config;
static uint8_t       *g_pkt = NULL;       /* Annex-B packet for a frame */
static size_t         g_pkt_cap = 0;
static u16           *g_out = NULL;        /* MVD writes the decoded frame here (linear BGR565) */
static int            g_w = 0, g_h = 0, g_nal_len = 4;   /* DISPLAY dims (from the stsd box) */
static int            g_cw = 0, g_ch = 0;   /* CODED dims: 16-aligned macroblock size. H.264 codes
                                             * whole MBs (e.g. 800x166 display = 800x176 coded);
                                             * MVD must be configured -- and the output buffer
                                             * sized -- for the CODED frame, or the sysmodule
                                             * itself data-aborts (hard system crash). The blit
                                             * crops back to the display size. */
static uint8_t        g_avcc[512];         /* saved SPS/PPS, for re-priming after a seek */
static int            g_avcc_len = 0;

static void set_corners(void) {
    uint8_t *d = (uint8_t *)g_out; int w = g_cw, h = g_ch;
    d[0] = 0x11; d[w*2 - 1] = 0x11; d[(w*h*2) - (w*2)] = 0x11; d[w*h*2 - 1] = 0x11;
}
static int corners_changed(void) {
    const uint8_t *d = (const uint8_t *)g_out; int w = g_cw, h = g_ch;
    return d[0] != 0x11 || d[w*2 - 1] != 0x11 || d[(w*h*2) - (w*2)] != 0x11 || d[w*h*2 - 1] != 0x11;
}

/* feed one Annex-B unit (00 00 01 + data) as its own ProcessVideoFrame -- used to prime SPS/PPS */
static void feed_annexb(const uint8_t *nal, int len) {
    if (len <= 0 || (size_t)(len + 3) > g_pkt_cap) return;
    g_pkt[0] = 0; g_pkt[1] = 0; g_pkt[2] = 1;
    memcpy(g_pkt + 3, nal, len);
    GSPGPU_FlushDataCache(g_pkt, len + 3);
    mvd_log("STEP prime: feeding %d-byte NAL (type %d)", len, nal[0] & 0x1F);
    Result pr = mvdstdProcessVideoFrame(g_pkt, len + 3, 0, NULL);
    mvd_log("STEP prime: fed, result=%08lX", (unsigned long)pr);
}

/* feed the saved SPS/PPS (from avcC) into MVD as Annex-B units */
static void prime_sps_pps(void) {
    const uint8_t *avcc = g_avcc; int avcc_len = g_avcc_len;
    if (avcc_len <= 6) return;
    int p = 5, nsps = avcc[p++] & 0x1F;
    for (int i = 0; i < nsps && p + 2 <= avcc_len; i++) { int l = (avcc[p]<<8)|avcc[p+1]; p+=2; if (p+l>avcc_len) break; feed_annexb(avcc+p, l); p+=l; }
    if (p < avcc_len) { int npps = avcc[p++];
        for (int i = 0; i < npps && p + 2 <= avcc_len; i++) { int l = (avcc[p]<<8)|avcc[p+1]; p+=2; if (p+l>avcc_len) break; feed_annexb(avcc+p, l); p+=l; } }
}

int mp4_mvd_init(int w, int h, const uint8_t *avcc, int avcc_len, int nal_len_size) {
    g_first = 1;
    g_nal_len = (nal_len_size == 1 || nal_len_size == 2 || nal_len_size == 4) ? nal_len_size : 4;
    g_w = w; g_h = h;
    g_cw = (w + 15) & ~15; g_ch = (h + 15) & ~15;
    g_avcc_len = (avcc && avcc_len > 0 && avcc_len <= (int)sizeof g_avcc) ? avcc_len : 0;
    if (g_avcc_len) memcpy(g_avcc, avcc, g_avcc_len);

    remove("sdmc:/mvd_log.txt");   /* fresh log per attempt */
    mvd_log("STEP 1: init w=%d h=%d coded=%dx%d nal_len=%d avcc=%d", w, h, g_cw, g_ch, g_nal_len, avcc_len);
    mvd_log("STEP 2: linearSpaceFree=%lu", (unsigned long)linearSpaceFree());
    g_pkt_cap = 2 * 1024 * 1024;
    g_pkt = (uint8_t *)linearAlloc(g_pkt_cap);
    g_out = (u16 *)linearAlloc((size_t)g_cw * g_ch * sizeof(u16));
    mvd_log("STEP 3: pkt=%p out=%p", g_pkt, g_out);
    if (!g_pkt || !g_out) { mp4_mvd_exit(); return 0; }

    Result ir = mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264, MVD_OUTPUT_BGR565,
                           MVD_DEFAULT_WORKBUF_SIZE, NULL);
    mvd_log("STEP 4: mvdstdInit result=%08lX linearFreeAfter=%lu", (unsigned long)ir, (unsigned long)linearSpaceFree());
    if (R_FAILED(ir)) { mp4_mvd_exit(); return 0; }
    mvdstdGenerateDefaultConfig(&g_config, (u32)g_cw, (u32)g_ch, (u32)g_cw, (u32)g_ch, NULL, NULL, NULL);
    g_config.output_type = MVD_OUTPUT_BGR565;
    mvd_log("STEP 5: config generated");

    prime_sps_pps();
    mvd_log("STEP 6: SPS/PPS primed");
    g_ready = 1;
    return 1;
}

/* after a seek: re-prime SPS/PPS and arm the "first frame twice" so MVD resyncs cleanly on the
 * next IDR/keyframe fed to it. */
void mp4_mvd_reset(void) {
    if (!g_ready) return;
    prime_sps_pps();
    g_first = 1;
}

void mp4_mvd_exit(void) {
    if (g_ready) mvdstdExit();
    if (g_pkt) { linearFree(g_pkt); g_pkt = NULL; }
    if (g_out) { linearFree(g_out); g_out = NULL; }
    g_ready = 0;
}

/* Blit one eye -- source columns [sx, sx+sw) of the decoded frame -- into a top-screen
 * framebuffer, aspect-fit (nearest-scaled, centered, letter/pillar-boxed) into 400x240 and
 * rotate-transposed into the column-major framebuffer.
 * The top screen runs 24-bit BGR8 (same as the moflex path -- required so the SHARED subtitle
 * overlay, which writes 3-byte pixels, can never overrun a 16-bit buffer; also kills banding).
 * MVD can only output 16-bit, so each 565 pixel is expanded to 888 here. */
static void blit_eye(gfx3dSide_t side, int sx, int sw) {
    u8 *fb = gfxGetFramebuffer(GFX_TOP, side, NULL, NULL);
    int sh = g_h, dw, dh;
    /* fit sw x sh into TOP_W x TOP_H preserving aspect */
    if (sw * TOP_H <= sh * TOP_W) { dh = TOP_H; dw = sw ? sw * TOP_H / sh : TOP_W; }  /* height-limited */
    else                          { dw = TOP_W; dh = sh ? sh * TOP_W / sw : TOP_H; }  /* width-limited  */
    if (dw < 1) dw = 1; if (dw > TOP_W) dw = TOP_W;
    if (dh < 1) dh = 1; if (dh > TOP_H) dh = TOP_H;
    int dxoff = (TOP_W - dw) / 2, dyoff = (TOP_H - dh) / 2;

    /* precompute the source row for each destination row (avoids a divide per pixel) */
    static int srcrow[TOP_H];
    for (int dy = 0; dy < dh; dy++) srcrow[dy] = (dy * sh / dh) * g_cw;   /* stride = CODED width */

    for (int dx = 0; dx < TOP_W; dx++) {
        u8 *col = fb + (size_t)dx * TOP_H * 3;
        if (dx < dxoff || dx >= dxoff + dw) {            /* pillarbox column */
            memset(col, 0, TOP_H * 3);
            continue;
        }
        int srcx = sx + (dx - dxoff) * sw / dw;
        if (dyoff)                memset(col + (TOP_H - dyoff) * 3, 0, dyoff * 3);            /* top bar */
        for (int dy = 0; dy < dh; dy++) {
            u16 v = g_out[srcrow[dy] + srcx];
            u8 *p = col + (TOP_H - 1 - (dy + dyoff)) * 3;
            p[0] = (u8)((v << 3) | ((v >> 2) & 7));                    /* B: 5 -> 8 bits */
            p[1] = (u8)(((v >> 3) & 0xFC) | ((v >> 9) & 3));           /* G: 6 -> 8 bits */
            p[2] = (u8)(((v >> 8) & 0xF8) | (v >> 13));                /* R: 5 -> 8 bits */
        }
        if (dyoff + dh < TOP_H)   memset(col, 0, (TOP_H - dyoff - dh) * 3);                   /* bottom bar */
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

    static int logframes = 0;
    int lg = (logframes < 4); if (lg) logframes++;
    g_config.physaddr_outdata0 = osConvertVirtToPhys(g_out);
    set_corners();
    GSPGPU_FlushDataCache(g_out, (u32)g_cw * g_ch * sizeof(u16));
    GSPGPU_FlushDataCache(g_pkt, off);

    if (lg) mvd_log("STEP 7: frame %d process (annexb %d bytes, first NAL type %d)", logframes, off, g_pkt[3] & 0x1F);
    Result r = mvdstdProcessVideoFrame(g_pkt, off, 0, NULL);
    if (lg) mvd_log("STEP 8: frame %d processed r=%08lX first=%d", logframes, (unsigned long)r, g_first);
    if (g_first) { mvdstdProcessVideoFrame(g_pkt, off, 0, NULL); g_first = 0;
                   if (lg) mvd_log("STEP 8b: first-frame-twice done"); }   /* MVD needs the first frame twice */
    if (!MVD_CHECKNALUPROC_SUCCESS(r)) return 0;

    /* render the decoded frame into g_out (the config points there) */
    if (lg) mvd_log("STEP 9: frame %d render", logframes);
    mvdstdRenderVideoFrame(&g_config, true);
    if (lg) mvd_log("STEP 10: frame %d rendered", logframes);
    GSPGPU_InvalidateDataCache(g_out, (u32)g_cw * g_ch * sizeof(u16));
    if (!corners_changed()) return 0;   /* MVD didn't actually write a frame yet */

    return 1;
}
