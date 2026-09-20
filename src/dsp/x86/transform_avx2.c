/*
 * transform_avx2.c - AVX2 kernels for the transform, quant and scan families
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Wave 3a of docs/x86-plan.md, the wide half. Same kernels as the SSE4.2 file
 * and the same values out of them, bit for bit: the two tiers are an A/B axis
 * for speed and never for output, which is what the kit's identity leg exists
 * to hold them to.
 *
 * WHERE A SECOND LANE IS WORTH SOMETHING HERE, AND WHERE IT IS WORTH NOTHING.
 *
 * This family is not the pixel family. There the wide register always had a
 * second block to hold; here the question is answered per kernel by the LANE
 * WIDTH the exactness claim forces (transform_x86.h):
 *
 *   - THE 32-BIT INVERSES ARE WHERE AVX2 PAYS. An 8x8 inverse in 32-bit lanes
 *     wants sixteen 128-bit registers and spills on a machine that has
 *     sixteen; in 256-bit registers a whole row of eight int32 is ONE
 *     register, the transform fits in eight of them with room to work, and the
 *     8x8 transpose loses its four-tile bookkeeping for eight in-lane unpacks
 *     and eight lane crossings.
 *   - THE BATCHED FORWARD goes from two 4x4 blocks per pass to four, because
 *     every step of the dual transpose pairs lanes inside a 128-bit half and
 *     AVX2's unpacks are two independent halves: the same source, twice the
 *     blocks, and a macroblock row of a 16x16 grid is exactly four.
 *   - QUANT, DEQUANT AND THE SCAN are elementwise or permutation streams and
 *     double their width with nothing else to decide. The scan's one change is
 *     that a 256-bit shuffle does TWO scan groups per source row, one per
 *     lane, which is why its tables are the SSE4.2 masks of a group PAIR laid
 *     end to end.
 *   - THE 16-BIT FORWARDS ARE WHERE IT IS NOT. A lone 4x4 or 8x8 forward in
 *     16-bit lanes already fits the narrow register, and a wide one would move
 *     zeros through its top lane and pay a vzeroupper for it. Those call the
 *     shared 128-bit core, which compiled here is VEX-encoded and gets the
 *     three-operand form and the free upper-zeroing -- the whole of what AVX2
 *     has to give that shape.
 *
 * C11 intrinsics only. There is no assembly in this tree, and hygiene_check
 * refuses any inline form under this directory.
 */
#include "dsp/x86/transform_x86.h"

#if Y264_HAVE_AVX2

/* ---- 256-bit plumbing ----------------------------------------------------- */

#define Y264_SRA32_1_256(x) _mm256_srai_epi32(x, 1)
#define Y264_SRA32_2_256(x) _mm256_srai_epi32(x, 2)

static inline __m256i y256_ldc16(const dctcoef *p)
{
    return _mm256_loadu_si256((const __m256i *)(const void *)p);
}

static inline void y256_stc16(dctcoef *p, __m256i v)
{
    _mm256_storeu_si256((__m256i *)(void *)p, v);
}

/* One row of eight int32 back to eight int16, by TRUNCATION (see the note on
 * y264_narrow_s32): the two halves narrow through the 128-bit shuffle and meet
 * in one register. */
static inline __m128i y256_narrow_s32(__m256i v)
{
    return _mm_unpacklo_epi64(y264_narrow_s32(_mm256_castsi256_si128(v)),
                              y264_narrow_s32(_mm256_extracti128_si256(v, 1)));
}

/* 8x8 of int32, eight registers: eight in-lane unpacks to get the columns of
 * each 128-bit half, then eight lane crossings to join the halves. */
static inline void y256_trn8_s32(__m256i v[8])
{
    __m256i a0 = _mm256_unpacklo_epi32(v[0], v[1]), a1 = _mm256_unpackhi_epi32(v[0], v[1]);
    __m256i a2 = _mm256_unpacklo_epi32(v[2], v[3]), a3 = _mm256_unpackhi_epi32(v[2], v[3]);
    __m256i a4 = _mm256_unpacklo_epi32(v[4], v[5]), a5 = _mm256_unpackhi_epi32(v[4], v[5]);
    __m256i a6 = _mm256_unpacklo_epi32(v[6], v[7]), a7 = _mm256_unpackhi_epi32(v[6], v[7]);
    __m256i b0 = _mm256_unpacklo_epi64(a0, a2), b1 = _mm256_unpackhi_epi64(a0, a2);
    __m256i b2 = _mm256_unpacklo_epi64(a1, a3), b3 = _mm256_unpackhi_epi64(a1, a3);
    __m256i b4 = _mm256_unpacklo_epi64(a4, a6), b5 = _mm256_unpackhi_epi64(a4, a6);
    __m256i b6 = _mm256_unpacklo_epi64(a5, a7), b7 = _mm256_unpackhi_epi64(a5, a7);
    v[0] = _mm256_permute2x128_si256(b0, b4, 0x20);
    v[4] = _mm256_permute2x128_si256(b0, b4, 0x31);
    v[1] = _mm256_permute2x128_si256(b1, b5, 0x20);
    v[5] = _mm256_permute2x128_si256(b1, b5, 0x31);
    v[2] = _mm256_permute2x128_si256(b2, b6, 0x20);
    v[6] = _mm256_permute2x128_si256(b2, b6, 0x31);
    v[3] = _mm256_permute2x128_si256(b3, b7, 0x20);
    v[7] = _mm256_permute2x128_si256(b3, b7, 0x31);
}

/* ---- the coefficient-domain transforms ----------------------------------- */

void y264_fdct4x4_avx2(const dctcoef diff[16], dctcoef coef[16])
{
    y264_fdct4x4_core(diff, coef);
}

void y264_fdct8x8_avx2(const dctcoef diff[64], dctcoef coef[64])
{
    y264_fdct8x8_core(diff, coef);
}

void y264_idct4x4_avx2(const dctcoef coef[16], dctcoef res[16])
{
    y264_idct4x4_core(coef, res);
}

/* The 8x8 inverse, one row of int32 per register. Transpose, 1-D, transpose,
 * 1-D, then the (x + 32) >> 6 the reference folds into its second pass. */
static inline void y256_idct8_core(const dctcoef coef[64], __m256i v[8])
{
    for (int i = 0; i < 8; i++)
        v[i] = _mm256_cvtepi16_epi32(y264_ldc8(coef + 8 * i));
    for (int pass = 0; pass < 2; pass++) {
        y256_trn8_s32(v);
        Y264_IDCT8_1D(__m256i, _mm256_add_epi32, _mm256_sub_epi32,
                      Y264_SRA32_1_256, Y264_SRA32_2_256,
                      v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
    }
    const __m256i r32 = _mm256_set1_epi32(32);
    for (int i = 0; i < 8; i++)
        v[i] = _mm256_srai_epi32(_mm256_add_epi32(v[i], r32), 6);
}

void y264_idct8x8_avx2(const dctcoef coef[64], dctcoef res[64])
{
    __m256i v[8];
    y256_idct8_core(coef, v);
    for (int i = 0; i < 8; i++)
        y264_stc8(res + 8 * i, y256_narrow_s32(v[i]));
}

/* ---- the fused pixel-domain transforms ----------------------------------- */

void y264_sub4x4_dct_avx2(dctcoef coef[16], const pixel *src, int ss,
                          const pixel *pred, int ps)
{
    y264_sub4x4_dct_core(coef, src, ss, pred, ps);
}

void y264_sub8x8_dct8_avx2(dctcoef coef[64], const pixel *src, int ss,
                           const pixel *pred, int ps)
{
    y264_sub8x8_dct8_core(coef, src, ss, pred, ps);
}

void y264_add4x4_idct_avx2(pixel *dst, int ds, const pixel *pred, int ps,
                           const dctcoef coef[16])
{
    y264_add4x4_idct_core(dst, ds, pred, ps, coef);
}

void y264_add8x8_idct8_avx2(pixel *dst, int ds, const pixel *pred, int ps,
                            const dctcoef coef[64])
{
    __m256i v[8];
    __m128i z = _mm_setzero_si128();
    y256_idct8_core(coef, v);
    for (int i = 0; i < 8; i++) {
        __m128i res = y256_narrow_s32(v[i]);
        __m128i p = _mm_unpacklo_epi8(y264_ldp8(pred + i * ps), z);
        y264_stp8(dst + i * ds, _mm_packus_epi16(_mm_adds_epi16(res, p), z));
    }
}

/* FOUR horizontally adjacent 4x4 forward transforms in one pass: two per
 * 128-bit lane, since every step of the dual transpose pairs lanes inside a
 * half. A grid row of the 16x16 luma macroblock is exactly four blocks; the
 * 4:2:0 and 4:2:2 chroma grids are two wide and take the 128-bit dual. */
static inline void y256_fdct4_quad(__m256i r0, __m256i r1, __m256i r2, __m256i r3,
                                   dctcoef (*coef)[16])
{
    Y264_TRN4X2_S16(__m256i, _mm256_unpacklo_epi16, _mm256_unpackhi_epi16,
                    _mm256_unpacklo_epi32, _mm256_unpackhi_epi32,
                    _mm256_unpacklo_epi64, _mm256_unpackhi_epi64, r0, r1, r2, r3);
    Y264_FDCT4_1D(__m256i, _mm256_add_epi16, _mm256_sub_epi16, r0, r1, r2, r3);
    Y264_TRN4X2_S16(__m256i, _mm256_unpacklo_epi16, _mm256_unpackhi_epi16,
                    _mm256_unpacklo_epi32, _mm256_unpackhi_epi32,
                    _mm256_unpacklo_epi64, _mm256_unpackhi_epi64, r0, r1, r2, r3);
    Y264_FDCT4_1D(__m256i, _mm256_add_epi16, _mm256_sub_epi16, r0, r1, r2, r3);
    /* Each register holds row k of all four blocks. A block's sixteen
     * coefficients are contiguous in memory, so two rows meet in one 128-bit
     * store rather than four 64-bit ones. */
    for (int half = 0; half < 2; half++) {
        __m128i h0 = half ? _mm256_extracti128_si256(r0, 1) : _mm256_castsi256_si128(r0);
        __m128i h1 = half ? _mm256_extracti128_si256(r1, 1) : _mm256_castsi256_si128(r1);
        __m128i h2 = half ? _mm256_extracti128_si256(r2, 1) : _mm256_castsi256_si128(r2);
        __m128i h3 = half ? _mm256_extracti128_si256(r3, 1) : _mm256_castsi256_si128(r3);
        y264_stc8(coef[2 * half + 0] + 0, _mm_unpacklo_epi64(h0, h1));
        y264_stc8(coef[2 * half + 0] + 8, _mm_unpacklo_epi64(h2, h3));
        y264_stc8(coef[2 * half + 1] + 0, _mm_unpackhi_epi64(h0, h1));
        y264_stc8(coef[2 * half + 1] + 8, _mm_unpackhi_epi64(h2, h3));
    }
}

static inline __m256i y256_diff16(const pixel *s, const pixel *p)
{
    __m128i zs = _mm_loadu_si128((const __m128i *)(const void *)s);
    __m128i zp = _mm_loadu_si128((const __m128i *)(const void *)p);
    return _mm256_sub_epi16(_mm256_cvtepu8_epi16(zs), _mm256_cvtepu8_epi16(zp));
}

void y264_sub_dct4_blocks_avx2(dctcoef (*coef)[16], int nbw, int nbh,
                               const pixel *src, int ss,
                               const pixel *pred, int ps)
{
    for (int by = 0; by < nbh; by++) {
        const pixel *s = src + (by * 4) * ss;
        const pixel *p = pred + (by * 4) * ps;
        int bx = 0;
        for (; bx + 4 <= nbw; bx += 4)
            y256_fdct4_quad(y256_diff16(s + bx * 4, p + bx * 4),
                            y256_diff16(s + ss + bx * 4, p + ps + bx * 4),
                            y256_diff16(s + 2 * ss + bx * 4, p + 2 * ps + bx * 4),
                            y256_diff16(s + 3 * ss + bx * 4, p + 3 * ps + bx * 4),
                            &coef[by * nbw + bx]);
        for (; bx < nbw; bx += 2)
            y264_fdct4_dual(y264_diff8(s + bx * 4, p + bx * 4),
                            y264_diff8(s + ss + bx * 4, p + ps + bx * 4),
                            y264_diff8(s + 2 * ss + bx * 4, p + 2 * ps + bx * 4),
                            y264_diff8(s + 3 * ss + bx * 4, p + 3 * ps + bx * 4),
                            coef[by * nbw + bx], coef[by * nbw + bx + 1]);
    }
}

/* ---- quant and dequant ---------------------------------------------------- */

#define Y264_MFLD256(row, g) \
    _mm256_loadu_si256((const __m256i *)(const void *)((row) + (g)))

/* Sixteen coefficients per iteration: two registers of eight int32 in, one of
 * sixteen int16 out. See y264_quant4 for why the sign is a blend. */
static inline __m256i y256_quant8(__m256i c, __m256i mf, __m256i f, __m128i sh)
{
    __m256i z = _mm256_setzero_si256();
    __m256i q = _mm256_sra_epi32(
        _mm256_add_epi32(_mm256_mullo_epi32(_mm256_abs_epi32(c), mf), f), sh);
    return _mm256_blendv_epi8(q, _mm256_sub_epi32(z, q), _mm256_cmpgt_epi32(z, c));
}

/* The 256-bit pack is two independent halves, so the sixteen results come out
 * as 0-3, 8-11, 4-7, 12-15 and one lane-crossing permute puts them back. */
static inline __m256i y256_pack16(__m256i lo, __m256i hi)
{
    return _mm256_permute4x64_epi64(_mm256_packs_epi32(lo, hi), 0xd8);
}

#define Y264_QUANT_AVX2(name, n, qbase)                                       \
void name(const dctcoef coef[n], dctcoef lev[n], int qp, int f,               \
          const int32_t mfrow[n])                                             \
{                                                                             \
    __m256i vf = _mm256_set1_epi32(f);                                        \
    __m128i sh = _mm_cvtsi32_si128((qbase) + qp / 6);                         \
    for (int g = 0; g < (n); g += 16) {                                       \
        __m256i c = y256_ldc16(coef + g);                                     \
        __m256i q0 = y256_quant8(_mm256_cvtepi16_epi32(                       \
            _mm256_castsi256_si128(c)), Y264_MFLD256(mfrow, g), vf, sh);      \
        __m256i q1 = y256_quant8(_mm256_cvtepi16_epi32(                       \
            _mm256_extracti128_si256(c, 1)), Y264_MFLD256(mfrow, g + 8), vf, sh);\
        y256_stc16(lev + g, y256_pack16(q0, q1));                             \
    }                                                                         \
}

Y264_QUANT_AVX2(y264_quant_4x4_f_avx2, 16, 15)
Y264_QUANT_AVX2(y264_quant_8x8_f_avx2, 64, 16)

void y264_quant_4x4_avx2(const dctcoef coef[16], dctcoef lev[16], int qp, int intra,
                         const int32_t mfrow[16])
{
    int qbits = 15 + qp / 6;
    y264_quant_4x4_f_avx2(coef, lev, qp, (1 << qbits) / (intra ? 3 : 6), mfrow);
}

#define Y264_DEQUANT_AVX2(name, n, big, rndbits)                              \
void name(const dctcoef lev[n], dctcoef coef[n], int qp,                      \
          const int32_t lsrow[n])                                             \
{                                                                             \
    int shift = qp / 6;                                                       \
    if (shift >= (big)) {                                                     \
        __m128i vs = _mm_cvtsi32_si128(shift - (big));                        \
        for (int g = 0; g < (n); g += 16) {                                   \
            __m256i l = y256_ldc16(lev + g);                                  \
            __m256i d0 = _mm256_sll_epi32(_mm256_mullo_epi32(                 \
                _mm256_cvtepi16_epi32(_mm256_castsi256_si128(l)),             \
                Y264_MFLD256(lsrow, g)), vs);                                 \
            __m256i d1 = _mm256_sll_epi32(_mm256_mullo_epi32(                 \
                _mm256_cvtepi16_epi32(_mm256_extracti128_si256(l, 1)),        \
                Y264_MFLD256(lsrow, g + 8)), vs);                             \
            y256_stc16(coef + g, y256_pack16(d0, d1));                        \
        }                                                                     \
    } else {                                                                  \
        __m256i vr = _mm256_set1_epi32(1 << ((rndbits) - shift));             \
        __m128i vs = _mm_cvtsi32_si128((big) - shift);                        \
        for (int g = 0; g < (n); g += 16) {                                   \
            __m256i l = y256_ldc16(lev + g);                                  \
            __m256i d0 = _mm256_sra_epi32(_mm256_add_epi32(_mm256_mullo_epi32(\
                _mm256_cvtepi16_epi32(_mm256_castsi256_si128(l)),             \
                Y264_MFLD256(lsrow, g)), vr), vs);                            \
            __m256i d1 = _mm256_sra_epi32(_mm256_add_epi32(_mm256_mullo_epi32(\
                _mm256_cvtepi16_epi32(_mm256_extracti128_si256(l, 1)),        \
                Y264_MFLD256(lsrow, g + 8)), vr), vs);                        \
            y256_stc16(coef + g, y256_pack16(d0, d1));                        \
        }                                                                     \
    }                                                                         \
}

Y264_DEQUANT_AVX2(y264_dequant_4x4_avx2, 16, 4, 3)
Y264_DEQUANT_AVX2(y264_dequant_8x8_avx2, 64, 6, 5)

/* ---- the zig-zag scan -----------------------------------------------------
 *
 * The same tables as the SSE4.2 file, with the two groups of a PAIR laid end
 * to end: a 256-bit shuffle is two independent 128-bit shuffles, so one
 * broadcast source row feeds scan group 2p in the low lane and 2p+1 in the
 * high one. That is also why the row list is the UNION of the pair's rows --
 * five to seven of the eight, against three to six per single group, for half
 * as many shuffles over the block.
 */
static const uint8_t zz8_src2[24] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x03, 0x04, 0x05, 0x06, 0x07
};
static const uint8_t zz8_off2[5] = {
    0x00, 0x05, 0x0c, 0x13, 0x18
};
static const uint8_t zz8_msk2[24][32] = {
    {
      0x00, 0x01, 0x02, 0x03, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x04, 0x05,
      0x06, 0x07, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80, 0x08, 0x09, 0x0a, 0x0b
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x00, 0x01, 0x80, 0x80, 0x02, 0x03, 0x80, 0x80,
      0x80, 0x80, 0x04, 0x05, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x06, 0x07, 0x80, 0x80, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x01, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80, 0x02, 0x03, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x04, 0x05, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x01, 0x80, 0x80, 0x02, 0x03,
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x01, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x0c, 0x0d,
      0x0e, 0x0f, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80
    },
    {
      0x08, 0x09, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x0a, 0x0b, 0x80, 0x80,
      0x80, 0x80, 0x0c, 0x0d, 0x80, 0x80, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x06, 0x07, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x08, 0x09, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80, 0x0a, 0x0b, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x04, 0x05, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80, 0x06, 0x07, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x08, 0x09
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x02, 0x03, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x04, 0x05, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x01, 0x80, 0x80,
      0x02, 0x03, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x01,
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x0e, 0x0f, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x0c, 0x0d, 0x80, 0x80, 0x0e, 0x0f,
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80, 0x0a, 0x0b, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x0c, 0x0d, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80
    },
    {
      0x06, 0x07, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x08, 0x09, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x0a, 0x0b, 0x80, 0x80, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x04, 0x05, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x06, 0x07, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80, 0x08, 0x09, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x02, 0x03, 0x80, 0x80, 0x80, 0x80, 0x04, 0x05,
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x06, 0x07
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x01, 0x02, 0x03, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x0e, 0x0f,
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x0c, 0x0d, 0x80, 0x80,
      0x0e, 0x0f, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x0a, 0x0b, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x0c, 0x0d, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x0e, 0x0f, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x08, 0x09, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80, 0x0a, 0x0b, 0x80, 0x80, 0x80, 0x80, 0x0c, 0x0d,
      0x80, 0x80, 0x0e, 0x0f, 0x80, 0x80, 0x80, 0x80
    },
    {
      0x04, 0x05, 0x06, 0x07, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x08, 0x09, 0x0a, 0x0b, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80, 0x0c, 0x0d, 0x0e, 0x0f
    },
};

static const uint8_t zz4_src2[2] = {
    0x00, 0x01
};
static const uint8_t zz4_off2[2] = {
    0x00, 0x02
};
static const uint8_t zz4_msk2[2][32] = {
    {
      0x00, 0x01, 0x02, 0x03, 0x08, 0x09, 0x80, 0x80, 0x0a, 0x0b, 0x04, 0x05,
      0x06, 0x07, 0x0c, 0x0d, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
      0x0e, 0x0f, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80
    },
    {
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x01, 0x80, 0x80, 0x80, 0x80,
      0x80, 0x80, 0x80, 0x80, 0x02, 0x03, 0x08, 0x09, 0x0a, 0x0b, 0x04, 0x05,
      0x80, 0x80, 0x06, 0x07, 0x0c, 0x0d, 0x0e, 0x0f
    },
};

static inline void y256_zz8_gather(const dctcoef in[64], __m256i g[4])
{
    __m256i row[8];
    for (int i = 0; i < 8; i++)
        row[i] = _mm256_broadcastsi128_si256(y264_ldc8(in + 8 * i));
    for (int p = 0; p < 4; p++) {
        __m256i v = _mm256_setzero_si256();
        for (int i = zz8_off2[p]; i < zz8_off2[p + 1]; i++)
            v = _mm256_or_si256(v, _mm256_shuffle_epi8(row[zz8_src2[i]],
                    _mm256_loadu_si256((const __m256i *)(const void *)zz8_msk2[i])));
        g[p] = v;
    }
}

/* Widen BEFORE the abs, for the -32768 corner (see the SSE4.2 twin). Each
 * lane of the pair is a whole scan group, so each becomes one register of
 * eight int32. */
static inline void y256_st_abs_s32(int *out, __m256i v)
{
    _mm256_storeu_si256((__m256i *)(void *)(out + 0),
        _mm256_abs_epi32(_mm256_cvtepi16_epi32(_mm256_castsi256_si128(v))));
    _mm256_storeu_si256((__m256i *)(void *)(out + 8),
        _mm256_abs_epi32(_mm256_cvtepi16_epi32(_mm256_extracti128_si256(v, 1))));
}

void y264_zigzag_abs_8x8_avx2(int out[64], const dctcoef in[64])
{
    __m256i g[4];
    y256_zz8_gather(in, g);
    for (int p = 0; p < 4; p++)
        y256_st_abs_s32(out + 16 * p, g[p]);
}

/* The nonzero mask of both groups of a pair at once. The 256-bit pack is two
 * independent halves, so the low lane's eight bytes land in movemask bits 0-7
 * and the high lane's in bits 16-23 -- which is one bit per coefficient of
 * each group, in scan order, with no further shuffling. */
static inline unsigned y256_nz_mask16(__m256i v, __m256i *bigacc)
{
    __m256i z = _mm256_setzero_si256();
    unsigned eq = (unsigned)_mm256_movemask_epi8(_mm256_packs_epi16(_mm256_cmpeq_epi16(v, z), z));
    *bigacc = _mm256_or_si256(*bigacc,
                  _mm256_or_si256(_mm256_cmpgt_epi16(v, _mm256_set1_epi16(1)),
                                  _mm256_cmpgt_epi16(_mm256_set1_epi16(-1), v)));
    return (~eq & 0xffu) | ((~(eq >> 16) & 0xffu) << 8);
}

void y264_scan_mask_8x8_avx2(const dctcoef lev[64], uint64_t *omsk, int *obig)
{
    __m256i g[4], big = _mm256_setzero_si256();
    uint64_t msk = 0;
    y256_zz8_gather(lev, g);
    for (int p = 0; p < 4; p++)
        msk |= (uint64_t)y256_nz_mask16(g[p], &big) << (16 * p);
    *omsk = msk;
    *obig = _mm256_movemask_epi8(big) != 0;
}

void y264_zigzag_scan_4x4_avx2(dctcoef out[16], const dctcoef in[16],
                               uint32_t *omsk, int *obig)
{
    __m256i big = _mm256_setzero_si256();
    __m256i row[2], v = _mm256_setzero_si256();
    row[0] = _mm256_broadcastsi128_si256(y264_ldc8(in + 0));
    row[1] = _mm256_broadcastsi128_si256(y264_ldc8(in + 8));
    for (int i = zz4_off2[0]; i < zz4_off2[1]; i++)
        v = _mm256_or_si256(v, _mm256_shuffle_epi8(row[zz4_src2[i]],
                _mm256_loadu_si256((const __m256i *)(const void *)zz4_msk2[i])));
    y256_stc16(out, v);
    *omsk = y256_nz_mask16(v, &big);
    *obig = _mm256_movemask_epi8(big) != 0;
}

#endif /* Y264_HAVE_AVX2 */
