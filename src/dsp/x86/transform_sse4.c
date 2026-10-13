/*
 * transform_sse4.c - SSE4.2 kernels for the transform, quant and scan families
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Wave 3a of docs/x86-plan.md: the SSE4.2 twin of every transform, quant and
 * scan kernel the NEON twin covers. Each one is bit-exact with the portable
 * reference in src/dsp/transform.c -- not close, exact -- which is what lets
 * the tier be a runtime axis (Y264_SIMD_FORCE) that cannot move a byte of
 * output.
 *
 * The butterflies, the transposes and the 128-bit cores are in
 * transform_x86.h, along with the reason each one runs in the lane width it
 * does. What is decided HERE is the shape of the two families that are not
 * transforms:
 *
 *   - QUANT AND DEQUANT are elementwise streams over a per-QP multiplier row
 *     the caller owns, with no shuffle anywhere. The forward quant has two
 *     forms. The usual one stays in the coefficient's own 16-bit lanes, eight
 *     to a register, and is used whenever the shift is at least sixteen and
 *     the rounding bias is under one quantiser step (transform_x86.h carries
 *     the argument). The other widens to four 32-bit lanes and takes any bias:
 *     the reference tests the COEFFICIENT's sign and not the quotient's, so a
 *     zero coefficient takes the positive branch even where the bias alone
 *     rounds up to one, and there the restore has to be a compare plus a blend
 *     rather than a sign-multiply, which would answer zero. The batched entry
 *     points (n blocks per call, and the AC form that also returns each
 *     block's DC and nonzero count) keep the row and the bias in registers
 *     across the blocks.
 *
 *   - THE SCAN is a 128-byte permutation and PSHUFB reaches sixteen bytes, so
 *     each output group of eight coefficients is assembled from the source
 *     ROWS it actually draws from -- three to six of the eight, never all
 *     eight -- with one shuffle per row and an OR to merge them. The tables
 *     below are the zig-zag scan of src/dsp/transform.c expressed that way:
 *     for output group g and source row r, the byte pair each lane wants, or
 *     0x80 in the lanes another row owns (PSHUFB writes a zero for an index
 *     with its top bit set, which is what makes the merge an OR). They were
 *     generated from y264_zigzag4 / y264_zigzag8 and every lane of them is
 *     checked by the checkasm scan group against the scalar gather.
 *
 * C11 intrinsics only. There is no assembly in this tree, and hygiene_check
 * refuses any inline form under this directory.
 */
#include "dsp/x86/transform_x86.h"

#if Y264_HAVE_SSE4

/* ---- the coefficient-domain transforms ----------------------------------- */

void y264_fdct4x4_sse4(const dctcoef diff[16], dctcoef coef[16])
{
    y264_fdct4x4_core(diff, coef);
}

void y264_idct4x4_sse4(const dctcoef coef[16], dctcoef res[16])
{
    y264_idct4x4_core(coef, res);
}

void y264_fdct8x8_sse4(const dctcoef diff[64], dctcoef coef[64])
{
    y264_fdct8x8_core(diff, coef);
}

void y264_idct8x8_sse4(const dctcoef coef[64], dctcoef res[64])
{
    y264_idct8x8_core(coef, res);
}

/* ---- the fused pixel-domain transforms ----------------------------------- */

void y264_sub4x4_dct_sse4(dctcoef coef[16], const pixel *src, int ss,
                          const pixel *pred, int ps)
{
    y264_sub4x4_dct_core(coef, src, ss, pred, ps);
}

void y264_add4x4_idct_sse4(pixel *dst, int ds, const pixel *pred, int ps,
                           const dctcoef coef[16])
{
    y264_add4x4_idct_core(dst, ds, pred, ps, coef);
}

void y264_sub8x8_dct8_sse4(dctcoef coef[64], const pixel *src, int ss,
                           const pixel *pred, int ps)
{
    y264_sub8x8_dct8_core(coef, src, ss, pred, ps);
}

void y264_add8x8_idct8_sse4(pixel *dst, int ds, const pixel *pred, int ps,
                            const dctcoef coef[64])
{
    y264_add8x8_idct8_core(dst, ds, pred, ps, coef);
}

/* The batched forward transform, two horizontally adjacent blocks at a time:
 * nbw is even at every call site, the eight-byte loads stay inside the grid
 * so an edge macroblock is safe, and the dispatch branch is paid once per grid
 * instead of once per block. */
void y264_sub_dct4_blocks_sse4(dctcoef (*coef)[16], int nbw, int nbh,
                               const pixel *src, int ss,
                               const pixel *pred, int ps)
{
    for (int by = 0; by < nbh; by++, src += 4 * ss, pred += 4 * ps)
        for (int bx = 0; bx < nbw; bx += 2, coef += 2)
            y264_fdct4_dual(y264_diff8(src + bx * 4, pred + bx * 4),
                            y264_diff8(src + ss + bx * 4, pred + ps + bx * 4),
                            y264_diff8(src + 2 * ss + bx * 4, pred + 2 * ps + bx * 4),
                            y264_diff8(src + 3 * ss + bx * 4, pred + 3 * ps + bx * 4),
                            coef[0], coef[1]);
}

/* The batched 8x8 forward: a grid of blocks, one at a time here. An 8x8 block
 * already fills this tier's register, so the batch buys the single dispatch
 * and nothing else; the AVX2 twin is the one that pairs blocks. */
void y264_sub_dct8_blocks_sse4(dctcoef (*coef)[64], int nbw, int nbh,
                               const pixel *src, int ss,
                               const pixel *pred, int ps)
{
    for (int by = 0; by < nbh; by++, src += 8 * ss, pred += 8 * ps)
        for (int bx = 0; bx < nbw; bx++, coef++)
            y264_sub8x8_dct8_core(coef[0], src + bx * 8, ss, pred + bx * 8, ps);
}

/* ---- quant and dequant --------------------------------------------------- */

#define Y264_MFLD(row, g) \
    _mm_loadu_si128((const __m128i *)(const void *)((row) + (g)))

/* The 4x4 forward quant, two registers of eight coefficients. The 16-bit form
 * where it applies (transform_x86.h), the 32-bit one otherwise. */
static inline void quant_4x4_s32(const dctcoef *coef, dctcoef *lev, int qbits,
                                 int f, const int32_t *mfrow)
{
    __m128i vf = _mm_set1_epi32(f);
    __m128i sh = _mm_cvtsi32_si128(qbits);
    y264_stc8(lev, y264_quant8_s32(y264_ldc8(coef), mfrow, vf, sh));
    y264_stc8(lev + 8, y264_quant8_s32(y264_ldc8(coef + 8), mfrow + 8, vf, sh));
}

void y264_quant_4x4_f_sse4(const dctcoef coef[16], dctcoef lev[16], int qp, int f,
                           const int32_t mfrow[16])
{
    int qbits = 15 + qp / 6;
    if (!y264_quant16_ok(f, qbits)) {
        quant_4x4_s32(coef, lev, qbits, f, mfrow);
        return;
    }
    __m128i flo = _mm_set1_epi16(Y264_QUANT16_FLO(f));
    __m128i fhi = _mm_set1_epi16(Y264_QUANT16_FHI1(f));
    __m128i sh = _mm_cvtsi32_si128(qbits - 16);
    y264_stc8(lev, y264_quant16(y264_ldc8(coef), y264_mf16(mfrow), flo, fhi, sh));
    y264_stc8(lev + 8, y264_quant16(y264_ldc8(coef + 8), y264_mf16(mfrow + 8),
                                    flo, fhi, sh));
}

void y264_quant_4x4_sse4(const dctcoef coef[16], dctcoef lev[16], int qp, int intra,
                         const int32_t mfrow[16])
{
    int qbits = 15 + qp / 6;
    y264_quant_4x4_f_sse4(coef, lev, qp, (1 << qbits) / (intra ? 3 : 6), mfrow);
}

/* n consecutive 4x4 blocks: the multiplier row and the bias stay in registers
 * across them, and the caller's dispatch is paid once. Block k is exactly
 * y264_quant_4x4_f_sse4 on coef + 16k. */
void y264_quant_4x4_f_n_sse4(const dctcoef *coef, dctcoef *lev, int n, int qp,
                             int f, const int32_t mfrow[16])
{
    int qbits = 15 + qp / 6;
    if (!y264_quant16_ok(f, qbits)) {
        for (int k = 0; k < n; k++)
            quant_4x4_s32(coef + 16 * k, lev + 16 * k, qbits, f, mfrow);
        return;
    }
    __m128i flo = _mm_set1_epi16(Y264_QUANT16_FLO(f));
    __m128i fhi = _mm_set1_epi16(Y264_QUANT16_FHI1(f));
    __m128i sh = _mm_cvtsi32_si128(qbits - 16);
    __m128i m0 = y264_mf16(mfrow), m1 = y264_mf16(mfrow + 8);
    for (int k = 0; k < n; k++, coef += 16, lev += 16) {
        y264_stc8(lev, y264_quant16(y264_ldc8(coef), m0, flo, fhi, sh));
        y264_stc8(lev + 8, y264_quant16(y264_ldc8(coef + 8), m1, flo, fhi, sh));
    }
}

/* n inter AC blocks (chroma): the quant above on each, then the block's DC
 * coefficient copied out to dc[k], its level zeroed, and its nonzero AC count
 * left in nz[k]. Returns nonzero if any block keeps an AC level. */
int y264_quant_4x4_ac_f_sse4(const dctcoef *coef, dctcoef *lev, int n, int qp,
                             int f, const int32_t mfrow[16], dctcoef *dc, int *nz)
{
    int qbits = 15 + qp / 6;
    int any = 0;
    if (!y264_quant16_ok(f, qbits)) {
        for (int k = 0; k < n; k++, coef += 16, lev += 16) {
            quant_4x4_s32(coef, lev, qbits, f, mfrow);
            any |= y264_quant_ac_tail(coef, lev, &dc[k], &nz[k]);
        }
        return any;
    }
    __m128i flo = _mm_set1_epi16(Y264_QUANT16_FLO(f));
    __m128i fhi = _mm_set1_epi16(Y264_QUANT16_FHI1(f));
    __m128i sh = _mm_cvtsi32_si128(qbits - 16);
    __m128i m0 = y264_mf16(mfrow), m1 = y264_mf16(mfrow + 8);
    const __m128i nodc = _mm_setr_epi16(0, -1, -1, -1, -1, -1, -1, -1);
    for (int k = 0; k < n; k++, coef += 16, lev += 16) {
        __m128i l0 = _mm_and_si128(
            y264_quant16(y264_ldc8(coef), m0, flo, fhi, sh), nodc);
        __m128i l1 = y264_quant16(y264_ldc8(coef + 8), m1, flo, fhi, sh);
        dc[k] = coef[0];
        y264_stc8(lev, l0);
        y264_stc8(lev + 8, l1);
        int c = y264_nnz16(l0, l1);
        nz[k] = c;
        any |= c;
    }
    return any;
}

/* n consecutive 8x8 blocks. The shift is 16 + qp/6, so the 16-bit form always
 * applies unless the bias is out of its range. */
void y264_quant_8x8_f_n_sse4(const dctcoef *coef, dctcoef *lev, int n, int qp,
                             int f, const int32_t mfrow[64])
{
    int qbits = 16 + qp / 6;
    if (!y264_quant16_ok(f, qbits)) {
        __m128i vf = _mm_set1_epi32(f);
        __m128i sh = _mm_cvtsi32_si128(qbits);
        for (int k = 0; k < n; k++, coef += 64, lev += 64)
            for (int g = 0; g < 64; g += 8)
                y264_stc8(lev + g, y264_quant8_s32(y264_ldc8(coef + g),
                                                   mfrow + g, vf, sh));
        return;
    }
    __m128i flo = _mm_set1_epi16(Y264_QUANT16_FLO(f));
    __m128i fhi = _mm_set1_epi16(Y264_QUANT16_FHI1(f));
    __m128i sh = _mm_cvtsi32_si128(qbits - 16);
    __m128i m[8];
    for (int g = 0; g < 8; g++)
        m[g] = y264_mf16(mfrow + 8 * g);
    for (int k = 0; k < n; k++, coef += 64, lev += 64)
        for (int g = 0; g < 8; g++)
            y264_stc8(lev + 8 * g, y264_quant16(y264_ldc8(coef + 8 * g), m[g],
                                                flo, fhi, sh));
}

void y264_quant_8x8_f_sse4(const dctcoef coef[64], dctcoef lev[64], int qp, int f,
                           const int32_t mfrow[64])
{
    y264_quant_8x8_f_n_sse4(coef, lev, 1, qp, f, mfrow);
}

/* The inverse scale. Two shapes rather than one, because the shift direction
 * is loop-invariant and the reference hoists the same branch for the same
 * reason. The left arm is written as a shift and not as the reference's
 * multiply by a power of two: the bits are the same and the shift does not
 * have the signed overflow the multiply form would have in C. */
#define Y264_DEQUANT(name, n, big, rndbits)                                   \
void name(const dctcoef lev[n], dctcoef coef[n], int qp,                      \
          const int32_t lsrow[n])                                             \
{                                                                             \
    int shift = qp / 6;                                                       \
    if (shift >= (big)) {                                                     \
        __m128i vs = _mm_cvtsi32_si128(shift - (big));                        \
        for (int g = 0; g < (n); g += 8) {                                    \
            __m128i l = y264_ldc8(lev + g);                                   \
            __m128i d0 = _mm_sll_epi32(_mm_mullo_epi32(                       \
                _mm_cvtepi16_epi32(l), Y264_MFLD(lsrow, g)), vs);             \
            __m128i d1 = _mm_sll_epi32(_mm_mullo_epi32(                       \
                _mm_cvtepi16_epi32(_mm_unpackhi_epi64(l, l)),                 \
                Y264_MFLD(lsrow, g + 4)), vs);                                \
            y264_stc8(coef + g, _mm_packs_epi32(d0, d1));                     \
        }                                                                     \
    } else {                                                                  \
        __m128i vr = _mm_set1_epi32(1 << ((rndbits) - shift));                \
        __m128i vs = _mm_cvtsi32_si128((big) - shift);                        \
        for (int g = 0; g < (n); g += 8) {                                    \
            __m128i l = y264_ldc8(lev + g);                                   \
            __m128i d0 = _mm_sra_epi32(_mm_add_epi32(_mm_mullo_epi32(         \
                _mm_cvtepi16_epi32(l), Y264_MFLD(lsrow, g)), vr), vs);        \
            __m128i d1 = _mm_sra_epi32(_mm_add_epi32(_mm_mullo_epi32(         \
                _mm_cvtepi16_epi32(_mm_unpackhi_epi64(l, l)),                 \
                Y264_MFLD(lsrow, g + 4)), vr), vs);                           \
            y264_stc8(coef + g, _mm_packs_epi32(d0, d1));                     \
        }                                                                     \
    }                                                                         \
}

Y264_DEQUANT(y264_dequant_4x4_sse4, 16, 4, 3)
Y264_DEQUANT(y264_dequant_8x8_sse4, 64, 6, 5)

/* ---- the zig-zag scan ----------------------------------------------------
 *
 * `_src[i]` is the source row of lookup i, `_msk[i]` its byte mask, and
 * `_off[g]` the first lookup of output group g. See the file header for how
 * they were derived and what checks them.
 */
static const uint8_t zz8_src[36] = {
    0x00, 0x01, 0x02, 0x00, 0x01, 0x02, 0x03, 0x04, 0x01, 0x02, 0x03, 0x04,
    0x05, 0x06, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x01, 0x02,
    0x03, 0x04, 0x05, 0x06, 0x03, 0x04, 0x05, 0x06, 0x07, 0x05, 0x06, 0x07
};
static const uint8_t zz8_off[9] = {
    0x00, 0x03, 0x08, 0x0e, 0x12, 0x16, 0x1c, 0x21, 0x24
};
static const uint8_t zz8_msk[36][16] = {
    {
      0x00, 0x01, 0x02, 0x03, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x04, 0x05,
      0x06, 0x07, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x00, 0x01, 0x80, 0x80, 0x02, 0x03, 0x80, 0x80,
      0x80, 0x80, 0x04, 0x05
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x01, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x08, 0x09, 0x0a, 0x0b
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x06, 0x07,
      0x80, 0x80, 0x80, 0x80
    },
    {
      0x02, 0x03, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x04, 0x05, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x00, 0x01, 0x80, 0x80, 0x02, 0x03, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x00, 0x01, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80
    },
    {
      0x08, 0x09, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x06, 0x07, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x04, 0x05, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x02, 0x03, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x04, 0x05
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x01, 0x80, 0x80,
      0x02, 0x03, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x01,
      0x80, 0x80, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x0c, 0x0d, 0x0e, 0x0f, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x0a, 0x0b, 0x80, 0x80, 0x80, 0x80, 0x0c, 0x0d,
      0x80, 0x80, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x08, 0x09, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x0a, 0x0b, 0x80, 0x80
    },
    {
      0x06, 0x07, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x08, 0x09
    },
    {
      0x06, 0x07, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x08, 0x09
    },
    {
      0x80, 0x80, 0x04, 0x05, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x06, 0x07, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x02, 0x03, 0x80, 0x80, 0x80, 0x80, 0x04, 0x05,
      0x80, 0x80, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x01, 0x02, 0x03, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x0e, 0x0f, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x0c, 0x0d, 0x80, 0x80, 0x0e, 0x0f, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80
    },
    {
      0x0a, 0x0b, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x0c, 0x0d, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x0a, 0x0b,
      0x80, 0x80, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x08, 0x09, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x06, 0x07
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x0e, 0x0f,
      0x80, 0x80, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x0c, 0x0d, 0x80, 0x80,
      0x0e, 0x0f, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x0a, 0x0b, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x0c, 0x0d
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x08, 0x09, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80
    },
    {
      0x04, 0x05, 0x06, 0x07, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x0e, 0x0f, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80
    },
    {
      0x0a, 0x0b, 0x80, 0x80, 0x80, 0x80, 0x0c, 0x0d, 0x80, 0x80, 0x0e, 0x0f,
      0x80, 0x80, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x08, 0x09, 0x0a, 0x0b, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x0c, 0x0d, 0x0e, 0x0f
    },
};

static const uint8_t zz4_src[4] = {
    0x00, 0x01, 0x00, 0x01
};
static const uint8_t zz4_off[3] = {
    0x00, 0x02, 0x04
};
static const uint8_t zz4_msk[4][16] = {
    {
      0x00, 0x01, 0x02, 0x03, 0x08, 0x09, 0x80, 0x80, 0x0a, 0x0b, 0x04, 0x05,
      0x06, 0x07, 0x0c, 0x0d
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x01, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x0e, 0x0f, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80
    },
    {
      0x02, 0x03, 0x08, 0x09, 0x0a, 0x0b, 0x04, 0x05, 0x80, 0x80, 0x06, 0x07,
      0x0c, 0x0d, 0x0e, 0x0f
    },
};

static inline void y264_zz8_gather(const dctcoef in[64], __m128i g[8])
{
    __m128i row[8];
    for (int i = 0; i < 8; i++)
        row[i] = y264_ldc8(in + 8 * i);
    for (int k = 0; k < 8; k++) {
        __m128i v = _mm_setzero_si128();
        for (int i = zz8_off[k]; i < zz8_off[k + 1]; i++)
            v = _mm_or_si128(v, _mm_shuffle_epi8(row[zz8_src[i]],
                    _mm_loadu_si128((const __m128i *)(const void *)zz8_msk[i])));
        g[k] = v;
    }
}

/* Widen BEFORE the abs: the reference reads the coefficient into an int and
 * negates there, so |-32768| is 32768 -- a value a 16-bit ABS returns
 * unchanged. checkasm's all--32768 block is exactly this corner. */
static inline void y264_st_abs_s32(int *out, __m128i v)
{
    _mm_storeu_si128((__m128i *)(void *)(out + 0),
                     _mm_abs_epi32(_mm_cvtepi16_epi32(v)));
    _mm_storeu_si128((__m128i *)(void *)(out + 4),
                     _mm_abs_epi32(_mm_cvtepi16_epi32(_mm_unpackhi_epi64(v, v))));
}

void y264_zigzag_abs_8x8_sse4(int out[64], const dctcoef in[64])
{
    __m128i g[8];
    y264_zz8_gather(in, g);
    for (int k = 0; k < 8; k++)
        y264_st_abs_s32(out + 8 * k, g[k]);
}

/* The 4x4 form: the RDOQ trellis asks for it twice per block (levels and
 * coefficients). */
void y264_zigzag_abs_4x4_sse4(int out[16], const dctcoef in[16])
{
    __m128i g0, g1;
    y264_zz4_groups(in, &g0, &g1);
    y264_st_abs_s32(out, g0);
    y264_st_abs_s32(out + 8, g1);
}

/* Per-group nonzero bitmask plus the decimator's |level| >= 2 test, written as
 * (v > 1) | (-1 > v) rather than through an abs so that the int16 corner
 * -32768 reads the same as the reference's (unsigned)(v + 1) > 2u. */
static inline unsigned y264_nz_mask8(__m128i v, __m128i *bigacc)
{
    __m128i z = _mm_setzero_si128();
    __m128i eq = _mm_packs_epi16(_mm_cmpeq_epi16(v, z), z);
    *bigacc = _mm_or_si128(*bigacc,
                           _mm_or_si128(_mm_cmpgt_epi16(v, _mm_set1_epi16(1)),
                                        _mm_cmpgt_epi16(_mm_set1_epi16(-1), v)));
    return (unsigned)(~_mm_movemask_epi8(eq)) & 0xffu;
}

/* ---- the scan mask, without the scan ---------------------------------------
 *
 * The decimator asks two things of an 8x8 block of levels: which scan
 * positions are nonzero, and whether any level is outside -1..1. Neither needs
 * the levels themselves in scan order, so the block is first narrowed to one
 * BYTE per level with the saturating pack, which keeps both properties (a
 * level is zero, or within -1..1, exactly when its saturated byte is). The 64
 * bytes are then four registers of sixteen, and each sixteen-position scan
 * group is a byte shuffle of the three or four of them it draws on, OR-ed
 * together: fourteen shuffles for the block where the 16-bit gather takes
 * thirty-two, and the mask falls out of four byte compares.
 *
 * "Outside -1..1" is order-free and is read off the same bytes before they
 * are shuffled: b + 1 as an unsigned byte is 0, 1 or 2 exactly for b in
 * -1..1, which is the reference's (unsigned)(v + 1) > 2u.
 *
 * The controls are generated from y264_zigzag8: for scan group k and source
 * register j, the byte of j each position reads, or -1 where another
 * register owns it.
 */
static const uint8_t y264_zzb_src[14] = { 0, 1, 2, 0, 1, 2, 3, 0, 1, 2, 3, 1, 2, 3 };
static const uint8_t y264_zzb_off[5] = { 0, 3, 7, 11, 14 };
static const int8_t y264_zzb_msk[14][16] = {
    {  0,  1,  8, -1,  9,  2,  3, 10, -1, -1, -1, -1, -1, 11,  4,  5 },   /* scan 0-15 from raster 0-15 */
    { -1, -1, -1,  0, -1, -1, -1, -1,  1,  8, -1,  9,  2, -1, -1, -1 },   /* scan 0-15 from raster 16-31 */
    { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,  0, -1, -1, -1, -1, -1 },   /* scan 0-15 from raster 32-47 */
    { 12, -1, -1, -1, -1, -1, -1, -1, -1, -1, 13,  6,  7, 14, -1, -1 },   /* scan 16-31 from raster 0-15 */
    { -1,  3, 10, -1, -1, -1, -1, -1, 11,  4, -1, -1, -1, -1,  5, 12 },   /* scan 16-31 from raster 16-31 */
    { -1, -1, -1,  1,  8, -1,  9,  2, -1, -1, -1, -1, -1, -1, -1, -1 },   /* scan 16-31 from raster 32-47 */
    { -1, -1, -1, -1, -1,  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },   /* scan 16-31 from raster 48-63 */
    { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 15, -1, -1, -1, -1, -1 },   /* scan 32-47 from raster 0-15 */
    { -1, -1, -1, -1, -1, -1, -1, -1, 13,  6, -1,  7, 14, -1, -1, -1 },   /* scan 32-47 from raster 16-31 */
    {  3, 10, -1, -1, -1, -1, 11,  4, -1, -1, -1, -1, -1,  5, 12, -1 },   /* scan 32-47 from raster 32-47 */
    { -1, -1,  1,  8,  9,  2, -1, -1, -1, -1, -1, -1, -1, -1, -1,  3 },   /* scan 32-47 from raster 48-63 */
    { -1, -1, -1, -1, -1, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },   /* scan 48-63 from raster 16-31 */
    { -1, -1, -1, 13,  6, -1,  7, 14, -1, -1, -1, -1, 15, -1, -1, -1 },   /* scan 48-63 from raster 32-47 */
    { 10, 11,  4, -1, -1, -1, -1, -1,  5, 12, 13,  6, -1,  7, 14, 15 },   /* scan 48-63 from raster 48-63 */
};

void y264_scan_mask_8x8_sse4(const dctcoef lev[64], uint64_t *omsk, int *obig)
{
    const __m128i z = _mm_setzero_si128();
    const __m128i one = _mm_set1_epi8(1), two = _mm_set1_epi8(2);
    __m128i p[4], big = z;
    uint64_t msk = 0;
    for (int j = 0; j < 4; j++) {
        p[j] = _mm_packs_epi16(y264_ldc8(lev + 16 * j), y264_ldc8(lev + 16 * j + 8));
        big = _mm_or_si128(big, _mm_subs_epu8(_mm_add_epi8(p[j], one), two));
    }
    for (int k = 0; k < 4; k++) {
        __m128i v = z;
        for (int i = y264_zzb_off[k]; i < y264_zzb_off[k + 1]; i++)
            v = _mm_or_si128(v, _mm_shuffle_epi8(p[y264_zzb_src[i]],
                    _mm_loadu_si128((const __m128i *)(const void *)y264_zzb_msk[i])));
        unsigned zero = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(v, z));
        msk |= (uint64_t)(~zero & 0xffffu) << (16 * k);
    }
    *omsk = msk;
    *obig = !_mm_testz_si128(big, big);
}

void y264_zigzag_scan_4x4_sse4(dctcoef out[16], const dctcoef in[16],
                               uint32_t *omsk, int *obig)
{
    __m128i row[2], big = _mm_setzero_si128();
    unsigned m[2];
    row[0] = y264_ldc8(in + 0);
    row[1] = y264_ldc8(in + 8);
    for (int k = 0; k < 2; k++) {
        __m128i v = _mm_setzero_si128();
        for (int i = zz4_off[k]; i < zz4_off[k + 1]; i++)
            v = _mm_or_si128(v, _mm_shuffle_epi8(row[zz4_src[i]],
                    _mm_loadu_si128((const __m128i *)(const void *)zz4_msk[i])));
        y264_stc8(out + 8 * k, v);
        m[k] = y264_nz_mask8(v, &big);
    }
    *omsk = m[0] | (m[1] << 8);
    *obig = _mm_movemask_epi8(big) != 0;
}

#endif /* Y264_HAVE_SSE4 */
