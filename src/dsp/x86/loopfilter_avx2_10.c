/*
 * loopfilter_avx2_10.c - AVX2 deblocking kernels for the 10-bit library
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * a row of sixteen 10-bit samples is one ymm, so a horizontal luma edge is
 * one pass where the SSE4.2 tier takes two, a vertical one transposes rows
 * 0-7 and 8-15 in the two halves at once, and a 4:2:0 chroma edge runs Cb in
 * the low half and Cr in the high. the strength derivation is the 8-bit
 * tier's sixteen-lane body (loopfilter_x86_core.h). the eight-line chroma
 * horizontal edge has nothing for an upper half, so it is the 128-bit body,
 * VEX-encoded.
 *
 * lane bounds are loopfilter_x86_10.h's: a 256-bit lane holds what a 128-bit
 * one does.
 */
#include "dsp/x86/loopfilter_x86_10.h"

#if Y264_HAVE_AVX2_10

Y264_DB_LUMA_LANES(y264_db_luma_lanes16, _mm256, __m256i)
Y264_DB_CHROMA_LANES(y264_db_chroma_lanes16, _mm256, __m256i)

/* lo in the low half */
static inline __m256i lf10_join(__m128i lo, __m128i hi)
{
    return _mm256_inserti128_si256(_mm256_castsi128_si256(lo), hi, 1);
}

void y264_deblock_strength_avx2(const struct y264_bs_ctx *c,
                                uint8_t bsv[4][4], uint8_t bsh[4][4])
{ y264_db_strength16_x86(c, bsv, bsh); }

void y264_deblock_chroma8_h_avx2(pixel *q0p, int stride, int alpha, int beta,
                                 const uint8_t bs[4], const uint8_t tc0tab[3],
                                 int span, int g)
{ lf10_chroma8_h(q0p, stride, alpha, beta, bs, tc0tab, span, g); }

/* each sample position is one sixteen-sample load */
void y264_deblock_luma_h16_avx2(pixel *q0p, int stride, int alpha, int beta,
                                const uint8_t bs[4], const uint8_t tc0tab[3])
{
    __m128i tc0b, bs4b, nzb;
    __m256i s[8], o[6];
    int has_n, has_4;
    y264_db_luma16_params(bs, tc0tab, &tc0b, &bs4b, &nzb);
    y264_db_luma16_need(bs, &has_n, &has_4);
    for (int k = 0; k < 8; k++)
        s[k] = _mm256_loadu_si256((const __m256i *)(const void *)(q0p + (k - 4) * stride));
    y264_db_luma_lanes16(s, alpha << Y264_DB_SH, beta << Y264_DB_SH,
                         _mm256_cvtepu8_epi16(tc0b), _mm256_cvtepi8_epi16(bs4b),
                         _mm256_cvtepi8_epi16(nzb), o, has_n, has_4);
    for (int k = 0; k < 6; k++)
        _mm256_storeu_si256((__m256i *)(void *)(q0p + (k - 3) * stride), o[k]);
}

/* rows 0-7 in the low halves, 8-15 in the high, so the in-half transpose
 * leaves each sample position as sixteen lanes in row order */
void y264_deblock_luma_v16_avx2(pixel *q0p, int stride, int alpha, int beta,
                                const uint8_t bs[4], const uint8_t tc0tab[3])
{
    pixel *b = q0p - 4;
    __m128i tc0b, bs4b, nzb;
    __m256i v[8], o[6];
    int has_n, has_4;
    y264_db_luma16_params(bs, tc0tab, &tc0b, &bs4b, &nzb);
    y264_db_luma16_need(bs, &has_n, &has_4);
    for (int k = 0; k < 8; k++)
        v[k] = lf10_join(lf10_ld8(b + k * stride), lf10_ld8(b + (k + 8) * stride));
    LF10_TRN8(_mm256, __m256i, v);
    y264_db_luma_lanes16(v, alpha << Y264_DB_SH, beta << Y264_DB_SH,
                         _mm256_cvtepu8_epi16(tc0b), _mm256_cvtepi8_epi16(bs4b),
                         _mm256_cvtepi8_epi16(nzb), o, has_n, has_4);
    for (int k = 0; k < 6; k++)
        v[k + 1] = o[k];
    LF10_TRN8(_mm256, __m256i, v);
    for (int k = 0; k < 8; k++) {
        lf10_st8(b + k * stride, _mm256_castsi256_si128(v[k]));
        lf10_st8(b + (k + 8) * stride, _mm256_extracti128_si256(v[k], 1));
    }
}

/* 4:2:0, both planes in one pass: Cb in the low half of every register, Cr
 * in the high */
void y264_deblock_chroma2_h_avx2(pixel *cbq0, pixel *crq0, int cbs, int crs,
                                 int alpha, int beta, const uint8_t bs[4],
                                 const uint8_t tc0tab[3])
{
    __m128i tcb, bs4b;
    __m256i po, qo;
    y264_db_chroma420_params(bs, tc0tab, &tcb, &bs4b);
#define LD2(k) lf10_join(lf10_ld8(cbq0 + (k) * cbs), lf10_ld8(crq0 + (k) * crs))
    y264_db_chroma_lanes16(LD2(-2), LD2(-1), LD2(0), LD2(1),
                           alpha << Y264_DB_SH, beta << Y264_DB_SH,
                           _mm256_cvtepu8_epi16(tcb), _mm256_cvtepi8_epi16(bs4b),
                           &po, &qo);
#undef LD2
    lf10_st8(cbq0 - cbs, _mm256_castsi256_si128(po));
    lf10_st8(cbq0, _mm256_castsi256_si128(qo));
    lf10_st8(crq0 - crs, _mm256_extracti128_si256(po, 1));
    lf10_st8(crq0, _mm256_extracti128_si256(qo, 1));
}

void y264_deblock_chroma2_v_avx2(pixel *cbq0, pixel *crq0, int cbs, int crs,
                                 int alpha, int beta, const uint8_t bs[4],
                                 const uint8_t tc0tab[3])
{
    __m128i tcb, bs4b;
    __m256i r[8], c[4], po, qo;
    y264_db_chroma420_params(bs, tc0tab, &tcb, &bs4b);
    for (int y = 0; y < 8; y++)
        r[y] = lf10_join(lf10_ld4(cbq0 - 2 + y * cbs), lf10_ld4(crq0 - 2 + y * crs));
    LF10_TRN4X8(_mm256, __m256i, r, c);
    y264_db_chroma_lanes16(c[0], c[1], c[2], c[3],
                           alpha << Y264_DB_SH, beta << Y264_DB_SH,
                           _mm256_cvtepu8_epi16(tcb), _mm256_cvtepi8_epi16(bs4b),
                           &po, &qo);
    __m256i lo = _mm256_unpacklo_epi16(po, qo), hi = _mm256_unpackhi_epi16(po, qo);
    lf10_st_p0q0(cbq0, cbs, _mm256_castsi256_si128(lo), _mm256_castsi256_si128(hi));
    lf10_st_p0q0(crq0, crs, _mm256_extracti128_si256(lo, 1),
                 _mm256_extracti128_si256(hi, 1));
}

#endif /* Y264_HAVE_AVX2_10 */
