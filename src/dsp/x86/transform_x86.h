/*
 * transform_x86.h - the butterflies, transposes and 128-bit cores the
 * transform, quant and scan kernels share
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Wave 3a of docs/x86-plan.md. Same arrangement as pixel_x86.h: everything
 * here is `static inline` and 128-bit, the SSE4.2 file gets SSE encodings of
 * it and the AVX2 file gets VEX encodings of the same source, so a shape with
 * nothing for a second lane to do -- a lone 4x4 transform, a lone 8x8 forward
 * in 16-bit lanes -- is written once and cannot disagree with itself.
 *
 * THREE THINGS DECIDE THIS FILE.
 *
 * 1. LANE WIDTH IS NOT A STYLE CHOICE, it is the exactness claim. The forward
 *    transforms run in 16-bit lanes because their input is a pixel difference,
 *    |d| <= 255, and the per-pass gain (6x then 6x at 4x4, 8x then 8x at 8x8)
 *    stays inside a signed 16-bit lane -- and that is the only domain any call
 *    site feeds them. The inverse transforms run in 32-bit lanes because their
 *    input is a full-range int16 coefficient and the first stage of the 8x8
 *    inverse already reaches ~4x that: narrowing it is the refusal recorded in
 *    docs/dsp-coverage-inventory.md and it stands here for the same reason it
 *    stands on NEON.
 *
 * 2. THE 1-D PASSES ARE MACROS, not inline functions, because the AVX2 file
 *    needs the identical algebra over 256-bit registers. An intrinsic name is
 *    the only thing that differs between the two, so it is a macro parameter;
 *    writing the butterflies twice is how a tier ends up bit-exact with its
 *    own C reference and not with the other tier.
 *
 * 3. THE ROW/COLUMN ORDER IS NORMATIVE. The 1-D passes shift, so they are not
 *    linear and the two passes do NOT commute: the C reference in
 *    src/dsp/transform.c runs rows first and columns second, and every kernel
 *    here reproduces that order as transpose -> 1-D -> transpose -> 1-D. The
 *    first transpose is what turns "along a row" into "between registers".
 *
 * NARROWING. The C reference ends the inverse with a `(dctcoef)` cast, which
 * TRUNCATES. `_mm_packs_epi32` saturates, so it is wrong here and the narrow
 * goes through a byte shuffle instead (y264_narrow_s32). The quant kernels are
 * the one place saturation is admissible, and it is admissible there because
 * the product cannot reach the boundary -- see the note on y264_quant_pack.
 *
 * WINDOWS. checkasm maps a PROT_NONE page hard against the block a kernel
 * declares, so pixel loads are sized to the block: four bytes for a 4-wide
 * row, eight for an 8-wide one, never a whole register.
 *
 * C11 intrinsics only. There is no assembly in this tree, and hygiene_check
 * refuses any inline form under this directory.
 */
#ifndef YAH264_DSP_X86_TRANSFORM_X86_H
#define YAH264_DSP_X86_TRANSFORM_X86_H

#include <immintrin.h>
#include <string.h>

#include "dsp/arch.h"

#if Y264_HAVE_SSE4

/* ---- loads and stores sized to the block --------------------------------- */

static inline __m128i y264_ldc4(const dctcoef *p)
{
    return _mm_loadl_epi64((const __m128i *)(const void *)p);
}

static inline __m128i y264_ldc8(const dctcoef *p)
{
    return _mm_loadu_si128((const __m128i *)(const void *)p);
}

static inline void y264_stc4(dctcoef *p, __m128i v)
{
    _mm_storel_epi64((__m128i *)(void *)p, v);
}

static inline void y264_stc8(dctcoef *p, __m128i v)
{
    _mm_storeu_si128((__m128i *)(void *)p, v);
}

/* Four pixels, read as exactly four bytes: a 4x4 block can sit against the
 * frame edge with nothing mapped after it. */
static inline __m128i y264_ldp4(const pixel *p)
{
    int32_t v;
    memcpy(&v, p, 4);
    return _mm_cvtsi32_si128(v);
}

static inline __m128i y264_ldp8(const pixel *p)
{
    return _mm_loadl_epi64((const __m128i *)(const void *)p);
}

/* Four pixels out, written as exactly four bytes (the destination row is
 * wider than the block and checkasm poisons the padding). */
static inline void y264_stp4(pixel *p, __m128i v)
{
    int32_t w = _mm_cvtsi128_si32(v);
    memcpy(p, &w, 4);
}

static inline void y264_stp8(pixel *p, __m128i v)
{
    _mm_storel_epi64((__m128i *)(void *)p, v);
}

/* ---- narrowing ------------------------------------------------------------
 *
 * int32 lanes -> int16 lanes by TRUNCATION, which is what the reference's
 * `(dctcoef)` cast does. The four low halves are gathered with one byte
 * shuffle; the top half of the result is zero and is never stored. */
static inline __m128i y264_narrow_s32(__m128i v)
{
    const __m128i k = _mm_setr_epi8(0, 1, 4, 5, 8, 9, 12, 13,
                                    -128, -128, -128, -128, -128, -128, -128, -128);
    return _mm_shuffle_epi8(v, k);
}

/* ---- the 1-D passes -------------------------------------------------------
 *
 * ADD/SUB and the two arithmetic right shifts come in as parameters so the
 * AVX2 file can instantiate the identical algebra at 256 bits. Every one of
 * them is a transcription of the correspondingly named scalar function in
 * src/dsp/transform.c; the shifts are arithmetic there (on a signed int) and
 * arithmetic here.
 */

/* 4x4 forward, 16-bit lanes (y264_fdct4x4_c's inner loop). */
#define Y264_FDCT4_1D(V, ADD, SUB, r0, r1, r2, r3) do {                       \
    V z0_ = ADD(r0, r3), z3_ = SUB(r0, r3);                                   \
    V z1_ = ADD(r1, r2), z2_ = SUB(r1, r2);                                   \
    (r0) = ADD(z0_, z1_);                                                     \
    (r1) = ADD(ADD(z3_, z3_), z2_);                                           \
    (r2) = SUB(z0_, z1_);                                                     \
    (r3) = SUB(z3_, ADD(z2_, z2_));                                           \
} while (0)

/* 4x4 inverse, 32-bit lanes (y264_idct4x4_c's inner loop, before the
 * (x + 32) >> 6 the caller folds in). */
#define Y264_IDCT4_1D(V, ADD, SUB, SR1, r0, r1, r2, r3) do {                  \
    V i0_ = ADD(r0, r2);                                                      \
    V i1_ = SUB(r0, r2);                                                      \
    V i2_ = SUB(SR1(r1), r3);                                                 \
    V i3_ = ADD(r1, SR1(r3));                                                 \
    (r0) = ADD(i0_, i3_);                                                     \
    (r1) = ADD(i1_, i2_);                                                     \
    (r2) = SUB(i1_, i2_);                                                     \
    (r3) = SUB(i0_, i3_);                                                     \
} while (0)

/* 8x8 forward, 16-bit lanes (fdct8_1d). */
#define Y264_FDCT8_1D(V, ADD, SUB, SR1, SR2, s0, s1, s2, s3, s4, s5, s6, s7) do {\
    V a0_ = ADD(s0, s7), a1_ = ADD(s1, s6), a2_ = ADD(s2, s5), a3_ = ADD(s3, s4);\
    V a4_ = SUB(s0, s7), a5_ = SUB(s1, s6), a6_ = SUB(s2, s5), a7_ = SUB(s3, s4);\
    V b0_ = ADD(a0_, a3_), b1_ = ADD(a1_, a2_);                               \
    V b2_ = SUB(a0_, a3_), b3_ = SUB(a1_, a2_);                               \
    V b4_ = ADD(ADD(a5_, a6_), ADD(SR1(a4_), a4_));                           \
    V b5_ = SUB(SUB(a4_, a7_), ADD(SR1(a6_), a6_));                           \
    V b6_ = SUB(ADD(a4_, a7_), ADD(SR1(a5_), a5_));                           \
    V b7_ = ADD(SUB(a5_, a6_), ADD(SR1(a7_), a7_));                           \
    (s0) = ADD(b0_, b1_);                                                     \
    (s2) = ADD(b2_, SR1(b3_));                                                \
    (s4) = SUB(b0_, b1_);                                                     \
    (s6) = SUB(SR1(b2_), b3_);                                                \
    (s1) = ADD(b4_, SR2(b7_));                                                \
    (s3) = ADD(b5_, SR2(b6_));                                                \
    (s5) = SUB(b6_, SR2(b5_));                                                \
    (s7) = SUB(SR2(b4_), b7_);                                                \
} while (0)

/* 8x8 inverse, 32-bit lanes (idct8_1d). */
#define Y264_IDCT8_1D(V, ADD, SUB, SR1, SR2, m0, m1, m2, m3, m4, m5, m6, m7) do {\
    V a0_ = ADD(m0, m4);                                                      \
    V a4_ = SUB(m0, m4);                                                      \
    V a2_ = SUB(SR1(m2), m6);                                                 \
    V a6_ = ADD(m2, SR1(m6));                                                 \
    V a1_ = SUB(SUB(SUB(m5, m3), m7), SR1(m7));                               \
    V a3_ = SUB(SUB(ADD(m1, m7), m3), SR1(m3));                               \
    V a5_ = ADD(ADD(SUB(m7, m1), m5), SR1(m5));                               \
    V a7_ = ADD(ADD(ADD(m3, m5), m1), SR1(m1));                               \
    V b0_ = ADD(a0_, a6_), b6_ = SUB(a0_, a6_);                               \
    V b2_ = ADD(a4_, a2_), b4_ = SUB(a4_, a2_);                               \
    V b1_ = ADD(a1_, SR2(a7_)), b7_ = SUB(a7_, SR2(a1_));                     \
    V b3_ = ADD(a3_, SR2(a5_)), b5_ = SUB(SR2(a3_), a5_);                     \
    (m0) = ADD(b0_, b7_);                                                     \
    (m7) = SUB(b0_, b7_);                                                     \
    (m1) = ADD(b2_, b5_);                                                     \
    (m6) = SUB(b2_, b5_);                                                     \
    (m2) = ADD(b4_, b3_);                                                     \
    (m5) = SUB(b4_, b3_);                                                     \
    (m3) = ADD(b6_, b1_);                                                     \
    (m4) = SUB(b6_, b1_);                                                     \
} while (0)

#define Y264_SRA16_1(x) _mm_srai_epi16(x, 1)
#define Y264_SRA16_2(x) _mm_srai_epi16(x, 2)
#define Y264_SRA32_1(x) _mm_srai_epi32(x, 1)
#define Y264_SRA32_2(x) _mm_srai_epi32(x, 2)

/* ---- transposes -----------------------------------------------------------
 *
 * Each one leaves register k holding element k of every input register, which
 * is exactly what turns the next 1-D pass from "along a row" into "between
 * registers". */

/* 4x4 of int16, one block, four registers with the block in their low half. */
static inline void y264_trn4_s16(__m128i *r0, __m128i *r1,
                                 __m128i *r2, __m128i *r3)
{
    __m128i a = _mm_unpacklo_epi16(*r0, *r1);
    __m128i b = _mm_unpacklo_epi16(*r2, *r3);
    __m128i c = _mm_unpacklo_epi32(a, b);
    __m128i d = _mm_unpackhi_epi32(a, b);
    *r0 = c;
    *r1 = _mm_unpackhi_epi64(c, c);
    *r2 = d;
    *r3 = _mm_unpackhi_epi64(d, d);
}

/* TWO 4x4 blocks of int16 side by side in one register (lanes 0-3 block A,
 * lanes 4-7 block B), transposed independently. Every step pairs lanes inside
 * a half, so the halves never mix -- which is also why the 256-bit form of the
 * identical sequence transposes FOUR blocks, two per 128-bit lane. */
#define Y264_TRN4X2_S16(V, UL16, UH16, UL32, UH32, UL64, UH64, r0, r1, r2, r3) do {\
    V t0_ = UL16(r0, r1), t2_ = UH16(r0, r1);                                 \
    V t1_ = UL16(r2, r3), t3_ = UH16(r2, r3);                                 \
    V u0_ = UL32(t0_, t1_), u1_ = UH32(t0_, t1_);                             \
    V v0_ = UL32(t2_, t3_), v1_ = UH32(t2_, t3_);                             \
    (r0) = UL64(u0_, v0_);                                                    \
    (r1) = UH64(u0_, v0_);                                                    \
    (r2) = UL64(u1_, v1_);                                                    \
    (r3) = UH64(u1_, v1_);                                                    \
} while (0)

/* 4x4 of int32, four full registers. */
#define Y264_TRN4_S32(V, UL32, UH32, UL64, UH64, r0, r1, r2, r3) do {         \
    V t0_ = UL32(r0, r1), t1_ = UH32(r0, r1);                                 \
    V t2_ = UL32(r2, r3), t3_ = UH32(r2, r3);                                 \
    (r0) = UL64(t0_, t2_);                                                    \
    (r1) = UH64(t0_, t2_);                                                    \
    (r2) = UL64(t1_, t3_);                                                    \
    (r3) = UH64(t1_, t3_);                                                    \
} while (0)

/* 8x8 of int16, eight full registers. */
static inline void y264_trn8_s16(__m128i r[8])
{
    __m128i a0 = _mm_unpacklo_epi16(r[0], r[1]), a1 = _mm_unpackhi_epi16(r[0], r[1]);
    __m128i a2 = _mm_unpacklo_epi16(r[2], r[3]), a3 = _mm_unpackhi_epi16(r[2], r[3]);
    __m128i a4 = _mm_unpacklo_epi16(r[4], r[5]), a5 = _mm_unpackhi_epi16(r[4], r[5]);
    __m128i a6 = _mm_unpacklo_epi16(r[6], r[7]), a7 = _mm_unpackhi_epi16(r[6], r[7]);
    __m128i b0 = _mm_unpacklo_epi32(a0, a2), b1 = _mm_unpackhi_epi32(a0, a2);
    __m128i b2 = _mm_unpacklo_epi32(a1, a3), b3 = _mm_unpackhi_epi32(a1, a3);
    __m128i b4 = _mm_unpacklo_epi32(a4, a6), b5 = _mm_unpackhi_epi32(a4, a6);
    __m128i b6 = _mm_unpacklo_epi32(a5, a7), b7 = _mm_unpackhi_epi32(a5, a7);
    r[0] = _mm_unpacklo_epi64(b0, b4); r[1] = _mm_unpackhi_epi64(b0, b4);
    r[2] = _mm_unpacklo_epi64(b1, b5); r[3] = _mm_unpackhi_epi64(b1, b5);
    r[4] = _mm_unpacklo_epi64(b2, b6); r[5] = _mm_unpackhi_epi64(b2, b6);
    r[6] = _mm_unpacklo_epi64(b3, b7); r[7] = _mm_unpackhi_epi64(b3, b7);
}

/* 8x8 of int32, as sixteen registers: v[i][0] is columns 0-3 of row i and
 * v[i][1] columns 4-7. The 8x8 transpose is the four 4x4 tile transposes with
 * the off-diagonal pair swapped, which costs nothing beyond naming. */
static inline void y264_trn8_s32(__m128i v[8][2])
{
    __m128i a0 = v[0][0], a1 = v[1][0], a2 = v[2][0], a3 = v[3][0];
    __m128i b0 = v[0][1], b1 = v[1][1], b2 = v[2][1], b3 = v[3][1];
    __m128i c0 = v[4][0], c1 = v[5][0], c2 = v[6][0], c3 = v[7][0];
    __m128i d0 = v[4][1], d1 = v[5][1], d2 = v[6][1], d3 = v[7][1];
    Y264_TRN4_S32(__m128i, _mm_unpacklo_epi32, _mm_unpackhi_epi32,
                  _mm_unpacklo_epi64, _mm_unpackhi_epi64, a0, a1, a2, a3);
    Y264_TRN4_S32(__m128i, _mm_unpacklo_epi32, _mm_unpackhi_epi32,
                  _mm_unpacklo_epi64, _mm_unpackhi_epi64, b0, b1, b2, b3);
    Y264_TRN4_S32(__m128i, _mm_unpacklo_epi32, _mm_unpackhi_epi32,
                  _mm_unpacklo_epi64, _mm_unpackhi_epi64, c0, c1, c2, c3);
    Y264_TRN4_S32(__m128i, _mm_unpacklo_epi32, _mm_unpackhi_epi32,
                  _mm_unpacklo_epi64, _mm_unpackhi_epi64, d0, d1, d2, d3);
    v[0][0] = a0; v[1][0] = a1; v[2][0] = a2; v[3][0] = a3;
    v[0][1] = c0; v[1][1] = c1; v[2][1] = c2; v[3][1] = c3;
    v[4][0] = b0; v[5][0] = b1; v[6][0] = b2; v[7][0] = b3;
    v[4][1] = d0; v[5][1] = d1; v[6][1] = d2; v[7][1] = d3;
}

/* ---- the 128-bit cores ---------------------------------------------------
 *
 * These are whole kernels rather than fragments, because the AVX2 file calls
 * them unchanged for the shapes a second lane has nothing to do in. */

#define Y264_TRN4_S16Q(r0, r1, r2, r3)                                        \
    Y264_TRN4X2_S16(__m128i, _mm_unpacklo_epi16, _mm_unpackhi_epi16,          \
                    _mm_unpacklo_epi32, _mm_unpackhi_epi32,                   \
                    _mm_unpacklo_epi64, _mm_unpackhi_epi64, r0, r1, r2, r3)

#define Y264_FDCT4_PASS(r0, r1, r2, r3)                                       \
    Y264_FDCT4_1D(__m128i, _mm_add_epi16, _mm_sub_epi16, r0, r1, r2, r3)

#define Y264_TRN4_S32Q(a, b, c, d)                                            \
    Y264_TRN4_S32(__m128i, _mm_unpacklo_epi32, _mm_unpackhi_epi32,            \
                  _mm_unpacklo_epi64, _mm_unpackhi_epi64, a, b, c, d)

/* The 4x4 forward over four rows already in registers. */
static inline void y264_fdct4_rows(__m128i r0, __m128i r1, __m128i r2, __m128i r3,
                                   dctcoef coef[16])
{
    y264_trn4_s16(&r0, &r1, &r2, &r3);
    Y264_FDCT4_PASS(r0, r1, r2, r3);
    y264_trn4_s16(&r0, &r1, &r2, &r3);
    Y264_FDCT4_PASS(r0, r1, r2, r3);
    y264_stc4(coef + 0, r0);
    y264_stc4(coef + 4, r1);
    y264_stc4(coef + 8, r2);
    y264_stc4(coef + 12, r3);
}

/* Two horizontally adjacent 4x4 forward transforms, the shape the batched
 * entry point is made of: the single-block kernel leaves half the datapath
 * idle and a macroblock's residual blocks always come in adjacent pairs. */
static inline void y264_fdct4_dual(__m128i r0, __m128i r1, __m128i r2, __m128i r3,
                                   dctcoef ca[16], dctcoef cb[16])
{
    Y264_TRN4_S16Q(r0, r1, r2, r3);
    Y264_FDCT4_PASS(r0, r1, r2, r3);
    Y264_TRN4_S16Q(r0, r1, r2, r3);
    Y264_FDCT4_PASS(r0, r1, r2, r3);
    y264_stc4(ca + 0,  r0); y264_stc4(cb + 0,  _mm_unpackhi_epi64(r0, r0));
    y264_stc4(ca + 4,  r1); y264_stc4(cb + 4,  _mm_unpackhi_epi64(r1, r1));
    y264_stc4(ca + 8,  r2); y264_stc4(cb + 8,  _mm_unpackhi_epi64(r2, r2));
    y264_stc4(ca + 12, r3); y264_stc4(cb + 12, _mm_unpackhi_epi64(r3, r3));
}

static inline void y264_fdct4x4_core(const dctcoef diff[16], dctcoef coef[16])
{
    y264_fdct4_rows(y264_ldc4(diff + 0), y264_ldc4(diff + 4),
                    y264_ldc4(diff + 8), y264_ldc4(diff + 12), coef);
}

/* src - pred as int16, four pixels wide. The unsigned widening subtract is
 * exactly the reference's (dctcoef)(src - pred). */
static inline __m128i y264_diff4(const pixel *s, const pixel *p)
{
    __m128i z = _mm_setzero_si128();
    return _mm_sub_epi16(_mm_unpacklo_epi8(y264_ldp4(s), z),
                         _mm_unpacklo_epi8(y264_ldp4(p), z));
}

static inline __m128i y264_diff8(const pixel *s, const pixel *p)
{
    __m128i z = _mm_setzero_si128();
    return _mm_sub_epi16(_mm_unpacklo_epi8(y264_ldp8(s), z),
                         _mm_unpacklo_epi8(y264_ldp8(p), z));
}

static inline void y264_sub4x4_dct_core(dctcoef coef[16], const pixel *src, int ss,
                                        const pixel *pred, int ps)
{
    y264_fdct4_rows(y264_diff4(src, pred),
                    y264_diff4(src + ss, pred + ps),
                    y264_diff4(src + 2 * ss, pred + 2 * ps),
                    y264_diff4(src + 3 * ss, pred + 3 * ps), coef);
}

/* The 4x4 inverse, up to and including the (x + 32) >> 6 normalization, left
 * in 32-bit lanes for the caller to narrow or to add to a prediction. */
static inline void y264_idct4_core(const dctcoef coef[16], __m128i out[4])
{
    __m128i r0 = _mm_cvtepi16_epi32(y264_ldc4(coef + 0));
    __m128i r1 = _mm_cvtepi16_epi32(y264_ldc4(coef + 4));
    __m128i r2 = _mm_cvtepi16_epi32(y264_ldc4(coef + 8));
    __m128i r3 = _mm_cvtepi16_epi32(y264_ldc4(coef + 12));
    const __m128i r32 = _mm_set1_epi32(32);
    Y264_TRN4_S32Q(r0, r1, r2, r3);
    Y264_IDCT4_1D(__m128i, _mm_add_epi32, _mm_sub_epi32, Y264_SRA32_1, r0, r1, r2, r3);
    Y264_TRN4_S32Q(r0, r1, r2, r3);
    Y264_IDCT4_1D(__m128i, _mm_add_epi32, _mm_sub_epi32, Y264_SRA32_1, r0, r1, r2, r3);
    out[0] = _mm_srai_epi32(_mm_add_epi32(r0, r32), 6);
    out[1] = _mm_srai_epi32(_mm_add_epi32(r1, r32), 6);
    out[2] = _mm_srai_epi32(_mm_add_epi32(r2, r32), 6);
    out[3] = _mm_srai_epi32(_mm_add_epi32(r3, r32), 6);
}

static inline void y264_idct4x4_core(const dctcoef coef[16], dctcoef res[16])
{
    __m128i r[4];
    y264_idct4_core(coef, r);
    for (int i = 0; i < 4; i++)
        y264_stc4(res + 4 * i, y264_narrow_s32(r[i]));
}

/* The fused recon add. The reference truncates the inverse to int16 and then
 * clips pred + res into the pixel range; the int16 truncation is the narrow
 * below, the saturating add cannot change the answer because the clip that
 * follows it is narrower than the saturation boundary, and the unsigned
 * saturating pack IS the clip. */
static inline void y264_add4x4_idct_core(pixel *dst, int ds, const pixel *pred, int ps,
                                         const dctcoef coef[16])
{
    __m128i r[4];
    __m128i z = _mm_setzero_si128();
    y264_idct4_core(coef, r);
    for (int i = 0; i < 4; i++) {
        __m128i res = y264_narrow_s32(r[i]);
        __m128i p = _mm_unpacklo_epi8(y264_ldp4(pred + i * ps), z);
        y264_stp4(dst + i * ds, _mm_packus_epi16(_mm_adds_epi16(res, p), z));
    }
}

/* ---- the 8x8 forward, 16-bit lanes --------------------------------------- */

#define Y264_FDCT8_PASS(r)                                                    \
    Y264_FDCT8_1D(__m128i, _mm_add_epi16, _mm_sub_epi16, Y264_SRA16_1,        \
                  Y264_SRA16_2, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7])

static inline void y264_fdct8_rows(__m128i r[8], dctcoef coef[64])
{
    y264_trn8_s16(r);
    Y264_FDCT8_PASS(r);
    y264_trn8_s16(r);
    Y264_FDCT8_PASS(r);
    for (int i = 0; i < 8; i++)
        y264_stc8(coef + 8 * i, r[i]);
}

static inline void y264_fdct8x8_core(const dctcoef diff[64], dctcoef coef[64])
{
    __m128i r[8];
    for (int i = 0; i < 8; i++)
        r[i] = y264_ldc8(diff + 8 * i);
    y264_fdct8_rows(r, coef);
}

static inline void y264_sub8x8_dct8_core(dctcoef coef[64], const pixel *src, int ss,
                                         const pixel *pred, int ps)
{
    __m128i r[8];
    for (int i = 0; i < 8; i++)
        r[i] = y264_diff8(src + i * ss, pred + i * ps);
    y264_fdct8_rows(r, coef);
}

/* ---- the 8x8 inverse, 32-bit lanes ---------------------------------------
 *
 * Sixteen live registers is more than the architecture has, so this one spills
 * and is written to spill tidily: the two halves of a row are adjacent in the
 * array and the 1-D pass walks them in order. Narrowing it to 16-bit lanes
 * would fit, and is the refusal in the inventory -- the first stage reaches
 * about four times the input magnitude and the input is a full-range int16. */
static inline void y264_idct8_core(const dctcoef coef[64], __m128i out[8][2])
{
    __m128i v[8][2];
    for (int i = 0; i < 8; i++) {
        __m128i row = y264_ldc8(coef + 8 * i);
        v[i][0] = _mm_cvtepi16_epi32(row);
        v[i][1] = _mm_cvtepi16_epi32(_mm_unpackhi_epi64(row, row));
    }
    for (int pass = 0; pass < 2; pass++) {
        y264_trn8_s32(v);
        for (int h = 0; h < 2; h++)
            Y264_IDCT8_1D(__m128i, _mm_add_epi32, _mm_sub_epi32,
                          Y264_SRA32_1, Y264_SRA32_2,
                          v[0][h], v[1][h], v[2][h], v[3][h],
                          v[4][h], v[5][h], v[6][h], v[7][h]);
    }
    const __m128i r32 = _mm_set1_epi32(32);
    for (int i = 0; i < 8; i++)
        for (int h = 0; h < 2; h++)
            out[i][h] = _mm_srai_epi32(_mm_add_epi32(v[i][h], r32), 6);
}

static inline void y264_idct8x8_core(const dctcoef coef[64], dctcoef res[64])
{
    __m128i v[8][2];
    y264_idct8_core(coef, v);
    for (int i = 0; i < 8; i++)
        y264_stc8(res + 8 * i, _mm_unpacklo_epi64(y264_narrow_s32(v[i][0]),
                                                  y264_narrow_s32(v[i][1])));
}

static inline void y264_add8x8_idct8_core(pixel *dst, int ds, const pixel *pred, int ps,
                                          const dctcoef coef[64])
{
    __m128i v[8][2];
    __m128i z = _mm_setzero_si128();
    y264_idct8_core(coef, v);
    for (int i = 0; i < 8; i++) {
        __m128i res = _mm_unpacklo_epi64(y264_narrow_s32(v[i][0]),
                                         y264_narrow_s32(v[i][1]));
        __m128i p = _mm_unpacklo_epi8(y264_ldp8(pred + i * ps), z);
        y264_stp8(dst + i * ds, _mm_packus_epi16(_mm_adds_epi16(res, p), z));
    }
}

/* ---- quant and dequant ----------------------------------------------------
 *
 * The flat-CQM multiplier and scale rows arrive fully expanded, one int32 per
 * raster position, from the per-QP tables transform.c builds at open with the
 * same expressions the scalar path uses. Only the flat path dispatches here;
 * a scaling-matrix stream stays scalar, exactly as on NEON.
 *
 * SATURATION. The narrow at the end saturates while the reference truncates,
 * and the two agree because the boundary is out of reach: |coef| <= 32767 and
 * the largest forward multiplier is 20972, so the product is under 2^31 and
 * the quotient after >> 15 is at most 20971. The dequant side is bounded by
 * the levels a conforming stream can carry -- past that the reference wraps
 * and this saturates, which is the same bargain the NEON twin struck. */

/* |c| * mf + f, arithmetic-shifted right by qbits, with c's sign restored.
 * The sign is restored with a compare and a blend rather than a sign-multiply
 * because a zero coefficient must take the POSITIVE branch: the reference
 * tests `c < 0`, and f can be large enough to make (0 * mf + f) >> qbits a
 * one. */
static inline __m128i y264_quant4(__m128i c, __m128i mf, __m128i f, __m128i sh)
{
    __m128i z = _mm_setzero_si128();
    __m128i q = _mm_sra_epi32(_mm_add_epi32(_mm_mullo_epi32(_mm_abs_epi32(c), mf), f), sh);
    return _mm_blendv_epi8(q, _mm_sub_epi32(z, q), _mm_cmpgt_epi32(z, c));
}

/* Two groups of four levels down to one register of eight int16. */
static inline __m128i y264_quant_pack(__m128i lo, __m128i hi)
{
    return _mm_packs_epi32(lo, hi);
}

#endif /* Y264_HAVE_SSE4 */

#endif /* YAH264_DSP_X86_TRANSFORM_X86_H */
