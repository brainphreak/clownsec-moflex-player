/* Host reference for mobi_entropy_asm (0x400): the SAME ctx-based lazy-refill loop the ARM .s
 * implements, in C. Lets the Mac verify the mobiclip.c 0x400 wiring + loop logic (bit-exact vs
 * 0x200/stock); the .s instruction translation itself is verified on-device via the gputest
 * checksum. Only compiled into the pc_verify build (the device build uses mobi_entropy_asm.s). */
#include "get_bits.h"
#include "vlc.h"
#include "mobi_entropy.h"
#include <string.h>

int mobi_entropy_asm(MobiEntropyCtx *c) {
    GetBitContext gb; memset(&gb, 0, sizeof gb);
    gb.buffer = c->buffer;
    gb.index = c->index;
    gb.size_in_bits = c->size_in_bits;
    gb.size_in_bits_plus8 = c->size_in_bits_plus8;
    gb.buffer_end = c->buffer + ((c->size_in_bits + 7) >> 3) + 8;
    GetBitContext *gbp = &gb;

    const VLCElem *rltab = c->rltab;
    const uint8_t *rres = c->rres;
    const int *qtab = c->qtab;
    const uint8_t *ztab = c->ztab;
    int *mat = c->mat;
    int size = c->size;
    unsigned rowmask = c->rowmask;
    int ac = c->ac;
    int rc = 0;

    OPEN_READER(re, gbp);
    #define VLC()   do { unsigned _ix = SHOW_UBITS(re, gbp, 12); n = rltab[_ix].sym; \
        SKIP_BITS(re, gbp, rltab[_ix].len); last = (n>>11)==1; run = (n>>5)&0x3F; level = n&0x1F; } while (0)
    #define U(d,u)  do { d = SHOW_UBITS(re, gbp, u); SKIP_BITS(re, gbp, u); } while (0)
    #define S(d,u)  do { d = SHOW_SBITS(re, gbp, u); SKIP_BITS(re, gbp, u); } while (0)

    for (int pos = 0; BITS_LEFT(re, gbp) > 0; pos++) {
        int qval, last, run, level, n, b;
        UPDATE_CACHE(re, gbp);
        VLC();
        if (level) { U(b,1); if (b) level = -level; }
        else {
            U(b,1);
            if (!b) { UPDATE_CACHE(re, gbp); VLC(); level += rres[(last?64:0)+run]; U(b,1); if (b) level = -level; }
            else {
                U(b,1);
                if (!b) { UPDATE_CACHE(re, gbp); VLC(); run += rres[128+(last?64:0)+level]; U(b,1); if (b) level = -level; }
                else { UPDATE_CACHE(re, gbp); U(last,1); U(run,6); S(level,12); }
            }
        }
        pos += run;
        if (pos >= size*size) { rc = 1; break; }
        qval = qtab[pos];
        mat[ztab[pos]] = qval * (unsigned)level;
        rowmask |= 1u << (ztab[pos] / size);
        if (ztab[pos]) ac++;
        if (last) break;
    }
    #undef VLC
    #undef U
    #undef S
    CLOSE_READER(re, gbp);

    c->index = gb.index; c->rowmask = rowmask; c->ac = ac;
    return rc;
}
