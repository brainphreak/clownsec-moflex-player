/* MVD (New-3DS hardware H.264) wrapper -- see mp4_mvd.h. Modeled on libctru's mvd example:
 * feed raw NAL units to mvdstdProcessVideoFrame (SPS/PPS return PARAMSET; a completed slice
 * yields a frame), then mvdstdRenderVideoFrame() blits it to the framebuffer.
 *
 * NOTE: this path is New-3DS-only and can only be verified on hardware (Citra doesn't emulate MVD
 * well). Output format (RGB565 vs BGR565) and the in/out dimension orientation are the two things
 * most likely to need adjustment after the first hardware test. */
#include "mp4_mvd.h"
#include <string.h>
#include <stdlib.h>
#include <malloc.h>

static int            g_ready = 0;
static MVDSTD_Config  g_config;
static uint8_t       *g_inbuf = NULL;        /* linear buffer holding the NAL unit being fed */
static size_t         g_inbuf_cap = 0;
static int            g_w = 0, g_h = 0;
static int            g_nal_len = 4;
static int            g_frame_ready = 0;     /* a decoded frame is waiting to be rendered */

/* feed one raw NAL unit (no start code) to MVD. Returns the MVD status. */
static Result feed_nal(const uint8_t *nal, int len) {
    if (len <= 0 || (size_t)len > g_inbuf_cap) return -1;
    memcpy(g_inbuf, nal, len);
    GSPGPU_FlushDataCache(g_inbuf, len);
    MVDSTD_ProcessNALUnitOut out;
    Result r = mvdstdProcessVideoFrame(g_inbuf, len, 0, &out);
    /* PARAMSET = SPS/PPS accepted; INCOMPLETEPROCESSING = need more; else a frame is ready */
    if (MVD_CHECKNALUPROC_SUCCESS(r) && r != MVD_STATUS_PARAMSET && r != MVD_STATUS_INCOMPLETEPROCESSING)
        g_frame_ready = 1;
    return r;
}

int mp4_mvd_init(int w, int h, const uint8_t *avcc, int avcc_len, int nal_len_size) {
    g_frame_ready = 0;
    g_nal_len = (nal_len_size == 1 || nal_len_size == 2 || nal_len_size == 4) ? nal_len_size : 4;
    g_w = w; g_h = h;

    g_inbuf_cap = 1024 * 1024;                /* a NAL fits comfortably in 1 MB */
    g_inbuf = (uint8_t *)linearAlloc(g_inbuf_cap);
    if (!g_inbuf) return 0;

    if (R_FAILED(mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264, MVD_OUTPUT_RGB565,
                            MVD_DEFAULT_WORKBUF_SIZE, NULL))) {
        linearFree(g_inbuf); g_inbuf = NULL; return 0;
    }
    /* output goes to a top-screen framebuffer; physaddr_outdata0 is set per-frame in decode() */
    mvdstdGenerateDefaultConfig(&g_config, (u32)w, (u32)h, (u32)w, (u32)h, NULL, NULL, NULL);

    /* prime with SPS/PPS from the avcC box:
     *   [0]=version [1..3]=profile/compat/level [4]=0xFC|(lenSize-1)
     *   [5]=0xE0|numSPS, then numSPS * (u16 len + SPS), then numPPS(u8), then numPPS * (u16 len + PPS) */
    if (avcc && avcc_len > 6) {
        int p = 5;
        int nsps = avcc[p++] & 0x1F;
        for (int i = 0; i < nsps && p + 2 <= avcc_len; i++) {
            int l = (avcc[p] << 8) | avcc[p + 1]; p += 2;
            if (p + l > avcc_len) break;
            feed_nal(avcc + p, l); p += l;
        }
        if (p < avcc_len) {
            int npps = avcc[p++];
            for (int i = 0; i < npps && p + 2 <= avcc_len; i++) {
                int l = (avcc[p] << 8) | avcc[p + 1]; p += 2;
                if (p + l > avcc_len) break;
                feed_nal(avcc + p, l); p += l;
            }
        }
    }
    g_frame_ready = 0;   /* SPS/PPS don't produce a displayable frame */
    g_ready = 1;
    return 1;
}

void mp4_mvd_exit(void) {
    if (!g_ready) return;
    mvdstdExit();
    if (g_inbuf) { linearFree(g_inbuf); g_inbuf = NULL; }
    g_ready = 0;
}

int mp4_mvd_decode(const uint8_t *sample, int size, gfx3dSide_t side) {
    if (!g_ready) return 0;
    g_frame_ready = 0;

    /* an MP4 sample is one or more length-prefixed NAL units */
    int i = 0;
    while (i + g_nal_len <= size) {
        uint32_t nlen = 0;
        for (int b = 0; b < g_nal_len; b++) nlen = (nlen << 8) | sample[i + b];
        i += g_nal_len;
        if (nlen == 0 || i + (int)nlen > size) break;
        feed_nal(sample + i, (int)nlen);
        i += (int)nlen;
    }

    if (!g_frame_ready) return 0;

    u8 *fb = gfxGetFramebuffer(GFX_TOP, side, NULL, NULL);
    g_config.physaddr_outdata0 = osConvertVirtToPhys(fb);
    Result r = mvdstdRenderVideoFrame(&g_config, true);
    return (r == MVD_STATUS_OK) ? 1 : 0;
}
