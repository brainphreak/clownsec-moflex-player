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

/* Ticks spent in recon_execute (the replay/reconstruct pass), summed across decoded frames.
 * Diagnostic for the cross-frame pipeline: if reconstruct-alone < base fused decode, the pipeline
 * (parse N+1 || reconstruct N) wins. Set by recon_execute on device; 0 on host. */
uint64_t mobi_recon_ticks;

/* runtime optimization switch so one binary can A/B decode paths:
 *   bit 0 = SWAR motion comp   bit 1 = IDCT zero-row skip
 *   bit 2 = DC-only block       bit 3 = cache prefetch (PLD)   bit 4 = UHADD8 motion comp
 * DEFAULT 0 = stock decode (the shipped app relies on this; the gputest harness sets it per test).
 * Findings: 1 SWAR hurt, 2 idct-skip ~2%, 4 DC-only noise, 8 prefetch helps pure-decode ~5% but
 * REGRESSES in-player (fights Y2R for the memory bus), 16 UHADD8 bit-exact but 0 gain (memory-bound). */
int mobi_opt = 0;
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
    /* portable host build (bit-identical): truncated per-byte average (a>>1)+(b>>1) */
    return ((a >> 1) & 0x7F7F7F7Fu) + ((b >> 1) & 0x7F7F7F7Fu);
#endif
}

/* ---- ARMv6 media ops for the residual add+clamp (4 pixels/instruction, like the official decoder).
 * Host builds get bit-identical scalar equivalents so pc_verify still checks the exact output. ---- */
#if defined(__arm__)
static inline uint32_t simd_uxtb16(uint32_t a)      { uint32_t r; __asm__("uxtb16 %0,%1"        :"=r"(r):"r"(a)); return r; }
static inline uint32_t simd_uxtb16_ror8(uint32_t a) { uint32_t r; __asm__("uxtb16 %0,%1,ror #8" :"=r"(r):"r"(a)); return r; }
static inline uint32_t simd_sadd16(uint32_t a, uint32_t b) { uint32_t r; __asm__("sadd16 %0,%1,%2":"=r"(r):"r"(a),"r"(b)); return r; }
static inline uint32_t simd_ssub16(uint32_t a, uint32_t b) { uint32_t r; __asm__("ssub16 %0,%1,%2":"=r"(r):"r"(a),"r"(b)); return r; }
static inline uint32_t simd_shadd16(uint32_t a, uint32_t b){ uint32_t r; __asm__("shadd16 %0,%1,%2":"=r"(r):"r"(a),"r"(b)); return r; }
static inline uint32_t simd_usat16_8(uint32_t a)    { uint32_t r; __asm__("usat16 %0,#8,%1"     :"=r"(r):"r"(a)); return r; }
#else
static inline uint32_t simd_uxtb16(uint32_t a)      { return a & 0x00ff00ffu; }
static inline uint32_t simd_uxtb16_ror8(uint32_t a) { return (a >> 8) & 0x00ff00ffu; }
static inline uint32_t simd_sadd16(uint32_t a, uint32_t b) {
    int lo = (int)(int16_t)(a & 0xffff) + (int)(int16_t)(b & 0xffff);
    int hi = (int)(int16_t)(a >> 16)    + (int)(int16_t)(b >> 16);
    return ((uint32_t)lo & 0xffff) | ((uint32_t)hi << 16);
}
static inline uint32_t simd_ssub16(uint32_t a, uint32_t b) {
    int lo = (int)(int16_t)(a & 0xffff) - (int)(int16_t)(b & 0xffff);
    int hi = (int)(int16_t)(a >> 16)    - (int)(int16_t)(b >> 16);
    return ((uint32_t)lo & 0xffff) | ((uint32_t)hi << 16);
}
static inline uint32_t simd_shadd16(uint32_t a, uint32_t b) {   /* signed halving add: (a+b)>>1 per lane */
    int lo = ((int)(int16_t)(a & 0xffff) + (int)(int16_t)(b & 0xffff)) >> 1;
    int hi = ((int)(int16_t)(a >> 16)    + (int)(int16_t)(b >> 16))    >> 1;
    return ((uint32_t)lo & 0xffff) | ((uint32_t)hi << 16);
}
static inline uint32_t simd_usat16_8(uint32_t a) {
    int lo = (int16_t)(a & 0xffff), hi = (int16_t)(a >> 16);
    lo = lo < 0 ? 0 : (lo > 255 ? 255 : lo);
    hi = hi < 0 ? 0 : (hi > 255 ? 255 : hi);
    return (uint32_t)lo | ((uint32_t)hi << 16);
}
#endif
/* dst[0..3] = clip_u8(dst[0..3] + res[0..3]), 4 pixels at once. res are the (mat>>6) residuals.
 * Bit-exact with clip_u8(dst + r) as long as each residual fits in a signed 16-bit lane (true for
 * valid content: pc_verify catches any clip where it doesn't). */
static inline uint32_t simd_add_clip4(uint32_t d, int r0, int r1, int r2, int r3) {
    uint32_t re = ((uint32_t)r0 & 0xffff) | ((uint32_t)r2 << 16);   /* lanes 0,2 */
    uint32_t rh = ((uint32_t)r1 & 0xffff) | ((uint32_t)r3 << 16);   /* lanes 1,3 */
    uint32_t se = simd_usat16_8(simd_sadd16(simd_uxtb16(d),      re));
    uint32_t sh = simd_usat16_8(simd_sadd16(simd_uxtb16_ror8(d), rh));
    return (se & 0xff) | ((sh & 0xff) << 8) | (((se >> 16) & 0xff) << 16) | (((sh >> 16) & 0xff) << 24);
}

/* hand-written ARM assembly half-pel motion comp (mc_asm.s); aligned src + width%4==0 only. */
#if defined(__arm__)
extern void mc_havg_a(uint8_t *dst, const uint8_t *src, int w, int h, int ds, int ss);
extern void mc_vavg_a(uint8_t *dst, const uint8_t *src, int w, int h, int ds, int ss);
extern void mc_diag_a(uint8_t *dst, const uint8_t *src, int w, int h, int ds, int ss);
#else
/* host verification build has no ARM asm -- faithful scalar equivalents (bit-identical) so the
 * opt 0x100 path still decodes correctly off-target. */
static void mc_havg_a(uint8_t *dst, const uint8_t *src, int w, int h, int ds, int ss) {
    for (int y = 0; y < h; y++) { for (int x = 0; x < w; x++) dst[x] = (uint8_t)((src[x] >> 1) + (src[x + 1] >> 1)); dst += ds; src += ss; } }
static void mc_vavg_a(uint8_t *dst, const uint8_t *src, int w, int h, int ds, int ss) {
    for (int y = 0; y < h; y++) { for (int x = 0; x < w; x++) dst[x] = (uint8_t)((src[x] >> 1) + (src[x + ss] >> 1)); dst += ds; src += ss; } }
static void mc_diag_a(uint8_t *dst, const uint8_t *src, int w, int h, int ds, int ss) {
    for (int y = 0; y < h; y++) { for (int x = 0; x < w; x++)
        dst[x] = (uint8_t)((((src[x] >> 1) + (src[x + 1] >> 1)) >> 1) + (((src[x + ss] >> 1) + (src[x + 1 + ss] >> 1)) >> 1));
        dst += ds; src += ss; } }
#endif

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

/* ---- deferred reconstruction (dual-core). The serial decode reads the bitstream and RECORDS the
 * per-block PIXEL work as jobs (recon_mode = RECON_RECORD); a second pass REPLAYS the jobs in decode
 * order (bit-exact single-thread) or splits them across two cores. The intricate bitstream parsing
 * stays inline and unchanged -- only the final pixel-write becomes a recorded job. ---- */
enum { RECON_INLINE = 0, RECON_RECORD = 1 };   /* recon_mode: 0 = original (parse+reconstruct fused) */
enum { RJ_MC = 0, RJ_IDCT = 1, RJ_INTRA = 2 };
typedef struct ReconJob {
    uint8_t  type, plane, size, param;   /* param = MC method / intra pmode */
    uint8_t  mb_start;                    /* 1 = first job of a macroblock (a safe parallel split point) */
    uint8_t *dst; int dst_ls;
    const uint8_t *src; int src_ls; int w, h;      /* MC */
    int matoff, ac; unsigned rowmask;              /* IDCT: coeff-pool offset + sparsity */
    int ax, ay, golomb;                            /* INTRA: position + mode-2 bitstream value */
} ReconJob;

/* One frame's worth of recorded reconstruction work. Double-buffered (rset[2]) so the cross-frame
 * pipeline can REPLAY set A (frame N-1) on the worker core while the main core PARSES frame N into
 * set B. Lists pre-partitioned by phase so replay never scans:
 *   [0] = inter MC + its residual IDCT (interleaved, block-hot; split at mb_start)   [2] = intra (serial) */
typedef struct ReconSet {
    ReconJob *rjob[3]; int rjob_n[3], rjob_cap[3];
    int      *rmat; int rmat_n, rmat_cap;   /* sparse coefficient pool */
    AVFrame  *out;                           /* frame this set reconstructs into (pipeline output) */
    int       valid;                         /* has pending replay work */
} ReconSet;

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

    int       recon_mode;                /* RECON_INLINE (default) or RECON_RECORD */
    int       rec_intra;                 /* 1 while recording an intra macroblock */
    int       rec_mark;                  /* 1 = next list-0 job begins a new macroblock (split point) */
    ReconSet  rset[2];                   /* double-buffered job sets (pipeline ping-pong) */
    int       parse_slot;                /* which set the current parse records into */
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
        return AVERROR_INVALIDDATA;
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
        return AVERROR_INVALIDDATA;

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

/* Coefficient blocks are stored as int16 (packed 16-bit = SIMD-ready). Verified bit-exact: transform
 * inputs peak ~14k and outputs ~13k, both well inside int16. The 8-point butterfly's `tmp`/`e..h`/`x`
 * intermediates can transiently hit ~49k before cancelling, so those locals stay 32-bit `int`. */
typedef int16_t dctcoef;
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

static void idct(dctcoef *arr, int size)
{
    int e, f, g, h;
    unsigned x3, x2, x1, x0;
    int tmp[4];

    if (size == 4) {
        tmp[0] = arr[0]; tmp[1] = arr[1]; tmp[2] = arr[2]; tmp[3] = arr[3];
        inverse4((unsigned *)tmp);
        arr[0] = tmp[0]; arr[1] = tmp[1]; arr[2] = tmp[2]; arr[3] = tmp[3];
        return;
    }

    tmp[0] = arr[0];
    tmp[1] = arr[2];
    tmp[2] = arr[4];
    tmp[3] = arr[6];

    inverse4((unsigned *)tmp);

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

/* ---- PACKED 2-row IDCT (opt MOBI_SIMD_IDCT): each u32 lane = one row; process two rows at once
 * with sadd16/ssub16/shadd16. Bit-exact with idct() as long as intermediates fit int16 (real content
 * peaks ~8k; a per-block coeff gate keeps it safe). Shifts: SH1=>>1, SH2=>>2 via signed halving add. ---- */
static inline uint32_t P16(int lo, int hi){ return ((uint32_t)lo & 0xffff) | ((uint32_t)hi << 16); }
static inline int P_lo(uint32_t a){ return (int16_t)(a & 0xffff); }
static inline int P_hi(uint32_t a){ return (int16_t)(a >> 16); }
#define SH1(a) simd_shadd16((a), 0)
#define SH2(a) simd_shadd16(simd_shadd16((a), 0), 0)
static void inverse4_pair(uint32_t *w){
    uint32_t a = simd_sadd16(w[0], w[2]), b = simd_ssub16(w[0], w[2]);
    uint32_t c = simd_sadd16(w[1], SH1(w[3])), d = simd_ssub16(SH1(w[1]), w[3]);
    w[0] = simd_sadd16(a, c); w[1] = simd_sadd16(b, d); w[2] = simd_ssub16(b, d); w[3] = simd_ssub16(a, c);
}
static void idct8_pair(uint32_t *w){
    uint32_t tmp[4] = { w[0], w[2], w[4], w[6] };
    inverse4_pair(tmp);
    uint32_t e = simd_ssub16(simd_ssub16(simd_sadd16(w[7], w[1]), w[3]), SH1(w[3]));
    uint32_t f = simd_sadd16(simd_sadd16(simd_ssub16(w[7], w[1]), w[5]), SH1(w[5]));
    uint32_t g = simd_ssub16(simd_ssub16(simd_ssub16(w[5], w[3]), w[7]), SH1(w[7]));
    uint32_t h = simd_sadd16(simd_sadd16(simd_sadd16(w[5], w[3]), w[1]), SH1(w[1]));
    uint32_t x3 = simd_sadd16(g, SH2(h)), x2 = simd_sadd16(e, SH2(f));
    uint32_t x1 = simd_ssub16(SH2(e), f), x0 = simd_ssub16(h, SH2(g));
    w[0] = simd_sadd16(tmp[0], x0); w[1] = simd_sadd16(tmp[1], x1); w[2] = simd_sadd16(tmp[2], x2); w[3] = simd_sadd16(tmp[3], x3);
    w[4] = simd_ssub16(tmp[3], x3); w[5] = simd_ssub16(tmp[2], x2); w[6] = simd_ssub16(tmp[1], x1); w[7] = simd_ssub16(tmp[0], x0);
}
static void idct4_pair(uint32_t *w){ inverse4_pair(w); }
/* Transform two adjacent rows (rowA=&mat[y*size], rowB=&mat[(y+1)*size]) of an int16 block in place. */
static void idct_pair_rows(dctcoef *rowA, dctcoef *rowB, int size){
    uint32_t w[8];
    for (int i = 0; i < size; i++) w[i] = P16(rowA[i], rowB[i]);
    if (size == 8) idct8_pair(w); else idct4_pair(w);
    for (int i = 0; i < size; i++) { rowA[i] = (dctcoef)P_lo(w[i]); rowB[i] = (dctcoef)P_hi(w[i]); }
}

static inline void read_run_encoding(GetBitContext *gb, int tab_idx,
                                     int *last, int *run, int *level)
{
    int n = get_vlc2(gb, rl_vlc[tab_idx], MOBI_RL_VLC_BITS, 1);

    *last = (n >> 11) == 1;
    *run  = (n >> 5) & 0x3F;
    *level = n & 0x1F;
}

/* ---- deferred-reconstruction record helpers (record into the set currently being parsed) ---- */
static ReconJob *rj_new(MobiClipContext *s, int k) {   /* k = phase list: 0 inter MC+IDCT, 2 intra */
    ReconSet *rs = &s->rset[s->parse_slot];
    if (rs->rjob_n[k] >= rs->rjob_cap[k]) {
        int nc = rs->rjob_cap[k] ? rs->rjob_cap[k] * 2 : 4096;
        ReconJob *nj = av_realloc(rs->rjob[k], (size_t)nc * sizeof(ReconJob));
        if (!nj) return NULL;
        rs->rjob[k] = nj; rs->rjob_cap[k] = nc;
    }
    ReconJob *j = &rs->rjob[k][rs->rjob_n[k]++];
    memset(j, 0, sizeof *j);
    if (k == 0 && s->rec_mark) { j->mb_start = 1; s->rec_mark = 0; }   /* mark this MB's first inter job */
    return j;
}
/* append a coefficient block to the pool, return its offset (jobs store the OFFSET, so pool realloc
 * is safe). -1 on OOM. */
/* Store the block's coefficients SPARSELY: [count, idx0,val0, idx1,val1, ...]. A residual block
 * averages ~4 non-zero coeffs, so this moves ~9 ints instead of a dense 64 -- far less memory
 * traffic on the record->replay round-trip (the dominant dual-core overhead on Old-3DS). */
static int rmat_put(MobiClipContext *s, const dctcoef *mat, int n) {
    ReconSet *rs = &s->rset[s->parse_slot];
    int need = 1 + 2 * n;                                 /* worst case: all non-zero */
    if (rs->rmat_n + need > rs->rmat_cap) {
        int nc = rs->rmat_cap ? rs->rmat_cap * 2 : 65536;
        while (nc < rs->rmat_n + need) nc *= 2;
        int *nm = av_realloc(rs->rmat, (size_t)nc * sizeof(int));
        if (!nm) return -1;
        rs->rmat = nm; rs->rmat_cap = nc;
    }
    int off = rs->rmat_n;
    int *p = rs->rmat + off + 1;
    int cnt = 0;
    for (int i = 0; i < n; i++) if (mat[i]) { *p++ = i; *p++ = mat[i]; cnt++; }
    rs->rmat[off] = cnt;
    rs->rmat_n = off + 1 + 2 * cnt;
    return off;
}
static void idct_writeback(dctcoef *mat, uint8_t *dst, int linesize, int size, int ac, unsigned rowmask);
/* Rebuild a dense coefficient block from the set's sparse pool, then run the IDCT + write-back. */
static inline void idct_job(ReconSet *rs, const ReconJob *j) {
    dctcoef mat[64];
    const int *p = rs->rmat + j->matoff;
    int cnt = *p++;
    memset(mat, 0, (size_t)(j->size * j->size) * sizeof(dctcoef));
    for (int i = 0; i < cnt; i++) { int idx = *p++; mat[idx] = *p++; }
    idct_writeback(mat, j->dst, j->dst_ls, j->size, j->ac, j->rowmask);
}

/* the RECONSTRUCT half of add_coefficients: IDCT the coefficient block (mat) and add it to dst,
 * saturating. Extracted verbatim from add_coefficients_impl so inline + replay are bit-identical. */
#define MOBI_SIMD_IDCT 0x04000000   /* packed 2-row IDCT (experiment) */
static void idct_writeback(dctcoef *mat, uint8_t *dst, int linesize, int size, int ac, unsigned rowmask)
{
    if (mobi_opt & MOBI_SIMD_IDCT) {                 /* packed 2-row transform (both passes) */
        for (int y = 0; y < size; y += 2) idct_pair_rows(&mat[y * size], &mat[(y + 1) * size], size);
        for (int y = 0; y < size; y++)               /* transpose (scalar) */
            for (int x = y + 1; x < size; x++) {
                dctcoef t = mat[x * size + y]; mat[x * size + y] = mat[y * size + x]; mat[y * size + x] = t;
            }
        for (int y = 0; y < size; y += 2) idct_pair_rows(&mat[y * size], &mat[(y + 1) * size], size);
        for (int y = 0; y < size; y++) {
            dctcoef *mr = &mat[y * size];
            for (int x = 0; x < size; x += 4)
                st4(dst + x, simd_add_clip4(ld4(dst + x), mr[x] >> 6, mr[x + 1] >> 6, mr[x + 2] >> 6, mr[x + 3] >> 6));
            dst += linesize;
        }
        return;
    }
    if ((mobi_opt & 4) && ac == 0) {          /* DC-only block -> uniform delta */
        dctcoef t[8] = { 0 }; t[0] = mat[0]; idct(t, size);
        dctcoef u[8] = { 0 }; u[0] = t[0];        idct(u, size);
        int d = u[0] >> 6;
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) dst[x] = av_clip_uint8(dst[x] + d);
            dst += linesize;
        }
        return;
    }
    if (mobi_opt & 2) {
        for (int y = 0; y < size; y++)
            if (rowmask & (1u << y))
                idct(&mat[y * size], size);
    } else {
        for (int y = 0; y < size; y++)
            idct(&mat[y * size], size);
    }
    for (int y = 0; y < size; y++) {
        for (int x = y + 1; x < size; x++) {
            int a = mat[x * size + y];
            int b = mat[y * size + x];
            mat[y * size + x] = a;
            mat[x * size + y] = b;
        }
        idct(&mat[y * size], size);
        dctcoef *mr = &mat[y * size];
        if (mobi_opt & 0x40) {           /* SIMD residual add+clamp: 4 pixels/instruction (size is 4 or 8) */
            for (int x = 0; x < size; x += 4)
                st4(dst + x, simd_add_clip4(ld4(dst + x), mr[x] >> 6, mr[x + 1] >> 6, mr[x + 2] >> 6, mr[x + 3] >> 6));
        } else if (mobi_opt & 32)
            for (int x = 0; x < size; x++)
                dst[x] = g_clamp[CLAMP_OFF + dst[x] + (mr[x] >> 6)];
        else
            for (int x = 0; x < size; x++)
                dst[x] = av_clip_uint8(dst[x] + (mr[x] >> 6));
        dst += linesize;
    }
}

static int add_coefficients_impl(AVCodecContext *avctx, AVFrame *frame,
                            int bx, int by, int size, int plane)
{
    MobiClipContext *s = avctx->priv_data;
    GetBitContext *gb = &s->gb;
    dctcoef mat[64] = { 0 };
    const uint8_t *ztab = size == 8 ? ff_zigzag_direct : zigzag4x4_tab;
    const int *qtab = s->qtab[size == 8];
    uint8_t *dst = frame->data[plane] + by * frame->linesize[plane] + bx;
    unsigned rowmask = 1;   /* row 0 is always live (mat[0] += 32 below) */
    int ac = 0;             /* number of AC (non-DC) coefficients */
    if ((mobi_opt & 32) && !g_clamp_ready) clamp_init();
    uint64_t _te = PROF_NOW();

    for (int pos = 0; get_bits_left(gb) > 0; pos++) {
        int qval, last, run, level;

        read_run_encoding(gb, s->dct_tab_idx, &last, &run, &level);

        if (level) {
            if (get_bits1(gb))
                level = -level;
        } else if (!get_bits1(gb)) {
            read_run_encoding(gb, s->dct_tab_idx, &last, &run, &level);
            level += run_residue[s->dct_tab_idx][(last ? 64 : 0) + run];
            if (get_bits1(gb))
                level = -level;
        } else if (!get_bits1(gb)) {
            read_run_encoding(gb, s->dct_tab_idx, &last, &run, &level);
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
            return AVERROR_INVALIDDATA;
        qval = qtab[pos];
        int zp = ztab[pos];
        mat[zp] = qval * (unsigned)level;
        rowmask |= 1u << (zp / size);
        if (zp) ac++;                         /* count AC (non-DC) coefficients */

        if (last)
            break;
    }

    mobi_prof[5] += PROF_NOW() - _te;         /* bucket 5 = coefficient entropy decode */
    uint64_t _tt = PROF_NOW();

    mat[0] += 32;
    if (s->recon_mode == RECON_RECORD) {      /* defer the IDCT + write-back to the reconstruct pass */
        ReconJob *j = rj_new(s, s->rec_intra ? 2 : 0);   /* inter residual -> list 0, right after its MC (stays cache-hot) */
        int off = rmat_put(s, mat, size * size);
        if (!j || off < 0) return AVERROR(ENOMEM);
        j->type = RJ_IDCT; j->dst = dst; j->dst_ls = frame->linesize[plane];
        j->size = (uint8_t)size; j->ac = ac; j->rowmask = rowmask; j->matoff = off;
        mobi_prof[4] += PROF_NOW() - _tt;
        return 0;
    }
    idct_writeback(mat, dst, frame->linesize[plane], size, ac, rowmask);
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
        return AVERROR_INVALIDDATA;
    }
}

static int adjust(int x, int size)
{
    return size == 16 ? (x + 1) >> 1 : x;
}

/* Intra prediction sample fetch. Refactored to take the block descriptor by CONST POINTER plus the
 * (already-transformed) coordinates px,py in registers -- the old code passed the whole 36-byte
 * BlockXY struct BY VALUE per pixel through a function pointer. Bit-identical; just leaner calls. */
static uint8_t pget(const BlockXY *b, int px, int py)
{
    int rx, ry;

    if (px == -1 && py >= b->size)      { rx = -1; ry = b->size - 1; }
    else if (px >= -1 && py >= -1)      { rx = px; ry = py; }
    else if (px == -1 && py == -2)      { rx =  0; ry = -1; }
    else if (px == -2 && py == -1)      { rx = -1; ry =  0; }
    else                                { rx = px; ry = py; }   /* fall-through kept ret = b */

    int y = av_clip(b->ay + ry, 0, b->h - 1);
    int x = av_clip(b->ax + rx, 0, b->w - 1);

    return b->block[y * b->linesize + x];
}

static uint8_t half(int a, int b)
{
    return ((a + b) + 1) / 2;
}

static uint8_t half3(int a, int b, int c)
{
    return ((a + b + b + c) * 2 / 4 + 1) / 2;
}

static uint8_t pick_above(const BlockXY *b, int x, int y) { return pget(b, x, y - 1); }
static uint8_t pick_left (const BlockXY *b, int x, int y) { return pget(b, x - 1, y); }

static uint8_t half_horz(const BlockXY *b, int x, int y)
{
    return half3(pget(b, x - 1, y), pget(b, x, y), pget(b, x + 1, y));
}

static uint8_t half_vert(const BlockXY *b, int x, int y)
{
    return half3(pget(b, x, y - 1), pget(b, x, y), pget(b, x, y + 1));
}

static uint8_t pick_4(const BlockXY *b, int x, int y)
{
    if ((x % 2) == 0)
        return half(pget(b, -1, y + x / 2), pget(b, -1, y + x / 2 + 1));
    return half_vert(b, -1, y + x / 2 + 1);
}

static uint8_t pick_5(const BlockXY *b, int x, int y)
{
    if (x == 0)      return half(pget(b, -1, y - 1), pget(b, -1, y));
    else if (y == 0) return half_horz(b, x - 2, y - 1);
    else if (x == 1) return half_vert(b, x - 2, y - 1);
    else             return pget(b, x - 2, y - 1);
}

static uint8_t pick_6(const BlockXY *b, int x, int y)
{
    if (y == 0)      return half(pget(b, x - 1, -1), pget(b, x, -1));
    else if (x == 0) return half_vert(b, x - 1, y - 2);
    else if (y == 1) return half_horz(b, x - 1, y - 2);
    else             return pget(b, x - 1, y - 2);
}

static uint8_t pick_7(const BlockXY *b, int x, int y)
{
    int clr = pget(b, x - 1, y - 1);
    if (x && y)
        return clr;

    int acc1 = (x == 0) ? pget(b, -1, y)  : pget(b, x - 2, -1);
    int acc2 = (y == 0) ? pget(b, x, -1)  : pget(b, -1, y - 2);

    return half3(acc1, clr, acc2);
}

static uint8_t pick_8(const BlockXY *b, int x, int y)
{
    if (y == 0)                return half(pget(b, x, -1), pget(b, x + 1, -1));
    else if (y == 1)           return half_horz(b, x + 1, y - 2);
    else if (x < b->size - 1)  return pget(b, x + 1, y - 2);
    else if (y % 2 == 0)       return half(pget(b, y / 2 + b->size - 1, -1),
                                           pget(b, y / 2 + b->size, -1));
    else                       return half_horz(b, y / 2 + b->size, -1);
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
                       uint8_t (*pick)(const BlockXY *, int, int))
{
    BlockXY bxy;

    bxy.size = size;
    bxy.block = block;
    bxy.linesize = linesize;
    bxy.w = w;
    bxy.h = h;
    bxy.ay = ay;
    bxy.ax = ax;

    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++)
            block[ax + x + (ay + y) * linesize] = pick(&bxy, x, y);
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

/* the RECONSTRUCT half of intra prediction: the prediction fill (reads reconstructed neighbours,
 * writes the block). The mode-2 bitstream value is passed in (read serially in predict_intra), so
 * this is pure pixel work -> deferrable/parallelisable. Byte-identical to the inline switch. */
static int intra_predict_fill(uint8_t *pdata, int linesize, int w, int h,
                              int ax, int ay, int pmode, int size, int golomb)
{
        switch (pmode) {
    case 0:
        block_fill(pdata, size, linesize, w, h, ax, ay, pick_above);
        break;
    case 1:
        block_fill(pdata, size, linesize, w, h, ax, ay, pick_left);
        break;
    case 2:
        {
            int arr1[16];
            int arr2[16];
            uint8_t *top = pdata + FFMAX(ay - 1, 0) * linesize + ax;
            uint8_t *left = pdata + ay * linesize + FFMAX(ax - 1, 0);
            int bottommost = pdata[(ay + size - 1) * linesize + FFMAX(ax - 1, 0)];
            int rightmost = pdata[FFMAX(ay - 1, 0) * linesize + ax + size - 1];
            int avg = (bottommost + rightmost + 1) / 2 + 2 * av_clip(golomb, -(1<<16), 1<<16);
            int r6 = adjust(avg - bottommost, size);
            int r9 = adjust(avg - rightmost, size);
            int shift = adjust(size, size) == 8 ? 3 : 2;
            uint8_t *block;

            for (int x = 0; x < size; x++) {
                int val = top[x];
                arr1[x] = adjust(((bottommost - val) * (1 << shift)) + r6 * (x + 1), size);
            }

            for (int y = 0; y < size; y++) {
                int val = left[y * linesize];
                arr2[y] = adjust(((rightmost - val) * (1 << shift)) + r9 * (y + 1), size);
            }

            block = pdata + ay * linesize + ax;
            for (int y = 0; y < size; y++) {
                for (int x = 0; x < size; x++) {
                    block[x] = (((top[x] + left[0] + ((arr1[x] * (y + 1) +
                                                       arr2[y] * (x + 1)) >> 2 * shift)) + 1) / 2) & 0xFF;
                }
                block += linesize;
                left  += linesize;
            }
        }
        break;
    case 3:
        {
            uint8_t fill;

            if (ax == 0 && ay == 0) {
                fill = 0x80;
            } else if (ax >= 1 && ay >= 1) {
                int left = block_sum(pdata + ay * linesize + ax - 1,
                                     1, size, linesize);
                int top  = block_sum(pdata + (ay - 1) * linesize + ax,
                                     size, 1, linesize);

                fill = ((left + top) * 2 / (2 * size) + 1) / 2;
            } else if (ax >= 1) {
                fill = (block_sum(pdata + ay * linesize + ax - 1,
                                  1, size, linesize) * 2 / size + 1) / 2;
            } else if (ay >= 1) {
                fill = (block_sum(pdata + (ay - 1) * linesize + ax,
                                  size, 1, linesize) * 2 / size + 1) / 2;
            } else {
                return -1;
            }

            block_fill_simple(pdata + ay * linesize + ax,
                              size, linesize, fill);
        }
        break;
    case 4:
        block_fill(pdata, size, linesize, w, h, ax, ay, pick_4);
        break;
    case 5:
        block_fill(pdata, size, linesize, w, h, ax, ay, pick_5);
        break;
    case 6:
        block_fill(pdata, size, linesize, w, h, ax, ay, pick_6);
        break;
    case 7:
        block_fill(pdata, size, linesize, w, h, ax, ay, pick_7);
        break;
    case 8:
        block_fill(pdata, size, linesize, w, h, ax, ay, pick_8);
        break;
    }
    return 0;
}

static int predict_intra(AVCodecContext *avctx, AVFrame *frame, int ax, int ay,
                          int pmode, int add_coeffs, int size, int plane)
{
    MobiClipContext *s = avctx->priv_data;
    GetBitContext *gb = &s->gb;
    int w = avctx->width >> !!plane, h = avctx->height >> !!plane;
    int ret = 0;
    uint64_t _pt = PROF_NOW();

    int golomb = (pmode == 2) ? get_se_golomb(gb) : 0;   /* mode 2 reads one value (serial) */
    if (s->recon_mode == RECON_RECORD) {
        ReconJob *j = rj_new(s, 2); if (!j) return AVERROR(ENOMEM);
        j->type = RJ_INTRA; j->dst = frame->data[plane]; j->dst_ls = frame->linesize[plane];
        j->w = w; j->h = h; j->ax = ax; j->ay = ay; j->param = (uint8_t)pmode;
        j->size = (uint8_t)size; j->golomb = golomb;
    } else {
        if (intra_predict_fill(frame->data[plane], frame->linesize[plane], w, h, ax, ay, pmode, size, golomb) < 0) return -1;
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
        return AVERROR_INVALIDDATA;

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
    s->rec_intra = 1;   /* this whole macroblock is intra (neighbour-dependent) -> serial phase */
    int flags, pmode_uv, idx = get_ue_golomb(gb);
    int ret = 0;

    if (idx < 0 || idx >= FF_ARRAY_ELEMS(block8x8_coefficients_tab))
        return AVERROR_INVALIDDATA;

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

/* the RECONSTRUCT half of motion comp: the extracted per-block copy (all opt paths), so inline and
 * replay are byte-identical. Pure pixel work: reads src (ref frame), writes dst. */
static void mc_copy(uint8_t *dst, const uint8_t *src, int width, int height,
                    int dst_linesize, int src_linesize, int method)
{
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

#ifdef MOBI_REFDIAG
unsigned long g_mobi_refdist[8];   /* [d&7] = motion blocks referencing d frames back; ODD d = cross-eye in 3D */
#endif
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
#ifdef MOBI_REFDIAG
        g_mobi_refdist[FFMAX(1, index) & 7]++;   /* reference-distance histogram (odd = cross-eye) */
#endif
        MotionXY mv = s->motion[0];

        if (sidx < 0)
            sidx += 6;

        if (index > 0) {
            mv.x = mv.x + (unsigned)get_se_golomb(gb);
            mv.y = mv.y + (unsigned)get_se_golomb(gb);
        }
        if (mv.x >= INT_MAX || mv.y >= INT_MAX)
            return AVERROR_INVALIDDATA;

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
                return AVERROR_INVALIDDATA;

            method = (mv.x & 1) | ((mv.y & 1) << 1);
            src_linesize = s->pic[sidx]->linesize[i];
            dst_linesize = s->pic[s->current_pic]->linesize[i];
            dst = s->pic[s->current_pic]->data[i] + offsetx + offsety * dst_linesize;

            if (offsetx + (mv.x >> 1) < 0 ||
                offsety + (mv.y >> 1) < 0 ||
                offsetx + width  + (mv.x + 1 >> 1) > fwidth ||
                offsety + height + (mv.y + 1 >> 1) > fheight)
                return AVERROR_INVALIDDATA;

            src = s->pic[sidx]->data[i] + offsetx + (mv.x >> 1) +
                           (offsety + (mv.y >> 1)) * src_linesize;
          if (s->recon_mode == RECON_RECORD) {   /* defer the copy to the reconstruct pass */
            ReconJob *j = rj_new(s, 0); if (!j) return AVERROR(ENOMEM);
            j->type = RJ_MC; j->dst = dst; j->src = src; j->w = width; j->h = height;
            j->dst_ls = dst_linesize; j->src_ls = src_linesize; j->param = (uint8_t)method;
          } else {
            mc_copy(dst, src, width, height, dst_linesize, src_linesize, method);
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

#define MOBI_RECON2 0x10000000   /* two-pass deferred reconstruction (dual-core foundation) */

/* Replay reconstruction jobs, PRE-PARTITIONED into phase lists during the parse so this pass never
 * scans or filters:
 *   list 0  inter: MC + its residual IDCT, interleaved in decode order -- each block reads the
 *           REFERENCE frame / its own pixels, so blocks are independent. Kept together (MC then
 *           IDCT per block) so the block stays cache-hot; split across two cores at block boundaries.
 *   list 2  intra pred+res  -- reads reconstructed NEIGHBOURS -> serial, decode order (runs last)
 * (list 1 is unused now that inter MC+IDCT share list 0.)
 * Host: single-threaded (proves the reorder is bit-exact). Device: list 0 split across two cores at
 * block boundaries, list 2 stays serial. */
static void recon_phase(ReconSet *rs, int lo, int hi, int k) {   /* jobs [lo,hi) of list k */
    ReconJob *a = rs->rjob[k];
    for (int i = lo; i < hi; i++) {
        ReconJob *j = &a[i];
        if (j->type == RJ_MC) mc_copy(j->dst, j->src, j->w, j->h, j->dst_ls, j->src_ls, j->param);
        else                  idct_job(rs, j);
    }
}
static void recon_list2(ReconSet *rs) {   /* intra: reads reconstructed neighbours -> serial, decode order */
    ReconJob *a = rs->rjob[2];
    for (int i = 0; i < rs->rjob_n[2]; i++) {
        ReconJob *j = &a[i];
        if (j->type == RJ_INTRA) intra_predict_fill(j->dst, j->dst_ls, j->w, j->h, j->ax, j->ay, j->param, j->size, j->golomb);
        else                     idct_job(rs, j);
    }
}
/* Reconstruct a whole set single-threaded (host path, and the pipeline worker's job). */
static void recon_set_serial(ReconSet *rs) {
    recon_phase(rs, 0, rs->rjob_n[0], 0);             /* inter MC+IDCT */
    recon_list2(rs);                                  /* intra */
}

#define MOBI_PAR      0x20000000   /* split list 0 across two CPU cores within a frame (device only) */
#define MOBI_REVSPLIT 0x40000000   /* host-only: run the split-halves reversed to prove independence */
#define MOBI_PIPE     0x08000000   /* cross-frame pipeline: reconstruct(N-1) on worker || parse(N) on main */
#ifdef __3DS__
#include <3ds.h>
static Thread            g_rthr;
static LightEvent        g_rgo, g_rdone;
static ReconSet * volatile g_rset;
static volatile int      g_rwhich, g_rlo, g_rhi, g_rrun, g_rfull;
static void recon_worker(void *a) {
    (void)a;
    while (g_rrun) {
        LightEvent_Wait(&g_rgo);
        if (!g_rrun) break;
        if (g_rfull) {
            recon_set_serial(g_rset);                          /* pipeline: whole frame on this core */
            /* This core wrote the whole frame; clean ITS cache to main RAM so the Y2R DMA (which the
             * main core kicks) doesn't read stale rows -> otherwise a half-flushed frame shows as a
             * left/right split after the GPU rotation. */
            /* Use the RAW cache syscall, NOT GSPGPU_FlushDataCache: the latter goes through the shared
             * GSP session, and calling it from this worker core while the main thread renders can wedge
             * the GSP (persistent black screen until app restart). svcFlushProcessDataCache is a pure
             * per-core syscall with no shared session. */
            AVFrame *o = g_rset->out;
            if (o && o->data[0]) {
                svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)o->data[0], (u32)o->linesize[0] * (u32)o->height);
                if (o->data[1]) svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)o->data[1], (u32)o->linesize[1] * (u32)((o->height + 1) / 2));
                if (o->data[2]) svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)o->data[2], (u32)o->linesize[2] * (u32)((o->height + 1) / 2));
            }
        } else {
            recon_phase(g_rset, g_rlo, g_rhi, g_rwhich);
        }
        LightEvent_Signal(&g_rdone);
    }
}
static void recon_par_init(void) {
    if (g_rthr) return;
    g_rrun = 1;
    LightEvent_Init(&g_rgo,   RESET_ONESHOT);
    LightEvent_Init(&g_rdone, RESET_ONESHOT);
    bool isnew = false; APT_CheckNew3DS(&isnew);
    int core = isnew ? 2 : 1;                 /* New: free core 2; Old: syscore (needs the time limit) */
    if (!isnew) APT_SetAppCpuTimeLimit(80);
    s32 prio = 0x30; svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    g_rthr = threadCreate(recon_worker, NULL, 128 * 1024, prio - 1, core, false);
}
void mobi_recon_par_exit(void) {              /* call on app shutdown to join the worker */
    if (!g_rthr) return;
    g_rrun = 0; LightEvent_Signal(&g_rgo);
    threadJoin(g_rthr, UINT64_MAX); threadFree(g_rthr); g_rthr = NULL;
}
/* run one phase across two cores: worker does the second half, main the first, then barrier. */
static void recon_phase_par(ReconSet *rs, int which) {
    int n = rs->rjob_n[which];
    if (n < 16 || !g_rthr) { recon_phase(rs, 0, n, which); return; }
    int mid = n / 2;
    ReconJob *a = rs->rjob[which];
    while (mid < n && !a[mid].mb_start) mid++;   /* split only at a macroblock boundary so each MB runs wholly on one core */
    if (mid >= n) { recon_phase(rs, 0, n, which); return; }
    g_rfull = 0; g_rset = rs; g_rwhich = which; g_rlo = mid; g_rhi = n;
    LightEvent_Signal(&g_rgo);
    recon_phase(rs, 0, mid, which);
    LightEvent_Wait(&g_rdone);
}
/* pipeline: hand a whole set to the worker core (non-blocking); pair with recon_pipe_wait(). */
static void recon_pipe_kick(ReconSet *rs) {
    recon_par_init();
    g_rfull = 1; g_rset = rs;
    LightEvent_Signal(&g_rgo);
}
static void recon_pipe_wait(void) { LightEvent_Wait(&g_rdone); }
#else
/* Host: exercise the pipeline bookkeeping synchronously (no worker) to prove frame/slot logic is bit-exact. */
static void recon_pipe_kick(ReconSet *rs) { recon_set_serial(rs); }
static void recon_pipe_wait(void) { }
#endif

/* Immediate (non-pipelined) reconstruct of one set: serial, within-frame dual-core, or host revsplit. */
static void recon_execute(MobiClipContext *s, ReconSet *rs) {
#ifdef __3DS__
    uint64_t _rt = svcGetSystemTick();
    if (mobi_opt & MOBI_PAR) {
        recon_par_init();
        recon_phase_par(rs, 0);                       /* inter MC+IDCT, split across 2 cores at block boundaries */
        recon_list2(rs);
        mobi_recon_ticks += svcGetSystemTick() - _rt;
        return;
    }
#endif
    if (mobi_opt & MOBI_REVSPLIT) {                   /* host proxy for the parallel hazard: run the two
                                                         split-halves in REVERSED order. Bit-exact <=> the
                                                         mb_start split points are truly independent. */
        int n = rs->rjob_n[0], mid = n / 2;
        ReconJob *a0 = rs->rjob[0];
        while (mid < n && !a0[mid].mb_start) mid++;
        recon_phase(rs, mid, n, 0);
        recon_phase(rs, 0, mid, 0);
        recon_list2(rs);
    } else {
        recon_set_serial(rs);
    }
#ifdef __3DS__
    mobi_recon_ticks += svcGetSystemTick() - _rt;
#endif
}

static int mobiclip_decode(AVCodecContext *avctx, AVFrame *rframe,
                           int *got_frame, AVPacket *pkt)
{
    MobiClipContext *s = avctx->priv_data;
    GetBitContext *gb = &s->gb;
    int ret;

    s->recon_mode = (mobi_opt & MOBI_RECON2) ? RECON_RECORD : RECON_INLINE;
    int pipe = (mobi_opt & MOBI_PIPE) && s->recon_mode == RECON_RECORD;

    /* flush (empty packet): drain the one frame still pending in the pipeline */
    if (!pkt->data || pkt->size == 0) {
        *got_frame = 0;
        for (int sl = 0; sl < 2; sl++) if (s->rset[sl].valid) {
            recon_set_serial(&s->rset[sl]);
            s->rset[sl].valid = 0;
            if ((ret = av_frame_ref(rframe, s->rset[sl].out)) < 0) return ret;
            *got_frame = 1; break;
        }
        return 0;
    }

    int slot = pipe ? s->parse_slot : 0;
    ReconSet *rs = &s->rset[slot];
    s->parse_slot = slot;                      /* rj_new / rmat_put record into rs */
    AVFrame *frame = s->pic[s->current_pic];

    if (avctx->height/16 * (avctx->width/16) * 2 > 8LL*FFALIGN(pkt->size, 2))
        return AVERROR_INVALIDDATA;

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

    if (s->recon_mode == RECON_RECORD) {
        rs->rjob_n[0] = rs->rjob_n[1] = rs->rjob_n[2] = 0; rs->rmat_n = 0; rs->out = frame;
    }

    ReconSet *pend = (pipe && s->rset[slot ^ 1].valid) ? &s->rset[slot ^ 1] : NULL;
    if (pend) recon_pipe_kick(pend);           /* reconstruct N-1 on the worker, concurrently with parse below */

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
                s->rec_intra = 0;   /* default: inter MB (decode_macroblock sets 1 for intra MBs) */
                s->rec_mark  = 1;   /* this MB's first list-0 job is a safe parallel split boundary */

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
                        return AVERROR_INVALIDDATA;
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

    if (s->recon_mode == RECON_RECORD) {
        if (pipe) {
            if (pend) recon_pipe_wait();      /* N-1 reconstruct finished (overlapped the parse above) */
            rs->valid = 1;                    /* this frame becomes next call's pending reconstruct */
            s->parse_slot = slot ^ 1;
        } else
            recon_execute(s, rs);             /* immediate: replay reconstruction into frame */
    }

    if (!s->moflex)
        avctx->colorspace = AVCOL_SPC_YCGCO;

    s->current_pic = (s->current_pic + 1) % 6;

    if (pipe) {
        if (pend) { if ((ret = av_frame_ref(rframe, pend->out)) < 0) return ret; pend->valid = 0; *got_frame = 1; }
        else *got_frame = 0;                  /* priming: first frame parsed, not yet reconstructed/output */
    } else {
        if ((ret = av_frame_ref(rframe, frame)) < 0) return ret;
        *got_frame = 1;
    }

    return 0;
}

static void mobiclip_flush(AVCodecContext *avctx)
{
    MobiClipContext *s = avctx->priv_data;

    for (int i = 0; i < 6; i++)
        av_frame_unref(s->pic[i]);
    /* drop any pending pipeline frame so a seek doesn't reconstruct against a stale reference */
    s->rset[0].valid = s->rset[1].valid = 0;
    s->parse_slot = 0;
}

static av_cold int mobiclip_close(AVCodecContext *avctx)
{
    MobiClipContext *s = avctx->priv_data;

    av_freep(&s->bitstream);
    s->bitstream_size = 0;
    av_freep(&s->motion);
    s->motion_size = 0;
    for (int sl = 0; sl < 2; sl++) {
        ReconSet *rs = &s->rset[sl];
        for (int k = 0; k < 3; k++) { av_freep(&rs->rjob[k]); rs->rjob_cap[k] = rs->rjob_n[k] = 0; }
        av_freep(&rs->rmat); rs->rmat_cap = rs->rmat_n = 0;
    }

    for (int i = 0; i < 6; i++) {
        av_frame_free(&s->pic[i]);
    }

    return 0;
}

/* ---- dual-core reconstruction SCALING benchmark (not part of decode). Reconstructs luma rows
 * [y0,y1) of a WxH frame using the REAL kernels: diagonal half-pel motion comp (u8avg, the expensive
 * MC path) + a full 8x8 2D IDCT + clip-add. This is representative of decode reconstruction, which is
 * ~80% of decode time. Run it on ONE core (full frame) vs TWO cores (each half) to measure whether the
 * ARM11 memory bus lets a 2nd core add real throughput or caps it -> decides dual-core reconstruction. */
void mobi_recon_bench(const uint8_t *ref, uint8_t *dst, int W, int H, int y0, int y1)
{
    for (int by = y0; by + 8 <= y1; by += 8) {
        for (int bx = 0; bx + 8 <= W; bx += 8) {
            const uint8_t *src = ref + (size_t)by * W + bx;
            uint8_t *d = dst + (size_t)by * W + bx;
            for (int y = 0; y < 8; y++) {           /* motion comp: diagonal half-pel average */
                for (int x = 0; x < 8; x += 4)
                    st4(d + x, u8avg(u8avg(ld4(src + x), ld4(src + x + 1)),
                                     u8avg(ld4(src + x + W), ld4(src + x + 1 + W))));
                d += W; src += W;
            }
            dctcoef blk[8][8], col[8];              /* residual: real 8x8 2D IDCT (8 rows + 8 cols) */
            for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) blk[y][x] = ((bx + x) ^ (by + y)) & 31;
            for (int y = 0; y < 8; y++) idct(blk[y], 8);
            for (int x = 0; x < 8; x++) { for (int y = 0; y < 8; y++) col[y] = blk[y][x];
                                          idct(col, 8);
                                          for (int y = 0; y < 8; y++) blk[y][x] = col[y]; }
            d = dst + (size_t)by * W + bx;          /* clip-add the residual */
            for (int y = 0; y < 8; y++) { for (int x = 0; x < 8; x++) { int v = d[x] + (blk[y][x] >> 6);
                                          d[x] = v < 0 ? 0 : v > 255 ? 255 : v; } d += W; }
        }
    }
}

/* ---- standalone entry points (replaces FFCodec registration) ---- */
int  mobi_init(AVCodecContext *avctx)  { return mobiclip_init(avctx); }
int  mobi_decode(AVCodecContext *avctx, AVFrame *frame, int *got, AVPacket *pkt)
                                       { return mobiclip_decode(avctx, frame, got, pkt); }
void mobi_flush(AVCodecContext *avctx) { mobiclip_flush(avctx); }
int  mobi_close(AVCodecContext *avctx) { return mobiclip_close(avctx); }
size_t mobi_ctx_size(void) { return sizeof(MobiClipContext); }
