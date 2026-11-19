/*
 * loopfilter_x86.h - the building blocks the x86 deblocking kernels share
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Wave 3b of docs/simd.md. Every helper here is `static inline` and
 * 128-bit, so the SSE4.2 file gets SSE encodings and the AVX2 file gets VEX
 * encodings of the same source. The eight-line chroma edge uses them whole
 * at both tiers, since a 256-bit register has nothing to put in its upper
 * lane. The strength derivation widens properly -- sixteen edges of an axis
 * are sixteen lanes, so the AVX2 twin does one pass per axis where the
 * SSE4.2 one does two.
 *
 * THREE FACTS CARRY THIS FILE.
 *
 * The whole filter fits SIGNED 16-BIT lanes. Samples are 8-bit, the widest
 * sum the strong filter forms is 2*p3 + 3*p2 + p1 + p0 + q0 + 4 <= 8*255 + 4,
 * and every delta is clipped to +-tc with tc <= 25. So there is no widening
 * step anywhere: unpack once on the way in, saturate-pack once on the way
 * out.
 *
 * The scalar filter's BRANCHES become masks. The reference returns early when
 * the alpha/beta gate rejects a line and writes p1/q1 only when ap/aq fall
 * under beta; here every lane computes every branch and `_mm_blendv_epi8`
 * picks. A rejected lane is therefore rewritten with its own samples, which is
 * why the stores may cover the whole span the filter OWNS ([-3, +2] around the
 * edge) without touching a sample the scalar path would have left alone.
 *
 * Clip1 is a saturating pack. PACKUSWB narrows signed 16-bit to unsigned
 * 8-bit saturating at both ends, which IS clip(v, 0, 255) for every value
 * these filters produce -- but only for the values that are ALLOWED to be out
 * of range. The bS < 4 weak filter's p0/q0 are clipped explicitly with
 * min/max before the pack, because 8.7.2.3 clips them and the pack would clip
 * them the same way, while its p1/q1 results are in range by construction and
 * must NOT be re-clipped anywhere the spec does not.
 *
 * The pieces that do not depend on the sample width -- the strength
 * derivation, the per-lane whole-edge filters and their parameters -- are in
 * loopfilter_x86_core.h, which the 10-bit tier includes as well.
 *
 * C11 intrinsics only. There is no assembly in this tree, and hygiene_check
 * refuses any inline form under this directory.
 */
#ifndef YAH264_DSP_X86_DEBLOCK_X86_H
#define YAH264_DSP_X86_DEBLOCK_X86_H

#include <immintrin.h>
#include <string.h>

#include "dsp/x86/metrics_x86.h"   /* the block-sized loads, and dsp/arch.h */
#include "dsp/x86/loopfilter_x86_core.h"

/* ---- the small vocabulary the NEON twin gets from its instruction set ----- */

/* a < b, as an all-ones mask. */
static inline __m128i y264_lt16(__m128i a, __m128i b)
{
    return _mm_cmpgt_epi16(b, a);
}

/* Clip3(-t, t, v). */
static inline __m128i y264_clipt16(__m128i v, __m128i t)
{
    return _mm_max_epi16(_mm_sub_epi16(_mm_setzero_si128(), t),
                         _mm_min_epi16(t, v));
}

static inline __m128i y264_clip255(__m128i v)
{
    return _mm_min_epi16(_mm_max_epi16(v, _mm_setzero_si128()),
                         _mm_set1_epi16(255));
}

/* ---- the chroma edge filter (chromaStyleFilteringFlag == 1) -------------- *
 *
 * Only p0 and q0 move and the bS < 4 clip is tc0 + 1 with no ap/aq term, so
 * the whole eight-line edge is four sample vectors and one pass. tc and the
 * bS == 4 select are per-LANE, derived from the caller's four bS entries, and
 * a bS == 0 line is passed as tc 0 with the strong select clear: its delta
 * clips to zero and the line is rewritten with its own samples. The
 * parameters come from loopfilter_x86_core.h.
 */
static inline void y264_db_chroma8_h_x86(pixel *q0p, int stride, int alpha,
                                         int beta, const uint8_t bs[4],
                                         const uint8_t tc0tab[3],
                                         int span, int g)
{
    __m128i tc, bs4;
    y264_db_chroma8_params(bs, tc0tab, span, g, &tc, &bs4);

    __m128i p1 = _mm_cvtepu8_epi16(y264_ld64(q0p - 2 * stride));
    __m128i p0 = _mm_cvtepu8_epi16(y264_ld64(q0p - stride));
    __m128i q0 = _mm_cvtepu8_epi16(y264_ld64(q0p));
    __m128i q1 = _mm_cvtepu8_epi16(y264_ld64(q0p + stride));

    const __m128i vb = _mm_set1_epi16((short)beta);
    __m128i filt = y264_lt16(y264_abd16(p0, q0), _mm_set1_epi16((short)alpha));
    filt = _mm_and_si128(filt, y264_lt16(y264_abd16(p1, p0), vb));
    filt = _mm_and_si128(filt, y264_lt16(y264_abd16(q1, q0), vb));

    __m128i d = _mm_add_epi16(_mm_slli_epi16(_mm_sub_epi16(q0, p0), 2),
                              _mm_sub_epi16(p1, q1));
    d = _mm_srai_epi16(_mm_add_epi16(d, _mm_set1_epi16(4)), 3);
    d = y264_clipt16(d, tc);
    __m128i p0w = y264_clip255(_mm_add_epi16(p0, d));
    __m128i q0w = y264_clip255(_mm_sub_epi16(q0, d));

    const __m128i c2 = _mm_set1_epi16(2);
    __m128i p0s = _mm_srai_epi16(_mm_add_epi16(_mm_add_epi16(_mm_slli_epi16(p1, 1),
                      _mm_add_epi16(p0, q1)), c2), 2);
    __m128i q0s = _mm_srai_epi16(_mm_add_epi16(_mm_add_epi16(_mm_slli_epi16(q1, 1),
                      _mm_add_epi16(q0, p1)), c2), 2);

    __m128i po = y264_sel16(filt, y264_sel16(bs4, p0s, p0w), p0);
    __m128i qo = y264_sel16(filt, y264_sel16(bs4, q0s, q0w), q0);
    _mm_storel_epi64((__m128i *)(void *)(q0p - stride), _mm_packus_epi16(po, po));
    _mm_storel_epi64((__m128i *)(void *)q0p, _mm_packus_epi16(qo, qo));
}

/* Eight rows of eight bytes (the low halves of r[0..7]) transposed: c[j]
 * leaves with byte column 2j in its low half and column 2j+1 in its high
 * half, one byte per row. */
#define Y264_DB_TRN8X8(P, V, r, c) do {                                       \
    V a0_ = P##_unpacklo_epi8(r[0], r[1]), a1_ = P##_unpacklo_epi8(r[2], r[3]);\
    V a2_ = P##_unpacklo_epi8(r[4], r[5]), a3_ = P##_unpacklo_epi8(r[6], r[7]);\
    V b0_ = P##_unpacklo_epi16(a0_, a1_), b1_ = P##_unpackhi_epi16(a0_, a1_); \
    V b2_ = P##_unpacklo_epi16(a2_, a3_), b3_ = P##_unpackhi_epi16(a2_, a3_); \
    c[0] = P##_unpacklo_epi32(b0_, b2_); c[1] = P##_unpackhi_epi32(b0_, b2_); \
    c[2] = P##_unpacklo_epi32(b1_, b3_); c[3] = P##_unpackhi_epi32(b1_, b3_); \
} while (0)

/* The way back. v[0..7] are the eight byte columns as 16-bit lanes with
 * values in 0-255 (every filter output is), one lane per row; g[j] leaves
 * with row 2j in its low half and row 2j+1 in its high half, eight bytes
 * each. Two columns meet in one lane as (even | odd << 8), which is the
 * interleave a pack and an unpack would otherwise spell. */
#define Y264_DB_UNTRN8X8(P, V, v, g) do {                                     \
    V e0_ = Y264_DB_OR_##P(v[0], P##_slli_epi16(v[1], 8));                 \
    V e1_ = Y264_DB_OR_##P(v[2], P##_slli_epi16(v[3], 8));                 \
    V e2_ = Y264_DB_OR_##P(v[4], P##_slli_epi16(v[5], 8));                 \
    V e3_ = Y264_DB_OR_##P(v[6], P##_slli_epi16(v[7], 8));                 \
    V f0_ = P##_unpacklo_epi16(e0_, e1_), f1_ = P##_unpackhi_epi16(e0_, e1_); \
    V f2_ = P##_unpacklo_epi16(e2_, e3_), f3_ = P##_unpackhi_epi16(e2_, e3_); \
    g[0] = P##_unpacklo_epi32(f0_, f2_); g[1] = P##_unpackhi_epi32(f0_, f2_); \
    g[2] = P##_unpacklo_epi32(f1_, f3_); g[3] = P##_unpackhi_epi32(f1_, f3_); \
} while (0)

/* the single-precision form: gcc writes the double form as a plain C store of
 * a double, which is undefined at a pixel address that is not 8-byte aligned */
static inline void y264_st64h(pixel *p, __m128i v)
{
    _mm_storeh_pi((__m64 *)(void *)p, _mm_castsi128_ps(v));
}

/* ---- the whole luma edge, eight lanes at a time (the SSE4.2 tier) --------- */

static inline void y264_db_luma_h16_x86(pixel *q0p, int stride, int alpha,
                                        int beta, const uint8_t bs[4],
                                        const uint8_t tc0tab[3])
{
    const __m128i z = _mm_setzero_si128();
    __m128i tc0b, bs4b, nzb, r[8], o[2][6];
    int has_n, has_4;
    y264_db_luma16_params(bs, tc0tab, &tc0b, &bs4b, &nzb);
    y264_db_luma16_need(bs, &has_n, &has_4);
    for (int k = 0; k < 8; k++)
        r[k] = y264_ld128(q0p + (k - 4) * stride);
    for (int g = 0; g < 2; g++) {
        __m128i s[8];
        for (int k = 0; k < 8; k++)
            s[k] = g ? _mm_unpackhi_epi8(r[k], z) : _mm_cvtepu8_epi16(r[k]);
        y264_db_luma_lanes8(s, alpha, beta,
            _mm_cvtepu8_epi16(g ? _mm_srli_si128(tc0b, 8) : tc0b),
            _mm_cvtepi8_epi16(g ? _mm_srli_si128(bs4b, 8) : bs4b),
            _mm_cvtepi8_epi16(g ? _mm_srli_si128(nzb, 8) : nzb),
            o[g], has_n, has_4);
    }
    for (int k = 0; k < 6; k++)
        _mm_storeu_si128((__m128i *)(void *)(q0p + (k - 3) * stride),
                         _mm_packus_epi16(o[0][k], o[1][k]));
}

/* Vertical edge: sixteen rows of eight samples [-4, +3], transposed so the
 * samples across the edge become registers, eight rows per pass. p3 and q3
 * go back unchanged, so each row is one eight-byte store. A half whose two
 * bS values are both 0 is not touched. */
static inline void y264_db_luma_v16_x86(pixel *q0p, int stride, int alpha,
                                        int beta, const uint8_t bs[4],
                                        const uint8_t tc0tab[3])
{
    const __m128i z = _mm_setzero_si128();
    __m128i tc0b, bs4b, nzb;
    int has_n, has_4;
    y264_db_luma16_params(bs, tc0tab, &tc0b, &bs4b, &nzb);
    y264_db_luma16_need(bs, &has_n, &has_4);
    for (int g = 0; g < 2; g++) {
        pixel *b = q0p - 4 + (8 * g) * stride;
        __m128i r[8], c[4], s[8], o[6], v[8], w[4];
        if (!(bs[2 * g] | bs[2 * g + 1]))
            continue;
        for (int k = 0; k < 8; k++)
            r[k] = y264_ld64(b + k * stride);
        Y264_DB_TRN8X8(_mm, __m128i, r, c);
        for (int j = 0; j < 4; j++) {
            s[2 * j] = _mm_cvtepu8_epi16(c[j]);
            s[2 * j + 1] = _mm_unpackhi_epi8(c[j], z);
        }
        y264_db_luma_lanes8(s, alpha, beta,
            _mm_cvtepu8_epi16(g ? _mm_srli_si128(tc0b, 8) : tc0b),
            _mm_cvtepi8_epi16(g ? _mm_srli_si128(bs4b, 8) : bs4b),
            _mm_cvtepi8_epi16(g ? _mm_srli_si128(nzb, 8) : nzb),
            o, has_n, has_4);
        v[0] = s[0]; v[7] = s[7];
        for (int k = 0; k < 6; k++) v[k + 1] = o[k];
        Y264_DB_UNTRN8X8(_mm, __m128i, v, w);
        for (int j = 0; j < 4; j++) {
            _mm_storel_epi64((__m128i *)(void *)(b + (2 * j) * stride), w[j]);
            y264_st64h(b + (2 * j + 1) * stride, w[j]);
        }
    }
}

/* ---- 4:2:0 chroma edges, both planes per call ----------------------------- *
 *
 * Cb and Cr share an edge's bS, alpha, beta and tc (one chroma QP offset), so
 * one parameter set-up serves both and the planes are two passes of eight
 * lanes here (the AVX2 twin runs them as the halves of one register).
 */

/* Horizontal edge: eight columns of each plane at q0, the samples across the
 * edge running down the rows. */
static inline void y264_db_chroma2_h_x86(pixel *cbq0, pixel *crq0, int cbs,
                                         int crs, int alpha, int beta,
                                         const uint8_t bs[4],
                                         const uint8_t tc0tab[3])
{
    __m128i tcb, bs4b, po, qo;
    y264_db_chroma420_params(bs, tc0tab, &tcb, &bs4b);
    __m128i tc = _mm_cvtepu8_epi16(tcb), bs4 = _mm_cvtepi8_epi16(bs4b);
    for (int pl = 0; pl < 2; pl++) {
        pixel *q = pl ? crq0 : cbq0;
        int st = pl ? crs : cbs;
        y264_db_chroma_lanes8(_mm_cvtepu8_epi16(y264_ld64(q - 2 * st)),
                              _mm_cvtepu8_epi16(y264_ld64(q - st)),
                              _mm_cvtepu8_epi16(y264_ld64(q)),
                              _mm_cvtepu8_epi16(y264_ld64(q + st)),
                              alpha, beta, tc, bs4, &po, &qo);
        __m128i pk = _mm_packus_epi16(po, qo);
        _mm_storel_epi64((__m128i *)(void *)(q - st), pk);
        y264_st64h(q, pk);
    }
}

/* Rows a..a+3 of a vertical chroma edge: four bytes p1 p0 q0 q1 from each,
 * one row per 32-bit lane. Four bytes per row and not a fifth. */
static inline __m128i y264_db_ld4rows(const pixel *p, int stride)
{
    __m128i v = y264_ld32(p);
    int32_t w;
    memcpy(&w, p + stride, 4);     v = _mm_insert_epi32(v, w, 1);
    memcpy(&w, p + 2 * stride, 4); v = _mm_insert_epi32(v, w, 2);
    memcpy(&w, p + 3 * stride, 4); v = _mm_insert_epi32(v, w, 3);
    return v;
}

/* Sample position j of four rows gathered into the j-th 32-bit lane. */
#define Y264_DB_C4TR \
    _mm_setr_epi8(0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15)

/* Vertical edge: eight rows of each plane, four samples across the edge per
 * row. Only p0 and q0 move, so each row gets one two-byte store. */
static inline void y264_db_chroma2_v_x86(pixel *cbq0, pixel *crq0, int cbs,
                                         int crs, int alpha, int beta,
                                         const uint8_t bs[4],
                                         const uint8_t tc0tab[3])
{
    const __m128i z = _mm_setzero_si128();
    __m128i tcb, bs4b, po, qo;
    y264_db_chroma420_params(bs, tc0tab, &tcb, &bs4b);
    __m128i tc = _mm_cvtepu8_epi16(tcb), bs4 = _mm_cvtepi8_epi16(bs4b);
    for (int pl = 0; pl < 2; pl++) {
        pixel *q = pl ? crq0 : cbq0;
        int st = pl ? crs : cbs;
        __m128i a = _mm_shuffle_epi8(y264_db_ld4rows(q - 2, st), Y264_DB_C4TR);
        __m128i b = _mm_shuffle_epi8(y264_db_ld4rows(q - 2 + 4 * st, st),
                                     Y264_DB_C4TR);
        __m128i pp = _mm_unpacklo_epi32(a, b);      /* p1 x8, p0 x8 */
        __m128i qq = _mm_unpackhi_epi32(a, b);      /* q0 x8, q1 x8 */
        y264_db_chroma_lanes8(_mm_cvtepu8_epi16(pp), _mm_unpackhi_epi8(pp, z),
                              _mm_cvtepu8_epi16(qq), _mm_unpackhi_epi8(qq, z),
                              alpha, beta, tc, bs4, &po, &qo);
        /* p0 | q0 << 8 per row: both are 0-255 */
        __m128i o = _mm_or_si128(po, _mm_slli_epi16(qo, 8));
#define Y264_DB_ST2(y) do {                                                   \
            uint16_t h_ = (uint16_t)_mm_extract_epi16(o, y);                  \
            memcpy(q - 1 + (y) * st, &h_, 2);                                 \
        } while (0)
        Y264_DB_ST2(0); Y264_DB_ST2(1); Y264_DB_ST2(2); Y264_DB_ST2(3);
        Y264_DB_ST2(4); Y264_DB_ST2(5); Y264_DB_ST2(6); Y264_DB_ST2(7);
#undef Y264_DB_ST2
    }
}

#endif /* YAH264_DSP_X86_DEBLOCK_X86_H */
