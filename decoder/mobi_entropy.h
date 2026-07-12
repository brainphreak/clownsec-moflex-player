/* Hand-written ARM entropy loop (mobi_entropy_asm.s) -- the coefficient run of
 * add_coefficients_impl. Gated behind mobi_opt 0x400. Mirrors the verified 0x200 C loop.
 * The struct offsets are hardcoded in the .s file: keep this layout in sync (all 4-byte fields). */
#ifndef MOBI_ENTROPY_H
#define MOBI_ENTROPY_H

#include <stdint.h>

typedef struct {
    const uint8_t *buffer;          /* 0  gb->buffer                          */
    int index;                      /* 4  gb->index (bit position, in/out)    */
    int size_in_bits;               /* 8  gb->size_in_bits (loop bound)        */
    int size_in_bits_plus8;         /* 12 gb->size_in_bits_plus8 (skip clamp)  */
    const void *rltab;              /* 16 rl_vlc[dct_tab_idx] (flat 12-bit)    */
    const void *rres;               /* 20 run_residue[dct_tab_idx] (uint8[])   */
    const void *qtab;               /* 24 s->qtab[size==8] (int[])             */
    const void *ztab;               /* 28 zigzag table (uint8[])               */
    int *mat;                       /* 32 coefficient matrix (int[64])          */
    int size;                       /* 36 4 or 8                               */
    unsigned rowmask;               /* 40 live-row mask (in: 1, out)           */
    int ac;                         /* 44 AC coefficient count (in: 0, out)    */
} MobiEntropyCtx;                   /* sizeof 48 (ARM32)                        */

/* Decode the coefficient run into c->mat/rowmask/ac and advance c->index.
 * Returns 0 on success (last-terminated or bits exhausted), 1 on pos overflow. */
extern int mobi_entropy_asm(MobiEntropyCtx *c);

#endif
