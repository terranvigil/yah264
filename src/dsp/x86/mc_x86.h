/*
 * mc_x86.h - the 128-bit building blocks the mc and hpel kernels share
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Wave 2 of docs/x86-plan.md. Every helper here is `static inline` and
 * 128-bit, so the SSE4.2 file gets SSE encodings and the AVX2 file gets VEX
 * encodings of the same source -- and the AVX2 file uses several of them
 * whole, because a shape with no second half has nothing to put in an upper
 * lane (an 8-wide prediction row, a 16-byte plane copy). The load helpers
 * come from pixel_x86.h, which is where this tree's "load the block, never
 * the register" discipline is written down.
 *
 * FOUR FACTS CARRY THIS FILE.
 *
 * The six-tap (1, -5, 20, 20, -5, 1) over 8-bit samples lands in [-2550,
 * 10710], so the luma half-pel filters run entirely in SIGNED 16-BIT lanes and
 * only the centre plane, which filters the filtered, needs 32. That is what
 * makes a 16-wide row one register at SSE4.2 and what makes `-5` and `20` two
 * PMULLW rather than a widening dance.
 *
 * Clip1 is a saturating pack. PACKUSWB narrows signed 16-bit to unsigned 8-bit
 * with saturation at both ends, which IS clip(v, 0, 255) for every value the
 * filters produce, so the clip costs nothing beyond the store's own narrowing.
 * PACKSSDW does the same job one width up for the centre plane, where the
 * shifted result is already inside 16 bits and the pack is exact.
 *
 * The quarter-pel average is PAVGB. (a + b + 1) >> 1 is the rounding halving
 * add exactly, which is the one place x86's rounding is the rounding the
 * standard asks for rather than the one to be undone (compare the truncating
 * average the intra edge filter needs, pixel_x86.h).
 *
 * Chroma is PMADDUBSW. The bilinear weights (8-fx)(8-fy), fx(8-fy), (8-fx)fy
 * and fx*fy are all in [0, 64] and sum to 64, so interleaving the two source
 * columns byte-wise and multiplying by the weight PAIR gives w0*A + w1*B in
 * one instruction per pair, and the saturation PMADDUBSW carries cannot be
 * reached: the largest a pair can produce is 64*255.
 *
 * C11 intrinsics only. There is no assembly in this tree, and hygiene_check
 * refuses any inline form under this directory.
 */
#ifndef YAH264_DSP_X86_MC_X86_H
#define YAH264_DSP_X86_MC_X86_H

#include <immintrin.h>
#include <string.h>

#include "dsp/x86/pixel_x86.h"   /* the block-sized loads, and dsp/arch.h */

/* The row stride of the small half-pel planes a luma kernel builds on its own
 * stack. 24 leaves room for the 17th column the odd phases reach and keeps
 * every row's 16-byte store inside the buffer. */
#define Y264_MCL_ST 24

/* ---- the six-tap, in both widths ----------------------------------------- */

/* (a - 5b + 20c + 20d - 5e + f) over signed 16-bit lanes. Every intermediate
 * stays inside int16 for 8-bit inputs: (a+f) <= 510, 5*(b+e) <= 2550 and
 * 20*(c+d) <= 10200, so the running value never leaves [-2550, 10710]. */
static inline __m128i y264_tap6_16(__m128i a, __m128i b, __m128i c,
                                   __m128i d, __m128i e, __m128i f)
{
    __m128i s05 = _mm_add_epi16(a, f);
    __m128i s14 = _mm_add_epi16(b, e);
    __m128i s23 = _mm_add_epi16(c, d);
    __m128i t = _mm_sub_epi16(s05, _mm_mullo_epi16(s14, _mm_set1_epi16(5)));
    return _mm_add_epi16(t, _mm_mullo_epi16(s23, _mm_set1_epi16(20)));
}

/* The same tap over 32-bit lanes, for the centre plane and for the half-pel
 * plane build's vertical pass, whose inputs are themselves unclipped taps.
 * The constants are shifts and adds rather than PMULLD: x*5 is (x<<2)+x and
 * x*20 is (x<<4)+(x<<2), which is three cheap operations against one that is
 * not cheap on any part this tier targets. */
static inline __m128i y264_tap6_32(__m128i a, __m128i b, __m128i c,
                                   __m128i d, __m128i e, __m128i f)
{
    __m128i s05 = _mm_add_epi32(a, f);
    __m128i s14 = _mm_add_epi32(b, e);
    __m128i s23 = _mm_add_epi32(c, d);
    __m128i m5  = _mm_add_epi32(_mm_slli_epi32(s14, 2), s14);
    __m128i m20 = _mm_add_epi32(_mm_slli_epi32(s23, 4), _mm_slli_epi32(s23, 2));
    return _mm_add_epi32(_mm_sub_epi32(s05, m5), m20);
}

/* clip1((t + 16) >> 5) for eight lanes, packed into the low eight bytes. */
static inline __m128i y264_clip5_8(__m128i t)
{
    __m128i x = _mm_srai_epi16(_mm_add_epi16(t, _mm_set1_epi16(16)), 5);
    return _mm_packus_epi16(x, x);
}

/* The same for sixteen lanes held as two registers. */
static inline __m128i y264_clip5_16(__m128i lo, __m128i hi)
{
    __m128i r = _mm_set1_epi16(16);
    return _mm_packus_epi16(_mm_srai_epi16(_mm_add_epi16(lo, r), 5),
                            _mm_srai_epi16(_mm_add_epi16(hi, r), 5));
}

/* clip1((t + 512) >> 10) for two 32-bit quads, packed into eight bytes. The
 * intermediate pack is exact rather than merely saturating: the centre tap of
 * taps lands in [-112200, 475320] and the shift brings that to [-110, 464]. */
static inline __m128i y264_clip10_8(__m128i lo, __m128i hi)
{
    __m128i r = _mm_set1_epi32(512);
    __m128i a = _mm_srai_epi32(_mm_add_epi32(lo, r), 10);
    __m128i b = _mm_srai_epi32(_mm_add_epi32(hi, r), 10);
    __m128i s = _mm_packs_epi32(a, b);
    return _mm_packus_epi16(s, s);
}

/* Eight samples, zero-extended to 16-bit lanes. */
static inline __m128i y264_ld8w(const uint8_t *p)
{
    return _mm_cvtepu8_epi16(y264_ld64(p));
}

/* Horizontal six-tap over eight columns; `p` addresses sample (ix-2, row), so
 * the read is exactly thirteen bytes -- six loads of eight, not one of sixteen
 * that would declare three columns it never uses. */
static inline __m128i y264_hfilt8(const uint8_t *p)
{
    return y264_tap6_16(y264_ld8w(p), y264_ld8w(p + 1), y264_ld8w(p + 2),
                        y264_ld8w(p + 3), y264_ld8w(p + 4), y264_ld8w(p + 5));
}

/* Vertical six-tap over eight columns of six rows. */
static inline __m128i y264_vfilt8(const uint8_t *r0, const uint8_t *r1,
                                  const uint8_t *r2, const uint8_t *r3,
                                  const uint8_t *r4, const uint8_t *r5)
{
    return y264_tap6_16(y264_ld8w(r0), y264_ld8w(r1), y264_ld8w(r2),
                        y264_ld8w(r3), y264_ld8w(r4), y264_ld8w(r5));
}

/* The centre plane over eight columns: the vertical six-tap of six rows of
 * unclipped horizontal intermediates, widened to 32-bit lanes because a tap of
 * taps no longer fits in 16. */
static inline __m128i y264_jfilt8(const int16_t *c0, const int16_t *c1,
                                  const int16_t *c2, const int16_t *c3,
                                  const int16_t *c4, const int16_t *c5)
{
    __m128i r0 = _mm_loadu_si128((const __m128i *)(const void *)c0);
    __m128i r1 = _mm_loadu_si128((const __m128i *)(const void *)c1);
    __m128i r2 = _mm_loadu_si128((const __m128i *)(const void *)c2);
    __m128i r3 = _mm_loadu_si128((const __m128i *)(const void *)c3);
    __m128i r4 = _mm_loadu_si128((const __m128i *)(const void *)c4);
    __m128i r5 = _mm_loadu_si128((const __m128i *)(const void *)c5);
    __m128i lo = y264_tap6_32(_mm_cvtepi16_epi32(r0), _mm_cvtepi16_epi32(r1),
                              _mm_cvtepi16_epi32(r2), _mm_cvtepi16_epi32(r3),
                              _mm_cvtepi16_epi32(r4), _mm_cvtepi16_epi32(r5));
    __m128i hi = y264_tap6_32(_mm_cvtepi16_epi32(_mm_srli_si128(r0, 8)),
                              _mm_cvtepi16_epi32(_mm_srli_si128(r1, 8)),
                              _mm_cvtepi16_epi32(_mm_srli_si128(r2, 8)),
                              _mm_cvtepi16_epi32(_mm_srli_si128(r3, 8)),
                              _mm_cvtepi16_epi32(_mm_srli_si128(r4, 8)),
                              _mm_cvtepi16_epi32(_mm_srli_si128(r5, 8)));
    return y264_clip10_8(lo, hi);
}

/* ---- the 8-wide luma interpolation --------------------------------------
 *
 * The plane scheme is the portable one: build whichever of G (integer), H
 * (horizontal half), V (vertical half) and J (centre) this phase reads, then
 * emit each row as a copy of one of them or as the rounding average of two.
 * Column 8 of G and V is filled scalar because the phases that reach a column
 * further on read it and nothing else does.
 *
 * The caller guarantees the window: columns [ix-2, ix+10] and rows
 * [iy-2, iy+h+3] lie inside the reference's replicated border.
 */
static inline void y264_mc_luma8_x86(uint8_t *dst, int dstride,
                                     const uint8_t *ref, int rstride,
                                     int ix, int iy, int fx, int fy, int h)
{
    const int ST = Y264_MCL_ST;

    if (fx == 0 && fy == 0) {
        for (int y = 0; y < h; y++)
            _mm_storel_epi64((__m128i *)(void *)(dst + (size_t)y * dstride),
                             y264_ld64(ref + (size_t)(iy + y) * rstride + ix));
        return;
    }

    uint8_t G[17 * Y264_MCL_ST], H[17 * Y264_MCL_ST], V[17 * Y264_MCL_ST];
    uint8_t J[16 * Y264_MCL_ST];
    int16_t cc[21 * Y264_MCL_ST];

    int needH = (fx != 0);
    int needV = (fy != 0);
    int needJ = (fx == 2 && fy != 0) || (fy == 2 && fx != 0);
    int needG = ((fx & 1) && fy == 0) || (fx == 0 && (fy & 1));

    if (needG)
        for (int y = 0; y <= h; y++) {
            const uint8_t *p = ref + (size_t)(iy + y) * rstride + ix;
            _mm_storel_epi64((__m128i *)(void *)(G + y * ST), y264_ld64(p));
            G[y * ST + 8] = p[8];
        }
    if (needH)
        for (int y = 0; y <= h; y++)
            _mm_storel_epi64((__m128i *)(void *)(H + y * ST),
                y264_clip5_8(y264_hfilt8(ref + (size_t)(iy + y) * rstride + ix - 2)));
    if (needV)
        for (int y = 0; y <= h; y++) {
            const uint8_t *p = ref + (size_t)(iy + y) * rstride + ix;
            _mm_storel_epi64((__m128i *)(void *)(V + y * ST),
                y264_clip5_8(y264_vfilt8(p - 2 * rstride, p - rstride, p,
                                         p + rstride, p + 2 * rstride,
                                         p + 3 * rstride)));
            const uint8_t *q = p + 8;
            int s = q[-2 * rstride] - 5 * q[-rstride] + 20 * q[0]
                  + 20 * q[rstride] - 5 * q[2 * rstride] + q[3 * rstride];
            s = (s + 16) >> 5;
            V[y * ST + 8] = (uint8_t)(s < 0 ? 0 : s > 255 ? 255 : s);
        }
    if (needJ) {
        for (int y = -2; y <= h + 2; y++)
            _mm_storeu_si128((__m128i *)(void *)(cc + (y + 2) * ST),
                             y264_hfilt8(ref + (size_t)(iy + y) * rstride + ix - 2));
        for (int y = 0; y < h; y++)
            _mm_storel_epi64((__m128i *)(void *)(J + y * ST),
                y264_jfilt8(cc + (y + 0) * ST, cc + (y + 1) * ST,
                            cc + (y + 2) * ST, cc + (y + 3) * ST,
                            cc + (y + 4) * ST, cc + (y + 5) * ST));
    }

    for (int y = 0; y < h; y++) {
        const uint8_t *Gy = G + y * ST, *Hy = H + y * ST;
        const uint8_t *Vy = V + y * ST, *Jy = J + y * ST;
        __m128i out;
        switch (fy * 4 + fx) {
        case 0*4+1: out = _mm_avg_epu8(y264_ld64(Gy), y264_ld64(Hy)); break;
        case 0*4+2: out = y264_ld64(Hy); break;
        case 0*4+3: out = _mm_avg_epu8(y264_ld64(Gy + 1), y264_ld64(Hy)); break;
        case 1*4+0: out = _mm_avg_epu8(y264_ld64(Gy), y264_ld64(Vy)); break;
        case 2*4+0: out = y264_ld64(Vy); break;
        case 3*4+0: out = _mm_avg_epu8(y264_ld64(Gy + ST), y264_ld64(Vy)); break;
        case 1*4+1: out = _mm_avg_epu8(y264_ld64(Hy), y264_ld64(Vy)); break;
        case 1*4+2: out = _mm_avg_epu8(y264_ld64(Hy), y264_ld64(Jy)); break;
        case 1*4+3: out = _mm_avg_epu8(y264_ld64(Hy), y264_ld64(Vy + 1)); break;
        case 2*4+1: out = _mm_avg_epu8(y264_ld64(Vy), y264_ld64(Jy)); break;
        case 2*4+2: out = y264_ld64(Jy); break;
        case 2*4+3: out = _mm_avg_epu8(y264_ld64(Jy), y264_ld64(Vy + 1)); break;
        case 3*4+1: out = _mm_avg_epu8(y264_ld64(Vy), y264_ld64(Hy + ST)); break;
        case 3*4+2: out = _mm_avg_epu8(y264_ld64(Jy), y264_ld64(Hy + ST)); break;
        case 3*4+3: out = _mm_avg_epu8(y264_ld64(Vy + 1), y264_ld64(Hy + ST)); break;
        default:    out = y264_ld64(Gy); break;
        }
        _mm_storel_epi64((__m128i *)(void *)(dst + (size_t)y * dstride), out);
    }
}

/* ---- the 16-wide luma interpolation, once ---------------------------------
 *
 * The plane scheme above again at sixteen columns, and it is written ONCE
 * because only the three row filters differ between the tiers: at SSE4.2 a
 * row is two registers of eight lanes, at AVX2 it is one of sixteen, and the
 * quarter-pel average that ends it is a 16-byte PAVGB either way. The four
 * macro parameters are that tier's filters --
 *
 *   HCLIP(p, out)          horizontal six-tap, clipped, 16 bytes out
 *   HRAW(p, out16)         the same tap unclipped, 16 int16 out
 *   VCL(p, rstride, out)   vertical six-tap over six rows, clipped
 *   JCL(c0..c5, out)       the centre tap over six intermediate rows
 *
 * -- and everything else, the window, the +1 margin column and the phase
 * table, is the same argument at both.
 *
 * The caller guarantees columns [ix-2, ix+18] and rows [iy-2, iy+h+3] lie
 * inside the reference's replicated border.
 */
#define Y264_MC_LUMA16_BODY(NAME, HCLIP, HRAW, VCL, JCL)                       \
static void NAME(uint8_t *dst, int dstride, const uint8_t *ref, int rstride,   \
                 int ix, int iy, int fx, int fy, int h)                        \
{                                                                              \
    const int ST = Y264_MCL_ST;                                                \
                                                                               \
    if (fx == 0 && fy == 0) {                                                  \
        for (int y = 0; y < h; y++)                                            \
            _mm_storeu_si128((__m128i *)(void *)(dst + (size_t)y * dstride),   \
                y264_ld128(ref + (size_t)(iy + y) * rstride + ix));            \
        return;                                                                \
    }                                                                          \
                                                                               \
    uint8_t G[17 * Y264_MCL_ST], H[17 * Y264_MCL_ST], V[17 * Y264_MCL_ST];     \
    uint8_t J[16 * Y264_MCL_ST];                                               \
    int16_t cc[21 * Y264_MCL_ST];                                              \
                                                                               \
    int needH = (fx != 0);                                                     \
    int needV = (fy != 0);                                                     \
    int needJ = (fx == 2 && fy != 0) || (fy == 2 && fx != 0);                  \
    int needG = ((fx & 1) && fy == 0) || (fx == 0 && (fy & 1));                \
                                                                               \
    if (needG)                                                                 \
        for (int y = 0; y <= h; y++) {                                         \
            const uint8_t *p = ref + (size_t)(iy + y) * rstride + ix;          \
            _mm_storeu_si128((__m128i *)(void *)(G + y * ST), y264_ld128(p));  \
            G[y * ST + 16] = p[16];                                            \
        }                                                                      \
    if (needH)                                                                 \
        for (int y = 0; y <= h; y++)                                           \
            HCLIP(ref + (size_t)(iy + y) * rstride + ix - 2, H + y * ST);      \
    if (needV)                                                                 \
        for (int y = 0; y <= h; y++) {                                         \
            const uint8_t *p = ref + (size_t)(iy + y) * rstride + ix;          \
            VCL(p, rstride, V + y * ST);                                       \
            const uint8_t *q = p + 16;                                         \
            int s = q[-2 * rstride] - 5 * q[-rstride] + 20 * q[0]              \
                  + 20 * q[rstride] - 5 * q[2 * rstride] + q[3 * rstride];     \
            s = (s + 16) >> 5;                                                 \
            V[y * ST + 16] = (uint8_t)(s < 0 ? 0 : s > 255 ? 255 : s);         \
        }                                                                      \
    if (needJ) {                                                               \
        for (int y = -2; y <= h + 2; y++)                                      \
            HRAW(ref + (size_t)(iy + y) * rstride + ix - 2, cc + (y + 2) * ST);\
        for (int y = 0; y < h; y++)                                            \
            JCL(cc + (y + 0) * ST, cc + (y + 1) * ST, cc + (y + 2) * ST,       \
                cc + (y + 3) * ST, cc + (y + 4) * ST, cc + (y + 5) * ST,       \
                J + y * ST);                                                   \
    }                                                                          \
                                                                               \
    for (int y = 0; y < h; y++) {                                              \
        const uint8_t *Gy = G + y * ST, *Hy = H + y * ST;                      \
        const uint8_t *Vy = V + y * ST, *Jy = J + y * ST;                      \
        __m128i out;                                                           \
        switch (fy * 4 + fx) {                                                 \
        case 0*4+1: out = _mm_avg_epu8(y264_ld128(Gy), y264_ld128(Hy)); break; \
        case 0*4+2: out = y264_ld128(Hy); break;                               \
        case 0*4+3: out = _mm_avg_epu8(y264_ld128(Gy+1), y264_ld128(Hy)); break;\
        case 1*4+0: out = _mm_avg_epu8(y264_ld128(Gy), y264_ld128(Vy)); break; \
        case 2*4+0: out = y264_ld128(Vy); break;                               \
        case 3*4+0: out = _mm_avg_epu8(y264_ld128(Gy+ST), y264_ld128(Vy)); break;\
        case 1*4+1: out = _mm_avg_epu8(y264_ld128(Hy), y264_ld128(Vy)); break; \
        case 1*4+2: out = _mm_avg_epu8(y264_ld128(Hy), y264_ld128(Jy)); break; \
        case 1*4+3: out = _mm_avg_epu8(y264_ld128(Hy), y264_ld128(Vy+1)); break;\
        case 2*4+1: out = _mm_avg_epu8(y264_ld128(Vy), y264_ld128(Jy)); break; \
        case 2*4+2: out = y264_ld128(Jy); break;                               \
        case 2*4+3: out = _mm_avg_epu8(y264_ld128(Jy), y264_ld128(Vy+1)); break;\
        case 3*4+1: out = _mm_avg_epu8(y264_ld128(Vy), y264_ld128(Hy+ST)); break;\
        case 3*4+2: out = _mm_avg_epu8(y264_ld128(Jy), y264_ld128(Hy+ST)); break;\
        case 3*4+3: out = _mm_avg_epu8(y264_ld128(Vy+1), y264_ld128(Hy+ST)); break;\
        default:    out = y264_ld128(Gy); break;                               \
        }                                                                      \
        _mm_storeu_si128((__m128i *)(void *)(dst + (size_t)y * dstride), out); \
    }                                                                          \
}

/* ---- chroma bilinear ------------------------------------------------------
 *
 * One PMADDUBSW per source row pair: the two horizontally adjacent samples are
 * interleaved byte-wise and multiplied by the weight pair, so a whole row of
 * the (8-fx)(8-fy)A + fx(8-fy)B + (8-fx)fy*C + fx*fy*D sum costs two
 * multiplies and one add. The weights are built once per call.
 */
static inline __m128i y264_mcc_w(int a, int b)
{
    return _mm_set1_epi16((short)(a | (b << 8)));
}

/* Eight columns of one output row, from two source rows. */
static inline __m128i y264_mcc_row8(const uint8_t *r, int rstride,
                                    __m128i wab, __m128i wcd)
{
    __m128i ab = _mm_unpacklo_epi8(y264_ld64(r), y264_ld64(r + 1));
    __m128i cd = _mm_unpacklo_epi8(y264_ld64(r + rstride),
                                   y264_ld64(r + rstride + 1));
    __m128i acc = _mm_add_epi16(_mm_maddubs_epi16(ab, wab),
                                _mm_maddubs_epi16(cd, wcd));
    acc = _mm_srli_epi16(_mm_add_epi16(acc, _mm_set1_epi16(32)), 6);
    return _mm_packus_epi16(acc, acc);
}

static inline void y264_mc_chroma_w8h_x86(uint8_t *dst, int dstride,
                                          const uint8_t *ref, int rstride,
                                          int ix, int iy, int fx, int fy, int h)
{
    __m128i wab = y264_mcc_w((8 - fx) * (8 - fy), fx * (8 - fy));
    __m128i wcd = y264_mcc_w((8 - fx) * fy, fx * fy);
    for (int y = 0; y < h; y++)
        _mm_storel_epi64((__m128i *)(void *)(dst + (size_t)y * dstride),
                         y264_mcc_row8(ref + (size_t)(iy + y) * rstride + ix,
                                       rstride, wab, wcd));
}

/* Four columns at a time is half a register, so this packs TWO output rows per
 * pass: lanes 0-3 are row y and lanes 4-7 row y+1. Width 4 is not a corner --
 * it is most of the chroma MC calls on a CIF clip -- and the dispatcher only
 * routes even heights here. */
static inline __m128i y264_mcc_pair4(const uint8_t *r0, const uint8_t *r1)
{
    return _mm_unpacklo_epi64(_mm_unpacklo_epi8(y264_ld32(r0), y264_ld32(r0 + 1)),
                              _mm_unpacklo_epi8(y264_ld32(r1), y264_ld32(r1 + 1)));
}

static inline void y264_mc_chroma_w4h_x86(uint8_t *dst, int dstride,
                                          const uint8_t *ref, int rstride,
                                          int ix, int iy, int fx, int fy, int h)
{
    __m128i wab = y264_mcc_w((8 - fx) * (8 - fy), fx * (8 - fy));
    __m128i wcd = y264_mcc_w((8 - fx) * fy, fx * fy);
    for (int y = 0; y < h; y += 2) {
        const uint8_t *r0 = ref + (size_t)(iy + y) * rstride + ix;
        const uint8_t *r1 = r0 + rstride, *r2 = r1 + rstride;
        __m128i acc = _mm_add_epi16(_mm_maddubs_epi16(y264_mcc_pair4(r0, r1), wab),
                                    _mm_maddubs_epi16(y264_mcc_pair4(r1, r2), wcd));
        acc = _mm_srli_epi16(_mm_add_epi16(acc, _mm_set1_epi16(32)), 6);
        __m128i o = _mm_packus_epi16(acc, acc);
        int32_t o0 = _mm_cvtsi128_si32(o), o1 = _mm_extract_epi32(o, 1);
        memcpy(dst + (size_t)y * dstride, &o0, 4);
        memcpy(dst + (size_t)(y + 1) * dstride, &o1, 4);
    }
}

/* ---- the half-pel plane fetches -------------------------------------------
 *
 * A prediction row is 4, 8 or 16 samples and the source and destination
 * strides differ, so a row is the largest unit either tier can move: there is
 * no second row to put in an upper lane, and the AVX2 file uses these as they
 * stand. (a + b + 1) >> 1 is PAVGB.
 */
static inline void y264_pred_copy_x86(uint8_t *dst, int dstride,
                                      const uint8_t *s, int sstride, int w, int h)
{
    if (w == 16) {
        for (int y = 0; y < h; y++)
            _mm_storeu_si128((__m128i *)(void *)(dst + (size_t)y * dstride),
                             y264_ld128(s + (size_t)y * sstride));
    } else if (w == 8) {
        for (int y = 0; y < h; y++)
            _mm_storel_epi64((__m128i *)(void *)(dst + (size_t)y * dstride),
                             y264_ld64(s + (size_t)y * sstride));
    } else {
        for (int y = 0; y < h; y++)
            memcpy(dst + (size_t)y * dstride, s + (size_t)y * sstride, 4);
    }
}

static inline void y264_pred_avg2_x86(uint8_t *dst, int dstride, const uint8_t *s1,
                                      const uint8_t *s2, int sstride, int w, int h)
{
    if (w == 16) {
        for (int y = 0; y < h; y++)
            _mm_storeu_si128((__m128i *)(void *)(dst + (size_t)y * dstride),
                             _mm_avg_epu8(y264_ld128(s1 + (size_t)y * sstride),
                                          y264_ld128(s2 + (size_t)y * sstride)));
    } else if (w == 8) {
        for (int y = 0; y < h; y++)
            _mm_storel_epi64((__m128i *)(void *)(dst + (size_t)y * dstride),
                             _mm_avg_epu8(y264_ld64(s1 + (size_t)y * sstride),
                                          y264_ld64(s2 + (size_t)y * sstride)));
    } else {
        for (int y = 0; y < h; y++) {
            __m128i v = _mm_avg_epu8(y264_ld32(s1 + (size_t)y * sstride),
                                     y264_ld32(s2 + (size_t)y * sstride));
            int32_t o = _mm_cvtsi128_si32(v);
            memcpy(dst + (size_t)y * dstride, &o, 4);
        }
    }
}

/* ---- the weighted average ------------------------------------------------
 *
 * dst[i] = Clip1((a[i]*w0 + b[i]*w1 + 32) >> 6) over a PACKED run, which is
 * the one shape in this family with no stride in it at all.
 *
 * (32, 32) -- ordinary bi-prediction, and the overwhelmingly common case -- is
 * exactly the rounding average, so it is one PAVGB per sixteen samples. The
 * weighted form cannot use PMADDUBSW: an implicit weight reaches 128 and that
 * operand is signed. It stays in 16-bit lanes instead, where w0 + w1 == 64
 * with both in [-64, 128] bounds each product at 255*128 and the sum at
 * 32672, and PACKUSWB's saturation is the Clip1.
 */
static inline __m128i y264_avgwt16(const uint8_t *a, const uint8_t *b,
                                   __m128i k0, __m128i k1)
{
    __m128i va = y264_ld128(a), vb = y264_ld128(b);
    __m128i r = _mm_set1_epi16(32);
    __m128i alo = _mm_cvtepu8_epi16(va), ahi = _mm_cvtepu8_epi16(_mm_srli_si128(va, 8));
    __m128i blo = _mm_cvtepu8_epi16(vb), bhi = _mm_cvtepu8_epi16(_mm_srli_si128(vb, 8));
    __m128i lo = _mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(alo, k0),
                                             _mm_mullo_epi16(blo, k1)), r);
    __m128i hi = _mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(ahi, k0),
                                             _mm_mullo_epi16(bhi, k1)), r);
    return _mm_packus_epi16(_mm_srai_epi16(lo, 6), _mm_srai_epi16(hi, 6));
}

/* ---- the half-pel plane build rows ---------------------------------------
 *
 * The interiors of y264_mc_build_hpel_rows; mc.c keeps the border columns
 * scalar. Both are pure functions of their inputs, so the overlapping tail --
 * a last pass that steps back to end on the span -- recomputes identical
 * values and the result is bit-exact and band-safe, exactly as the NEON twin's
 * is.
 *
 * The reference plane is only guaranteed readable on [0, pw), so the
 * horizontal helper loads eight bytes six times over [x-2, x+11) rather than
 * sixteen bytes once.
 */
static inline void y264_hpel_hrow8(int32_t *srow, const uint8_t *row, int x)
{
    __m128i t = y264_hfilt8(row + x - 2);
    _mm_storeu_si128((__m128i *)(void *)(srow + x), _mm_cvtepi16_epi32(t));
    _mm_storeu_si128((__m128i *)(void *)(srow + x + 4),
                     _mm_cvtepi16_epi32(_mm_srli_si128(t, 8)));
}

static inline __m128i y264_ld128i(const int32_t *p)
{
    return _mm_loadu_si128((const __m128i *)(const void *)p);
}

/* H, V and C at eight columns from x. H is the middle intermediate rescaled,
 * C the vertical tap over the six intermediate rows, V the vertical tap over
 * the six integer rows. */
static inline void y264_hpel_outrow8(uint8_t *Hr, uint8_t *Vr, uint8_t *Cr,
                                     const int32_t *s0, const int32_t *s1,
                                     const int32_t *s2, const int32_t *s3,
                                     const int32_t *s4, const int32_t *s5,
                                     const uint8_t *r0, const uint8_t *r1,
                                     const uint8_t *r2, const uint8_t *r3,
                                     const uint8_t *r4, const uint8_t *r5, int x)
{
    __m128i r16 = _mm_set1_epi32(16);
    __m128i h0 = _mm_srai_epi32(_mm_add_epi32(y264_ld128i(s2 + x), r16), 5);
    __m128i h1 = _mm_srai_epi32(_mm_add_epi32(y264_ld128i(s2 + x + 4), r16), 5);
    __m128i hp = _mm_packs_epi32(h0, h1);
    _mm_storel_epi64((__m128i *)(void *)(Hr + x), _mm_packus_epi16(hp, hp));

    __m128i clo = y264_tap6_32(y264_ld128i(s0 + x), y264_ld128i(s1 + x),
                               y264_ld128i(s2 + x), y264_ld128i(s3 + x),
                               y264_ld128i(s4 + x), y264_ld128i(s5 + x));
    __m128i chi = y264_tap6_32(y264_ld128i(s0 + x + 4), y264_ld128i(s1 + x + 4),
                               y264_ld128i(s2 + x + 4), y264_ld128i(s3 + x + 4),
                               y264_ld128i(s4 + x + 4), y264_ld128i(s5 + x + 4));
    _mm_storel_epi64((__m128i *)(void *)(Cr + x), y264_clip10_8(clo, chi));

    _mm_storel_epi64((__m128i *)(void *)(Vr + x),
        y264_clip5_8(y264_vfilt8(r0 + x, r1 + x, r2 + x, r3 + x, r4 + x, r5 + x)));
}

#endif /* YAH264_DSP_X86_MC_X86_H */
