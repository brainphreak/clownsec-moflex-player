/*
 * MobiClip Video decoder
 * Copyright (c) 2015-2016 Florian Nouwt
 * Copyright (c) 2017 Adib Surani
 * Copyright (c) 2020 Paul B Mahol
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <inttypes.h>

#include "libavutil/avassert.h"
#include "libavutil/mem.h"

#include "mobicompat.h"   /* replaces avcodec.h/codec_internal.h/decode.h/thread.h/bswapdsp.h */
#include "get_bits.h"
#include "golomb.h"
#include "mathops.h"
#include "mobi_entropy.h"   /* ARM asm coefficient-entropy loop (mobi_opt 0x400) */

/* --- lightweight phase profiler: accumulate ARM11 ticks per decode phase.
 * OFF by default (zero overhead in the shipped app); define MOBI_PROFILE to measure. --- */
#ifdef MOBI_PROFILE
#include <3ds.h>
#define PROF_NOW() svcGetSystemTick()
#else
#define PROF_NOW() 0ULL
#endif
/* 0=intra prediction, 1=intra residual (entropy+IDCT), 2=pframe residual, 3=motion comp */
uint64_t mobi_prof[8];
/* decode statistics (MOBI_PROFILE only): [0]=residual blocks [1]=DC-only blocks
 * [2]=sum of (highest live coeff row) [3]=sum of AC-coeff counts.  Reveals the sparse-IDCT ceiling. */
uint64_t mobi_stat[8];

/* runtime optimization switch so one binary can A/B decode paths:
 *   bit 0 = SWAR motion comp   bit 1 = IDCT zero-row skip
 *   bit 2 = DC-only block       bit 3 = cache prefetch (PLD)   bit 4 = UHADD8 motion comp
 *   bit 0x2000 = burst-prefetch whole ref block  bit 0x4000 = zero only mat[size*size]
 * DEFAULT 0 = stock decode (the shipped app relies on this; the gputest harness sets it per test).
 * Findings: 1 SWAR hurt, 2 idct-skip ~2%, 4 DC-only noise, 8 prefetch helps pure-decode ~5% but
 * REGRESSES in-player (fights Y2R for the memory bus), 16 UHADD8 bit-exact but 0 gain (memory-bound).
 *
 * 0x2000 BURST PREFETCH: TRIED 2026-07-13, MEASURED ON OLD 3DS, **DOES NOT WORK -- DO NOT RETRY.**
 * Theory was that bit 8's PLDs sit INSIDE the row loop one row ahead (~30 cycles of cover for a
 * ~100+ cycle FCRAM miss), so hoisting every row's PLD to the top of the block would put several
 * line fills in flight at once. Bit-exact, but on hardware it is SLOWER than the naive bit 8:
 * Kung Fury ms/frame base 20.02 / PLD8 alone 19.52 / burst alone 19.79; best 0x1A0E 17.78 vs
 * best+burst 0x3A06 18.10. Why: PLD on ARM11 is a DROPPABLE hint -- once the line-fill buffer
 * (~3 entries) is full the remaining ~30 PLDs per block are discarded, yet still cost issue slots,
 * and they are paid on every block including 4x4s whose row fits in ONE cache line. Motion-comp
 * prefetch has a hard ceiling of ~2.5% and bit 8 already collects it. Kept only as a negative result. */
int mobi_opt = 0;
/* Prefetch distance is FIXED at 1 row. Kept as a global only so the harnesses still link; changing
 * it does nothing now. MEASURED 2026-07-13 (distance sweep, Kung Fury ms/f): d=1 18.03, d=2 18.14,
 * d=3 18.25, d=4 18.37, d=6 18.61, prefetch OFF 18.29. MONOTONICALLY WORSE with distance -- and d=6 is
 * worse than no prefetch at all. Why: 65% of blocks are 4x4, so prefetching 3-6 rows ahead fetches
 * memory OUTSIDE the block entirely = wasted bus traffic + cache pollution. Motion-comp prefetch is
 * CLOSED: d=1 is optimal, worth ~1.4% over off, and that is the whole prize.
 * (Also: making this a runtime variable at all cost ~2% -- a global load + multiply in the inner
 * loop -- which is why the sites below are back to compile-time constants. Don't re-parameterize it.) */
int mobi_pfd = 1;
/* One counter per decoder failure path, so a freeze can be attributed to an EXACT line instead of
 * guessed at. Indices are assigned in source order; see mobi_err_name() in mobi_entropy.h consumers.
 * 0/1 qtab  2 asm-entropy  3 coef-pos  4 coef-pos(stock)  5 ?  6/7 intra  8 mv-bounds  9/10 null-ref
 * 11 pkt-size  12 pframe-coef */
int mobi_err[16];
/* substitution tiers actually taken: [0]=forward-walk hit  [1]=any-frame-in-pool  [2]=POOL EMPTY
 * (referenced the black frame we're building -- this is what makes the picture green). */
int mobi_sub[3];
static inline int mobi_fail(int id) { mobi_err[id]++; return AVERROR_INVALIDDATA; }
#define PF(p) do { if (mobi_opt & 8) __builtin_prefetch((p), 0, 1); } while (0)

/* SWAR (SIMD-within-a-register): process 4 packed bytes per 32-bit op. The >>1 + mask
 * keeps each byte independent (no inter-byte carry, since 127+127 = 254 < 256). */
#include <string.h>
static inline uint32_t ld4(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline void     st4(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
#define AVG2(a, b) ((((a) >> 1) & 0x7F7F7F7Fu) + (((b) >> 1) & 0x7F7F7F7Fu))

/* ARMv6 media instruction: parallel per-byte (a+b)>>1 (rounded). Correct back to mobiclip's
 * truncated (a>>1)+(b>>1): they differ by 1 only when both bytes are odd, and the UHADD8
 * result is >=1 there, so a plain subtract can't underflow -> bit-identical. */
static inline uint32_t u8avg(uint32_t a, uint32_t b) {
#if defined(__arm__)
    uint32_t r;
    __asm__("uhadd8 %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    return r - ((a & b) & 0x01010101u);
#else
    /* portable per-byte (a>>1)+(b>>1) for the host verify build; bit-identical to the UHADD8 path */
    return ((a >> 1) & 0x7f7f7f7fu) + ((b >> 1) & 0x7f7f7f7fu);
#endif
}

/* hand-written ARM assembly half-pel motion comp (mc_asm.s); aligned src + width%4==0 only. */
extern void mc_havg_a(uint8_t *dst, const uint8_t *src, int w, int h, int ds, int ss);
extern void mc_vavg_a(uint8_t *dst, const uint8_t *src, int w, int h, int ds, int ss);
extern void mc_diag_a(uint8_t *dst, const uint8_t *src, int w, int h, int ds, int ss);

/* load the 4 bytes at p (any alignment) using only ALIGNED word loads + a barrel shift --
 * no unaligned access (its penalty is what killed the naive SWAR), no memcpy. */
static inline uint32_t load_uw(const uint8_t *p) {
    uintptr_t a = (uintptr_t)p;
    const uint32_t *aw = (const uint32_t *)(a & ~(uintptr_t)3);
    unsigned sh = ((unsigned)a & 3u) * 8;
    if (sh == 0) return aw[0];
    return (aw[0] >> sh) | (aw[1] << (32 - sh));
}

/* clamp lookup table (ported from the official decoder: it fuses the residual add + the
 * 0..255 saturate into one load, no branch). g_clamp[CLAMP_OFF + v] == clip_uint8(v). */
#define CLAMP_OFF  1024
#define CLAMP_SIZE (CLAMP_OFF * 2 + 256)
static uint8_t g_clamp[CLAMP_SIZE];
static int g_clamp_ready = 0;
static void clamp_init(void) {
    for (int i = 0; i < CLAMP_SIZE; i++) {
        int v = i - CLAMP_OFF;
        g_clamp[i] = v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
    }
    g_clamp_ready = 1;
}

#define MOBI_RL_VLC_BITS 12
#define MOBI_MV_VLC_BITS 6

static const uint8_t zigzag4x4_tab[] =
{
    0x00, 0x04, 0x01, 0x02, 0x05, 0x08, 0x0C, 0x09, 0x06, 0x03, 0x07, 0x0A,
    0x0D, 0x0E, 0x0B, 0x0F
};

static const uint8_t quant4x4_tab[][16] =
{
    { 10, 13, 13, 10, 16, 10, 13, 13, 13, 13, 16, 10, 16, 13, 13, 16 },
    { 11, 14, 14, 11, 18, 11, 14, 14, 14, 14, 18, 11, 18, 14, 14, 18 },
    { 13, 16, 16, 13, 20, 13, 16, 16, 16, 16, 20, 13, 20, 16, 16, 20 },
    { 14, 18, 18, 14, 23, 14, 18, 18, 18, 18, 23, 14, 23, 18, 18, 23 },
    { 16, 20, 20, 16, 25, 16, 20, 20, 20, 20, 25, 16, 25, 20, 20, 25 },
    { 18, 23, 23, 18, 29, 18, 23, 23, 23, 23, 29, 18, 29, 23, 23, 29 },
};

static const uint8_t quant8x8_tab[][64] =
{
    { 20, 19, 19, 25, 18, 25, 19, 24, 24, 19, 20, 18, 32, 18, 20, 19, 19, 24, 24, 19, 19, 25, 18, 25, 18, 25, 18, 25, 19, 24, 24, 19,
      19, 24, 24, 19, 18, 32, 18, 20, 18, 32, 18, 24, 24, 19, 19, 24, 24, 18, 25, 18, 25, 18, 19, 24, 24, 19, 18, 32, 18, 24, 24, 18,},
    { 22, 21, 21, 28, 19, 28, 21, 26, 26, 21, 22, 19, 35, 19, 22, 21, 21, 26, 26, 21, 21, 28, 19, 28, 19, 28, 19, 28, 21, 26, 26, 21,
      21, 26, 26, 21, 19, 35, 19, 22, 19, 35, 19, 26, 26, 21, 21, 26, 26, 19, 28, 19, 28, 19, 21, 26, 26, 21, 19, 35, 19, 26, 26, 19,},
    { 26, 24, 24, 33, 23, 33, 24, 31, 31, 24, 26, 23, 42, 23, 26, 24, 24, 31, 31, 24, 24, 33, 23, 33, 23, 33, 23, 33, 24, 31, 31, 24,
      24, 31, 31, 24, 23, 42, 23, 26, 23, 42, 23, 31, 31, 24, 24, 31, 31, 23, 33, 23, 33, 23, 24, 31, 31, 24, 23, 42, 23, 31, 31, 23,},
    { 28, 26, 26, 35, 25, 35, 26, 33, 33, 26, 28, 25, 45, 25, 28, 26, 26, 33, 33, 26, 26, 35, 25, 35, 25, 35, 25, 35, 26, 33, 33, 26,
      26, 33, 33, 26, 25, 45, 25, 28, 25, 45, 25, 33, 33, 26, 26, 33, 33, 25, 35, 25, 35, 25, 26, 33, 33, 26, 25, 45, 25, 33, 33, 25,},
    { 32, 30, 30, 40, 28, 40, 30, 38, 38, 30, 32, 28, 51, 28, 32, 30, 30, 38, 38, 30, 30, 40, 28, 40, 28, 40, 28, 40, 30, 38, 38, 30,
      30, 38, 38, 30, 28, 51, 28, 32, 28, 51, 28, 38, 38, 30, 30, 38, 38, 28, 40, 28, 40, 28, 30, 38, 38, 30, 28, 51, 28, 38, 38, 28,},
    { 36, 34, 34, 46, 32, 46, 34, 43, 43, 34, 36, 32, 58, 32, 36, 34, 34, 43, 43, 34, 34, 46, 32, 46, 32, 46, 32, 46, 34, 43, 43, 34,
      34, 43, 43, 34, 32, 58, 32, 36, 32, 58, 32, 43, 43, 34, 34, 43, 43, 32, 46, 32, 46, 32, 34, 43, 43, 34, 32, 58, 32, 43, 43, 32,},
};

static const uint8_t block4x4_coefficients_tab[] =
{
    15, 0, 2, 1, 4, 8, 12, 3, 11, 13, 14, 7, 10, 5, 9, 6,
};

static const uint8_t pframe_block4x4_coefficients_tab[] =
{
    0, 4, 1, 8, 2, 12, 3, 5, 10, 15, 7, 13, 14, 11, 9, 6,
};

static const uint8_t block8x8_coefficients_tab[] =
{
    0x00, 0x1F, 0x3F, 0x0F, 0x08, 0x04, 0x02, 0x01, 0x0B, 0x0E, 0x1B, 0x0D,
    0x03, 0x07, 0x0C, 0x17, 0x1D, 0x0A, 0x1E, 0x05, 0x10, 0x2F, 0x37, 0x3B,
    0x13, 0x3D, 0x3E, 0x09, 0x1C, 0x06, 0x15, 0x1A, 0x33, 0x11, 0x12, 0x14,
    0x18, 0x20, 0x3C, 0x35, 0x19, 0x16, 0x3A, 0x30, 0x31, 0x32, 0x27, 0x34,
    0x2B, 0x2D, 0x39, 0x38, 0x23, 0x36, 0x2E, 0x21, 0x25, 0x22, 0x24, 0x2C,
    0x2A, 0x28, 0x29, 0x26,
};

static const uint8_t pframe_block8x8_coefficients_tab[] =
{
    0x00, 0x0F, 0x04, 0x01, 0x08, 0x02, 0x0C, 0x03, 0x05, 0x0A, 0x0D, 0x07, 0x0E, 0x0B, 0x1F, 0x09,
    0x06, 0x10, 0x3F, 0x1E, 0x17, 0x1D, 0x1B, 0x1C, 0x13, 0x18, 0x1A, 0x12, 0x11, 0x14, 0x15, 0x20,
    0x2F, 0x16, 0x19, 0x37, 0x3D, 0x3E, 0x3B, 0x3C, 0x33, 0x35, 0x21, 0x24, 0x22, 0x28, 0x23, 0x2C,
    0x30, 0x27, 0x2D, 0x25, 0x3A, 0x2B, 0x2E, 0x2A, 0x31, 0x34, 0x38, 0x32, 0x29, 0x26, 0x39, 0x36
};

static const uint8_t run_residue[2][256] =
{
    {
       12,  6,  4,  3,  3,  3,  3,  2,  2,  2,  2,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  0,  0,  0,  0,  0,
        0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
        3,  2,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
        1,  1,  1,  1,  1,  1,  1,  1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
        1, 27, 11,  7,  3,  2,  2,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
        1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
        1, 41,  2,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
        1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    },
    {
       27, 10,  5,  4,  3,  3,  3,  3,  2,  2,  1,  1,  1,  1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
        0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
        8,  3,  2,  2,  2,  2,  2,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
        0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
        1, 15, 10,  8,  4,  3,  2,  2,  2,  2,  2,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
        1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
        1, 21,  7,  2,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
        1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    },
};

static const uint8_t bits0[] = {
     9, 11, 11, 11, 11, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 11, 11, 11, 11, 11, 11, 11, 11, 12, 12, 12, 12, 12,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,  7, 10, 10,  9,
     9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,
     9,  9,  9,  9,  9,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,
     8,  8,  8,  7,  7,  7,  7,  7,  7,  7,  7,  6,  6,  6,  6,
     6,  6,  6,  6,  6,  6,  5,  5,  5,  4,  2,  3,  4,  4,
};

static const uint16_t syms0[] = {
    0x0, 0x822, 0x803, 0xB, 0xA, 0xB81, 0xB61, 0xB41, 0xB21, 0x122,
    0x102, 0xE2, 0xC2, 0xA2, 0x63, 0x43, 0x24, 0xC, 0x25, 0x2E1, 0x301,
    0xBA1, 0xBC1, 0xBE1, 0xC01, 0x26, 0x44, 0x83, 0xA3, 0xC3, 0x142,
    0x321, 0x341, 0xC21, 0xC41, 0xC61, 0xC81, 0xCA1, 0xCC1, 0xCE1, 0xD01,
    0x0, 0x9, 0x8, 0xB01, 0xAE1, 0xAC1, 0xAA1, 0xA81, 0xA61, 0xA41, 0xA21,
    0x802, 0x2C1, 0x2A1, 0x281, 0x261, 0x241, 0x221, 0x201, 0x1E1, 0x82,
    0x62, 0x7, 0x6, 0xA01, 0x9E1, 0x9C1, 0x9A1, 0x981, 0x961, 0x941, 0x921,
    0x1C1, 0x1A1, 0x42, 0x23, 0x5, 0x901, 0x8E1, 0x8C1, 0x8A1, 0x181, 0x161,
    0x141, 0x4, 0x881, 0x861, 0x841, 0x821, 0x121, 0x101, 0xE1, 0xC1, 0x22,
    0x3, 0xA1, 0x81, 0x61, 0x801, 0x1, 0x21, 0x41, 0x2,
};

static const uint16_t syms1[] = {
    0x0, 0x807, 0x806, 0x16, 0x15, 0x842, 0x823, 0x805, 0x1A1, 0xA3, 0x102, 0x83,
    0x64, 0x44, 0x27, 0x14, 0x13, 0x17, 0x18, 0x28, 0x122, 0x862, 0x882, 0x9E1, 0xA01,
    0x19, 0x1A, 0x1B, 0x29, 0xC3, 0x2A, 0x45, 0xE3, 0x1C1, 0x808, 0x8A2, 0x8C2, 0xA21,
    0xA41, 0xA61, 0xA81, 0x0, 0x12, 0x11, 0x9C1, 0x9A1, 0x981, 0x961, 0x941, 0x822, 0x804,
    0x181, 0x161, 0xE2, 0xC2, 0xA2, 0x63, 0x43, 0x26, 0x25, 0x10, 0x82, 0xF, 0xE, 0xD, 0x901,
    0x8E1, 0x8C1, 0x803, 0x141, 0x121, 0x101, 0x921, 0x62, 0x24, 0xC, 0xB, 0xA, 0x881, 0x861,
    0xC1, 0x8A1, 0xE1, 0x42, 0x23, 0x9, 0x802, 0xA1, 0x841, 0x821, 0x81, 0x61, 0x8, 0x7, 0x22,
    0x6, 0x41, 0x5, 0x4, 0x801, 0x1, 0x2, 0x21, 0x3,
};

static const uint8_t mv_len[16] =
{
    10, 8, 8, 7, 8, 8, 8, 7, 8, 8, 8, 7, 7, 7, 7, 6,
};

static const uint8_t mv_bits[2][16][10] =
{
    {
        { 2, 3, 3, 5, 5, 4, 4, 5, 5, 2 },
        { 2, 3, 4, 4, 3, 4, 4, 2 },
        { 3, 4, 4, 2, 4, 4, 3, 2 },
        { 1, 3, 4, 5, 5, 3, 3 },
        { 2, 4, 4, 3, 3, 4, 4, 2 },
        { 2, 3, 4, 4, 4, 4, 3, 2 },
        { 2, 3, 4, 4, 4, 4, 3, 2 },
        { 2, 2, 3, 4, 5, 5, 2 },
        { 2, 3, 4, 4, 3, 4, 4, 2 },
        { 2, 4, 4, 3, 4, 4, 3, 2 },
        { 2, 3, 3, 5, 5, 4, 3, 2 },
        { 2, 3, 4, 4, 3, 3, 2 },
        { 1, 4, 4, 3, 3, 4, 4 },
        { 2, 3, 4, 4, 3, 3, 2 },
        { 2, 3, 4, 4, 3, 3, 2 },
        { 3, 3, 2, 2, 3, 3 },
    },
    {
        { 3, 4, 5, 5, 3, 5, 6, 6, 4, 1 },
        { 2, 3, 4, 5, 5, 2, 3, 3 },
        { 2, 4, 4, 3, 3, 4, 4, 2 },
        { 1, 4, 4, 3, 4, 4, 3 },
        { 3, 3, 2, 4, 5, 5, 3, 2 },
        { 3, 4, 4, 3, 3, 3, 3, 2 },
        { 1, 3, 3, 4, 4, 4, 5, 5 },
        { 1, 4, 4, 3, 3, 4, 4 },
        { 2, 4, 4, 3, 3, 4, 4, 2 },
        { 1, 3, 3, 4, 4, 4, 5, 5 },
        { 2, 3, 4, 4, 4, 4, 3, 2 },
        { 2, 3, 3, 4, 4, 3, 2 },
        { 1, 4, 4, 3, 3, 4, 4 },
        { 1, 4, 4, 3, 3, 4, 4 },
        { 2, 3, 3, 4, 4, 3, 2 },
        { 2, 3, 3, 3, 3, 2 },
    }
};

static const uint8_t mv_syms[2][16][10] =
{
    {
        { 1, 8, 9, 4, 3, 2, 7, 5, 6, 0 },
        { 0, 9, 5, 4, 2, 3, 8, 1 },
        { 3, 9, 5, 0, 4, 8, 2, 1 },
        { 1, 3, 4, 8, 5, 2, 0 },
        { 0, 5, 4, 8, 2, 3, 9, 1 },
        { 0, 3, 5, 9, 4, 8, 2, 1 },
        { 0, 3, 9, 5, 8, 4, 2, 1 },
        { 0, 2, 3, 4, 8, 5, 1 },
        { 0, 3, 8, 4, 2, 5, 9, 1 },
        { 2, 8, 9, 3, 5, 4, 0, 1 },
        { 0, 4, 3, 8, 9, 5, 2, 1 },
        { 0, 4, 8, 5, 3, 2, 1 },
        { 1, 9, 4, 2, 0, 5, 3 },
        { 2, 4, 9, 5, 3, 0, 1 },
        { 0, 4, 9, 5, 3, 2, 1 },
        { 5, 4, 1, 0, 3, 2 },
    },
    {
        { 8, 2, 3, 6, 1, 7, 5, 4, 9, 0 },
        { 9, 2, 3, 5, 4, 1, 8, 0 },
        { 0, 5, 4, 2, 9, 3, 8, 1 },
        { 1, 5, 4, 2, 8, 3, 0 },
        { 2, 9, 8, 3, 5, 4, 0, 1 },
        { 3, 5, 4, 2, 9, 8, 0, 1 },
        { 1, 2, 0, 9, 8, 3, 5, 4 },
        { 1, 8, 5, 2, 0, 4, 3 },
        { 0, 5, 4, 2, 8, 3, 9, 1 },
        { 1, 2, 0, 9, 8, 3, 5, 4 },
        { 0, 3, 9, 8, 5, 4, 2, 1 },
        { 0, 4, 3, 8, 5, 2, 1 },
        { 1, 5, 4, 2, 0, 9, 3 },
        { 1, 9, 5, 2, 0, 4, 3 },
        { 0, 5, 3, 9, 4, 2, 1 },
        { 0, 4, 5, 3, 2, 1 },
    }
};

typedef struct BlockXY {
    int w, h;
    int ax, ay;
    int x, y;
    int size;
    uint8_t *block;
    int linesize;
} BlockXY;

typedef struct MotionXY {
    int x, y;
} MotionXY;

typedef struct MobiClipContext {
    AVFrame *pic[6];

    int current_pic;
    int moflex;
    int dct_tab_idx;
    int quantizer;

    GetBitContext gb;

    uint8_t *bitstream;
    int bitstream_size;

    int     qtab[2][64];
    uint8_t pre[32];
    MotionXY *motion;
    int     motion_size;

    BswapDSPContext bdsp;
} MobiClipContext;

static const VLCElem *rl_vlc[2];
static const VLCElem *mv_vlc[2][16];

static av_cold void mobiclip_init_static(void)
{
    static VLCElem vlc_buf[(2 << MOBI_RL_VLC_BITS) + (2 * 16 << MOBI_MV_VLC_BITS)];
    VLCInitState state =VLC_INIT_STATE(vlc_buf);

    for (int i = 0; i < 2; i++) {
        rl_vlc[i] =
            ff_vlc_init_tables_from_lengths(&state, MOBI_RL_VLC_BITS, 104,
                                            bits0, sizeof(*bits0),
                                            i ? syms1 : syms0, sizeof(*syms0), sizeof(*syms0),
                                            0, 0);
        for (int j = 0; j < 16; j++) {
            mv_vlc[i][j] =
                ff_vlc_init_tables_from_lengths(&state, MOBI_MV_VLC_BITS, mv_len[j],
                                                mv_bits[i][j], sizeof(*mv_bits[i][j]),
                                                mv_syms[i][j], sizeof(*mv_syms[i][j]), sizeof(*mv_syms[i][j]),
                                                0, 0);
        }
    }
}

static av_cold int mobiclip_init(AVCodecContext *avctx)
{
    static AVOnce init_static_once = AV_ONCE_INIT;
    MobiClipContext *s = avctx->priv_data;

    if (avctx->width & 15 || avctx->height & 15) {
        av_log(avctx, AV_LOG_ERROR, "width/height not multiple of 16\n");
        return mobi_fail(0);
    }

    ff_bswapdsp_init(&s->bdsp);

    avctx->pix_fmt = AV_PIX_FMT_YUV420P;

    s->motion = av_calloc(avctx->width / 16 + 3, sizeof(MotionXY));
    if (!s->motion)
        return AVERROR(ENOMEM);
    s->motion_size = (avctx->width / 16 + 3) * sizeof(MotionXY);

    for (int i = 0; i < 6; i++) {
        s->pic[i] = av_frame_alloc();
        if (!s->pic[i])
            return AVERROR(ENOMEM);
    }

    ff_thread_once(&init_static_once, mobiclip_init_static);

    return 0;
}

static int setup_qtables(AVCodecContext *avctx, int64_t quantizer)
{
    MobiClipContext *s = avctx->priv_data;
    int qx, qy;

    if (quantizer < 12 || quantizer > 161)
        return mobi_fail(1);

    s->quantizer = quantizer;

    qx = quantizer % 6;
    qy = quantizer / 6;

    for (int i = 0; i < 16; i++)
        s->qtab[0][i] = quant4x4_tab[qx][i] << qy;

    for (int i = 0; i < 64; i++)
        s->qtab[1][i] = quant8x8_tab[qx][i] << (qy - 2);

    for (int i = 0; i < 20; i++)
        s->pre[i] = 9;

    return 0;
}

static void inverse4(unsigned *rs)
{
    unsigned a = rs[0] + rs[2];
    unsigned b = rs[0] - rs[2];
    unsigned c = rs[1] + ((int)rs[3] >> 1);
    unsigned d = ((int)rs[1] >> 1) - rs[3];

    rs[0] = a + c;
    rs[1] = b + d;
    rs[2] = b - d;
    rs[3] = a - c;
}

static void idct(int *arr, int size)
{
    int e, f, g, h;
    unsigned x3, x2, x1, x0;
    int tmp[4];

    if (size == 4) {
        inverse4(arr);
        return;
    }

    tmp[0] = arr[0];
    tmp[1] = arr[2];
    tmp[2] = arr[4];
    tmp[3] = arr[6];

    inverse4(tmp);

    e = (unsigned)arr[7] + arr[1] - arr[3] - (arr[3] >> 1);
    f = (unsigned)arr[7] - arr[1] + arr[5] + (arr[5] >> 1);
    g = (unsigned)arr[5] - arr[3] - arr[7] - (arr[7] >> 1);
    h = (unsigned)arr[5] + arr[3] + arr[1] + (arr[1] >> 1);
    x3 = (unsigned)g + (h >> 2);
    x2 = (unsigned)e + (f >> 2);
    x1 = (e >> 2) - (unsigned)f;
    x0 = (unsigned)h - (g >> 2);

    arr[0] = tmp[0] + x0;
    arr[1] = tmp[1] + x1;
    arr[2] = tmp[2] + x2;
    arr[3] = tmp[3] + x3;
    arr[4] = tmp[3] - x3;
    arr[5] = tmp[2] - x2;
    arr[6] = tmp[1] - x1;
    arr[7] = tmp[0] - x0;
}

/* Sparse IDCT: same result as idct() when arr[nlive..size-1] are all zero, but it skips the
 * provably-zero terms. Bit-exact (the skipped terms multiply zeros); the arithmetic/casts mirror
 * idct()/inverse4() exactly so overflow behaviour matches. Used by the column pass, where every
 * transposed row shares the coefficient rows' sparsity (nlive = highest live row + 1). */
static void idct_sparse(int *arr, int size, int nlive)
{
    if (nlive <= 1) {                       /* DC only -> every output equals arr[0] */
        int v = arr[0];
        for (int i = 1; i < size; i++) arr[i] = v;
        return;
    }
    if (size == 4) { inverse4((unsigned *)arr); return; }   /* 4-pt is already cheap */
    if (nlive <= 4) {                       /* arr[4..7] == 0 */
        int e, f, g, h;
        unsigned x3, x2, x1, x0;
        int tmp[4];
        tmp[0] = arr[0]; tmp[1] = arr[2]; tmp[2] = arr[4]; tmp[3] = arr[6];   /* arr[4],arr[6]=0 */
        inverse4((unsigned *)tmp);
        e = (unsigned)0 + arr[1] - arr[3] - (arr[3] >> 1);   /* arr7=0 */
        f = (unsigned)0 - arr[1];                            /* arr7=arr5=0 */
        g = (unsigned)0 - arr[3];                            /* arr5=arr7=0 */
        h = (unsigned)0 + arr[3] + arr[1] + (arr[1] >> 1);   /* arr5=0 */
        x3 = (unsigned)g + (h >> 2);
        x2 = (unsigned)e + (f >> 2);
        x1 = (e >> 2) - (unsigned)f;
        x0 = (unsigned)h - (g >> 2);
        arr[0] = tmp[0] + x0; arr[1] = tmp[1] + x1; arr[2] = tmp[2] + x2; arr[3] = tmp[3] + x3;
        arr[4] = tmp[3] - x3; arr[5] = tmp[2] - x2; arr[6] = tmp[1] - x1; arr[7] = tmp[0] - x0;
        return;
    }
    idct(arr, size);                        /* 5..8 live -> full */
}

static void read_run_encoding(AVCodecContext *avctx,
                              int *last, int *run, int *level)
{
    MobiClipContext *s = avctx->priv_data;
    GetBitContext *gb = &s->gb;
    int n = get_vlc2(gb, rl_vlc[s->dct_tab_idx], MOBI_RL_VLC_BITS, 1);

    *last = (n >> 11) == 1;
    *run  = (n >> 5) & 0x3F;
    *level = n & 0x1F;
}

static int add_coefficients_impl(AVCodecContext *avctx, AVFrame *frame,
                            int bx, int by, int size, int plane)
{
    MobiClipContext *s = avctx->priv_data;
    GetBitContext *gb = &s->gb;
    /* mat[] only ever spans size*size (every access is mat[y*size+x], x,y < size), but the plain
     * `= {0}` initializer zeroes all 64 ints -- 256 bytes -- even for a 4x4 block that needs 64.
     * P-frames split 8x8 into four 4x4s (add_pframe_coefficients), so 4x4 is the COMMON block and
     * three quarters of that zeroing is dead. It is also invisible to the profiler: _te = PROF_NOW()
     * below starts the entropy bucket AFTER this line, so this cost lands in no bucket at all.
     * Bit 0x4000 zeroes only the live square. Bit-exact by construction. */
    int mat[64];
    if (mobi_opt & 0x4000) memset(mat, 0, (size_t)size * size * sizeof(int));
    else                   memset(mat, 0, sizeof(mat));
    const uint8_t *ztab = size == 8 ? ff_zigzag_direct : zigzag4x4_tab;
    const int *qtab = s->qtab[size == 8];
    uint8_t *dst = frame->data[plane] + by * frame->linesize[plane] + bx;
    unsigned rowmask = 1;   /* row 0 is always live (mat[0] += 32 below) */
    unsigned colmask = 1;   /* col 0 always live; for the row-pass sparse IDCT (mobi_opt 0x1000) */
    int ac = 0;             /* number of AC (non-DC) coefficients */
    if ((mobi_opt & 32) && !g_clamp_ready) clamp_init();
    uint64_t _te = PROF_NOW();

    if (mobi_opt & 0x400) {
        /* --- ARM assembly entropy loop (mobi_entropy_asm.s): same work as 0x200, hand-scheduled with
         * the reader + coefficient state pinned in registers. Verified on-device via the gputest
         * checksum against 0x200. --- */
        MobiEntropyCtx c;
        c.buffer = gb->buffer; c.index = gb->index;
        c.size_in_bits = gb->size_in_bits; c.size_in_bits_plus8 = gb->size_in_bits_plus8;
        c.rltab = rl_vlc[s->dct_tab_idx]; c.rres = run_residue[s->dct_tab_idx];
        c.qtab = qtab; c.ztab = ztab; c.mat = mat; c.size = size;
        c.rowmask = 1; c.ac = 0;
        int rc = mobi_entropy_asm(&c);
        gb->index = c.index; rowmask = c.rowmask; ac = c.ac;
        colmask = (size == 8) ? 0xFFu : 0x0Fu;   /* asm path doesn't track colmask -> row-sparse = full (safe) */
        if (rc) return mobi_fail(2);
    } else if (mobi_opt & 0x200) {
        /* --- opt entropy loop (mobi_opt 0x200): hold the bit-reader cache/index in locals across the
         * whole coefficient run and inline get_vlc2 (rl_vlc is a FLAT 12-bit table, max_depth 1 -> one
         * lookup) + get_bits, refilling the 32-bit cache LAZILY -- only at points where more than
         * MIN_CACHE_BITS(25) could be consumed since the last refill, not before every read. The reads
         * happen in the same order over the same bits, so this is BIT-EXACT vs stock (verified on host).
         * This is the structure the ARM assembly (0x400) mirrors (cache + bit-budget in registers). --- */
        const VLCElem *rltab = rl_vlc[s->dct_tab_idx];
        const uint8_t *rres = run_residue[s->dct_tab_idx];
        OPEN_READER(re, gb);
        /* reads with NO refill (caller guarantees <=25 bits consumed since the last UPDATE_CACHE) */
        #define RRE_VLC()  do { unsigned _ix = SHOW_UBITS(re, gb, MOBI_RL_VLC_BITS); \
            n = rltab[_ix].sym; SKIP_BITS(re, gb, rltab[_ix].len); \
            last = (n >> 11) == 1; run = (n >> 5) & 0x3F; level = n & 0x1F; } while (0)
        #define RRE_U(dst, u) do { dst = SHOW_UBITS(re, gb, (u)); SKIP_BITS(re, gb, (u)); } while (0)
        #define RRE_S(dst, u) do { dst = SHOW_SBITS(re, gb, (u)); SKIP_BITS(re, gb, (u)); } while (0)

        for (int pos = 0; BITS_LEFT(re, gb) > 0; pos++) {
            int qval, last, run, level, n, b;

            UPDATE_CACHE(re, gb);                     /* >=25 valid bits */
            RRE_VLC();                                /* VLC <=12 -> >=13 left */
            if (level) {
                RRE_U(b, 1); if (b) level = -level;   /* common path: <=13 bits, ONE refill/coeff */
            } else {
                RRE_U(b, 1);                          /* <=13 bits used */
                if (!b) {
                    UPDATE_CACHE(re, gb);             /* refill before the 2nd VLC (would exceed 25) */
                    RRE_VLC();
                    level += rres[(last ? 64 : 0) + run];
                    RRE_U(b, 1); if (b) level = -level;
                } else {
                    RRE_U(b, 1);                      /* <=14 bits used */
                    if (!b) {
                        UPDATE_CACHE(re, gb);         /* refill before the 2nd VLC */
                        RRE_VLC();
                        run += rres[128 + (last ? 64 : 0) + level];
                        RRE_U(b, 1); if (b) level = -level;
                    } else {
                        UPDATE_CACHE(re, gb);         /* refill before the 19-bit escape */
                        RRE_U(last, 1);
                        RRE_U(run, 6);
                        RRE_S(level, 12);
                    }
                }
            }

            pos += run;
            if (pos >= size * size) { CLOSE_READER(re, gb); return mobi_fail(3); }
            qval = qtab[pos];
            mat[ztab[pos]] = qval * (unsigned)level;
            rowmask |= 1u << (ztab[pos] / size);
            colmask |= 1u << (ztab[pos] % size);
            if (ztab[pos]) ac++;

            if (last) break;
        }
        CLOSE_READER(re, gb);
        #undef RRE_VLC
        #undef RRE_U
        #undef RRE_S
    } else
    for (int pos = 0; get_bits_left(gb) > 0; pos++) {
        int qval, last, run, level;

        read_run_encoding(avctx, &last, &run, &level);

        if (level) {
            if (get_bits1(gb))
                level = -level;
        } else if (!get_bits1(gb)) {
            read_run_encoding(avctx, &last, &run, &level);
            level += run_residue[s->dct_tab_idx][(last ? 64 : 0) + run];
            if (get_bits1(gb))
                level = -level;
        } else if (!get_bits1(gb)) {
            read_run_encoding(avctx, &last, &run, &level);
            run += run_residue[s->dct_tab_idx][128 + (last ? 64 : 0) + level];
            if (get_bits1(gb))
                level = -level;
        } else {
            last  = get_bits1(gb);
            run   = get_bits(gb, 6);
            level = get_sbits(gb, 12);
        }

        pos += run;
        if (pos >= size * size)
            return mobi_fail(4);
        qval = qtab[pos];
        mat[ztab[pos]] = qval *(unsigned)level;
        rowmask |= 1u << (ztab[pos] / size);
        colmask |= 1u << (ztab[pos] % size);
        if (ztab[pos]) ac++;                  /* count AC (non-DC) coefficients */

        if (last)
            break;
    }

    mobi_prof[5] += PROF_NOW() - _te;         /* bucket 5 = coefficient entropy decode */
    uint64_t _tt = PROF_NOW();

#ifdef MOBI_PROFILE
    { int mr = 0; for (int r = size - 1; r >= 1; r--) if (rowmask & (1u << r)) { mr = r; break; }
      mobi_stat[0]++; if (ac == 0) mobi_stat[1]++; mobi_stat[2] += mr; mobi_stat[3] += ac; }
#endif

    mat[0] += 32;
    if ((mobi_opt & 4) && ac == 0) {          /* DC-only block -> uniform delta, skip all IDCTs */
        int t[8] = { 0 }; t[0] = mat[0]; idct(t, size);
        int u[8] = { 0 }; u[0] = t[0];        idct(u, size);
        int d = u[0] >> 6;
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) dst[x] = av_clip_uint8(dst[x] + d);
            dst += frame->linesize[plane];
        }
        mobi_prof[4] += PROF_NOW() - _tt;
        return 0;
    }
    /* row pass: sparse variant (0x1000) skips zero coeff columns -- nlive = highest live column + 1 */
    int nlive_row = size;
    if (mobi_opt & 0x1000) {
        nlive_row = 1;
        for (int cc = size - 1; cc >= 1; cc--) if (colmask & (1u << cc)) { nlive_row = cc + 1; break; }
    }
    if (mobi_opt & 2) {                       /* skip the row IDCT for all-zero rows */
        for (int y = 0; y < size; y++)
            if (rowmask & (1u << y))
                (mobi_opt & 0x1000) ? idct_sparse(&mat[y * size], size, nlive_row) : idct(&mat[y * size], size);
    } else {
        for (int y = 0; y < size; y++)
            (mobi_opt & 0x1000) ? idct_sparse(&mat[y * size], size, nlive_row) : idct(&mat[y * size], size);
    }

    /* column pass: after the transpose, every column-IDCT input has the coefficient rows' sparsity
     * (nonzero only where a row was live), so nlive = highest live row + 1 -> a sparse IDCT. */
    int nlive_col = size;
    if (mobi_opt & 0x800) {
        nlive_col = 1;
        for (int r = size - 1; r >= 1; r--) if (rowmask & (1u << r)) { nlive_col = r + 1; break; }
    }
    for (int y = 0; y < size; y++) {
        for (int x = y + 1; x < size; x++) {
            int a = mat[x * size + y];
            int b = mat[y * size + x];

            mat[y * size + x] = a;
            mat[x * size + y] = b;
        }

        if (mobi_opt & 0x800) idct_sparse(&mat[y * size], size, nlive_col);
        else                  idct(&mat[y * size], size);
        if (mobi_opt & 32)                    /* clamp-LUT: fused residual-add + saturate, no branch */
            for (int x = 0; x < size; x++)
                dst[x] = g_clamp[CLAMP_OFF + dst[x] + (mat[y * size + x] >> 6)];
        else
            for (int x = 0; x < size; x++)
                dst[x] = av_clip_uint8(dst[x] + (mat[y * size + x] >> 6));
        dst += frame->linesize[plane];
    }

    mobi_prof[4] += PROF_NOW() - _tt;         /* bucket 4 = transform (IDCT + write-back) */
    return 0;
}

static int add_coefficients(AVCodecContext *avctx, AVFrame *frame,
                            int bx, int by, int size, int plane)
{
    uint64_t t = PROF_NOW();
    int r = add_coefficients_impl(avctx, frame, bx, by, size, plane);
    mobi_prof[1] += PROF_NOW() - t;
    return r;
}

static int add_pframe_coefficients(AVCodecContext *avctx, AVFrame *frame,
                                   int bx, int by, int size, int plane)
{
    MobiClipContext *s = avctx->priv_data;
    GetBitContext *gb = &s->gb;
    int ret, idx = get_ue_golomb_31(gb);

    if (idx == 0) {
        return add_coefficients(avctx, frame, bx, by, size, plane);
    } else if ((unsigned)idx < FF_ARRAY_ELEMS(pframe_block4x4_coefficients_tab)) {
        int flags = pframe_block4x4_coefficients_tab[idx];

        for (int y = by; y < by + 8; y += 4) {
            for (int x = bx; x < bx + 8; x += 4) {
                if (flags & 1) {
                    ret = add_coefficients(avctx, frame, x, y, 4, plane);
                    if (ret < 0)
                        return ret;
                }
                flags >>= 1;
            }
        }
        return 0;
    } else {
        return mobi_fail(5);
    }
}

static int adjust(int x, int size)
{
    return size == 16 ? (x + 1) >> 1 : x;
}

static uint8_t pget(BlockXY b)
{
    BlockXY ret = b;
    int x, y;

    if (b.x == -1 && b.y >= b.size) {
        ret.x = -1, ret.y = b.size - 1;
    } else if (b.x >= -1 && b.y >= -1) {
        ret.x = b.x, ret.y = b.y;
    } else if (b.x == -1 && b.y == -2) {
        ret.x = 0, ret.y = -1;
    } else if (b.x == -2 && b.y == -1) {
        ret.x = -1, ret.y = 0;
    }

    y = av_clip(ret.ay + ret.y, 0, ret.h - 1);
    x = av_clip(ret.ax + ret.x, 0, ret.w - 1);

    return ret.block[y * ret.linesize + x];
}

static uint8_t half(int a, int b)
{
    return ((a + b) + 1) / 2;
}

static uint8_t half3(int a, int b, int c)
{
    return ((a + b + b + c) * 2 / 4 + 1) / 2;
}

static uint8_t pick_above(BlockXY bxy)
{
    bxy.y = bxy.y - 1;

    return pget(bxy);
}

static uint8_t pick_left(BlockXY bxy)
{
    bxy.x = bxy.x - 1;

    return pget(bxy);
}

static uint8_t half_horz(BlockXY bxy)
{
    BlockXY a = bxy, b = bxy, c = bxy;

    a.x -= 1;
    c.x += 1;

    return half3(pget(a), pget(b), pget(c));
}

static uint8_t half_vert(BlockXY bxy)
{
    BlockXY a = bxy, b = bxy, c = bxy;

    a.y -= 1;
    c.y += 1;

    return half3(pget(a), pget(b), pget(c));
}

static uint8_t pick_4(BlockXY bxy)
{
    int val;

    if ((bxy.x % 2) == 0) {
        BlockXY ba, bb;
        int a, b;

        ba = bxy;
        ba.x = -1;
        ba.y = bxy.y + bxy.x / 2;
        a = pget(ba);

        bb = bxy;
        bb.x = -1;
        bb.y = bxy.y + bxy.x / 2 + 1;
        b = pget(bb);

        val = half(a, b);
    } else {
        BlockXY ba;

        ba = bxy;
        ba.x = -1;
        ba.y = bxy.y + bxy.x / 2 + 1;
        val = half_vert(ba);
    }

    return val;
}

static uint8_t pick_5(BlockXY bxy)
{
    int val;

    if (bxy.x == 0) {
        BlockXY a = bxy;
        BlockXY b = bxy;

        a.x = -1;
        a.y -= 1;

        b.x = -1;

        val = half(pget(a), pget(b));
    } else if (bxy.y == 0) {
        BlockXY a = bxy;

        a.x -= 2;
        a.y -= 1;

        val = half_horz(a);
    } else if (bxy.x == 1) {
        BlockXY a = bxy;

        a.x -= 2;
        a.y -= 1;

        val = half_vert(a);
    } else {
        BlockXY a = bxy;

        a.x -= 2;
        a.y -= 1;

        val = pget(a);
    }

    return val;
}

static uint8_t pick_6(BlockXY bxy)
{
    int val;

    if (bxy.y == 0) {
        BlockXY a = bxy;
        BlockXY b = bxy;

        a.x -= 1;
        a.y = -1;

        b.y = -1;

        val = half(pget(a), pget(b));
    } else if (bxy.x == 0) {
        BlockXY a = bxy;

        a.x -= 1;
        a.y -= 2;

        val = half_vert(a);
    } else if (bxy.y == 1) {
        BlockXY a = bxy;

        a.x -= 1;
        a.y -= 2;

        val = half_horz(a);
    } else {
        BlockXY a = bxy;

        a.x -= 1;
        a.y -= 2;

        val = pget(a);
    }

    return val;
}

static uint8_t pick_7(BlockXY bxy)
{
    int clr, acc1, acc2;
    BlockXY a = bxy;

    a.x -= 1;
    a.y -= 1;
    clr = pget(a);
    if (bxy.x && bxy.y)
        return clr;

    if (bxy.x == 0) {
        a.x = -1;
        a.y = bxy.y;
    } else {
        a.x = bxy.x - 2;
        a.y = -1;
    }
    acc1 = pget(a);

    if (bxy.y == 0) {
        a.x = bxy.x;
        a.y = -1;
    } else {
        a.x = -1;
        a.y = bxy.y - 2;
    }
    acc2 = pget(a);

    return half3(acc1, clr, acc2);
}

static uint8_t pick_8(BlockXY bxy)
{
    BlockXY ba = bxy;
    BlockXY bb = bxy;
    int val;

    if (bxy.y == 0) {
        int a, b;

        ba.y = -1;
        a = pget(ba);

        bb.x += 1;
        bb.y = -1;

        b = pget(bb);

        val = half(a, b);
    } else if (bxy.y == 1) {
        ba.x += 1;
        ba.y -= 2;

        val = half_horz(ba);
    } else if (bxy.x < bxy.size - 1) {
        ba.x += 1;
        ba.y -= 2;

        val = pget(ba);
    } else if (bxy.y % 2 == 0) {
        int a, b;

        ba.x = bxy.y / 2 + bxy.size - 1;
        ba.y = -1;
        a = pget(ba);

        bb.x = bxy.y / 2 + bxy.size;
        bb.y = -1;

        b = pget(bb);

        val = half(a, b);
    } else {
        ba.x = bxy.y / 2 + bxy.size;
        ba.y = -1;

        val = half_horz(ba);
    }

    return val;
}

static void block_fill_simple(uint8_t *block, int size, int linesize, int fill)
{
    for (int y = 0; y < size; y++) {
        memset(block, fill, size);
        block += linesize;
    }
}

static void block_fill(uint8_t *block, int size, int linesize,
                       int w, int h, int ax, int ay,
                       uint8_t (*pick)(BlockXY bxy))
{
    BlockXY bxy;

    bxy.size = size;
    bxy.block = block;
    bxy.linesize = linesize;
    bxy.w = w;
    bxy.h = h;
    bxy.ay = ay;
    bxy.ax = ax;

    for (int y = 0; y < size; y++) {
        bxy.y = y;
        for (int x = 0; x < size; x++) {
            uint8_t val;

            bxy.x = x;

            val = pick(bxy);

            block[ax + x + (ay + y) * linesize] = val;
        }
    }
}

static int block_sum(const uint8_t *block, int w, int h, int linesize)
{
    int sum = 0;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            sum += block[x];
        }
        block += linesize;
    }

    return sum;
}

static int predict_intra(AVCodecContext *avctx, AVFrame *frame, int ax, int ay,
                          int pmode, int add_coeffs, int size, int plane)
{
    MobiClipContext *s = avctx->priv_data;
    GetBitContext *gb = &s->gb;
    int w = avctx->width >> !!plane, h = avctx->height >> !!plane;
    int ret = 0;
    uint64_t _pt = PROF_NOW();

    switch (pmode) {
    case 0:
        block_fill(frame->data[plane], size, frame->linesize[plane], w, h, ax, ay, pick_above);
        break;
    case 1:
        block_fill(frame->data[plane], size, frame->linesize[plane], w, h, ax, ay, pick_left);
        break;
    case 2:
        {
            int arr1[16];
            int arr2[16];
            uint8_t *top = frame->data[plane] + FFMAX(ay - 1, 0) * frame->linesize[plane] + ax;
            uint8_t *left = frame->data[plane] + ay * frame->linesize[plane] + FFMAX(ax - 1, 0);
            int bottommost = frame->data[plane][(ay + size - 1) * frame->linesize[plane] + FFMAX(ax - 1, 0)];
            int rightmost = frame->data[plane][FFMAX(ay - 1, 0) * frame->linesize[plane] + ax + size - 1];
            int avg = (bottommost + rightmost + 1) / 2 + 2 * av_clip(get_se_golomb(gb), -(1<<16), 1<<16);
            int r6 = adjust(avg - bottommost, size);
            int r9 = adjust(avg - rightmost, size);
            int shift = adjust(size, size) == 8 ? 3 : 2;
            uint8_t *block;

            for (int x = 0; x < size; x++) {
                int val = top[x];
                arr1[x] = adjust(((bottommost - val) * (1 << shift)) + r6 * (x + 1), size);
            }

            for (int y = 0; y < size; y++) {
                int val = left[y * frame->linesize[plane]];
                arr2[y] = adjust(((rightmost - val) * (1 << shift)) + r9 * (y + 1), size);
            }

            block = frame->data[plane] + ay * frame->linesize[plane] + ax;
            for (int y = 0; y < size; y++) {
                for (int x = 0; x < size; x++) {
                    block[x] = (((top[x] + left[0] + ((arr1[x] * (y + 1) +
                                                       arr2[y] * (x + 1)) >> 2 * shift)) + 1) / 2) & 0xFF;
                }
                block += frame->linesize[plane];
                left  += frame->linesize[plane];
            }
        }
        break;
    case 3:
        {
            uint8_t fill;

            if (ax == 0 && ay == 0) {
                fill = 0x80;
            } else if (ax >= 1 && ay >= 1) {
                int left = block_sum(frame->data[plane] + ay * frame->linesize[plane] + ax - 1,
                                     1, size, frame->linesize[plane]);
                int top  = block_sum(frame->data[plane] + (ay - 1) * frame->linesize[plane] + ax,
                                     size, 1, frame->linesize[plane]);

                fill = ((left + top) * 2 / (2 * size) + 1) / 2;
            } else if (ax >= 1) {
                fill = (block_sum(frame->data[plane] + ay * frame->linesize[plane] + ax - 1,
                                  1, size, frame->linesize[plane]) * 2 / size + 1) / 2;
            } else if (ay >= 1) {
                fill = (block_sum(frame->data[plane] + (ay - 1) * frame->linesize[plane] + ax,
                                  size, 1, frame->linesize[plane]) * 2 / size + 1) / 2;
            } else {
                return -1;
            }

            block_fill_simple(frame->data[plane] + ay * frame->linesize[plane] + ax,
                              size, frame->linesize[plane], fill);
        }
        break;
    case 4:
        block_fill(frame->data[plane], size, frame->linesize[plane], w, h, ax, ay, pick_4);
        break;
    case 5:
        block_fill(frame->data[plane], size, frame->linesize[plane], w, h, ax, ay, pick_5);
        break;
    case 6:
        block_fill(frame->data[plane], size, frame->linesize[plane], w, h, ax, ay, pick_6);
        break;
    case 7:
        block_fill(frame->data[plane], size, frame->linesize[plane], w, h, ax, ay, pick_7);
        break;
    case 8:
        block_fill(frame->data[plane], size, frame->linesize[plane], w, h, ax, ay, pick_8);
        break;
    }

    mobi_prof[0] += PROF_NOW() - _pt;

    if (add_coeffs)
        ret = add_coefficients(avctx, frame, ax, ay, size, plane);

    return ret;
}

static int get_prediction(AVCodecContext *avctx, int x, int y, int size)
{
    MobiClipContext *s = avctx->priv_data;
    GetBitContext *gb = &s->gb;
    int index = (y & 0xC) | (x / 4 % 4);

    uint8_t val = FFMIN(s->pre[index], index % 4 == 0 ? 9 : s->pre[index + 3]);
    if (val == 9)
        val = 3;

    if (!get_bits1(gb)) {
        int x = get_bits(gb, 3);
        val = x + (x >= val ? 1 : 0);
    }

    s->pre[index + 4] = val;
    if (size == 8)
        s->pre[index + 5] = s->pre[index + 8] = s->pre[index + 9] = val;

    return val;
}

static int process_block(AVCodecContext *avctx, AVFrame *frame,
                         int x, int y, int pmode, int has_coeffs, int plane)
{
    MobiClipContext *s = avctx->priv_data;
    GetBitContext *gb = &s->gb;
    int tmp, ret;

    if (!has_coeffs) {
        if (pmode < 0)
            pmode = get_prediction(avctx, x, y, 8);
        return predict_intra(avctx, frame, x, y, pmode, 0, 8, plane);
    }

    tmp = get_ue_golomb_31(gb);
    if ((unsigned)tmp > FF_ARRAY_ELEMS(block4x4_coefficients_tab))
        return mobi_fail(6);

    if (tmp == 0) {
        if (pmode < 0)
            pmode = get_prediction(avctx, x, y, 8);
        ret = predict_intra(avctx, frame, x, y, pmode, 1, 8, plane);
    } else {
        int flags = block4x4_coefficients_tab[tmp - 1];

        for (int by = y; by < y + 8; by += 4) {
            for (int bx = x; bx < x + 8; bx += 4) {
                int new_pmode = pmode;

                if (new_pmode < 0)
                    new_pmode = get_prediction(avctx, bx, by, 4);
                ret = predict_intra(avctx, frame, bx, by, new_pmode, flags & 1, 4, plane);
                if (ret < 0)
                    return ret;
                flags >>= 1;
            }
        }
    }

    return ret;
}

static int decode_macroblock(AVCodecContext *avctx, AVFrame *frame,
                             int x, int y, int predict)
{
    MobiClipContext *s = avctx->priv_data;
    GetBitContext *gb = &s->gb;
    int flags, pmode_uv, idx = get_ue_golomb(gb);
    int ret = 0;

    if (idx < 0 || idx >= FF_ARRAY_ELEMS(block8x8_coefficients_tab))
        return mobi_fail(7);

    flags = block8x8_coefficients_tab[idx];

    if (predict) {
        ret = process_block(avctx, frame, x, y, -1, flags & 1, 0);
        if (ret < 0)
            return ret;
        flags >>= 1;
        ret = process_block(avctx, frame, x + 8, y, -1, flags & 1, 0);
        if (ret < 0)
            return ret;
        flags >>= 1;
        ret = process_block(avctx, frame, x, y + 8, -1, flags & 1, 0);
        if (ret < 0)
            return ret;
        flags >>= 1;
        ret = process_block(avctx, frame, x + 8, y + 8, -1, flags & 1, 0);
        if (ret < 0)
            return ret;
        flags >>= 1;
    } else {
        int pmode = get_bits(gb, 3);

        if (pmode == 2) {
            ret = predict_intra(avctx, frame, x, y, pmode, 0, 16, 0);
            if (ret < 0)
                return ret;
            pmode = 9;
        }

        ret = process_block(avctx, frame, x, y, pmode, flags & 1, 0);
        if (ret < 0)
            return ret;
        flags >>= 1;
        ret = process_block(avctx, frame, x + 8, y, pmode, flags & 1, 0);
        if (ret < 0)
            return ret;
        flags >>= 1;
        ret = process_block(avctx, frame, x, y + 8, pmode, flags & 1, 0);
        if (ret < 0)
            return ret;
        flags >>= 1;
        ret = process_block(avctx, frame, x + 8, y + 8, pmode, flags & 1, 0);
        if (ret < 0)
            return ret;
        flags >>= 1;
    }

    pmode_uv = get_bits(gb, 3);
    if (pmode_uv == 2) {
        ret = predict_intra(avctx, frame, x >> 1, y >> 1, pmode_uv, 0, 8, 1 + !s->moflex);
        if (ret < 0)
            return ret;
        ret = predict_intra(avctx, frame, x >> 1, y >> 1, pmode_uv, 0, 8, 2 - !s->moflex);
        if (ret < 0)
            return ret;
        pmode_uv = 9;
    }

    ret = process_block(avctx, frame, x >> 1, y >> 1, pmode_uv, flags & 1, 1 + !s->moflex);
    if (ret < 0)
        return ret;
    flags >>= 1;
    ret = process_block(avctx, frame, x >> 1, y >> 1, pmode_uv, flags & 1, 2 - !s->moflex);
    if (ret < 0)
        return ret;

    return 0;
}

static int get_index(int x)
{
    return x == 16 ? 0 : x == 8 ? 1 : x == 4 ? 2 : x == 2 ? 3 : 0;
}

static int predict_motion_impl(AVCodecContext *avctx,
                          int width, int height, int index,
                          int offsetm, int offsetx, int offsety)
{
    MobiClipContext *s = avctx->priv_data;
    MotionXY *motion = s->motion;
    GetBitContext *gb = &s->gb;
    int fheight = avctx->height;
    int fwidth = avctx->width;

    if (index <= 5) {
        int sidx = -FFMAX(1, index) + s->current_pic;
        MotionXY mv = s->motion[0];

        if (sidx < 0)
            sidx += 6;

        /* MISSING-REFERENCE TOLERANCE (bit 0x8000) -- this is what turns a multi-second FREEZE into a
         * few frames of soft artifacts.
         *
         * After a catch-up skip we mobi_flush(), which NULLs the 6-frame reference pool. The resync
         * keyframe then decodes fine (intra needs no refs), but the P-frames after it still reference
         * 2..5 frames BACK -- i.e. frames from before the skip, now NULL. A mobiclip "keyframe" is just
         * the intra bit, NOT a clean IDR, so this is normal and expected.
         *
         * Stock behaviour is to fail the whole frame (AVERROR_INVALIDDATA below). That is a CASCADE:
         * s->current_pic only advances on a SUCCESSFUL decode (see the end of mobiclip_decode), so a
         * failed frame never fills its own pool slot -- which means the next P-frame finds the same slot
         * missing and fails too. The pool can never refill, and every P-frame errors out until the next
         * NATURAL keyframe, seconds away. That is the entire freeze.
         *
         * Instead, walk FORWARD (toward current_pic = more recent) to the nearest frame that actually
         * has pixels and use that. The prediction is wrong only by a small TEMPORAL OFFSET (we use frame
         * n-1 where the encoder meant n-2), and adjacent frames are nearly identical, so the error is
         * mild -- unlike seeding every slot with the keyframe (tried before: propagating blur). Crucially
         * the frame now DECODES, so current_pic advances, the slot fills, and each following frame has one
         * more valid reference: the substitution count decays to zero within ~5 frames (~0.2s) and decode
         * is bit-exact again. Never fires in normal playback (no flush => no missing refs), so ordinary
         * decoding stays bit-identical -- verified on host. */
        if ((mobi_opt & 0x8000) && !s->pic[sidx]->data[0]) {
            int probe = sidx, found = -1;

            /* 1st choice: nearest valid frame walking FORWARD toward current_pic (most recent wins,
             * so the error is the smallest possible temporal offset). */
            for (int k = 0; k < 5; k++) {
                probe = (probe + 1) % 6;
                if (probe == s->current_pic)      /* don't reference the frame we're building */
                    break;
                if (s->pic[probe]->data[0]) { found = probe; mobi_sub[0]++; break; }
            }

            /* 2nd choice: ANY valid frame still in the pool. (The forward walk alone was the bug: it
             * gave up at current_pic and left sidx on the NULL slot, so err9 fired and the cascade ran
             * exactly as before -- observed as fail=250 / err9=500 with only drop=4 skips.) */
            if (found < 0)
                for (int k = 0; k < 6; k++)
                    if (k != s->current_pic && s->pic[k]->data[0]) { found = k; mobi_sub[1]++; break; }

            /* Pool COMPLETELY empty -> DO NOT substitute. We used to fall back to the frame being built
             * (freshly calloc'd, i.e. BLACK) so that the frame would always decode. That is what turned
             * the picture GREEN: a black frame lands in the pool and every later P-frame references it,
             * so the blackness propagates until the next natural keyframe. A clean failure is strictly
             * better than a poisoned reference. Fall through to err9 and let this one frame drop.
             *
             * With the caller now only flushing when the next packet is genuinely intra, the pool always
             * receives a clean keyframe first, so this case should not arise at all -- mobi_sub[2] counts
             * it so we can see if it ever does. */
            if (found < 0) { mobi_sub[2]++; }
            else sidx = found;
        }

        if (index > 0) {
            mv.x = mv.x + (unsigned)get_se_golomb(gb);
            mv.y = mv.y + (unsigned)get_se_golomb(gb);
        }
        if (mv.x >= INT_MAX || mv.y >= INT_MAX)
            return mobi_fail(8);

        motion[offsetm].x = mv.x;
        motion[offsetm].y = mv.y;

        for (int i = 0; i < 3; i++) {
            int method, src_linesize, dst_linesize;
            uint8_t *src, *dst;

            if (i == 1) {
                offsetx = offsetx >> 1;
                offsety = offsety >> 1;
                mv.x = mv.x >> 1;
                mv.y = mv.y >> 1;
                width = width >> 1;
                height = height >> 1;
                fwidth = fwidth >> 1;
                fheight = fheight >> 1;
            }

            av_assert0(s->pic[sidx]);
            av_assert0(s->pic[s->current_pic]);
            av_assert0(s->pic[s->current_pic]->data[i]);
            if (!s->pic[sidx]->data[i])
                return mobi_fail(9);

            method = (mv.x & 1) | ((mv.y & 1) << 1);
            src_linesize = s->pic[sidx]->linesize[i];
            dst_linesize = s->pic[s->current_pic]->linesize[i];
            dst = s->pic[s->current_pic]->data[i] + offsetx + offsety * dst_linesize;

            if (offsetx + (mv.x >> 1) < 0 ||
                offsety + (mv.y >> 1) < 0 ||
                offsetx + width  + (mv.x + 1 >> 1) > fwidth ||
                offsety + height + (mv.y + 1 >> 1) > fheight)
                return mobi_fail(10);

            src = s->pic[sidx]->data[i] + offsetx + (mv.x >> 1) +
                           (offsety + (mv.y >> 1)) * src_linesize;

            /* Burst-prefetch the whole reference block before touching it (bit 0x2000).
             * The per-row PF() below only looks ONE row ahead, so a row's line fill has
             * just that row's copy loop (~30 cycles) to hide behind while FCRAM needs
             * ~100+ -- most of the miss stays exposed, which is why PLD only ever bought
             * ~5%. ARM11's D-cache is non-blocking, so issuing every row's PLD back to
             * back puts several line fills in flight at once and overlaps the latencies.
             * height+1 rows and the +width line cover the extra row/column that half-pel
             * interpolation reads. Prefetch is a pure hint: output stays bit-identical. */
            if (mobi_opt & 0x2000) {
                const uint8_t *p = src;
                for (int y = 0; y <= height; y++, p += src_linesize) {
                    __builtin_prefetch(p, 0, 1);
                    __builtin_prefetch(p + width, 0, 1);
                }
            }
          if (mobi_opt & 0x100) {                      /* hand-written ARM assembly half-pel */
            int al = (((uintptr_t)src & 3) == 0) && ((width & 3) == 0);
            switch (method) {
            case 0:
                for (int y = 0; y < height; y++) { memcpy(dst, src, width); dst += dst_linesize; src += src_linesize; }
                break;
            case 1:
                if (al) mc_havg_a(dst, src, width, height, dst_linesize, src_linesize);
                else for (int y = 0; y < height; y++) { for (int x = 0; x < width; x++) dst[x] = (uint8_t)((src[x] >> 1) + (src[x + 1] >> 1)); dst += dst_linesize; src += src_linesize; }
                break;
            case 2:
                if (al) mc_vavg_a(dst, src, width, height, dst_linesize, src_linesize);
                else for (int y = 0; y < height; y++) { for (int x = 0; x < width; x++) dst[x] = (uint8_t)((src[x] >> 1) + (src[x + src_linesize] >> 1)); dst += dst_linesize; src += src_linesize; }
                break;
            case 3:
                if (al) mc_diag_a(dst, src, width, height, dst_linesize, src_linesize);
                else for (int y = 0; y < height; y++) { for (int x = 0; x < width; x++)
                        dst[x] = (uint8_t)((((src[x] >> 1) + (src[x + 1] >> 1)) >> 1) + (((src[x + src_linesize] >> 1) + (src[x + 1 + src_linesize] >> 1)) >> 1));
                    dst += dst_linesize; src += src_linesize; }
                break;
            }
          } else if (mobi_opt & 16) {                  /* ARMv6 media path (UHADD8), 4 px/op */
            switch (method) {
            case 0:
                for (int y = 0; y < height; y++) {
                    PF(src + src_linesize);
                    int x = 0;
                    for (; x + 4 <= width; x += 4) st4(dst + x, ld4(src + x));
                    for (; x < width; x++) dst[x] = src[x];
                    dst += dst_linesize; src += src_linesize;
                }
                break;
            case 1:
                for (int y = 0; y < height; y++) {
                    PF(src + src_linesize);
                    int x = 0;
                    for (; x + 4 <= width; x += 4) st4(dst + x, u8avg(ld4(src + x), ld4(src + x + 1)));
                    for (; x < width; x++) dst[x] = (uint8_t)((src[x] >> 1) + (src[x + 1] >> 1));
                    dst += dst_linesize; src += src_linesize;
                }
                break;
            case 2:
                for (int y = 0; y < height; y++) {
                    PF(src + 2 * src_linesize);
                    int x = 0;
                    for (; x + 4 <= width; x += 4) st4(dst + x, u8avg(ld4(src + x), ld4(src + x + src_linesize)));
                    for (; x < width; x++) dst[x] = (uint8_t)((src[x] >> 1) + (src[x + src_linesize] >> 1));
                    dst += dst_linesize; src += src_linesize;
                }
                break;
            case 3:
                for (int y = 0; y < height; y++) {
                    PF(src + 2 * src_linesize);
                    int x = 0;
                    for (; x + 4 <= width; x += 4)
                        st4(dst + x, u8avg(u8avg(ld4(src + x), ld4(src + x + 1)),
                                           u8avg(ld4(src + x + src_linesize), ld4(src + x + 1 + src_linesize))));
                    for (; x < width; x++)
                        dst[x] = (uint8_t)((((src[x] >> 1) + (src[x + 1] >> 1)) >> 1) +
                                           (((src[x + src_linesize] >> 1) + (src[x + 1 + src_linesize] >> 1)) >> 1));
                    dst += dst_linesize; src += src_linesize;
                }
                break;
            }
          } else if (mobi_opt & 0x40) {
            /* ported motion comp: copy each source row to an ALIGNED temp, then SWAR using aligned
             * word loads + a shift to build the +1 neighbor -- no unaligned loads (that penalty is
             * what made the naive SWAR regress). u8avg is bit-exact to (a>>1)+(b>>1). */
            uint8_t ta[24] __attribute__((aligned(4)));
            uint8_t tb[24] __attribute__((aligned(4)));
            switch (method) {
            case 0:
                for (int y = 0; y < height; y++) { memcpy(dst, src, width); dst += dst_linesize; src += src_linesize; }
                break;
            case 1:
                for (int y = 0; y < height; y++) {
                    memcpy(ta, src, width + 1);
                    int x = 0;
                    for (; x + 4 <= width; x += 4) {
                        uint32_t w = *(const uint32_t *)(ta + x);
                        uint32_t wn = (w >> 8) | ((uint32_t)ta[x + 4] << 24);
                        st4(dst + x, u8avg(w, wn));
                    }
                    for (; x < width; x++) dst[x] = (uint8_t)((ta[x] >> 1) + (ta[x + 1] >> 1));
                    dst += dst_linesize; src += src_linesize;
                }
                break;
            case 2:
                for (int y = 0; y < height; y++) {
                    memcpy(ta, src, width); memcpy(tb, src + src_linesize, width);
                    int x = 0;
                    for (; x + 4 <= width; x += 4)
                        st4(dst + x, u8avg(*(const uint32_t *)(ta + x), *(const uint32_t *)(tb + x)));
                    for (; x < width; x++) dst[x] = (uint8_t)((ta[x] >> 1) + (tb[x] >> 1));
                    dst += dst_linesize; src += src_linesize;
                }
                break;
            case 3:
                for (int y = 0; y < height; y++) {
                    memcpy(ta, src, width + 1); memcpy(tb, src + src_linesize, width + 1);
                    int x = 0;
                    for (; x + 4 <= width; x += 4) {
                        uint32_t wa = *(const uint32_t *)(ta + x), wan = (wa >> 8) | ((uint32_t)ta[x + 4] << 24);
                        uint32_t wb = *(const uint32_t *)(tb + x), wbn = (wb >> 8) | ((uint32_t)tb[x + 4] << 24);
                        st4(dst + x, u8avg(u8avg(wa, wan), u8avg(wb, wbn)));
                    }
                    for (; x < width; x++)
                        dst[x] = (uint8_t)((((ta[x] >> 1) + (ta[x + 1] >> 1)) >> 1) +
                                           (((tb[x] >> 1) + (tb[x + 1] >> 1)) >> 1));
                    dst += dst_linesize; src += src_linesize;
                }
                break;
            }
          } else if (mobi_opt & 0x80) {
            /* direct aligned-read SWAR: no memcpy, no unaligned loads -- load_uw reads via aligned
             * words + shift. (PF prefetch is gated on bit 8, so 0x88 = this + prefetch.) */
            switch (method) {
            case 0:
                for (int y = 0; y < height; y++) { memcpy(dst, src, width); dst += dst_linesize; src += src_linesize; }
                break;
            case 1:
                for (int y = 0; y < height; y++) {
                    PF(src + src_linesize);
                    int x = 0;
                    for (; x + 4 <= width; x += 4) st4(dst + x, u8avg(load_uw(src + x), load_uw(src + x + 1)));
                    for (; x < width; x++) dst[x] = (uint8_t)((src[x] >> 1) + (src[x + 1] >> 1));
                    dst += dst_linesize; src += src_linesize;
                }
                break;
            case 2:
                for (int y = 0; y < height; y++) {
                    PF(src + 2 * src_linesize);
                    int x = 0;
                    for (; x + 4 <= width; x += 4) st4(dst + x, u8avg(load_uw(src + x), load_uw(src + x + src_linesize)));
                    for (; x < width; x++) dst[x] = (uint8_t)((src[x] >> 1) + (src[x + src_linesize] >> 1));
                    dst += dst_linesize; src += src_linesize;
                }
                break;
            case 3:
                for (int y = 0; y < height; y++) {
                    PF(src + 2 * src_linesize);
                    int x = 0;
                    for (; x + 4 <= width; x += 4)
                        st4(dst + x, u8avg(u8avg(load_uw(src + x), load_uw(src + x + 1)),
                                           u8avg(load_uw(src + x + src_linesize), load_uw(src + x + 1 + src_linesize))));
                    for (; x < width; x++)
                        dst[x] = (uint8_t)((((src[x] >> 1) + (src[x + 1] >> 1)) >> 1) +
                                           (((src[x + src_linesize] >> 1) + (src[x + 1 + src_linesize] >> 1)) >> 1));
                    dst += dst_linesize; src += src_linesize;
                }
                break;
            }
          } else if (mobi_opt & 1) {
            switch (method) {
            case 0:                                    /* full-pel: straight copy */
                for (int y = 0; y < height; y++) {
                    memcpy(dst, src, width);
                    dst += dst_linesize; src += src_linesize;
                }
                break;
            case 1:                                    /* half-pel horizontal */
                for (int y = 0; y < height; y++) {
                    int x = 0;
                    for (; x + 4 <= width; x += 4)
                        st4(dst + x, AVG2(ld4(src + x), ld4(src + x + 1)));
                    for (; x < width; x++)
                        dst[x] = (uint8_t)((src[x] >> 1) + (src[x + 1] >> 1));
                    dst += dst_linesize; src += src_linesize;
                }
                break;
            case 2:                                    /* half-pel vertical */
                for (int y = 0; y < height; y++) {
                    int x = 0;
                    for (; x + 4 <= width; x += 4)
                        st4(dst + x, AVG2(ld4(src + x), ld4(src + x + src_linesize)));
                    for (; x < width; x++)
                        dst[x] = (uint8_t)((src[x] >> 1) + (src[x + src_linesize] >> 1));
                    dst += dst_linesize; src += src_linesize;
                }
                break;
            case 3:                                    /* half-pel diagonal (2x2 avg) */
                for (int y = 0; y < height; y++) {
                    int x = 0;
                    for (; x + 4 <= width; x += 4) {
                        uint32_t top = AVG2(ld4(src + x), ld4(src + x + 1));
                        uint32_t bot = AVG2(ld4(src + x + src_linesize), ld4(src + x + 1 + src_linesize));
                        st4(dst + x, AVG2(top, bot));
                    }
                    for (; x < width; x++)
                        dst[x] = (uint8_t)((((src[x] >> 1) + (src[x + 1] >> 1)) >> 1) +
                                           (((src[x + src_linesize] >> 1) + (src[x + 1 + src_linesize] >> 1)) >> 1));
                    dst += dst_linesize; src += src_linesize;
                }
                break;
            }
          } else {
            switch (method) {                          /* original byte-at-a-time paths */
            case 0:
                for (int y = 0; y < height; y++) {
                    PF(src + src_linesize);
                    for (int x = 0; x < width; x++) dst[x] = src[x];
                    dst += dst_linesize; src += src_linesize;
                }
                break;
            case 1:
                for (int y = 0; y < height; y++) {
                    PF(src + src_linesize);
                    for (int x = 0; x < width; x++) dst[x] = (uint8_t)((src[x] >> 1) + (src[x + 1] >> 1));
                    dst += dst_linesize; src += src_linesize;
                }
                break;
            case 2:
                for (int y = 0; y < height; y++) {
                    PF(src + 2 * src_linesize);
                    for (int x = 0; x < width; x++) dst[x] = (uint8_t)((src[x] >> 1) + (src[x + src_linesize] >> 1));
                    dst += dst_linesize; src += src_linesize;
                }
                break;
            case 3:
                for (int y = 0; y < height; y++) {
                    PF(src + 2 * src_linesize);
                    for (int x = 0; x < width; x++)
                        dst[x] = (uint8_t)((((src[x] >> 1) + (src[x + 1] >> 1)) >> 1) +
                                           (((src[x + src_linesize] >> 1) + (src[x + 1 + src_linesize] >> 1)) >> 1));
                    dst += dst_linesize; src += src_linesize;
                }
                break;
            }
          }
        }
    } else {
        int tidx;
        int adjx = index == 8 ? 0 :  width / 2;
        int adjy = index == 8 ? height / 2 : 0;

        width  = width  - adjx;
        height = height - adjy;
        tidx = get_index(height) * 4 + get_index(width);

        for (int i = 0; i < 2; i++) {
            int ret, idx2;

            idx2 = get_vlc2(gb, mv_vlc[s->moflex][tidx], MOBI_MV_VLC_BITS, 1);

            ret = predict_motion_impl(avctx, width, height, idx2,
                                 offsetm, offsetx + i * adjx, offsety + i * adjy);
            if (ret < 0)
                return ret;
        }
    }

    return 0;
}

static int predict_motion(AVCodecContext *avctx,
                          int width, int height, int index,
                          int offsetm, int offsetx, int offsety)
{
    uint64_t t = PROF_NOW();
    int r = predict_motion_impl(avctx, width, height, index, offsetm, offsetx, offsety);
    mobi_prof[3] += PROF_NOW() - t;
    return r;
}

static int mobiclip_decode(AVCodecContext *avctx, AVFrame *rframe,
                           int *got_frame, AVPacket *pkt)
{
    MobiClipContext *s = avctx->priv_data;
    GetBitContext *gb = &s->gb;
    AVFrame *frame = s->pic[s->current_pic];
    int ret;

    if (avctx->height/16 * (avctx->width/16) * 2 > 8LL*FFALIGN(pkt->size, 2))
        return mobi_fail(11);

    av_fast_padded_malloc(&s->bitstream, &s->bitstream_size,
                          pkt->size);

    if ((ret = ff_reget_buffer(avctx, frame, 0)) < 0)
        return ret;

    s->bdsp.bswap16_buf((uint16_t *)s->bitstream,
                        (uint16_t *)pkt->data,
                        (pkt->size + 1) >> 1);

    ret = init_get_bits8(gb, s->bitstream, FFALIGN(pkt->size, 2));
    if (ret < 0)
        return ret;

    if (get_bits1(gb)) {
        frame->pict_type = AV_PICTURE_TYPE_I;
        frame->flags |= AV_FRAME_FLAG_KEY;
        s->moflex = get_bits1(gb);
        s->dct_tab_idx = get_bits1(gb);

        ret = setup_qtables(avctx, get_bits(gb, 6));
        if (ret < 0)
            return ret;

        for (int y = 0; y < avctx->height; y += 16) {
            for (int x = 0; x < avctx->width; x += 16) {
                ret = decode_macroblock(avctx, frame, x, y, get_bits1(gb));
                if (ret < 0)
                    return ret;
            }
        }
    } else {
        MotionXY *motion = s->motion;

        memset(motion, 0, s->motion_size);

        frame->pict_type = AV_PICTURE_TYPE_P;
        frame->flags &= ~AV_FRAME_FLAG_KEY;
        s->dct_tab_idx = 0;

        ret = setup_qtables(avctx, s->quantizer + (int64_t)get_se_golomb(gb));
        if (ret < 0)
            return ret;

        for (int y = 0; y < avctx->height; y += 16) {
            for (int x = 0; x < avctx->width; x += 16) {
                int idx;

                motion[0].x = mid_pred(motion[x / 16 + 1].x, motion[x / 16 + 2].x, motion[x / 16 + 3].x);
                motion[0].y = mid_pred(motion[x / 16 + 1].y, motion[x / 16 + 2].y, motion[x / 16 + 3].y);
                motion[x / 16 + 2].x = 0;
                motion[x / 16 + 2].y = 0;

                idx = get_vlc2(gb, mv_vlc[s->moflex][0], MOBI_MV_VLC_BITS, 1);

                if (idx == 6 || idx == 7) {
                    ret = decode_macroblock(avctx, frame, x, y, idx == 7);
                    if (ret < 0)
                        return ret;
                } else {
                    int flags, idx2;
                    ret = predict_motion(avctx, 16, 16, idx, x / 16 + 2, x, y);
                    if (ret < 0)
                        return ret;
                    idx2 = get_ue_golomb(gb);
                    if (idx2 >= FF_ARRAY_ELEMS(pframe_block8x8_coefficients_tab))
                        return mobi_fail(12);
                    flags = pframe_block8x8_coefficients_tab[idx2];

                    for (int sy = y; sy < y + 16; sy += 8) {
                        for (int sx = x; sx < x + 16; sx += 8) {
                            if (flags & 1)
                                add_pframe_coefficients(avctx, frame, sx, sy, 8, 0);
                            flags >>= 1;
                        }
                    }

                    if (flags & 1)
                        add_pframe_coefficients(avctx, frame, x >> 1, y >> 1, 8, 1 + !s->moflex);
                    flags >>= 1;
                    if (flags & 1)
                        add_pframe_coefficients(avctx, frame, x >> 1, y >> 1, 8, 2 - !s->moflex);
                }
            }
        }
    }

    if (!s->moflex)
        avctx->colorspace = AVCOL_SPC_YCGCO;

    s->current_pic = (s->current_pic + 1) % 6;
    ret = av_frame_ref(rframe, frame);
    if (ret < 0)
        return ret;
    *got_frame = 1;

    return 0;
}

static void mobiclip_flush(AVCodecContext *avctx)
{
    MobiClipContext *s = avctx->priv_data;

    for (int i = 0; i < 6; i++)
        av_frame_unref(s->pic[i]);
}

static av_cold int mobiclip_close(AVCodecContext *avctx)
{
    MobiClipContext *s = avctx->priv_data;

    av_freep(&s->bitstream);
    s->bitstream_size = 0;
    av_freep(&s->motion);
    s->motion_size = 0;

    for (int i = 0; i < 6; i++) {
        av_frame_free(&s->pic[i]);
    }

    return 0;
}

/* ---- standalone entry points (replaces FFCodec registration) ---- */
int  mobi_init(AVCodecContext *avctx)  { return mobiclip_init(avctx); }
int  mobi_decode(AVCodecContext *avctx, AVFrame *frame, int *got, AVPacket *pkt)
                                       { return mobiclip_decode(avctx, frame, got, pkt); }
void mobi_flush(AVCodecContext *avctx) { mobiclip_flush(avctx); }
/* After a skip+resync, DEEP-COPY the just-decoded keyframe into every other reference slot (each keeps its
 * OWN buffer via ff_reget_buffer -- NOT av_frame_ref, which is a shallow pointer-share here and makes the
 * decoder decode into the shared buffer -> corruption/UAF crash). Post-skip P-frames then reference valid
 * recent keyframe content instead of stale frames -> no ghosting; non-null -> no freeze. */
void mobi_seed_refs(AVCodecContext *avctx)
{
    MobiClipContext *s = avctx->priv_data;
    int cur = (s->current_pic + 5) % 6;               /* current_pic already advanced past the decode */
    AVFrame *src = s->pic[cur];
    if (!src || !src->data[0]) return;
    int w = avctx->width, h = avctx->height, cw = w / 2, ch = h / 2;
    for (int i = 0; i < 6; i++) {
        if (i == cur) continue;
        AVFrame *d = s->pic[i];
        if (ff_reget_buffer(avctx, d, 0) < 0) continue;   /* d gets its own correctly-sized buffer */
        memcpy(d->data[0], src->data[0], (size_t)w  * h);
        memcpy(d->data[1], src->data[1], (size_t)cw * ch);
        memcpy(d->data[2], src->data[2], (size_t)cw * ch);
        d->pict_type = src->pict_type; d->flags = src->flags;
    }
}
int  mobi_close(AVCodecContext *avctx) { return mobiclip_close(avctx); }
size_t mobi_ctx_size(void) { return sizeof(MobiClipContext); }
