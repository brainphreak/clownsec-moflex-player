/* Helix AAC-LC wrapper -- see mp4_aac.h. */
#include "mp4_aac.h"
#include "aacdec.h"
#include <string.h>

/* standard AAC sampling-frequency table (ISO 14496-3), indexed by the ASC samplingFrequencyIndex */
static const int aac_sr_tab[16] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
    16000, 12000, 11025,  8000,  7350,     0,     0,     0
};

/* Pull (objectType, sampleRate, channels) out of the AudioSpecificConfig bitstream. Enough of a
 * parse for plain AAC-LC (objType 2): 5b objType, 4b freqIndex (15 => 24b explicit), 4b chanCfg. */
static int parse_asc(const uint8_t *asc, int len, int *obj, int *rate, int *ch) {
    if (!asc || len < 2) return 0;
    uint32_t bits = ((uint32_t)asc[0] << 24) | ((uint32_t)asc[1] << 16) |
                    (len > 2 ? (uint32_t)asc[2] << 8 : 0) | (len > 3 ? asc[3] : 0);
    int p = 0;
    int o = (bits >> (32 - 5)) & 0x1F; p += 5;
    int fi = (bits >> (32 - p - 4)) & 0xF; p += 4;
    int sr;
    if (fi == 15) { sr = (bits >> (32 - p - 24)) & 0xFFFFFF; p += 24; }  /* explicit rate (rare) */
    else          { sr = aac_sr_tab[fi]; }
    int cc = (bits >> (32 - p - 4)) & 0xF; p += 4;
    *obj = o; *rate = sr; *ch = cc;
    return 1;
}

int mp4_aac_open(Mp4Aac *a, const uint8_t *asc, int asc_len, int fb_rate, int fb_ch) {
    memset(a, 0, sizeof *a);
    int obj = 2, rate = fb_rate, ch = fb_ch;
    parse_asc(asc, asc_len, &obj, &rate, &ch);
    if (rate <= 0) rate = fb_rate;
    if (ch <= 0)   ch   = fb_ch;
    if (rate <= 0 || ch <= 0 || ch > 2) return 0;   /* Helix built for <=2 channels */
    if (obj != 2) return 0;                          /* AAC-LC only (matches AAC_PROFILE_LC) */

    HAACDecoder h = AACInitDecoder();
    if (!h) return 0;
    AACFrameInfo fi; memset(&fi, 0, sizeof fi);
    fi.nChans       = ch;
    fi.sampRateCore = rate;
    fi.profile      = AAC_PROFILE_LC;
    if (AACSetRawBlockParams(h, 0, &fi) != 0) { AACFreeDecoder(h); return 0; }

    a->h = h; a->rate = rate; a->channels = ch;
    return 1;
}

void mp4_aac_close(Mp4Aac *a) {
    if (a->h) { AACFreeDecoder((HAACDecoder)a->h); a->h = NULL; }
}

int mp4_aac_decode(Mp4Aac *a, const uint8_t *data, int size, short *pcm) {
    if (!a->h || size <= 0) return 0;
    unsigned char *in = (unsigned char *)data;
    int left = size;
    if (AACDecode((HAACDecoder)a->h, &in, &left, pcm) != 0) return 0;
    AACFrameInfo fi;
    AACGetLastFrameInfo((HAACDecoder)a->h, &fi);
    int nch = fi.nChans ? fi.nChans : a->channels;
    a->channels = nch;
    if (fi.sampRateOut) a->rate = fi.sampRateOut;
    return nch ? fi.outputSamps / nch : 0;
}
