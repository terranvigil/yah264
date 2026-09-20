/*
 * deblock_x86.h - the building blocks the x86 deblocking kernels share
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Wave 3b of docs/x86-plan.md. Every helper here is `static inline` and
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

#include "dsp/x86/pixel_x86.h"   /* the block-sized loads, and dsp/arch.h */

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

#endif /* YAH264_DSP_X86_DEBLOCK_X86_H */
