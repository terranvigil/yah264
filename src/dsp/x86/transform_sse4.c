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
 *     the caller owns, so the kernel is four lanes at a time with no shuffle
 *     anywhere. The whole art is the sign: the reference tests the
 *     COEFFICIENT's sign and not the quotient's, so a zero coefficient takes
 *     the positive branch even where the rounding bias alone would round up to
 *     one, and the restore is a compare plus a blend rather than a
 *     sign-multiply, which would answer zero there.
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
    for (int by = 0; by < nbh; by++) {
        const pixel *s = src + (by * 4) * ss;
        const pixel *p = pred + (by * 4) * ps;
        for (int bx = 0; bx < nbw; bx += 2)
            y264_fdct4_dual(y264_diff8(s + bx * 4, p + bx * 4),
                            y264_diff8(s + ss + bx * 4, p + ps + bx * 4),
                            y264_diff8(s + 2 * ss + bx * 4, p + 2 * ps + bx * 4),
                            y264_diff8(s + 3 * ss + bx * 4, p + 3 * ps + bx * 4),
                            coef[by * nbw + bx], coef[by * nbw + bx + 1]);
    }
}

/* ---- quant and dequant --------------------------------------------------- */

#define Y264_MFLD(row, g) \
    _mm_loadu_si128((const __m128i *)(const void *)((row) + (g)))

void y264_quant_4x4_f_sse4(const dctcoef coef[16], dctcoef lev[16], int qp, int f,
                           const int32_t mfrow[16])
{
    __m128i vf = _mm_set1_epi32(f);
    __m128i sh = _mm_cvtsi32_si128(15 + qp / 6);
    for (int g = 0; g < 16; g += 8) {
        __m128i c = y264_ldc8(coef + g);
        __m128i q0 = y264_quant4(_mm_cvtepi16_epi32(c), Y264_MFLD(mfrow, g), vf, sh);
        __m128i q1 = y264_quant4(_mm_cvtepi16_epi32(_mm_unpackhi_epi64(c, c)),
                                 Y264_MFLD(mfrow, g + 4), vf, sh);
        y264_stc8(lev + g, y264_quant_pack(q0, q1));
    }
}

void y264_quant_4x4_sse4(const dctcoef coef[16], dctcoef lev[16], int qp, int intra,
                         const int32_t mfrow[16])
{
    int qbits = 15 + qp / 6;
    y264_quant_4x4_f_sse4(coef, lev, qp, (1 << qbits) / (intra ? 3 : 6), mfrow);
}

void y264_quant_8x8_f_sse4(const dctcoef coef[64], dctcoef lev[64], int qp, int f,
                           const int32_t mfrow[64])
{
    __m128i vf = _mm_set1_epi32(f);
    __m128i sh = _mm_cvtsi32_si128(16 + qp / 6);
    for (int g = 0; g < 64; g += 8) {
        __m128i c = y264_ldc8(coef + g);
        __m128i q0 = y264_quant4(_mm_cvtepi16_epi32(c), Y264_MFLD(mfrow, g), vf, sh);
        __m128i q1 = y264_quant4(_mm_cvtepi16_epi32(_mm_unpackhi_epi64(c, c)),
                                 Y264_MFLD(mfrow, g + 4), vf, sh);
        y264_stc8(lev + g, y264_quant_pack(q0, q1));
    }
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

void y264_scan_mask_8x8_sse4(const dctcoef lev[64], uint64_t *omsk, int *obig)
{
    __m128i g[8], big = _mm_setzero_si128();
    uint64_t msk = 0;
    y264_zz8_gather(lev, g);
    for (int k = 0; k < 8; k++)
        msk |= (uint64_t)y264_nz_mask8(g[k], &big) << (8 * k);
    *omsk = msk;
    *obig = _mm_movemask_epi8(big) != 0;
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
