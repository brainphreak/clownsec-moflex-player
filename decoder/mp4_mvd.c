/* MVD (New-3DS hardware H.264) wrapper -- see mp4_mvd.h. Feed raw NAL units to
 * mvdstdProcessVideoFrame (SPS/PPS -> PARAMSET, a completed slice -> a frame), render into a
 * linear RGB565 buffer, then TRANSPOSE that into the 3DS top framebuffer (which is stored rotated/
 * column-major) using the same mapping the moflex/Y2R path uses. Rendering straight to the
 * framebuffer came out sideways + sheared because MVD writes linearly and the fb is rotated. */
#include "mp4_mvd.h"
#include <string.h>
#include <stdlib.h>
#include <malloc.h>

#define TOP_W 400
#define TOP_H 240

static int            g_ready = 0;
static MVDSTD_Config  g_config;
static uint8_t       *g_inbuf = NULL;    /* NAL unit being fed */
static size_t         g_inbuf_cap = 0;
static u16           *g_out = NULL;       /* MVD writes the decoded frame here (linear RGB565) */
static int            g_w = 0, g_h = 0;
static int            g_nal_len = 4;
static int            g_frame_ready = 0;

static Result feed_nal(const uint8_t *nal, int len) {
    if (len <= 0 || (size_t)len > g_inbuf_cap) return -1;
    memcpy(g_inbuf, nal, len);
    GSPGPU_FlushDataCache(g_inbuf, len);
    MVDSTD_ProcessNALUnitOut out;
    Result r = mvdstdProcessVideoFrame(g_inbuf, len, 0, &out);
    if (MVD_CHECKNALUPROC_SUCCESS(r) && r != MVD_STATUS_PARAMSET && r != MVD_STATUS_INCOMPLETEPROCESSING)
        g_frame_ready = 1;
    return r;
}

int mp4_mvd_init(int w, int h, const uint8_t *avcc, int avcc_len, int nal_len_size) {
    g_frame_ready = 0;
    g_nal_len = (nal_len_size == 1 || nal_len_size == 2 || nal_len_size == 4) ? nal_len_size : 4;
    g_w = w; g_h = h;

    g_inbuf_cap = 1024 * 1024;
    g_inbuf = (uint8_t *)linearAlloc(g_inbuf_cap);
    g_out   = (u16 *)linearAlloc((size_t)w * h * sizeof(u16));
    if (!g_inbuf || !g_out) { mp4_mvd_exit(); return 0; }

    /* BGR565 matches the RGB565_OES framebuffer's byte order (MVD's "RGB565" comes out R/B-swapped) */
    if (R_FAILED(mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264, MVD_OUTPUT_BGR565,
                            MVD_DEFAULT_WORKBUF_SIZE, NULL))) { mp4_mvd_exit(); return 0; }

    /* decode w x h, output into g_out (no scaling); we transpose g_out -> framebuffer */
    mvdstdGenerateDefaultConfig(&g_config, (u32)w, (u32)h, (u32)w, (u32)h, NULL, (u32 *)g_out, (u32 *)g_out);
    g_config.output_type = MVD_OUTPUT_BGR565;

    /* prime SPS/PPS from the avcC box */
    if (avcc && avcc_len > 6) {
        int p = 5, nsps = avcc[p++] & 0x1F;
        for (int i = 0; i < nsps && p + 2 <= avcc_len; i++) { int l = (avcc[p]<<8)|avcc[p+1]; p+=2; if (p+l>avcc_len) break; feed_nal(avcc+p,l); p+=l; }
        if (p < avcc_len) { int npps = avcc[p++];
            for (int i = 0; i < npps && p + 2 <= avcc_len; i++) { int l = (avcc[p]<<8)|avcc[p+1]; p+=2; if (p+l>avcc_len) break; feed_nal(avcc+p,l); p+=l; } }
    }
    g_frame_ready = 0;
    g_ready = 1;
    return 1;
}

void mp4_mvd_exit(void) {
    if (g_ready) mvdstdExit();
    if (g_inbuf) { linearFree(g_inbuf); g_inbuf = NULL; }
    if (g_out)   { linearFree(g_out);   g_out = NULL; }
    g_ready = 0;
}

/* rotate-transpose the decoded frame into the (column-major) top framebuffer, centered */
static void blit_rot(gfx3dSide_t side) {
    u16 *fb = (u16 *)gfxGetFramebuffer(GFX_TOP, side, NULL, NULL);
    GSPGPU_InvalidateDataCache(g_out, (u32)g_w * g_h * sizeof(u16));
    int cw = g_w < TOP_W ? g_w : TOP_W;
    int ch = g_h < TOP_H ? g_h : TOP_H;
    int xoff = (TOP_W - cw) / 2, yoff = (TOP_H - ch) / 2;
    for (int x = 0; x < cw; x++) {
        u16 *col = fb + (x + xoff) * TOP_H;
        const u16 *s = g_out + x;
        for (int y = 0; y < ch; y++) col[TOP_H - 1 - (y + yoff)] = s[y * g_w];
    }
}

int mp4_mvd_decode(const uint8_t *sample, int size, gfx3dSide_t side) {
    if (!g_ready) return 0;
    g_frame_ready = 0;

    int i = 0;                               /* an MP4 sample is one or more length-prefixed NALs */
    while (i + g_nal_len <= size) {
        uint32_t nlen = 0;
        for (int b = 0; b < g_nal_len; b++) nlen = (nlen << 8) | sample[i + b];
        i += g_nal_len;
        if (nlen == 0 || i + (int)nlen > size) break;
        feed_nal(sample + i, (int)nlen);
        i += (int)nlen;
    }
    if (!g_frame_ready) return 0;

    if (mvdstdRenderVideoFrame(&g_config, true) != MVD_STATUS_OK) return 0;
    blit_rot(side);
    return 1;
}
