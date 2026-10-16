/*
 * interp_x86.h - the 128-bit building blocks the mc and hpel kernels share
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Wave 2 of docs/x86-plan.md. Every helper here is `static inline` and
 * 128-bit, so the SSE4.2 file gets SSE encodings and the AVX2 file gets VEX
 * encodings of the same source -- and the AVX2 file uses several of them
 * whole, because a shape with no second half has nothing to put in an upper
 * lane (an 8-wide prediction row, a 16-byte plane copy). The load helpers
 * come from metrics_x86.h, which is where this tree's "load the block, never
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
 * average the intra edge filter needs, metrics_x86.h).
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
#include <stddef.h>
#include <string.h>

#include "dsp/x86/metrics_x86.h"   /* the block-sized loads, and dsp/arch.h */

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
                             y264_ld64(ref + (ptrdiff_t)(iy + y) * rstride + ix));
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
            const uint8_t *p = ref + (ptrdiff_t)(iy + y) * rstride + ix;
            _mm_storel_epi64((__m128i *)(void *)(G + y * ST), y264_ld64(p));
            G[y * ST + 8] = p[8];
        }
    if (needH)
        for (int y = 0; y <= h; y++)
            _mm_storel_epi64((__m128i *)(void *)(H + y * ST),
                y264_clip5_8(y264_hfilt8(ref + (ptrdiff_t)(iy + y) * rstride + ix - 2)));
    if (needV)
        for (int y = 0; y <= h; y++) {
            const uint8_t *p = ref + (ptrdiff_t)(iy + y) * rstride + ix;
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
                             y264_hfilt8(ref + (ptrdiff_t)(iy + y) * rstride + ix - 2));
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
                y264_ld128(ref + (ptrdiff_t)(iy + y) * rstride + ix));         \
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
            const uint8_t *p = ref + (ptrdiff_t)(iy + y) * rstride + ix;       \
            _mm_storeu_si128((__m128i *)(void *)(G + y * ST), y264_ld128(p));  \
            G[y * ST + 16] = p[16];                                            \
        }                                                                      \
    if (needH)                                                                 \
        for (int y = 0; y <= h; y++)                                           \
            HCLIP(ref + (ptrdiff_t)(iy + y) * rstride + ix - 2, H + y * ST);   \
    if (needV)                                                                 \
        for (int y = 0; y <= h; y++) {                                         \
            const uint8_t *p = ref + (ptrdiff_t)(iy + y) * rstride + ix;       \
            VCL(p, rstride, V + y * ST);                                       \
            const uint8_t *q = p + 16;                                         \
            int s = q[-2 * rstride] - 5 * q[-rstride] + 20 * q[0]              \
                  + 20 * q[rstride] - 5 * q[2 * rstride] + q[3 * rstride];     \
            s = (s + 16) >> 5;                                                 \
            V[y * ST + 16] = (uint8_t)(s < 0 ? 0 : s > 255 ? 255 : s);         \
        }                                                                      \
    if (needJ) {                                                               \
        for (int y = -2; y <= h + 2; y++)                                      \
            HRAW(ref + (ptrdiff_t)(iy + y) * rstride + ix - 2, cc + (y + 2) * ST);\
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
    /* the zero phase is a copy: (64 * a + 32) >> 6 == a */
    if (!(fx | fy)) {
        for (int y = 0; y < h; y++)
            _mm_storel_epi64((__m128i *)(void *)(dst + (size_t)y * dstride),
                             y264_ld64(ref + (ptrdiff_t)(iy + y) * rstride + ix));
        return;
    }
    __m128i wab = y264_mcc_w((8 - fx) * (8 - fy), fx * (8 - fy));
    __m128i wcd = y264_mcc_w((8 - fx) * fy, fx * fy);
    for (int y = 0; y < h; y++)
        _mm_storel_epi64((__m128i *)(void *)(dst + (size_t)y * dstride),
                         y264_mcc_row8(ref + (ptrdiff_t)(iy + y) * rstride + ix,
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
    /* the zero phase is a copy: (64 * a + 32) >> 6 == a */
    if (!(fx | fy)) {
        for (int y = 0; y < h; y++)
            memcpy(dst + (size_t)y * dstride,
                   ref + (ptrdiff_t)(iy + y) * rstride + ix, 4);
        return;
    }
    __m128i wab = y264_mcc_w((8 - fx) * (8 - fy), fx * (8 - fy));
    __m128i wcd = y264_mcc_w((8 - fx) * fy, fx * fy);
    for (int y = 0; y < h; y += 2) {
        const uint8_t *r0 = ref + (ptrdiff_t)(iy + y) * rstride + ix;
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

/* Eight weighted samples in 16-bit lanes, packed into the low eight bytes;
 * the upper lanes of a narrower load are zero and stay zero. */
static inline __m128i y264_avgwt8(__m128i a8, __m128i b8, __m128i k0, __m128i k1)
{
    __m128i v = _mm_add_epi16(_mm_add_epi16(
                    _mm_mullo_epi16(_mm_cvtepu8_epi16(a8), k0),
                    _mm_mullo_epi16(_mm_cvtepu8_epi16(b8), k1)),
                              _mm_set1_epi16(32));
    v = _mm_srai_epi16(v, 6);
    return _mm_packus_epi16(v, v);
}

static inline void y264_st32(uint8_t *p, __m128i v)
{
    int32_t o = _mm_cvtsi128_si32(v);
    memcpy(p, &o, 4);
}

/* One row of the strided weighted average, w samples and not one more: steps
 * of sixteen, eight and four, then a scalar pair for the 2-wide chroma block.
 * `flat` is the (32, 32) pair, where the whole formula is PAVGB. */
static inline void y264_avgwt_row(uint8_t *dst, const uint8_t *a, const uint8_t *b,
                                  int w, int flat, int w0, int w1,
                                  __m128i k0, __m128i k1)
{
    int x = 0;
    if (flat) {
        for (; x + 16 <= w; x += 16)
            _mm_storeu_si128((__m128i *)(void *)(dst + x),
                             _mm_avg_epu8(y264_ld128(a + x), y264_ld128(b + x)));
        if (x + 8 <= w) {
            _mm_storel_epi64((__m128i *)(void *)(dst + x),
                             _mm_avg_epu8(y264_ld64(a + x), y264_ld64(b + x)));
            x += 8;
        }
        if (x + 4 <= w) {
            y264_st32(dst + x, _mm_avg_epu8(y264_ld32(a + x), y264_ld32(b + x)));
            x += 4;
        }
    } else {
        for (; x + 16 <= w; x += 16)
            _mm_storeu_si128((__m128i *)(void *)(dst + x),
                             y264_avgwt16(a + x, b + x, k0, k1));
        if (x + 8 <= w) {
            _mm_storel_epi64((__m128i *)(void *)(dst + x),
                             y264_avgwt8(y264_ld64(a + x), y264_ld64(b + x), k0, k1));
            x += 8;
        }
        if (x + 4 <= w) {
            y264_st32(dst + x, y264_avgwt8(y264_ld32(a + x), y264_ld32(b + x), k0, k1));
            x += 4;
        }
    }
    for (; x < w; x++) {
        int v = (a[x] * w0 + b[x] * w1 + 32) >> 6;
        dst[x] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
    }
}

/* The strided weighted average over a w x h block. The dispatcher admits only
 * the implicit weight domain (w0 + w1 == 64, both in [-64, 128]), which is
 * what keeps the 16-bit lanes exact. The flat pair at the two macroblock
 * widths skips the general row. */
static inline void y264_avg_wt_2d_x86(uint8_t *dst, int ds, const uint8_t *a, int as,
                                      const uint8_t *b, int bs, int w, int h,
                                      int w0, int w1)
{
    int flat = w0 == 32 && w1 == 32;
    if (flat && w == 16) {
        for (int y = 0; y < h; y++, dst += ds, a += as, b += bs)
            _mm_storeu_si128((__m128i *)(void *)dst,
                             _mm_avg_epu8(y264_ld128(a), y264_ld128(b)));
        return;
    }
    if (flat && w == 8) {
        for (int y = 0; y < h; y++, dst += ds, a += as, b += bs)
            _mm_storel_epi64((__m128i *)(void *)dst,
                             _mm_avg_epu8(y264_ld64(a), y264_ld64(b)));
        return;
    }
    __m128i k0 = _mm_set1_epi16((short)w0), k1 = _mm_set1_epi16((short)w1);
    for (int y = 0; y < h; y++, dst += ds, a += as, b += bs)
        y264_avgwt_row(dst, a, b, w, flat, w0, w1, k0, k1);
}

/* A 4:2:0 macroblock's flat bi-average from whole-sample positions: luma
 * 16x16 into pred (stride 16) and each chroma 8x8 into its plane (stride 8),
 * every sample (a + b + 1) >> 1. Three flat strided averages in one call, and
 * a row is a whole register or half of one, so both tiers use this body. */
static inline void y264_avg_mb420_flat_x86(uint8_t *pred, uint8_t *cu, uint8_t *cv,
                                           const uint8_t *l0, int s0,
                                           const uint8_t *l1, int s1,
                                           const uint8_t *u0, const uint8_t *v0, int t0,
                                           const uint8_t *u1, const uint8_t *v1, int t1)
{
    for (int y = 0; y < 16; y++, l0 += s0, l1 += s1)
        _mm_storeu_si128((__m128i *)(void *)(pred + 16 * y),
                         _mm_avg_epu8(y264_ld128(l0), y264_ld128(l1)));
    for (int y = 0; y < 8; y++, u0 += t0, v0 += t0, u1 += t1, v1 += t1) {
        _mm_storel_epi64((__m128i *)(void *)(cu + 8 * y),
                         _mm_avg_epu8(y264_ld64(u0), y264_ld64(u1)));
        _mm_storel_epi64((__m128i *)(void *)(cv + 8 * y),
                         _mm_avg_epu8(y264_ld64(v0), y264_ld64(v1)));
    }
}

/* Two in-window 8x8 chroma bilinear predictions, one per list, and their
 * weighted average in the same pass: the bilinear row for each list, then
 * PAVGB at the flat pair or the 16-bit weighted form. r0 and r1 address each
 * list's top-left sample; each list is read over nine columns of nine rows. */
static inline void y264_mc_chroma_bi8_x86(uint8_t *dst, int dstride,
                                          const uint8_t *r0, int s0, int fx0, int fy0,
                                          const uint8_t *r1, int s1, int fx1, int fy1,
                                          int w0, int w1)
{
    __m128i a01 = y264_mcc_w((8 - fx0) * (8 - fy0), fx0 * (8 - fy0));
    __m128i a23 = y264_mcc_w((8 - fx0) * fy0, fx0 * fy0);
    __m128i b01 = y264_mcc_w((8 - fx1) * (8 - fy1), fx1 * (8 - fy1));
    __m128i b23 = y264_mcc_w((8 - fx1) * fy1, fx1 * fy1);
    __m128i k0 = _mm_set1_epi16((short)w0), k1 = _mm_set1_epi16((short)w1);
    int flat = w0 == 32 && w1 == 32;
    for (int y = 0; y < 8; y++, r0 += s0, r1 += s1, dst += dstride) {
        __m128i p = y264_mcc_row8(r0, s0, a01, a23);
        __m128i q = y264_mcc_row8(r1, s1, b01, b23);
        _mm_storel_epi64((__m128i *)(void *)dst,
                         flat ? _mm_avg_epu8(p, q) : y264_avgwt8(p, q, k0, k1));
    }
}

/* ---- the half-pel plane build rows ---------------------------------------
 *
 * The interiors of y264_mc_build_hpel_rows; interp.c keeps the border columns
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

/* ---- the half-pel plane build, a whole band --------------------------------
 *
 * The two row kernels above leave the border columns to scalar code and stage
 * the horizontal taps as int32. This builds a band of output rows, borders
 * included, in one pass. Each source row is clamped into a padded copy once
 * (the replicated columns are two memsets), and its horizontal six-tap is
 * kept as int16, which the tap of bytes fits. A ring of six padded rows and
 * six tap rows feeds each output row:
 *
 *   H = clip1((htap + 16) >> 5)            from the centre tap row
 *   V = clip1((vtap of bytes + 16) >> 5)   16-bit lanes
 *   C = clip1((vtap of htaps + 512) >> 10) the three pair sums stay in int16
 *                                          (at most 21420) and PMADDWD forms
 *                                          the weighted sum in 32 bits
 *
 * Bit-exact with y264_mc_build_hpel_rows. `mem` holds 6 * (W + 32) + 12 * W
 * bytes, W = pw + 2 * border, and W >= 16. The four macro parameters are the
 * tier's sixteen-column steps:
 *
 *   HTAP(pad, t)            six-tap of pad[0 .. 20] into sixteen int16
 *   HOUT(t, out)            sixteen taps rescaled and clipped
 *   VOUT(p0 .. p5, out)     the vertical tap over six padded rows
 *   COUT(t0 .. t5, out)     the vertical tap over six tap rows
 *
 * The last pass of every row steps back to end on the span; each step is a
 * pure function of the padded rows, so the overlap recomputes the same values.
 */
#define Y264_HPEL_BAND_BODY(NAME, HTAP, HOUT, VOUT, COUT)                      \
void NAME(uint8_t *Hp, uint8_t *Vp, uint8_t *Cp, int stride,                   \
          const uint8_t *ref, int rstride, int pw, int ph, int border,         \
          uint8_t *mem, int y0, int y1)                                        \
{                                                                              \
    const int W = pw + 2 * border, PW = W + 32;                                \
    uint8_t *P[6];                                                             \
    int16_t *T[6];                                                             \
    for (int k = 0; k < 6; k++) {                                              \
        P[k] = mem + (size_t)k * PW;                                           \
        T[k] = (int16_t *)(void *)(mem + (size_t)6 * PW + (size_t)k * W * 2);  \
    }                                                                          \
    const int sy0 = y0 - 2;                                                    \
    for (int y = sy0; y < y1 + 3; y++) {                                       \
        int k = (y - sy0) % 6;                                                 \
        int cy = y < 0 ? 0 : (y >= ph ? ph - 1 : y);                           \
        const uint8_t *row = ref + (ptrdiff_t)cy * rstride;                    \
        uint8_t *pad = P[k];      /* pad[j] = row[clamp(j - border - 2)] */    \
        memset(pad, row[0], (size_t)border + 2);                               \
        memcpy(pad + border + 2, row, (size_t)pw);                             \
        memset(pad + border + 2 + pw, row[pw - 1], (size_t)border + 3);        \
        int16_t *t = T[k];                                                     \
        for (int j = 0; j < W; j += 16) {                                      \
            if (j > W - 16) j = W - 16;                                        \
            HTAP(pad + j, t + j);                                              \
        }                                                                      \
        if (y < y0 + 3) continue;                                              \
        int oy = y - 3;           /* rows oy-2 .. oy+3 are in the rings */     \
        int k0 = (oy - 2 - sy0) % 6;                                           \
        const uint8_t *p0 = P[k0], *p1 = P[(k0 + 1) % 6];                      \
        const uint8_t *p2 = P[(k0 + 2) % 6], *p3 = P[(k0 + 3) % 6];            \
        const uint8_t *p4 = P[(k0 + 4) % 6], *p5 = P[(k0 + 5) % 6];            \
        const int16_t *t0 = T[k0], *t1 = T[(k0 + 1) % 6];                      \
        const int16_t *t2 = T[(k0 + 2) % 6], *t3 = T[(k0 + 3) % 6];            \
        const int16_t *t4 = T[(k0 + 4) % 6], *t5 = T[(k0 + 5) % 6];            \
        uint8_t *Hr = Hp + (ptrdiff_t)oy * stride - border;                    \
        uint8_t *Vr = Vp + (ptrdiff_t)oy * stride - border;                    \
        uint8_t *Cr = Cp + (ptrdiff_t)oy * stride - border;                    \
        for (int j = 0; j < W; j += 16) {                                      \
            if (j > W - 16) j = W - 16;                                        \
            HOUT(t2 + j, Hr + j);                                              \
            /* pad column j + 2 is output column j */                          \
            VOUT(p0 + j + 2, p1 + j + 2, p2 + j + 2, p3 + j + 2, p4 + j + 2,   \
                 p5 + j + 2, Vr + j);                                          \
            COUT(t0 + j, t1 + j, t2 + j, t3 + j, t4 + j, t5 + j, Cr + j);      \
        }                                                                      \
    }                                                                          \
}

/* ---- the lowres bilinear phase row -----------------------------------------
 *
 * o[X] = (w00*r0[X] + w10*r0[X+ax] + w01*r1[X] + w11*r1[X+ax] + 8) >> 4 with
 * the four weights summing to 16, so the sum is at most 16 * 255 and the
 * chroma kernel's PMADDUBSW pairing serves it unchanged: each row's two
 * samples interleaved against that row's weight pair. Sixteen columns from X;
 * reads r0 and r1 over [X, X + 15 + ax].
 */
static inline __m128i y264_bilin4_16(const uint8_t *r0, const uint8_t *r1, int ax,
                                     __m128i wa, __m128i wb)
{
    __m128i a = y264_ld128(r0), b = y264_ld128(r0 + ax);
    __m128i c = y264_ld128(r1), d = y264_ld128(r1 + ax);
    __m128i k8 = _mm_set1_epi16(8);
    __m128i lo = _mm_add_epi16(_mm_maddubs_epi16(_mm_unpacklo_epi8(a, b), wa),
                               _mm_maddubs_epi16(_mm_unpacklo_epi8(c, d), wb));
    __m128i hi = _mm_add_epi16(_mm_maddubs_epi16(_mm_unpackhi_epi8(a, b), wa),
                               _mm_maddubs_epi16(_mm_unpackhi_epi8(c, d), wb));
    return _mm_packus_epi16(_mm_srli_epi16(_mm_add_epi16(lo, k8), 4),
                            _mm_srli_epi16(_mm_add_epi16(hi, k8), 4));
}

/* ---- the lowres half-pel plane row ----------------------------------------
 *
 * Each output is the rounded mean of two rounded vertical pair means, and both
 * rounds are PAVGB. The vertical one is taken on sixteen source columns; the
 * horizontal one pairs ADJACENT columns of that result, which is the register
 * averaged with itself moved one byte down inside each 16-bit lane, and the
 * even bytes of that are the eight outputs.
 *
 * The three planes read the source at two alignments only. The down plane
 * pairs columns (2x, 2x+1), so it loads from 2x; the right plane and the
 * diagonal one pair (2x+1, 2x+2) and load from 2x+1. Loading from 2x+2 as
 * well would reach one column past the last sample the row needs.
 */
static inline __m128i y264_lrhp_pairs(__m128i m)
{
    __m128i a = _mm_avg_epu8(m, _mm_srli_epi16(m, 8));
    a = _mm_and_si128(a, _mm_set1_epi16(0x00ff));
    return _mm_packus_epi16(a, a);
}

/* Eight columns from x: reads r0[2x+1 .. 2x+16] and r1, r2[2x .. 2x+16]. */
static inline void y264_lr_hp_row8(uint8_t *oh, uint8_t *ov, uint8_t *oc,
                                   const uint8_t *r0, const uint8_t *r1,
                                   const uint8_t *r2, int x)
{
    __m128i a1 = y264_ld128(r1 + 2 * x), a2 = y264_ld128(r2 + 2 * x);
    __m128i b0 = y264_ld128(r0 + 2 * x + 1), b1 = y264_ld128(r1 + 2 * x + 1);
    __m128i b2 = y264_ld128(r2 + 2 * x + 1);
    _mm_storel_epi64((__m128i *)(void *)(oh + x),
                     y264_lrhp_pairs(_mm_avg_epu8(b0, b1)));
    _mm_storel_epi64((__m128i *)(void *)(ov + x),
                     y264_lrhp_pairs(_mm_avg_epu8(a1, a2)));
    _mm_storel_epi64((__m128i *)(void *)(oc + x),
                     y264_lrhp_pairs(_mm_avg_epu8(b1, b2)));
}

#endif /* YAH264_DSP_X86_MC_X86_H */
