#include "adpcm_moflex.h"

static const int16_t step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37,
    41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173,
    190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
    20350, 22385, 24623, 27086, 29794, 32767
};
static const int8_t index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8
};

typedef struct { int predictor, step_index; } AdpcmCh;

static inline int16_t expand_nibble(AdpcmCh *c, int nibble) {
    int step = step_table[c->step_index];
    int si   = c->step_index + index_table[nibble & 15];
    if (si < 0) si = 0; else if (si > 88) si = 88;
    int diff = ((2 * (nibble & 7) + 1) * step) >> 3;
    int pred = c->predictor + ((nibble & 8) ? -diff : diff);
    if (pred < -32768) pred = -32768; else if (pred > 32767) pred = 32767;
    c->predictor = pred;
    c->step_index = si;
    return (int16_t)pred;
}

int adpcm_moflex_decode(const uint8_t *buf, int size, int channels, int16_t *out) {
    if (channels < 1 || channels > 2) return -1;
    int header = 4 * channels;
    if (size < header) return 0;
    int frames = (size - header) * 2 / channels;   /* samples per channel */

    AdpcmCh ch[2];
    const uint8_t *p = buf;
    for (int c = 0; c < channels; c++) {
        int si = (int16_t)(p[0] | (p[1] << 8));    /* le16, sign-extended */
        int pr = (int16_t)(p[2] | (p[3] << 8));
        p += 4;
        if ((unsigned)si > 88u) return -1;
        ch[c].step_index = si;
        ch[c].predictor  = pr;
    }

    int subframes = frames / 256;
    for (int sf = 0; sf < subframes; sf++) {
        for (int c = 0; c < channels; c++) {
            int16_t *dst = out + (sf * 256) * channels + c;
            for (int n = 0; n < 256; n += 2) {
                int v = *p++;
                *dst = expand_nibble(&ch[c], v & 0x0F); dst += channels;
                *dst = expand_nibble(&ch[c], v >> 4);   dst += channels;
            }
        }
    }
    return subframes * 256;
}
