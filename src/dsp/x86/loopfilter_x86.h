/*
 * loopfilter_x86.h - the building blocks the x86 deblocking kernels share
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Wave 3b of docs/simd.md. Every helper here is `static inline` and
 * 128-bit, so the SSE4.2 file gets SSE encodings and the AVX2 file gets VEX
 * encodings of the same source. Three of the four kernels in this family USE
 * them whole at both tiers, because their shapes have no second half: a luma
 * edge segment is four lines and a chroma edge is eight, and a 256-bit
 * register has nothing to put in its upper lane. Only the strength derivation
 * widens, and it widens properly -- sixteen edges of an axis are sixteen
 * lanes, so the AVX2 twin does one pass per axis where the SSE4.2 one does
 * two.
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
 * C11 intrinsics only. There is no assembly in this tree, and hygiene_check
 * refuses any inline form under this directory.
 */
#ifndef YAH264_DSP_X86_DEBLOCK_X86_H
#define YAH264_DSP_X86_DEBLOCK_X86_H

#include <immintrin.h>
#include <string.h>

#include "dsp/x86/metrics_x86.h"   /* the block-sized loads, and dsp/arch.h */

/* ---- the small vocabulary the NEON twin gets from its instruction set ----- */

/* |a - b| over signed 16-bit lanes (NEON has this as one instruction). */
static inline __m128i y264_abd16(__m128i a, __m128i b)
{
    return _mm_sub_epi16(_mm_max_epi16(a, b), _mm_min_epi16(a, b));
}

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

/* mask ? x : y, lane-wise (the bit-select the NEON body is written in). */
static inline __m128i y264_sel16(__m128i mask, __m128i x, __m128i y)
{
    return _mm_blendv_epi8(y, x, mask);
}

static inline __m128i y264_clip255(__m128i v)
{
    return _mm_min_epi16(_mm_max_epi16(v, _mm_setzero_si128()),
                         _mm_set1_epi16(255));
}

/* ---- the luma edge filter, 8.7.2.3 and 8.7.2.4 --------------------------- *
 *
 * `s` holds the eight sample positions p3 p2 p1 p0 q0 q1 q2 q3, one register
 * each, with one lane per LINE of the segment -- four for the shapes this
 * family dispatches. Lanes above the segment's width hold whatever the load
 * brought and are discarded by the caller; nothing here crosses lanes, so
 * they cannot contaminate a live one.
 *
 * `out` receives the six positions the filter may write, p2 p1 p0 q0 q1 q2.
 */
static inline void y264_db_luma_core(const __m128i s[8], int bs, int alpha,
                                     int beta, int tc0, __m128i out[6])
{
    const __m128i p3 = s[0], p2 = s[1], p1 = s[2], p0 = s[3];
    const __m128i q0 = s[4], q1 = s[5], q2 = s[6], q3 = s[7];
    const __m128i vb = _mm_set1_epi16((short)beta);

    __m128i filt = y264_lt16(y264_abd16(p0, q0), _mm_set1_epi16((short)alpha));
    filt = _mm_and_si128(filt, y264_lt16(y264_abd16(p1, p0), vb));
    filt = _mm_and_si128(filt, y264_lt16(y264_abd16(q1, q0), vb));

    __m128i apb = y264_lt16(y264_abd16(p2, p0), vb);      /* ap < beta */
    __m128i aqb = y264_lt16(y264_abd16(q2, q0), vb);      /* aq < beta */

    out[0] = p2; out[5] = q2;

    if (bs < 4) {
        /* tc = tc0 + (ap < beta) + (aq < beta); the masks are -1, so the two
         * terms SUBTRACT. */
        __m128i tc = _mm_set1_epi16((short)tc0);
        tc = _mm_sub_epi16(tc, apb);
        tc = _mm_sub_epi16(tc, aqb);
        __m128i d = _mm_add_epi16(_mm_slli_epi16(_mm_sub_epi16(q0, p0), 2),
                                  _mm_sub_epi16(p1, q1));
        d = _mm_srai_epi16(_mm_add_epi16(d, _mm_set1_epi16(4)), 3);
        d = y264_clipt16(d, tc);
        __m128i p0n = y264_clip255(_mm_add_epi16(p0, d));
        __m128i q0n = y264_clip255(_mm_sub_epi16(q0, d));

        const __m128i tcl = _mm_set1_epi16((short)tc0);
        /* (p0 + q0 + 1) >> 1, exactly: both are in [0, 255], so the unsigned
         * rounding average is the signed one. */
        __m128i av = _mm_avg_epu16(p0, q0);
        __m128i dp1 = _mm_srai_epi16(_mm_sub_epi16(_mm_add_epi16(p2, av),
                                                   _mm_slli_epi16(p1, 1)), 1);
        dp1 = y264_clipt16(dp1, tcl);
        __m128i dq1 = _mm_srai_epi16(_mm_sub_epi16(_mm_add_epi16(q2, av),
                                                   _mm_slli_epi16(q1, 1)), 1);
        dq1 = y264_clipt16(dq1, tcl);

        out[1] = y264_sel16(_mm_and_si128(filt, apb), _mm_add_epi16(p1, dp1), p1);
        out[2] = y264_sel16(filt, p0n, p0);
        out[3] = y264_sel16(filt, q0n, q0);
        out[4] = y264_sel16(_mm_and_si128(filt, aqb), _mm_add_epi16(q1, dq1), q1);
        return;
    }

    /* bS == 4 */
    __m128i strong = y264_lt16(y264_abd16(p0, q0),
                               _mm_set1_epi16((short)((alpha >> 2) + 2)));
    __m128i sp = _mm_and_si128(_mm_and_si128(filt, strong), apb);
    __m128i sq = _mm_and_si128(_mm_and_si128(filt, strong), aqb);
    const __m128i c2 = _mm_set1_epi16(2), c4 = _mm_set1_epi16(4);

    __m128i pq = _mm_add_epi16(p0, q0);
    /* strong P: (p2 + 2p1 + 2p0 + 2q0 + q1 + 4) >> 3, (p2 + p1 + p0 + q0 + 2) >> 2,
     * (2p3 + 3p2 + p1 + p0 + q0 + 4) >> 3 */
    __m128i p0s = _mm_srai_epi16(_mm_add_epi16(_mm_add_epi16(_mm_add_epi16(p2, q1),
                      _mm_slli_epi16(_mm_add_epi16(p1, pq), 1)), c4), 3);
    __m128i p1s = _mm_srai_epi16(_mm_add_epi16(_mm_add_epi16(_mm_add_epi16(p2, p1),
                      pq), c2), 2);
    __m128i p2s = _mm_srai_epi16(_mm_add_epi16(_mm_add_epi16(
                      _mm_add_epi16(_mm_slli_epi16(p3, 1),
                                    _mm_add_epi16(_mm_slli_epi16(p2, 1), p2)),
                      _mm_add_epi16(p1, pq)), c4), 3);
    __m128i p0w = _mm_srai_epi16(_mm_add_epi16(_mm_add_epi16(_mm_slli_epi16(p1, 1),
                      _mm_add_epi16(p0, q1)), c2), 2);
    /* strong Q, the mirror */
    __m128i q0s = _mm_srai_epi16(_mm_add_epi16(_mm_add_epi16(_mm_add_epi16(q2, p1),
                      _mm_slli_epi16(_mm_add_epi16(q1, pq), 1)), c4), 3);
    __m128i q1s = _mm_srai_epi16(_mm_add_epi16(_mm_add_epi16(_mm_add_epi16(q2, q1),
                      pq), c2), 2);
    __m128i q2s = _mm_srai_epi16(_mm_add_epi16(_mm_add_epi16(
                      _mm_add_epi16(_mm_slli_epi16(q3, 1),
                                    _mm_add_epi16(_mm_slli_epi16(q2, 1), q2)),
                      _mm_add_epi16(q1, pq)), c4), 3);
    __m128i q0w = _mm_srai_epi16(_mm_add_epi16(_mm_add_epi16(_mm_slli_epi16(q1, 1),
                      _mm_add_epi16(q0, p1)), c2), 2);

    out[0] = y264_sel16(sp, p2s, p2);
    out[1] = y264_sel16(sp, p1s, p1);
    out[2] = y264_sel16(filt, y264_sel16(sp, p0s, p0w), p0);
    out[3] = y264_sel16(filt, y264_sel16(sq, q0s, q0w), q0);
    out[4] = y264_sel16(sq, q1s, q1);
    out[5] = y264_sel16(sq, q2s, q2);
}

/* ---- the two luma edge shapes -------------------------------------------- *
 *
 * A VERTICAL edge is four lines of eight samples running ACROSS the rows, so
 * the eight sample positions have to be transposed out of four row loads; a
 * HORIZONTAL one is four columns of eight samples running DOWN the rows, and
 * its lanes are the four columns already. Both are written once here and
 * instantiated per tier, because at four lanes the tiers differ only in how
 * the compiler encodes them.
 */
static inline void y264_db_luma_v4_x86(pixel *q0p, int stride, int bs,
                                       int alpha, int beta, int tc0)
{
    /* Eight bytes per line is exactly p3..q3; nothing reads a ninth. */
    __m128i r0 = y264_ld64(q0p - 4);
    __m128i r1 = y264_ld64(q0p - 4 + stride);
    __m128i r2 = y264_ld64(q0p - 4 + 2 * stride);
    __m128i r3 = y264_ld64(q0p - 4 + 3 * stride);

    /* 4x8 byte transpose: two interleaves leave each sample position as four
     * adjacent bytes, one per line. */
    __m128i a01 = _mm_unpacklo_epi8(r0, r1);
    __m128i a23 = _mm_unpacklo_epi8(r2, r3);
    __m128i clo = _mm_unpacklo_epi16(a01, a23);   /* p3 p2 p1 p0 */
    __m128i chi = _mm_unpackhi_epi16(a01, a23);   /* q0 q1 q2 q3 */

    __m128i s[8], o[6];
    s[0] = _mm_cvtepu8_epi16(clo);
    s[1] = _mm_cvtepu8_epi16(_mm_srli_si128(clo, 4));
    s[2] = _mm_cvtepu8_epi16(_mm_srli_si128(clo, 8));
    s[3] = _mm_cvtepu8_epi16(_mm_srli_si128(clo, 12));
    s[4] = _mm_cvtepu8_epi16(chi);
    s[5] = _mm_cvtepu8_epi16(_mm_srli_si128(chi, 4));
    s[6] = _mm_cvtepu8_epi16(_mm_srli_si128(chi, 8));
    s[7] = _mm_cvtepu8_epi16(_mm_srli_si128(chi, 12));

    y264_db_luma_core(s, bs, alpha, beta, tc0, o);

    /* Back to rows. Each pack leaves two output positions eight bytes apart,
     * and interleaving a register with its own upper half pairs them per line;
     * one more interleave gives four positions per line. */
    __m128i a = _mm_packus_epi16(o[0], o[1]);
    __m128i b = _mm_packus_epi16(o[2], o[3]);
    __m128i c = _mm_packus_epi16(o[4], o[5]);
    __m128i t = _mm_unpacklo_epi8(a, _mm_srli_si128(a, 8));
    __m128i u = _mm_unpacklo_epi8(b, _mm_srli_si128(b, 8));
    __m128i v = _mm_unpacklo_epi8(c, _mm_srli_si128(c, 8));
    __m128i w = _mm_unpacklo_epi16(t, u);         /* p2 p1 p0 q0, four lines */

    /* [-3, +2] per line and not one byte more: four bytes, then two. */
    int32_t w0 = _mm_cvtsi128_si32(w);
    int32_t w1 = _mm_extract_epi32(w, 1);
    int32_t w2 = _mm_extract_epi32(w, 2);
    int32_t w3 = _mm_extract_epi32(w, 3);
    memcpy(q0p - 3, &w0, 4);
    memcpy(q0p - 3 + stride, &w1, 4);
    memcpy(q0p - 3 + 2 * stride, &w2, 4);
    memcpy(q0p - 3 + 3 * stride, &w3, 4);
    uint16_t h0 = (uint16_t)_mm_extract_epi16(v, 0);
    uint16_t h1 = (uint16_t)_mm_extract_epi16(v, 1);
    uint16_t h2 = (uint16_t)_mm_extract_epi16(v, 2);
    uint16_t h3 = (uint16_t)_mm_extract_epi16(v, 3);
    memcpy(q0p + 1, &h0, 2);
    memcpy(q0p + 1 + stride, &h1, 2);
    memcpy(q0p + 1 + 2 * stride, &h2, 2);
    memcpy(q0p + 1 + 3 * stride, &h3, 2);
}

static inline void y264_db_luma_h4_x86(pixel *q0p, int stride, int bs,
                                       int alpha, int beta, int tc0)
{
    __m128i s[8], o[6];
    for (int k = 0; k < 8; k++)
        s[k] = _mm_cvtepu8_epi16(y264_ld32(q0p + (k - 4) * stride));

    y264_db_luma_core(s, bs, alpha, beta, tc0, o);

    for (int k = 0; k < 6; k++) {
        int32_t v = _mm_cvtsi128_si32(_mm_packus_epi16(o[k], o[k]));
        memcpy(q0p + (k - 3) * stride, &v, 4);
    }
}

/* ---- the chroma edge filter (chromaStyleFilteringFlag == 1) -------------- *
 *
 * Only p0 and q0 move and the bS < 4 clip is tc0 + 1 with no ap/aq term, so
 * the whole eight-line edge is four sample vectors and one pass. tc and the
 * bS == 4 select are per-LANE, derived from the caller's four bS entries, and
 * a bS == 0 line is passed as tc 0 with the strong select clear: its delta
 * clips to zero and the line is rewritten with its own samples.
 */
static inline void y264_db_chroma8_params(const uint8_t bs[4],
                                          const uint8_t tc0tab[3],
                                          int span, int g,
                                          __m128i *tc, __m128i *bs4)
{
    int16_t t[8], m[8];
    for (int i = 0; i < 8; i++) {
        int b = bs[(g * 8 + i) / span];
        t[i] = (int16_t)(b == 0 || b >= 4 ? 0 : tc0tab[b - 1] + 1);
        m[i] = (int16_t)(b >= 4 ? -1 : 0);
    }
    *tc  = _mm_loadu_si128((const __m128i *)(const void *)t);
    *bs4 = _mm_loadu_si128((const __m128i *)(const void *)m);
}

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

/* ---- boundary strengths, 8.7.2.1 ----------------------------------------- *
 *
 * The grid reads are shared: four consecutive entries of an int8 grid widen to
 * four 16-bit lanes without reading a fifth (the grids are exactly wide enough
 * for the macroblock), and the coefficient flag folds with the transform flag
 * of the macroblock the blocks BELONG to.
 */
static inline __m128i y264_db_ld4_s8(const int8_t *p)
{
    return _mm_cvtepi8_epi16(y264_ld32((const uint8_t *)(const void *)p));
}

static inline __m128i y264_db_ld4_s16(const int16_t *p)
{
    return y264_ld64((const uint8_t *)(const void *)p);
}

/* "Has coefficients" for four consecutive 4x4 blocks of row `dy`. Under the
 * 8x8 transform the coded status is the whole quadrant's, so the two rows of
 * the quadrant are OR-ed and then the columns are paired -- which is a swap
 * inside each 32-bit lane. */
static inline __m128i y264_db_co_row(const int8_t *nnz, int s, int dy, int tr8)
{
    const __m128i z = _mm_setzero_si128();
    if (!tr8)
        return _mm_cmpgt_epi16(y264_db_ld4_s8(nnz + dy * s), z);
    int qy = dy & ~1;                       /* -1 -> -2: the quadrant's first row */
    __m128i t = _mm_or_si128(_mm_cmpgt_epi16(y264_db_ld4_s8(nnz + qy * s), z),
                             _mm_cmpgt_epi16(y264_db_ld4_s8(nnz + (qy + 1) * s), z));
    return _mm_or_si128(t, _mm_shufflelo_epi16(t, _MM_SHUFFLE(2, 3, 0, 1)));
}

/* The same for one block, used for the left neighbour column. */
static inline int y264_db_co_at(const int8_t *nnz, int s, int dx, int dy, int tr8)
{
    if (!tr8)
        return nnz[dy * s + dx] > 0;
    int qx = dx & ~1, qy = dy & ~1;
    return nnz[qy * s + qx] > 0 || nnz[qy * s + qx + 1] > 0
        || nnz[(qy + 1) * s + qx] > 0 || nnz[(qy + 1) * s + qx + 1] > 0;
}

/* One row of four 4x4 blocks: the attributes 8.7.2.1 tests, in lanes 0..3. */
struct y264_db_side {
    __m128i r0, r1, x0, y0, x1, y1, co;
};

static inline struct y264_db_side y264_db_load4(const struct y264_bs_ctx *c,
                                                int dy, int dx, __m128i co)
{
    int i = dy * c->mv_stride + dx;
    struct y264_db_side s;
    s.r0 = y264_db_ld4_s8(c->ref0 + i);
    s.r1 = y264_db_ld4_s8(c->ref1 + i);
    s.x0 = y264_db_ld4_s16(c->mvx0 + i);
    s.y0 = y264_db_ld4_s16(c->mvy0 + i);
    s.x1 = y264_db_ld4_s16(c->mvx1 + i);
    s.y1 = y264_db_ld4_s16(c->mvy1 + i);
    s.co = co;
    return s;
}

/* Rotate a row's lanes right by one: (a0,a1,a2,a3) -> (a3,a0,a1,a2). Lane 0 is
 * then the wrong block, and only feeds edges the caller discards. */
static inline __m128i y264_db_rot1(__m128i v)
{
    return _mm_shufflelo_epi16(v, _MM_SHUFFLE(2, 1, 0, 3));
}

static inline struct y264_db_side y264_db_rot4(struct y264_db_side a)
{
    struct y264_db_side s;
    s.r0 = y264_db_rot1(a.r0);
    s.r1 = y264_db_rot1(a.r1);
    s.x0 = y264_db_rot1(a.x0);
    s.y0 = y264_db_rot1(a.y0);
    s.x1 = y264_db_rot1(a.x1);
    s.y1 = y264_db_rot1(a.y1);
    s.co = y264_db_rot1(a.co);
    return s;
}

/* Two rows of four blocks side by side: the eight lanes one pass covers. */
static inline struct y264_db_side y264_db_pair(struct y264_db_side a,
                                               struct y264_db_side b)
{
    struct y264_db_side s;
    s.r0 = _mm_unpacklo_epi64(a.r0, b.r0);
    s.r1 = _mm_unpacklo_epi64(a.r1, b.r1);
    s.x0 = _mm_unpacklo_epi64(a.x0, b.x0);
    s.y0 = _mm_unpacklo_epi64(a.y0, b.y0);
    s.x1 = _mm_unpacklo_epi64(a.x1, b.x1);
    s.y1 = _mm_unpacklo_epi64(a.y1, b.y1);
    s.co = _mm_unpacklo_epi64(a.co, b.co);
    return s;
}

/* The left neighbour's coefficient lane pushed in front of this row's first
 * three: (l, co0, co1, co2). */
static inline __m128i y264_db_co_shift(__m128i co, int l)
{
    __m128i sh = _mm_shufflelo_epi16(co, _MM_SHUFFLE(2, 1, 0, 0));
    return _mm_insert_epi16(sh, l ? 0xffff : 0, 0);
}

/* Eight edges at once. `edge` selects the macroblock-edge lanes, which take
 * bS 4 rather than 3 on the intra branch.
 *
 * "Used in list n" is the complement of "negative refIdx", so one compare per
 * side per list answers both that and the intra test. The motion test is one
 * compare per list and not two: the two components' absolute differences fold
 * with a max before the threshold, which is the same predicate as OR-ing two
 * thresholds and one operation cheaper. */
static inline __m128i y264_db_bs8(const struct y264_db_side *p,
                                  const struct y264_db_side *q, __m128i edge)
{
    const __m128i z = _mm_setzero_si128();
    const __m128i ones = _mm_cmpeq_epi16(z, z);
    const __m128i three = _mm_set1_epi16(3);

    __m128i pn0 = _mm_cmplt_epi16(p->r0, z), pn1 = _mm_cmplt_epi16(p->r1, z);
    __m128i qn0 = _mm_cmplt_epi16(q->r0, z), qn1 = _mm_cmplt_epi16(q->r1, z);
    __m128i intra = _mm_or_si128(_mm_and_si128(pn0, pn1), _mm_and_si128(qn0, qn1));
    __m128i coeff = _mm_or_si128(p->co, q->co);

    __m128i u0 = _mm_xor_si128(pn0, ones);      /* p side uses list 0 */
    __m128i u1 = _mm_xor_si128(pn1, ones);      /* ...and list 1 */

    /* different list membership: the XOR of the two "negative" masks */
    __m128i one = _mm_xor_si128(pn0, qn0);
    one = _mm_or_si128(one, _mm_xor_si128(pn1, qn1));
    /* multi-ref: same list, a different picture in it */
    one = _mm_or_si128(one, _mm_and_si128(u0,
              _mm_xor_si128(_mm_cmpeq_epi16(p->r0, q->r0), ones)));
    one = _mm_or_si128(one, _mm_and_si128(u1,
              _mm_xor_si128(_mm_cmpeq_epi16(p->r1, q->r1), ones)));
    __m128i d0 = _mm_cmpgt_epi16(_mm_max_epi16(y264_abd16(p->x0, q->x0),
                                               y264_abd16(p->y0, q->y0)), three);
    one = _mm_or_si128(one, _mm_and_si128(u0, d0));
    __m128i d1 = _mm_cmpgt_epi16(_mm_max_epi16(y264_abd16(p->x1, q->x1),
                                               y264_abd16(p->y1, q->y1)), three);
    one = _mm_or_si128(one, _mm_and_si128(u1, d1));

    __m128i bs = _mm_and_si128(one, _mm_set1_epi16(1));
    bs = y264_sel16(coeff, _mm_set1_epi16(2), bs);
    bs = y264_sel16(intra, y264_sel16(edge, _mm_set1_epi16(4),
                                      _mm_set1_epi16(3)), bs);
    return _mm_packus_epi16(bs, bs);            /* eight strengths, low half */
}

/* The transpose the vertical result needs: the kernel produces a row of four
 * edges at a time and bsv is stored edge-major. */
#define Y264_DB_TRV \
    _mm_setr_epi8(0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15)

/* ---- whole edges: per-lane strengths -------------------------------------- *
 *
 * The kernels above take one bS value and the lines it covers, so a macroblock
 * edge is four calls (luma) and each call pays its own loads, stores and
 * set-up. The forms below take the whole edge with its four bS values, the
 * way the chroma kernel always has: bS becomes per-LANE values. A lane whose
 * bS is 0 is masked out of the filter decision, a bS 4 lane takes the strong
 * filter and a bS 1-3 lane the normal one with its own tc0; both filters are
 * computed and selected per lane, and a filter no lane of the edge needs is
 * skipped.
 *
 * The bodies are generated per register width (P is the intrinsic prefix, V
 * the register type): eight lanes for the SSE4.2 tier, which runs an edge as
 * two halves, and sixteen for AVX2, which runs it whole.
 */

/* The luma filter over the lanes of one register. `s` is p3 p2 p1 p0 q0 q1 q2
 * q3; `out` receives p2 p1 p0 q0 q1 q2. tc0 is 0 on a lane that is not bS
 * 1-3. */
#define Y264_DB_LUMA_LANES(NAME, P, V)                                        \
static inline void NAME(const V s[8], int alpha, int beta, V tc0, V bs4,      \
                        V bsnz, V out[6], int has_n, int has_4)               \
{                                                                             \
    const V p3 = s[0], p2 = s[1], p1 = s[2], p0 = s[3];                       \
    const V q0 = s[4], q1 = s[5], q2 = s[6], q3 = s[7];                       \
    const V vb = P##_set1_epi16((short)beta);                                 \
    const V z = Y264_DB_ZERO_##P;                                      \
    const V c2 = P##_set1_epi16(2), c4 = P##_set1_epi16(4);                   \
    V dpq = P##_sub_epi16(P##_max_epi16(p0, q0), P##_min_epi16(p0, q0));      \
    V filt = P##_cmpgt_epi16(P##_set1_epi16((short)alpha), dpq);              \
    filt = Y264_DB_AND_##P(filt, P##_cmpgt_epi16(vb,                      \
        P##_sub_epi16(P##_max_epi16(p1, p0), P##_min_epi16(p1, p0))));        \
    filt = Y264_DB_AND_##P(filt, P##_cmpgt_epi16(vb,                      \
        P##_sub_epi16(P##_max_epi16(q1, q0), P##_min_epi16(q1, q0))));        \
    filt = Y264_DB_AND_##P(filt, bsnz);                                   \
    V apb = P##_cmpgt_epi16(vb,                                               \
        P##_sub_epi16(P##_max_epi16(p2, p0), P##_min_epi16(p2, p0)));         \
    V aqb = P##_cmpgt_epi16(vb,                                               \
        P##_sub_epi16(P##_max_epi16(q2, q0), P##_min_epi16(q2, q0)));         \
    out[0] = p2; out[1] = p1; out[2] = p0;                                    \
    out[3] = q0; out[4] = q1; out[5] = q2;                                    \
    if (has_n) {                                                              \
        /* tc = tc0 + (ap < beta) + (aq < beta): the masks are -1 */          \
        V tc = P##_sub_epi16(P##_sub_epi16(tc0, apb), aqb);                   \
        V d = P##_add_epi16(P##_slli_epi16(P##_sub_epi16(q0, p0), 2),         \
                            P##_sub_epi16(p1, q1));                           \
        d = P##_srai_epi16(P##_add_epi16(d, c4), 3);                          \
        d = P##_max_epi16(P##_sub_epi16(z, tc), P##_min_epi16(tc, d));        \
        const V m255 = P##_set1_epi16(255);                                   \
        V p0n = P##_min_epi16(P##_max_epi16(P##_add_epi16(p0, d), z), m255);  \
        V q0n = P##_min_epi16(P##_max_epi16(P##_sub_epi16(q0, d), z), m255);  \
        V av = P##_avg_epu16(p0, q0);                                         \
        V ntc0 = P##_sub_epi16(z, tc0);                                       \
        V dp1 = P##_srai_epi16(P##_sub_epi16(P##_add_epi16(p2, av),           \
                                             P##_slli_epi16(p1, 1)), 1);      \
        dp1 = P##_max_epi16(ntc0, P##_min_epi16(tc0, dp1));                   \
        V dq1 = P##_srai_epi16(P##_sub_epi16(P##_add_epi16(q2, av),           \
                                             P##_slli_epi16(q1, 1)), 1);      \
        dq1 = P##_max_epi16(ntc0, P##_min_epi16(tc0, dq1));                   \
        V fn = Y264_DB_ANDNOT_##P(bs4, filt);                             \
        out[1] = P##_blendv_epi8(p1, P##_add_epi16(p1, dp1),                  \
                                 Y264_DB_AND_##P(fn, apb));               \
        out[2] = P##_blendv_epi8(p0, p0n, fn);                                \
        out[3] = P##_blendv_epi8(q0, q0n, fn);                                \
        out[4] = P##_blendv_epi8(q1, P##_add_epi16(q1, dq1),                  \
                                 Y264_DB_AND_##P(fn, aqb));               \
    }                                                                         \
    if (!has_4)                                                               \
        return;                                                               \
    V strong = P##_cmpgt_epi16(P##_set1_epi16((short)((alpha >> 2) + 2)), dpq);\
    V sp = Y264_DB_AND_##P(strong, apb), sq = Y264_DB_AND_##P(strong, aqb);\
    V pq = P##_add_epi16(p0, q0);                                             \
    V p0s = P##_srai_epi16(P##_add_epi16(P##_add_epi16(P##_add_epi16(p2, q1), \
                P##_slli_epi16(P##_add_epi16(p1, pq), 1)), c4), 3);           \
    V p1s = P##_srai_epi16(P##_add_epi16(P##_add_epi16(P##_add_epi16(p2, p1), \
                pq), c2), 2);                                                 \
    V p2s = P##_srai_epi16(P##_add_epi16(P##_add_epi16(                       \
                P##_add_epi16(P##_slli_epi16(p3, 1),                          \
                              P##_add_epi16(P##_slli_epi16(p2, 1), p2)),      \
                P##_add_epi16(p1, pq)), c4), 3);                              \
    V p0w = P##_srai_epi16(P##_add_epi16(P##_add_epi16(P##_slli_epi16(p1, 1), \
                P##_add_epi16(p0, q1)), c2), 2);                              \
    V q0s = P##_srai_epi16(P##_add_epi16(P##_add_epi16(P##_add_epi16(q2, p1), \
                P##_slli_epi16(P##_add_epi16(q1, pq), 1)), c4), 3);           \
    V q1s = P##_srai_epi16(P##_add_epi16(P##_add_epi16(P##_add_epi16(q2, q1), \
                pq), c2), 2);                                                 \
    V q2s = P##_srai_epi16(P##_add_epi16(P##_add_epi16(                       \
                P##_add_epi16(P##_slli_epi16(q3, 1),                          \
                              P##_add_epi16(P##_slli_epi16(q2, 1), q2)),      \
                P##_add_epi16(q1, pq)), c4), 3);                              \
    V q0w = P##_srai_epi16(P##_add_epi16(P##_add_epi16(P##_slli_epi16(q1, 1), \
                P##_add_epi16(q0, p1)), c2), 2);                              \
    V f4 = Y264_DB_AND_##P(filt, bs4);                                    \
    V f4p = Y264_DB_AND_##P(f4, sp), f4q = Y264_DB_AND_##P(f4, sq);   \
    out[0] = P##_blendv_epi8(out[0], p2s, f4p);                               \
    out[1] = P##_blendv_epi8(out[1], p1s, f4p);                               \
    out[2] = P##_blendv_epi8(out[2], P##_blendv_epi8(p0w, p0s, sp), f4);      \
    out[3] = P##_blendv_epi8(out[3], P##_blendv_epi8(q0w, q0s, sq), f4);      \
    out[4] = P##_blendv_epi8(out[4], q1s, f4q);                               \
    out[5] = P##_blendv_epi8(out[5], q2s, f4q);                               \
}

/* The chroma filter (p0 and q0 only) over the lanes of one register, tc and
 * the bS 4 select per lane: the algebra of y264_db_chroma8_h_x86. */
#define Y264_DB_CHROMA_LANES(NAME, P, V)                                      \
static inline void NAME(V p1, V p0, V q0, V q1, int alpha, int beta, V tc,    \
                        V bs4, V *po, V *qo)                                  \
{                                                                             \
    const V vb = P##_set1_epi16((short)beta);                                 \
    const V z = Y264_DB_ZERO_##P;                                      \
    const V c2 = P##_set1_epi16(2), m255 = P##_set1_epi16(255);               \
    V filt = P##_cmpgt_epi16(P##_set1_epi16((short)alpha),                    \
        P##_sub_epi16(P##_max_epi16(p0, q0), P##_min_epi16(p0, q0)));         \
    filt = Y264_DB_AND_##P(filt, P##_cmpgt_epi16(vb,                      \
        P##_sub_epi16(P##_max_epi16(p1, p0), P##_min_epi16(p1, p0))));        \
    filt = Y264_DB_AND_##P(filt, P##_cmpgt_epi16(vb,                      \
        P##_sub_epi16(P##_max_epi16(q1, q0), P##_min_epi16(q1, q0))));        \
    V d = P##_add_epi16(P##_slli_epi16(P##_sub_epi16(q0, p0), 2),             \
                        P##_sub_epi16(p1, q1));                               \
    d = P##_srai_epi16(P##_add_epi16(d, P##_set1_epi16(4)), 3);               \
    d = P##_max_epi16(P##_sub_epi16(z, tc), P##_min_epi16(tc, d));            \
    V p0w = P##_min_epi16(P##_max_epi16(P##_add_epi16(p0, d), z), m255);      \
    V q0w = P##_min_epi16(P##_max_epi16(P##_sub_epi16(q0, d), z), m255);      \
    V p0s = P##_srai_epi16(P##_add_epi16(P##_add_epi16(P##_slli_epi16(p1, 1), \
                P##_add_epi16(p0, q1)), c2), 2);                              \
    V q0s = P##_srai_epi16(P##_add_epi16(P##_add_epi16(P##_slli_epi16(q1, 1), \
                P##_add_epi16(q0, p1)), c2), 2);                              \
    *po = P##_blendv_epi8(p0, P##_blendv_epi8(p0w, p0s, bs4), filt);          \
    *qo = P##_blendv_epi8(q0, P##_blendv_epi8(q0w, q0s, bs4), filt);          \
}

/* The four operations whose intrinsic names carry the register width instead
 * of the prefix, spelled per prefix so the bodies above can name them. */
#define Y264_DB_ZERO__mm        _mm_setzero_si128()
#define Y264_DB_AND__mm         _mm_and_si128
#define Y264_DB_ANDNOT__mm      _mm_andnot_si128
#define Y264_DB_OR__mm          _mm_or_si128
#define Y264_DB_ZERO__mm256     _mm256_setzero_si256()
#define Y264_DB_AND__mm256      _mm256_and_si256
#define Y264_DB_ANDNOT__mm256   _mm256_andnot_si256
#define Y264_DB_OR__mm256       _mm256_or_si256

Y264_DB_LUMA_LANES(y264_db_luma_lanes8, _mm, __m128i)
Y264_DB_CHROMA_LANES(y264_db_chroma_lanes8, _mm, __m128i)

/* Per-LINE parameters of a sixteen-line luma edge, one byte per line: each of
 * the four bS values covers four lines. tc0 comes from a four-entry table
 * indexed by bS (0 for bS 0 and 4), the two masks from compares. Three bytes
 * of tc0tab are read and no fourth. */
static inline void y264_db_luma16_params(const uint8_t bs[4],
                                         const uint8_t tc0tab[3],
                                         __m128i *tc0, __m128i *bs4,
                                         __m128i *bsnz)
{
    int32_t w;
    memcpy(&w, bs, 4);
    __m128i b = _mm_shuffle_epi8(_mm_cvtsi32_si128(w),
        _mm_setr_epi8(0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3));
    __m128i tab = _mm_cvtsi32_si128((int)((uint32_t)tc0tab[0] << 8 |
                                          (uint32_t)tc0tab[1] << 16 |
                                          (uint32_t)tc0tab[2] << 24));
    *tc0 = _mm_shuffle_epi8(tab, b);
    *bs4 = _mm_cmpgt_epi8(b, _mm_set1_epi8(3));
    *bsnz = _mm_cmpgt_epi8(b, _mm_setzero_si128());
}

/* Which of the two luma filters an edge needs at all: bS is 0-4, so "some bS
 * is 1-3" and "some bS is 4" are two masks of the four bytes. */
static inline void y264_db_luma16_need(const uint8_t bs[4], int *has_n, int *has_4)
{
    uint32_t w;
    memcpy(&w, bs, 4);
    *has_n = (w & 0x03030303u) != 0;
    *has_4 = (w & 0x04040404u) != 0;
}

/* Per-line parameters of an eight-line 4:2:0 chroma edge, laid out twice (the
 * low eight bytes for one plane, the high eight for the other): each bS
 * covers two lines, tc is tc0 + 1 for bS 1-3 and 0 for bS 0 and 4 (a bS 0
 * line then moves nothing). */
static inline void y264_db_chroma420_params(const uint8_t bs[4],
                                            const uint8_t tc0tab[3],
                                            __m128i *tc, __m128i *bs4)
{
    int32_t w;
    memcpy(&w, bs, 4);
    __m128i b = _mm_shuffle_epi8(_mm_cvtsi32_si128(w),
        _mm_setr_epi8(0, 0, 1, 1, 2, 2, 3, 3, 0, 0, 1, 1, 2, 2, 3, 3));
    __m128i tab = _mm_cvtsi32_si128((int)((uint32_t)(tc0tab[0] + 1) << 8 |
                                          (uint32_t)(tc0tab[1] + 1) << 16 |
                                          (uint32_t)(tc0tab[2] + 1) << 24));
    *tc = _mm_shuffle_epi8(tab, b);
    *bs4 = _mm_cmpgt_epi8(b, _mm_set1_epi8(3));
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
