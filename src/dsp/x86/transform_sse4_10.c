/*
 * transform_sse4_10.c - SSE4.2 transform, quant and scan kernels for the
 * 10-bit library
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * each kernel is a transform_x86_10.h body under its SSE4.2 name. the names
 * are the 8-bit tier's; src/common/symbols10.h adds the _10 suffix, so arch.h
 * keeps one prototype set and the dispatch sites are shared.
 */
#include "dsp/x86/transform_x86_10.h"

#if Y264_HAVE_SSE4_10

void y264_fdct4x4_sse4(const dctcoef diff[16], dctcoef coef[16])
{ t10_fdct4x4(diff, coef); }

void y264_idct4x4_sse4(const dctcoef coef[16], dctcoef res[16])
{ t10_idct4x4(coef, res); }

void y264_fdct8x8_sse4(const dctcoef diff[64], dctcoef coef[64])
{ t10_fdct8x8(diff, coef); }

void y264_idct8x8_sse4(const dctcoef coef[64], dctcoef res[64])
{ t10_idct8x8(coef, res); }

void y264_sub4x4_dct_sse4(dctcoef coef[16], const pixel *src, int ss,
                          const pixel *pred, int ps)
{ t10_sub4x4_dct(coef, src, ss, pred, ps); }

void y264_add4x4_idct_sse4(pixel *dst, int ds, const pixel *pred, int ps,
                           const dctcoef coef[16])
{ t10_add4x4_idct(dst, ds, pred, ps, coef); }

void y264_sub8x8_dct8_sse4(dctcoef coef[64], const pixel *src, int ss,
                           const pixel *pred, int ps)
{ t10_sub8x8_dct8(coef, src, ss, pred, ps); }

void y264_add8x8_idct8_sse4(pixel *dst, int ds, const pixel *pred, int ps,
                            const dctcoef coef[64])
{ t10_add8x8_idct8(dst, ds, pred, ps, coef); }

void y264_sub_dct4_blocks_sse4(dctcoef (*coef)[16], int nbw, int nbh,
                               const pixel *src, int ss,
                               const pixel *pred, int ps)
{
    for (int by = 0; by < nbh; by++, src += 4 * ss, pred += 4 * ps)
        for (int bx = 0; bx < nbw; bx++, coef++)
            t10_sub4x4_dct(coef[0], src + 4 * bx, ss, pred + 4 * bx, ps);
}

void y264_sub_dct8_blocks_sse4(dctcoef (*coef)[64], int nbw, int nbh,
                               const pixel *src, int ss,
                               const pixel *pred, int ps)
{
    for (int by = 0; by < nbh; by++, src += 8 * ss, pred += 8 * ps)
        for (int bx = 0; bx < nbw; bx++, coef++)
            t10_sub8x8_dct8(coef[0], src + 8 * bx, ss, pred + 8 * bx, ps);
}

void y264_quant_4x4_f_sse4(const dctcoef coef[16], dctcoef lev[16], int qp, int f,
                           const int32_t mfrow[16])
{ t10_quant_n(coef, lev, 16, f, 15 + qp / 6, mfrow); }

void y264_quant_4x4_sse4(const dctcoef coef[16], dctcoef lev[16], int qp, int intra,
                         const int32_t mfrow[16])
{
    int qbits = 15 + qp / 6;
    t10_quant_n(coef, lev, 16, (1 << qbits) / (intra ? 3 : 6), qbits, mfrow);
}

void y264_quant_8x8_f_sse4(const dctcoef coef[64], dctcoef lev[64], int qp, int f,
                           const int32_t mfrow[64])
{ t10_quant_n(coef, lev, 64, f, 16 + qp / 6, mfrow); }

void y264_quant_4x4_f_n_sse4(const dctcoef *coef, dctcoef *lev, int n, int qp,
                             int f, const int32_t mfrow[16])
{
    for (int k = 0; k < n; k++)
        t10_quant_n(coef + 16 * k, lev + 16 * k, 16, f, 15 + qp / 6, mfrow);
}

void y264_quant_8x8_f_n_sse4(const dctcoef *coef, dctcoef *lev, int n, int qp,
                             int f, const int32_t mfrow[64])
{
    for (int k = 0; k < n; k++)
        t10_quant_n(coef + 64 * k, lev + 64 * k, 64, f, 16 + qp / 6, mfrow);
}

int y264_quant_4x4_ac_f_sse4(const dctcoef *coef, dctcoef *lev, int n, int qp,
                             int f, const int32_t mfrow[16], dctcoef *dc, int *nz)
{
    int any = 0;
    for (int k = 0; k < n; k++, coef += 16, lev += 16) {
        t10_quant_n(coef, lev, 16, f, 15 + qp / 6, mfrow);
        any |= t10_quant_ac_tail(coef, lev, &dc[k], &nz[k]);
    }
    return any;
}

void y264_dequant_4x4_sse4(const dctcoef lev[16], dctcoef coef[16], int qp,
                           const int32_t lsrow[16])
{ t10_dequant(lev, coef, 16, qp, lsrow, 4); }

void y264_dequant_8x8_sse4(const dctcoef lev[64], dctcoef coef[64], int qp,
                           const int32_t lsrow[64])
{ t10_dequant(lev, coef, 64, qp, lsrow, 6); }

void y264_zigzag_abs_4x4_sse4(int out[16], const dctcoef in[16])
{ t10_zigzag_abs_4x4(out, in); }

/* sixteen source registers of four int32 (raster 4j to 4j + 3), sixteen scan
 * groups of four, each the OR of byte shuffles of the registers it draws on.
 * abs comes after the gather, on the int the reference reads. controls come
 * from y264_zigzag8, like the scan mask's. */
static const uint8_t t10_zz8_src[52] = {
     0,  2,  4,  0,  2,  4,  6,  8,  1,  2,  4,  3,  4,  6,  8,  8, 10, 12,
     1,  3,  5,  6,  1,  3,  5,  7,  8, 10, 12, 14,  9, 10, 12, 14,  3,  5,
     7,  7,  9, 11, 12, 11, 13, 14,  7,  9, 11, 13, 15, 11, 13, 15
};
static const uint8_t t10_zz8_off[17] = {
    0, 3, 5, 8, 11, 15, 18, 22, 26, 30, 34, 37, 41, 44, 47, 49, 52
};
#define T10_L(x) 4 * (x), 4 * (x) + 1, 4 * (x) + 2, 4 * (x) + 3
#define T10_N -1, -1, -1, -1
static const int8_t t10_zz8_msk[52][16] = {
    { T10_L(0), T10_L(1), T10_N, T10_N }, { T10_N, T10_N, T10_L(0), T10_N },
    { T10_N, T10_N, T10_N, T10_L(0) },
    { T10_N, T10_L(2), T10_L(3), T10_N }, { T10_L(1), T10_N, T10_N, T10_L(2) },
    { T10_L(1), T10_N, T10_N, T10_N }, { T10_N, T10_L(0), T10_N, T10_L(1) },
    { T10_N, T10_N, T10_L(0), T10_N },
    { T10_N, T10_N, T10_L(0), T10_L(1) }, { T10_N, T10_L(3), T10_N, T10_N },
    { T10_L(2), T10_N, T10_N, T10_N },
    { T10_L(0), T10_N, T10_N, T10_N }, { T10_N, T10_L(3), T10_N, T10_N },
    { T10_N, T10_N, T10_L(2), T10_N }, { T10_N, T10_N, T10_N, T10_L(1) },
    { T10_N, T10_N, T10_N, T10_L(2) }, { T10_L(0), T10_N, T10_L(1), T10_N },
    { T10_N, T10_L(0), T10_N, T10_N },
    { T10_N, T10_N, T10_N, T10_L(2) }, { T10_N, T10_N, T10_L(1), T10_N },
    { T10_N, T10_L(0), T10_N, T10_N }, { T10_L(3), T10_N, T10_N, T10_N },
    { T10_L(3), T10_N, T10_N, T10_N }, { T10_N, T10_L(2), T10_N, T10_N },
    { T10_N, T10_N, T10_L(1), T10_N }, { T10_N, T10_N, T10_N, T10_L(0) },
    { T10_L(3), T10_N, T10_N, T10_N }, { T10_N, T10_L(2), T10_N, T10_N },
    { T10_N, T10_N, T10_L(1), T10_N }, { T10_N, T10_N, T10_N, T10_L(0) },
    { T10_N, T10_N, T10_N, T10_L(0) }, { T10_N, T10_N, T10_L(3), T10_N },
    { T10_N, T10_L(2), T10_N, T10_N }, { T10_L(1), T10_N, T10_N, T10_N },
    { T10_N, T10_N, T10_L(3), T10_N }, { T10_N, T10_L(2), T10_N, T10_L(3) },
    { T10_L(1), T10_N, T10_N, T10_N },
    { T10_L(2), T10_N, T10_N, T10_N }, { T10_N, T10_L(1), T10_N, T10_N },
    { T10_N, T10_N, T10_L(0), T10_N }, { T10_N, T10_N, T10_N, T10_L(3) },
    { T10_N, T10_N, T10_N, T10_L(1) }, { T10_N, T10_N, T10_L(0), T10_N },
    { T10_L(2), T10_L(3), T10_N, T10_N },
    { T10_N, T10_L(3), T10_N, T10_N }, { T10_L(2), T10_N, T10_L(3), T10_N },
    { T10_N, T10_N, T10_N, T10_L(2) },
    { T10_L(1), T10_N, T10_N, T10_L(2) }, { T10_N, T10_L(0), T10_L(1), T10_N },
    { T10_L(3), T10_N, T10_N, T10_N }, { T10_N, T10_L(3), T10_N, T10_N },
    { T10_N, T10_N, T10_L(2), T10_L(3) },
};
#undef T10_L
#undef T10_N

void y264_zigzag_abs_8x8_sse4(int out[64], const dctcoef in[64])
{
    __m128i r[16];
    for (int j = 0; j < 16; j++)
        r[j] = t10_ldc(in + 4 * j);
    for (int k = 0; k < 16; k++) {
        __m128i v = _mm_setzero_si128();
        for (int i = t10_zz8_off[k]; i < t10_zz8_off[k + 1]; i++)
            v = _mm_or_si128(v, _mm_shuffle_epi8(r[t10_zz8_src[i]],
                    _mm_loadu_si128((const __m128i *)(const void *)t10_zz8_msk[i])));
        t10_stc(out + 4 * k, _mm_abs_epi32(v));
    }
}

void y264_scan_mask_8x8_sse4(const dctcoef lev[64], uint64_t *omsk, int *obig)
{ t10_scan_mask_8x8(lev, omsk, obig); }

void y264_zigzag_scan_4x4_sse4(dctcoef out[16], const dctcoef in[16],
                               uint32_t *omsk, int *obig)
{ t10_zigzag_scan_4x4(out, in, omsk, obig); }

#endif /* Y264_HAVE_SSE4_10 */
