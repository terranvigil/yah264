/*
 * transform_x86_10.h - the 10-bit transform, quant and scan kernels' shared
 * 128-bit bodies
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * int32 twins of transform_x86.h. a 10-bit coefficient is an int32
 * (dctcoef), so every transform runs four int32 lanes a register, the C
 * reference's own width, and is exact wherever its int arithmetic is. the
 * SSE4.2 file wraps every body; the AVX2 file wraps the shapes with no second
 * half and writes 256-bit forms where eight int32 or two 4x4 blocks fill a
 * ymm.
 *
 * the 1-D passes are macros over the register type and its add, sub and
 * shift, so both widths run identical algebra. where a pass shifts (both 8x8
 * transforms, the 4x4 inverse) the order is the reference's, rows then
 * columns; the 4x4 forward has no shift, so its passes commute and it runs
 * columns first.
 *
 * bounds:
 *   - the forward quant forms |c| * mf + f in an unsigned 32-bit lane, exact
 *     while |c| < 2^17 (the largest 10-bit coefficient is the 8x8 DC of a
 *     full-swing residual, 64 * 1023 = 65472), mf <= 20972 (the largest flat
 *     multiplier) and 0 <= f < 2^30. a block outside that takes the scalar
 *     loop, so the kernels are exact on any input.
 *   - reconstruction clips to [0, 1023] with the signed-to-unsigned 32-bit
 *     pack for the floor and a 16-bit min for the top, never the byte pack.
 *
 * loads are sized to the block, never the register: checkasm puts a
 * PROT_NONE page at both ends of each kernel's window.
 */
#ifndef YAH264_DSP_X86_TRANSFORM_X86_10_H
#define YAH264_DSP_X86_TRANSFORM_X86_10_H

#include <immintrin.h>
#include <string.h>

#include "dsp/arch.h"

#if Y264_BIT_DEPTH != 10
#error "transform_x86_10.h is the 10-bit library's x86 tier"
#endif

/* ---- loads and stores ---------------------------------------------------- */

static inline __m128i t10_ldc(const dctcoef *p)
{
    return _mm_loadu_si128((const __m128i *)(const void *)p);
}

static inline void t10_stc(dctcoef *p, __m128i v)
{
    _mm_storeu_si128((__m128i *)(void *)p, v);
}

/* four samples, eight bytes */
static inline __m128i t10_ldp4(const pixel *p)
{
    return _mm_loadl_epi64((const __m128i *)(const void *)p);
}

static inline __m128i t10_ldp8(const pixel *p)
{
    return _mm_loadu_si128((const __m128i *)(const void *)p);
}

/* src - pred of four samples as int32 */
static inline __m128i t10_diff4(const pixel *s, const pixel *p)
{
    return _mm_sub_epi32(_mm_cvtepu16_epi32(t10_ldp4(s)),
                         _mm_cvtepu16_epi32(t10_ldp4(p)));
}

/* the unsigned 32-bit pack floors at zero, the min takes the top */
static inline __m128i t10_recon8(const pixel *pred, __m128i r0, __m128i r1)
{
    __m128i p = t10_ldp8(pred);
    __m128i a = _mm_add_epi32(_mm_cvtepu16_epi32(p), r0);
    __m128i b = _mm_add_epi32(_mm_cvtepu16_epi32(_mm_unpackhi_epi64(p, p)), r1);
    return _mm_min_epu16(_mm_packus_epi32(a, b), _mm_set1_epi16(PIXEL_MAX));
}

/* ---- the 1-D passes --------------------------------------------------------
 *
 * transcriptions of the scalar passes in src/dsp/transform.c, with ADD, SUB
 * and the arithmetic shifts as parameters. */

#define T10_FDCT4_1D(V, ADD, SUB, r0, r1, r2, r3) do {                        \
    V z0_ = ADD(r0, r3), z3_ = SUB(r0, r3);                                   \
    V z1_ = ADD(r1, r2), z2_ = SUB(r1, r2);                                   \
    (r0) = ADD(z0_, z1_);                                                     \
    (r1) = ADD(ADD(z3_, z3_), z2_);                                           \
    (r2) = SUB(z0_, z1_);                                                     \
    (r3) = SUB(z3_, ADD(z2_, z2_));                                           \
} while (0)

#define T10_IDCT4_1D(V, ADD, SUB, SR1, r0, r1, r2, r3) do {                   \
    V i0_ = ADD(r0, r2);                                                      \
    V i1_ = SUB(r0, r2);                                                      \
    V i2_ = SUB(SR1(r1), r3);                                                 \
    V i3_ = ADD(r1, SR1(r3));                                                 \
    (r0) = ADD(i0_, i3_);                                                     \
    (r1) = ADD(i1_, i2_);                                                     \
    (r2) = SUB(i1_, i2_);                                                     \
    (r3) = SUB(i0_, i3_);                                                     \
} while (0)

#define T10_FDCT8_1D(V, ADD, SUB, SR1, SR2, s0, s1, s2, s3, s4, s5, s6, s7) do {\
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

#define T10_IDCT8_1D(V, ADD, SUB, SR1, SR2, m0, m1, m2, m3, m4, m5, m6, m7) do {\
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

/* in-lane, so at 256 bits it transposes two 4x4 blocks, one a half */
#define T10_TRN4(V, UL32, UH32, UL64, UH64, r0, r1, r2, r3) do {              \
    V t0_ = UL32(r0, r1), t1_ = UH32(r0, r1);                                 \
    V t2_ = UL32(r2, r3), t3_ = UH32(r2, r3);                                 \
    (r0) = UL64(t0_, t2_);                                                    \
    (r1) = UH64(t0_, t2_);                                                    \
    (r2) = UL64(t1_, t3_);                                                    \
    (r3) = UH64(t1_, t3_);                                                    \
} while (0)

#define T10_SRA1(x) _mm_srai_epi32(x, 1)
#define T10_SRA2(x) _mm_srai_epi32(x, 2)
#define T10_TRN4Q(r0, r1, r2, r3)                                             \
    T10_TRN4(__m128i, _mm_unpacklo_epi32, _mm_unpackhi_epi32,                 \
             _mm_unpacklo_epi64, _mm_unpackhi_epi64, r0, r1, r2, r3)

/* ---- 4x4 transforms ------------------------------------------------------ */

/* columns first (the passes commute), then rows, then back to row order */
static inline void t10_fdct4_rows(__m128i r0, __m128i r1, __m128i r2, __m128i r3,
                                  dctcoef coef[16])
{
    T10_FDCT4_1D(__m128i, _mm_add_epi32, _mm_sub_epi32, r0, r1, r2, r3);
    T10_TRN4Q(r0, r1, r2, r3);
    T10_FDCT4_1D(__m128i, _mm_add_epi32, _mm_sub_epi32, r0, r1, r2, r3);
    T10_TRN4Q(r0, r1, r2, r3);
    t10_stc(coef, r0);
    t10_stc(coef + 4, r1);
    t10_stc(coef + 8, r2);
    t10_stc(coef + 12, r3);
}

static inline void t10_fdct4x4(const dctcoef diff[16], dctcoef coef[16])
{
    t10_fdct4_rows(t10_ldc(diff), t10_ldc(diff + 4), t10_ldc(diff + 8),
                   t10_ldc(diff + 12), coef);
}

static inline void t10_sub4x4_dct(dctcoef coef[16], const pixel *src, int ss,
                                  const pixel *pred, int ps)
{
    t10_fdct4_rows(t10_diff4(src, pred), t10_diff4(src + ss, pred + ps),
                   t10_diff4(src + 2 * ss, pred + 2 * ps),
                   t10_diff4(src + 3 * ss, pred + 3 * ps), coef);
}

/* the inverse through its (x + 32) >> 6, rows in r[0..3] */
static inline void t10_idct4_core(const dctcoef coef[16], __m128i r[4])
{
    __m128i r0 = t10_ldc(coef), r1 = t10_ldc(coef + 4);
    __m128i r2 = t10_ldc(coef + 8), r3 = t10_ldc(coef + 12);
    const __m128i k32 = _mm_set1_epi32(32);
    T10_TRN4Q(r0, r1, r2, r3);
    T10_IDCT4_1D(__m128i, _mm_add_epi32, _mm_sub_epi32, T10_SRA1, r0, r1, r2, r3);
    T10_TRN4Q(r0, r1, r2, r3);
    T10_IDCT4_1D(__m128i, _mm_add_epi32, _mm_sub_epi32, T10_SRA1, r0, r1, r2, r3);
    r[0] = _mm_srai_epi32(_mm_add_epi32(r0, k32), 6);
    r[1] = _mm_srai_epi32(_mm_add_epi32(r1, k32), 6);
    r[2] = _mm_srai_epi32(_mm_add_epi32(r2, k32), 6);
    r[3] = _mm_srai_epi32(_mm_add_epi32(r3, k32), 6);
}

static inline void t10_idct4x4(const dctcoef coef[16], dctcoef res[16])
{
    __m128i r[4];
    t10_idct4_core(coef, r);
    for (int i = 0; i < 4; i++)
        t10_stc(res + 4 * i, r[i]);
}

/* two rows of four per recon, so each store is exactly a row's eight bytes */
static inline void t10_add4x4_idct(pixel *dst, int ds, const pixel *pred, int ps,
                                   const dctcoef coef[16])
{
    __m128i r[4];
    const __m128i top = _mm_set1_epi16(PIXEL_MAX);
    t10_idct4_core(coef, r);
    for (int i = 0; i < 4; i += 2) {
        __m128i a = _mm_add_epi32(_mm_cvtepu16_epi32(t10_ldp4(pred + i * ps)), r[i]);
        __m128i b = _mm_add_epi32(_mm_cvtepu16_epi32(t10_ldp4(pred + (i + 1) * ps)),
                                  r[i + 1]);
        __m128i v = _mm_min_epu16(_mm_packus_epi32(a, b), top);
        _mm_storel_epi64((__m128i *)(void *)(dst + i * ds), v);
        _mm_storeh_pd((double *)(void *)(dst + (i + 1) * ds), _mm_castsi128_pd(v));
    }
}

/* ---- 8x8 transforms at 128 bits ---------------------------------------------
 *
 * v[i][0] is columns 0-3 of row i, v[i][1] columns 4-7. the 8x8 transpose is
 * the four tile transposes with the off-diagonal pair swapped. */
static inline void t10_trn8(__m128i v[8][2])
{
    __m128i a0 = v[0][0], a1 = v[1][0], a2 = v[2][0], a3 = v[3][0];
    __m128i b0 = v[0][1], b1 = v[1][1], b2 = v[2][1], b3 = v[3][1];
    __m128i c0 = v[4][0], c1 = v[5][0], c2 = v[6][0], c3 = v[7][0];
    __m128i d0 = v[4][1], d1 = v[5][1], d2 = v[6][1], d3 = v[7][1];
    T10_TRN4Q(a0, a1, a2, a3);
    T10_TRN4Q(b0, b1, b2, b3);
    T10_TRN4Q(c0, c1, c2, c3);
    T10_TRN4Q(d0, d1, d2, d3);
    v[0][0] = a0; v[1][0] = a1; v[2][0] = a2; v[3][0] = a3;
    v[0][1] = c0; v[1][1] = c1; v[2][1] = c2; v[3][1] = c3;
    v[4][0] = b0; v[5][0] = b1; v[6][0] = b2; v[7][0] = b3;
    v[4][1] = d0; v[5][1] = d1; v[6][1] = d2; v[7][1] = d3;
}

#define T10_PASS8(OP, v) do {                                                 \
    for (int h_ = 0; h_ < 2; h_++)                                            \
        OP(__m128i, _mm_add_epi32, _mm_sub_epi32, T10_SRA1, T10_SRA2,         \
           v[0][h_], v[1][h_], v[2][h_], v[3][h_],                            \
           v[4][h_], v[5][h_], v[6][h_], v[7][h_]);                           \
} while (0)

/* rows (after the first transpose they run between registers), then columns */
#define T10_XFORM8(OP, v) do {                                                \
    t10_trn8(v); T10_PASS8(OP, v);                                            \
    t10_trn8(v); T10_PASS8(OP, v);                                            \
} while (0)

static inline void t10_st8(dctcoef *p, __m128i v[8][2])
{
    for (int i = 0; i < 8; i++) {
        t10_stc(p + 8 * i, v[i][0]);
        t10_stc(p + 8 * i + 4, v[i][1]);
    }
}

static inline void t10_fdct8x8(const dctcoef diff[64], dctcoef coef[64])
{
    __m128i v[8][2];
    for (int i = 0; i < 8; i++) {
        v[i][0] = t10_ldc(diff + 8 * i);
        v[i][1] = t10_ldc(diff + 8 * i + 4);
    }
    T10_XFORM8(T10_FDCT8_1D, v);
    t10_st8(coef, v);
}

static inline void t10_sub8x8_dct8(dctcoef coef[64], const pixel *src, int ss,
                                   const pixel *pred, int ps)
{
    __m128i v[8][2];
    for (int i = 0; i < 8; i++) {
        __m128i s = t10_ldp8(src + i * ss), p = t10_ldp8(pred + i * ps);
        v[i][0] = _mm_sub_epi32(_mm_cvtepu16_epi32(s), _mm_cvtepu16_epi32(p));
        v[i][1] = _mm_sub_epi32(_mm_cvtepu16_epi32(_mm_unpackhi_epi64(s, s)),
                                _mm_cvtepu16_epi32(_mm_unpackhi_epi64(p, p)));
    }
    T10_XFORM8(T10_FDCT8_1D, v);
    t10_st8(coef, v);
}

static inline void t10_idct8_core(const dctcoef coef[64], __m128i v[8][2])
{
    const __m128i k32 = _mm_set1_epi32(32);
    for (int i = 0; i < 8; i++) {
        v[i][0] = t10_ldc(coef + 8 * i);
        v[i][1] = t10_ldc(coef + 8 * i + 4);
    }
    T10_XFORM8(T10_IDCT8_1D, v);
    for (int i = 0; i < 8; i++)
        for (int h = 0; h < 2; h++)
            v[i][h] = _mm_srai_epi32(_mm_add_epi32(v[i][h], k32), 6);
}

static inline void t10_idct8x8(const dctcoef coef[64], dctcoef res[64])
{
    __m128i v[8][2];
    t10_idct8_core(coef, v);
    t10_st8(res, v);
}

static inline void t10_add8x8_idct8(pixel *dst, int ds, const pixel *pred, int ps,
                                    const dctcoef coef[64])
{
    __m128i v[8][2];
    t10_idct8_core(coef, v);
    for (int i = 0; i < 8; i++)
        _mm_storeu_si128((__m128i *)(void *)(dst + i * ds),
                         t10_recon8(pred + i * ps, v[i][0], v[i][1]));
}

/* ---- forward quant ---------------------------------------------------------
 *
 * q = (|c| * mf + f) >> qbits with the sign restored, in an unsigned 32-bit
 * lane (exact in the domain at the top of the file). the sign comes back
 * through a compare and blend, not a sign-multiply: the reference treats a
 * zero coefficient as positive, and a bias of a whole step makes its level
 * nonzero. */
#define T10_QUANT_SAFE (1 << 17)
#define T10_QUANT_FMAX (1u << 30)

static inline __m128i t10_quant4(__m128i c, __m128i mf, __m128i vf, __m128i sh)
{
    __m128i z = _mm_setzero_si128();
    __m128i q = _mm_srl_epi32(_mm_add_epi32(_mm_mullo_epi32(_mm_abs_epi32(c), mf), vf), sh);
    return _mm_blendv_epi8(q, _mm_sub_epi32(z, q), _mm_cmpgt_epi32(z, c));
}

/* the reference's loop, for a block outside the 32-bit domain */
static inline void t10_quant_scalar(const dctcoef *coef, dctcoef *lev, int n, int f,
                                    int qbits, const int32_t *mfrow)
{
    for (int i = 0; i < n; i++) {
        int c = coef[i];
        int a = c < 0 ? -c : c;
        int q = (int)(((int64_t)a * mfrow[i] + f) >> qbits);
        lev[i] = (dctcoef)(c < 0 ? -q : q);
    }
}

/* nonzero when any of the n coefficients is >= 2^17 in magnitude; |INT_MIN|
 * stays negative and is caught the same way */
static inline int t10_quant_wide(const dctcoef *coef, int n)
{
    __m128i m = _mm_setzero_si128();
    for (int i = 0; i < n; i += 4)
        m = _mm_or_si128(m, _mm_abs_epi32(t10_ldc(coef + i)));
    return !_mm_testz_si128(m, _mm_set1_epi32(-T10_QUANT_SAFE));
}

static inline void t10_quant_n(const dctcoef *coef, dctcoef *lev, int n, int f,
                               int qbits, const int32_t *mfrow)
{
    if ((unsigned)f >= T10_QUANT_FMAX || t10_quant_wide(coef, n)) {
        t10_quant_scalar(coef, lev, n, f, qbits, mfrow);
        return;
    }
    __m128i vf = _mm_set1_epi32(f), sh = _mm_cvtsi32_si128(qbits);
    for (int i = 0; i < n; i += 4)
        t10_stc(lev + i, t10_quant4(t10_ldc(coef + i), t10_ldc(mfrow + i), vf, sh));
}

/* the AC tail of one quantised 4x4 block: DC out, its level zeroed, and the
 * count of nonzero levels; the four compares sum to minus the zero count */
static inline int t10_quant_ac_tail(const dctcoef *coef, dctcoef *lev, dctcoef *dc,
                                    int *nz)
{
    __m128i z = _mm_setzero_si128();
    *dc = coef[0];
    lev[0] = 0;
    __m128i s = _mm_add_epi32(
        _mm_add_epi32(_mm_cmpeq_epi32(t10_ldc(lev), z), _mm_cmpeq_epi32(t10_ldc(lev + 4), z)),
        _mm_add_epi32(_mm_cmpeq_epi32(t10_ldc(lev + 8), z), _mm_cmpeq_epi32(t10_ldc(lev + 12), z)));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0x4e));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0xb1));
    int c = 16 + _mm_cvtsi128_si32(s);
    *nz = c;
    return c;
}

/* ---- dequant ---------------------------------------------------------------
 *
 * the reference's int multiply and shift: lev * ls is shifted up once qp/6
 * reaches the block's shift point, and down with rounding below it. the low
 * 32 bits of the product are the reference's int product wherever that is
 * defined (levels to +-65535 at every qp). */
static inline void t10_dequant(const dctcoef *lev, dctcoef *coef, int n, int qp,
                               const int32_t *lsrow, int point)
{
    int shift = qp / 6;
    if (shift >= point) {
        __m128i vs = _mm_cvtsi32_si128(shift - point);
        for (int g = 0; g < n; g += 4)
            t10_stc(coef + g, _mm_sll_epi32(_mm_mullo_epi32(t10_ldc(lev + g),
                                                            t10_ldc(lsrow + g)), vs));
    } else {
        __m128i vr = _mm_set1_epi32(1 << (point - 1 - shift));
        __m128i vs = _mm_cvtsi32_si128(point - shift);
        for (int g = 0; g < n; g += 4)
            t10_stc(coef + g, _mm_sra_epi32(_mm_add_epi32(_mm_mullo_epi32(
                t10_ldc(lev + g), t10_ldc(lsrow + g)), vr), vs));
    }
}

/* ---- scans -----------------------------------------------------------------
 *
 * 4x4 zig-zag over int32 rows r0-r3: scan 0-3 is raster 0 1 4 8, 4-7 is
 * 5 2 3 6, 8-11 is 9 12 13 10, 12-15 is 7 11 14 15. the outer groups are
 * unpacks, the inner two a shuffle of two rows each and a blend of their
 * middle lanes. */
static inline void t10_zz4(const dctcoef in[16], __m128i g[4])
{
    __m128i r0 = t10_ldc(in), r1 = t10_ldc(in + 4);
    __m128i r2 = t10_ldc(in + 8), r3 = t10_ldc(in + 12);
    g[0] = _mm_unpacklo_epi64(r0, _mm_unpacklo_epi32(r1, r2));
    g[1] = _mm_blend_epi16(_mm_shuffle_epi32(r1, _MM_SHUFFLE(2, 0, 0, 1)),
                           _mm_shuffle_epi32(r0, _MM_SHUFFLE(0, 3, 2, 0)), 0x3c);
    g[2] = _mm_blend_epi16(_mm_shuffle_epi32(r2, _MM_SHUFFLE(2, 0, 0, 1)),
                           _mm_shuffle_epi32(r3, _MM_SHUFFLE(0, 1, 0, 0)), 0x3c);
    g[3] = _mm_unpackhi_epi64(_mm_unpackhi_epi32(r1, r2), r3);
}

static inline void t10_zigzag_abs_4x4(int out[16], const dctcoef in[16])
{
    __m128i g[4];
    t10_zz4(in, g);
    for (int i = 0; i < 4; i++)
        t10_stc(out + 4 * i, _mm_abs_epi32(g[i]));
}

/* |v| >= 2 as (v > 1) | (-1 > v), the reference's (unsigned)(v + 1) > 2u for
 * every int32 */
static inline __m128i t10_big4(__m128i v)
{
    return _mm_or_si128(_mm_cmpgt_epi32(v, _mm_set1_epi32(1)),
                        _mm_cmpgt_epi32(_mm_set1_epi32(-1), v));
}

static inline void t10_zigzag_scan_4x4(dctcoef out[16], const dctcoef in[16],
                                       uint32_t *omsk, int *obig)
{
    __m128i g[4], big = _mm_setzero_si128();
    const __m128i z = _mm_setzero_si128();
    unsigned zero = 0;
    t10_zz4(in, g);
    for (int i = 0; i < 4; i++) {
        t10_stc(out + 4 * i, g[i]);
        zero |= (unsigned)_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(g[i], z))) << (4 * i);
        big = _mm_or_si128(big, t10_big4(g[i]));
    }
    *omsk = ~zero & 0xffffu;
    *obig = !_mm_testz_si128(big, big);
}

/* the 8x8 nonzero mask and |level| >= 2 flag need no scan-order levels, so
 * two saturating packs narrow each level to a byte (zero stays zero, -1..1
 * stay themselves) and each sixteen-position scan group is a byte shuffle of
 * the three or four registers it draws on. controls come from y264_zigzag8:
 * for scan group k and source register j (raster 16j to 16j + 15), the byte
 * of j each position reads, or -1 where another register owns it. */
static const uint8_t t10_zzb_src[14] = { 0, 1, 2, 0, 1, 2, 3, 0, 1, 2, 3, 1, 2, 3 };
static const uint8_t t10_zzb_off[5] = { 0, 3, 7, 11, 14 };
static const int8_t t10_zzb_msk[14][16] = {
    {  0,  1,  8, -1,  9,  2,  3, 10, -1, -1, -1, -1, -1, 11,  4,  5 },
    { -1, -1, -1,  0, -1, -1, -1, -1,  1,  8, -1,  9,  2, -1, -1, -1 },
    { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,  0, -1, -1, -1, -1, -1 },
    { 12, -1, -1, -1, -1, -1, -1, -1, -1, -1, 13,  6,  7, 14, -1, -1 },
    { -1,  3, 10, -1, -1, -1, -1, -1, 11,  4, -1, -1, -1, -1,  5, 12 },
    { -1, -1, -1,  1,  8, -1,  9,  2, -1, -1, -1, -1, -1, -1, -1, -1 },
    { -1, -1, -1, -1, -1,  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
    { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 15, -1, -1, -1, -1, -1 },
    { -1, -1, -1, -1, -1, -1, -1, -1, 13,  6, -1,  7, 14, -1, -1, -1 },
    {  3, 10, -1, -1, -1, -1, 11,  4, -1, -1, -1, -1, -1,  5, 12, -1 },
    { -1, -1,  1,  8,  9,  2, -1, -1, -1, -1, -1, -1, -1, -1, -1,  3 },
    { -1, -1, -1, -1, -1, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
    { -1, -1, -1, 13,  6, -1,  7, 14, -1, -1, -1, -1, 15, -1, -1, -1 },
    { 10, 11,  4, -1, -1, -1, -1, -1,  5, 12, 13,  6, -1,  7, 14, 15 },
};

static inline void t10_scan_mask_8x8(const dctcoef lev[64], uint64_t *omsk, int *obig)
{
    const __m128i z = _mm_setzero_si128();
    __m128i p[4], big = z;
    uint64_t msk = 0;
    for (int j = 0; j < 4; j++) {
        const dctcoef *l = lev + 16 * j;
        __m128i a = t10_ldc(l), b = t10_ldc(l + 4), c = t10_ldc(l + 8), d = t10_ldc(l + 12);
        big = _mm_or_si128(big, _mm_or_si128(_mm_or_si128(t10_big4(a), t10_big4(b)),
                                             _mm_or_si128(t10_big4(c), t10_big4(d))));
        p[j] = _mm_packs_epi16(_mm_packs_epi32(a, b), _mm_packs_epi32(c, d));
    }
    for (int k = 0; k < 4; k++) {
        __m128i v = z;
        for (int i = t10_zzb_off[k]; i < t10_zzb_off[k + 1]; i++)
            v = _mm_or_si128(v, _mm_shuffle_epi8(p[t10_zzb_src[i]],
                    _mm_loadu_si128((const __m128i *)(const void *)t10_zzb_msk[i])));
        unsigned zero = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(v, z));
        msk |= (uint64_t)(~zero & 0xffffu) << (16 * k);
    }
    *omsk = msk;
    *obig = !_mm_testz_si128(big, big);
}

#endif /* YAH264_DSP_X86_TRANSFORM_X86_10_H */
