/*
 * loopfilter_x86_10.h - the 10-bit deblocking kernels' shared 128-bit bodies
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * 16-bit-sample twins of the whole-edge forms in loopfilter_x86.h: the
 * sixteen-line luma edges, the two-plane 4:2:0 chroma edges and the
 * eight-line chroma horizontal edge. every body is static inline and
 * 128-bit: the SSE4.2 file wraps them all, the AVX2 file wraps the chroma8
 * one and writes 256-bit forms for the rest.
 *
 * the filter arithmetic is loopfilter_x86_core.h's per-lane bodies, which
 * already run on 16-bit lanes. only each kernel's edge changes: a 10-bit
 * sample is a lane, so nothing widens on the way in and no saturating pack
 * (it clips to 255) runs on the way out; transposes move 16-bit lanes; and
 * alpha and beta are scaled by 4 first (the parameter helpers scale tc0).
 * every lane bound holds: a sample is <= 1023, the widest sum is the strong
 * filter's 8 * 1023 + 4 = 8188, a delta is clipped to tc <= 25 * 4 + 2, and
 * p0/q0 clip to PIXEL_MAX.
 *
 * loads and stores are sized to the filter's window: a vertical luma edge
 * moves eight samples a row (p3 and q3 go back unchanged), a horizontal one
 * eight rows, a chroma edge four samples or four rows.
 */
#ifndef YAH264_DSP_X86_LOOPFILTER_X86_10_H
#define YAH264_DSP_X86_LOOPFILTER_X86_10_H

#include "dsp/x86/loopfilter_x86_core.h"

#if Y264_BIT_DEPTH != 10
#error "loopfilter_x86_10.h is the 10-bit library's x86 tier"
#endif

static inline __m128i lf10_ld8(const pixel *p)
{
    return _mm_loadu_si128((const __m128i *)(const void *)p);
}

static inline __m128i lf10_ld4(const pixel *p)
{
    return _mm_loadl_epi64((const __m128i *)(const void *)p);
}

static inline void lf10_st8(pixel *p, __m128i v)
{
    _mm_storeu_si128((__m128i *)(void *)p, v);
}

/* in place and its own inverse. every step stays in a 128-bit half, so at
 * 256 bits it transposes each half's 8x8 independently. */
#define LF10_TRN8(P, V, v) do {                                               \
    V a0_ = P##_unpacklo_epi16(v[0], v[1]), a1_ = P##_unpackhi_epi16(v[0], v[1]);\
    V a2_ = P##_unpacklo_epi16(v[2], v[3]), a3_ = P##_unpackhi_epi16(v[2], v[3]);\
    V a4_ = P##_unpacklo_epi16(v[4], v[5]), a5_ = P##_unpackhi_epi16(v[4], v[5]);\
    V a6_ = P##_unpacklo_epi16(v[6], v[7]), a7_ = P##_unpackhi_epi16(v[6], v[7]);\
    V b0_ = P##_unpacklo_epi32(a0_, a2_), b1_ = P##_unpackhi_epi32(a0_, a2_); \
    V b2_ = P##_unpacklo_epi32(a1_, a3_), b3_ = P##_unpackhi_epi32(a1_, a3_); \
    V b4_ = P##_unpacklo_epi32(a4_, a6_), b5_ = P##_unpackhi_epi32(a4_, a6_); \
    V b6_ = P##_unpacklo_epi32(a5_, a7_), b7_ = P##_unpackhi_epi32(a5_, a7_); \
    v[0] = P##_unpacklo_epi64(b0_, b4_); v[1] = P##_unpackhi_epi64(b0_, b4_); \
    v[2] = P##_unpacklo_epi64(b1_, b5_); v[3] = P##_unpackhi_epi64(b1_, b5_); \
    v[4] = P##_unpacklo_epi64(b2_, b6_); v[5] = P##_unpackhi_epi64(b2_, b6_); \
    v[6] = P##_unpacklo_epi64(b3_, b7_); v[7] = P##_unpackhi_epi64(b3_, b7_); \
} while (0)

/* eight rows of four samples (the low halves of r[0..7]) to the four sample
 * positions, eight rows each; also in-half */
#define LF10_TRN4X8(P, V, r, c) do {                                          \
    V a0_ = P##_unpacklo_epi16(r[0], r[1]), a1_ = P##_unpacklo_epi16(r[2], r[3]);\
    V a2_ = P##_unpacklo_epi16(r[4], r[5]), a3_ = P##_unpacklo_epi16(r[6], r[7]);\
    V b0_ = P##_unpacklo_epi32(a0_, a1_), b1_ = P##_unpackhi_epi32(a0_, a1_); \
    V b2_ = P##_unpacklo_epi32(a2_, a3_), b3_ = P##_unpackhi_epi32(a2_, a3_); \
    c[0] = P##_unpacklo_epi64(b0_, b2_); c[1] = P##_unpackhi_epi64(b0_, b2_); \
    c[2] = P##_unpacklo_epi64(b1_, b3_); c[3] = P##_unpackhi_epi64(b1_, b3_); \
} while (0)

/* ---- luma, sixteen lines, eight a pass ----------------------------------- *
 *
 * bS 1-3 and bS 4 are tested per half, as a half is one pass: bS & 3 is
 * nonzero exactly for 1-3, bS & 4 exactly for 4. a half whose two bS values
 * are both 0 is skipped. */
static inline void lf10_luma_lanes_half(__m128i s[8], int alpha, int beta,
                                        __m128i tc0b, __m128i bs4b, __m128i nzb,
                                        int b0, int b1, int g, __m128i o[6])
{
    y264_db_luma_lanes8(s, alpha, beta,
        _mm_cvtepu8_epi16(g ? _mm_srli_si128(tc0b, 8) : tc0b),
        _mm_cvtepi8_epi16(g ? _mm_srli_si128(bs4b, 8) : bs4b),
        _mm_cvtepi8_epi16(g ? _mm_srli_si128(nzb, 8) : nzb),
        o, ((b0 | b1) & 3) != 0, ((b0 | b1) & 4) != 0);
}

/* eight columns a pass; the samples across the edge run down the rows, so
 * each sample position is one load */
static inline void lf10_luma_h16(pixel *q0p, int stride, int alpha, int beta,
                                 const uint8_t bs[4], const uint8_t tc0tab[3])
{
    __m128i tc0b, bs4b, nzb;
    y264_db_luma16_params(bs, tc0tab, &tc0b, &bs4b, &nzb);
    alpha <<= Y264_DB_SH;
    beta <<= Y264_DB_SH;
    for (int g = 0; g < 2; g++) {
        int b0 = bs[2 * g], b1 = bs[2 * g + 1];
        if (!(b0 | b1))
            continue;
        pixel *c = q0p + 8 * g;
        __m128i s[8], o[6];
        for (int k = 0; k < 8; k++)
            s[k] = lf10_ld8(c + (k - 4) * stride);
        lf10_luma_lanes_half(s, alpha, beta, tc0b, bs4b, nzb, b0, b1, g, o);
        for (int k = 0; k < 6; k++)
            lf10_st8(c + (k - 3) * stride, o[k]);
    }
}

/* eight rows of p3..q3 a pass, transposed so each sample position across
 * the edge is a register, then back */
static inline void lf10_luma_v16(pixel *q0p, int stride, int alpha, int beta,
                                 const uint8_t bs[4], const uint8_t tc0tab[3])
{
    __m128i tc0b, bs4b, nzb;
    y264_db_luma16_params(bs, tc0tab, &tc0b, &bs4b, &nzb);
    alpha <<= Y264_DB_SH;
    beta <<= Y264_DB_SH;
    for (int g = 0; g < 2; g++) {
        int b0 = bs[2 * g], b1 = bs[2 * g + 1];
        if (!(b0 | b1))
            continue;
        pixel *b = q0p - 4 + (8 * g) * stride;
        __m128i v[8], o[6];
        for (int k = 0; k < 8; k++)
            v[k] = lf10_ld8(b + k * stride);
        LF10_TRN8(_mm, __m128i, v);
        lf10_luma_lanes_half(v, alpha, beta, tc0b, bs4b, nzb, b0, b1, g, o);
        for (int k = 0; k < 6; k++)
            v[k + 1] = o[k];
        LF10_TRN8(_mm, __m128i, v);
        for (int k = 0; k < 8; k++)
            lf10_st8(b + k * stride, v[k]);
    }
}

/* ---- chroma (chromaStyleFilteringFlag == 1) ------------------------------ */

/* one plane, eight columns: the 4:2:0 and 4:2:2 single-plane form */
static inline void lf10_chroma8_h(pixel *q0p, int stride, int alpha, int beta,
                                  const uint8_t bs[4], const uint8_t tc0tab[3],
                                  int span, int g)
{
    __m128i tc, bs4, po, qo;
    y264_db_chroma8_params(bs, tc0tab, span, g, &tc, &bs4);
    y264_db_chroma_lanes8(lf10_ld8(q0p - 2 * stride), lf10_ld8(q0p - stride),
                          lf10_ld8(q0p), lf10_ld8(q0p + stride),
                          alpha << Y264_DB_SH, beta << Y264_DB_SH, tc, bs4,
                          &po, &qo);
    lf10_st8(q0p - stride, po);
    lf10_st8(q0p, qo);
}

/* 4:2:0, both planes: Cb and Cr share the edge's bS, alpha, beta and tc, so
 * one parameter set-up serves two eight-lane passes */
static inline void lf10_chroma2_h(pixel *cbq0, pixel *crq0, int cbs, int crs,
                                  int alpha, int beta, const uint8_t bs[4],
                                  const uint8_t tc0tab[3])
{
    __m128i tcb, bs4b, po, qo;
    y264_db_chroma420_params(bs, tc0tab, &tcb, &bs4b);
    __m128i tc = _mm_cvtepu8_epi16(tcb), bs4 = _mm_cvtepi8_epi16(bs4b);
    alpha <<= Y264_DB_SH;
    beta <<= Y264_DB_SH;
    for (int pl = 0; pl < 2; pl++) {
        pixel *q = pl ? crq0 : cbq0;
        int st = pl ? crs : cbs;
        y264_db_chroma_lanes8(lf10_ld8(q - 2 * st), lf10_ld8(q - st),
                              lf10_ld8(q), lf10_ld8(q + st),
                              alpha, beta, tc, bs4, &po, &qo);
        lf10_st8(q - st, po);
        lf10_st8(q, qo);
    }
}

/* one four-byte store a row; lo holds rows 0-3 as (p0, q0) pairs, hi rows
 * 4-7 */
static inline void lf10_st_p0q0(pixel *q, int st, __m128i lo, __m128i hi)
{
    int32_t w[8];
    _mm_storeu_si128((__m128i *)(void *)w, lo);
    _mm_storeu_si128((__m128i *)(void *)(w + 4), hi);
    for (int y = 0; y < 8; y++)
        memcpy(q - 1 + y * st, &w[y], 4);
}

/* 4:2:0, both planes: each row's p1 p0 q0 q1 is one eight-byte load, and
 * only p0 and q0 go back */
static inline void lf10_chroma2_v(pixel *cbq0, pixel *crq0, int cbs, int crs,
                                  int alpha, int beta, const uint8_t bs[4],
                                  const uint8_t tc0tab[3])
{
    __m128i tcb, bs4b, po, qo;
    y264_db_chroma420_params(bs, tc0tab, &tcb, &bs4b);
    __m128i tc = _mm_cvtepu8_epi16(tcb), bs4 = _mm_cvtepi8_epi16(bs4b);
    alpha <<= Y264_DB_SH;
    beta <<= Y264_DB_SH;
    for (int pl = 0; pl < 2; pl++) {
        pixel *q = pl ? crq0 : cbq0;
        int st = pl ? crs : cbs;
        __m128i r[8], c[4];
        for (int y = 0; y < 8; y++)
            r[y] = lf10_ld4(q - 2 + y * st);
        LF10_TRN4X8(_mm, __m128i, r, c);
        y264_db_chroma_lanes8(c[0], c[1], c[2], c[3], alpha, beta, tc, bs4,
                              &po, &qo);
        lf10_st_p0q0(q, st, _mm_unpacklo_epi16(po, qo), _mm_unpackhi_epi16(po, qo));
    }
}

#endif /* YAH264_DSP_X86_LOOPFILTER_X86_10_H */
