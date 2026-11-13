/*
 * loopfilter_x86_core.h - the deblocking pieces both x86 libraries share
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * What in the x86 deblock family does not depend on the sample width, written
 * once for the 8-bit tier (loopfilter_x86.h) and the 10-bit one
 * (loopfilter_x86_10.h):
 *
 *   - the boundary-strength derivation, which reads the motion and
 *     coefficient grids and never a sample. Its two bodies are here, and each
 *     library's loopfilter_<tier>.c wraps them under the tier's name, so the
 *     10-bit library gets its own copy of the same source.
 *   - the per-lane edge filters, on 16-bit lanes at both depths. Every sum
 *     they form fits int16 at 10 bits as at 8: the widest is the strong
 *     filter's 8 * 1023 + 4, a delta is clipped to tc <= 25 * 4 + 2, and the
 *     clip is to PIXEL_MAX. The callers widen 8-bit samples on the way in; 10-bit
 *     samples arrive as lanes already.
 *   - the per-lane parameters. The callers pass the 8-bit alpha, beta and tc0
 *     table values; at 10 bits 8.7.2.2 scales them by 4, so tc0 is scaled
 *     here and alpha and beta by the 10-bit kernels before the lanes run. The
 *     literal rounding terms stay as they are.
 *
 * C11 intrinsics only.
 */
#ifndef YAH264_DSP_X86_LOOPFILTER_X86_CORE_H
#define YAH264_DSP_X86_LOOPFILTER_X86_CORE_H

#include <immintrin.h>
#include <string.h>

#include "dsp/arch.h"

/* the threshold scale of 8.7.2.2 (0 at 8 bits, where every use folds away) */
#define Y264_DB_SH (Y264_BIT_DEPTH - 8)

/* |a - b| over signed 16-bit lanes (NEON has this as one instruction). */
static inline __m128i y264_abd16(__m128i a, __m128i b)
{
    return _mm_sub_epi16(_mm_max_epi16(a, b), _mm_min_epi16(a, b));
}

/* mask ? x : y, lane-wise (the bit-select the NEON body is written in). */
static inline __m128i y264_sel16(__m128i mask, __m128i x, __m128i y)
{
    return _mm_blendv_epi8(y, x, mask);
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
    int32_t v;
    memcpy(&v, p, 4);
    return _mm_cvtepi8_epi16(_mm_cvtsi32_si128(v));
}

static inline __m128i y264_db_ld4_s16(const int16_t *p)
{
    return _mm_loadl_epi64((const __m128i *)(const void *)p);
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

/* The strength kernel at 128 bits. Sixteen vertical and sixteen horizontal
 * strengths out of one pass over a 5x5 window of the motion and coefficient
 * grids. Every test in 8.7.2.1 is a lane-wise compare, so an axis is two
 * eight-lane passes here -- each pass two macroblock rows of four edges -- and
 * each pass narrows straight into one store.
 *
 * It bakes the FRAME rules in (bS 4 on every macroblock edge, motion
 * threshold 4 on both axes) and compares refIdx lanes directly, so the caller
 * keeps a field picture and a `--weightp 2` duplicate on the C reference. Both
 * paths produce the same strengths; that is what the checkasm row covers, and
 * the dispatcher only chooses which one runs.
 *
 * Forced inline, at 256 bits too: left to itself the compiler keeps the body
 * out of line behind a tail call, and the 8-bit library's code would no longer
 * be what it was when the body sat in loopfilter_sse4.c. */
static inline __attribute__((always_inline))
void y264_db_strength_x86(const struct y264_bs_ctx *c,
                          uint8_t bsv[4][4], uint8_t bsh[4][4])
{
    const int8_t *nnz = c->nnz;
    const int ns = c->nnz_stride;

    /* Columns 0..3 of rows -1..3; hi[0] is the top neighbour's row. */
    __m128i co[5];
    for (int y = 0; y < 4; y++)
        co[y + 1] = y264_db_co_row(nnz, ns, y, c->tr8_cur);
    co[0] = c->have_top ? y264_db_co_row(nnz, ns, -1, c->tr8_top) : co[1];

    struct y264_db_side hi[5];
    hi[0] = y264_db_load4(c, c->have_top ? -1 : 0, 0, co[0]);
    for (int y = 0; y < 4; y++)
        hi[y + 1] = y264_db_load4(c, y, 0, co[y + 1]);

    /* Vertical edges: the p side is the same row shifted one column left, and
     * its coefficient lane comes from the LEFT macroblock's fold. With no left
     * macroblock that column is outside the frame and must not be read, so the
     * p side is rotated out of the row itself -- lane 0 then holds column 3's
     * value, and the xb == 0 edge it feeds is one the caller does not filter
     * and is zeroed below. */
    struct y264_db_side lo[4];
    for (int y = 0; y < 4; y++) {
        if (c->have_left) {
            int l = y264_db_co_at(nnz, ns, -1, y, c->tr8_left);
            lo[y] = y264_db_load4(c, y, -1, y264_db_co_shift(co[y + 1], l));
        } else {
            lo[y] = y264_db_rot4(hi[y + 1]);
        }
    }
    {
        const __m128i ev = _mm_setr_epi16(-1, 0, 0, 0, -1, 0, 0, 0);
        struct y264_db_side p = y264_db_pair(lo[0], lo[1]);
        struct y264_db_side q = y264_db_pair(hi[1], hi[2]);
        __m128i r01 = y264_db_bs8(&p, &q, ev);
        p = y264_db_pair(lo[2], lo[3]);
        q = y264_db_pair(hi[3], hi[4]);
        __m128i r23 = y264_db_bs8(&p, &q, ev);

        __m128i rows = _mm_unpacklo_epi64(r01, r23);
        if (!c->have_left)
            rows = _mm_and_si128(rows, _mm_set1_epi32((int)0xffffff00u));
        _mm_storeu_si128((__m128i *)(void *)&bsv[0][0],
                         _mm_shuffle_epi8(rows, Y264_DB_TRV));
    }

    /* Horizontal edges: the p side is the row above, so no shift is needed. */
    {
        const __m128i eh = _mm_setr_epi16(-1, -1, -1, -1, 0, 0, 0, 0);
        struct y264_db_side p = y264_db_pair(hi[0], hi[1]);
        struct y264_db_side q = y264_db_pair(hi[1], hi[2]);
        __m128i r = y264_db_bs8(&p, &q, eh);
        if (!c->have_top)
            r = _mm_and_si128(r, _mm_set_epi32(0, 0, -1, 0));
        _mm_storel_epi64((__m128i *)(void *)&bsh[0][0], r);

        p = y264_db_pair(hi[2], hi[3]);
        q = y264_db_pair(hi[3], hi[4]);
        _mm_storel_epi64((__m128i *)(void *)&bsh[2][0],
                         y264_db_bs8(&p, &q, _mm_setzero_si128()));
    }
}

#ifdef __AVX2__
/* ---- sixteen edges of an axis, one pass (the AVX2 tier) ------------------- *
 *
 * Sixteen edges of an axis are sixteen 16-bit lanes, so a whole axis is ONE
 * pass where the 128-bit body takes two. The five grid rows are read once as
 * 128-bit quarters and assembled into 256-bit sides, and the sixteen
 * strengths narrow into one store.
 */
struct y264_db_side16 {
    __m256i r0, r1, x0, y0, x1, y1, co;
};

/* Four rows of four blocks into sixteen lanes, row-major -- which is the order
 * bsh wants directly and the order the vertical transpose expects. */
static inline __m256i y264_db_q4(__m128i a, __m128i b, __m128i c, __m128i d)
{
    return _mm256_setr_m128i(_mm_unpacklo_epi64(a, b), _mm_unpacklo_epi64(c, d));
}

static inline struct y264_db_side16 y264_db_quad(const struct y264_db_side *a,
                                                 const struct y264_db_side *b,
                                                 const struct y264_db_side *c,
                                                 const struct y264_db_side *d)
{
    struct y264_db_side16 s;
    s.r0 = y264_db_q4(a->r0, b->r0, c->r0, d->r0);
    s.r1 = y264_db_q4(a->r1, b->r1, c->r1, d->r1);
    s.x0 = y264_db_q4(a->x0, b->x0, c->x0, d->x0);
    s.y0 = y264_db_q4(a->y0, b->y0, c->y0, d->y0);
    s.x1 = y264_db_q4(a->x1, b->x1, c->x1, d->x1);
    s.y1 = y264_db_q4(a->y1, b->y1, c->y1, d->y1);
    s.co = y264_db_q4(a->co, b->co, c->co, d->co);
    return s;
}

static inline __m256i y264_abd16_256(__m256i a, __m256i b)
{
    return _mm256_sub_epi16(_mm256_max_epi16(a, b), _mm256_min_epi16(a, b));
}

/* 8.7.2.1 over sixteen edges, the algebra of y264_db_bs8. */
static inline __m128i y264_db_bs16(const struct y264_db_side16 *p,
                                   const struct y264_db_side16 *q, __m256i edge)
{
    const __m256i z = _mm256_setzero_si256();
    const __m256i ones = _mm256_cmpeq_epi16(z, z);
    const __m256i three = _mm256_set1_epi16(3);

    __m256i pn0 = _mm256_cmpgt_epi16(z, p->r0), pn1 = _mm256_cmpgt_epi16(z, p->r1);
    __m256i qn0 = _mm256_cmpgt_epi16(z, q->r0), qn1 = _mm256_cmpgt_epi16(z, q->r1);
    __m256i intra = _mm256_or_si256(_mm256_and_si256(pn0, pn1),
                                    _mm256_and_si256(qn0, qn1));
    __m256i coeff = _mm256_or_si256(p->co, q->co);

    __m256i u0 = _mm256_xor_si256(pn0, ones);
    __m256i u1 = _mm256_xor_si256(pn1, ones);

    __m256i one = _mm256_xor_si256(pn0, qn0);
    one = _mm256_or_si256(one, _mm256_xor_si256(pn1, qn1));
    one = _mm256_or_si256(one, _mm256_and_si256(u0,
              _mm256_xor_si256(_mm256_cmpeq_epi16(p->r0, q->r0), ones)));
    one = _mm256_or_si256(one, _mm256_and_si256(u1,
              _mm256_xor_si256(_mm256_cmpeq_epi16(p->r1, q->r1), ones)));
    __m256i d0 = _mm256_cmpgt_epi16(
        _mm256_max_epi16(y264_abd16_256(p->x0, q->x0),
                         y264_abd16_256(p->y0, q->y0)), three);
    one = _mm256_or_si256(one, _mm256_and_si256(u0, d0));
    __m256i d1 = _mm256_cmpgt_epi16(
        _mm256_max_epi16(y264_abd16_256(p->x1, q->x1),
                         y264_abd16_256(p->y1, q->y1)), three);
    one = _mm256_or_si256(one, _mm256_and_si256(u1, d1));

    __m256i bs = _mm256_and_si256(one, _mm256_set1_epi16(1));
    bs = _mm256_blendv_epi8(bs, _mm256_set1_epi16(2), coeff);
    bs = _mm256_blendv_epi8(bs, _mm256_blendv_epi8(_mm256_set1_epi16(3),
                                                   _mm256_set1_epi16(4), edge),
                            intra);
    /* PACKUSWB works inside each 128-bit half, so the sixteen strengths land
     * as two eight-byte groups a whole lane apart; one qword permute brings
     * them together in the low half. */
    __m256i pk = _mm256_packus_epi16(bs, bs);
    return _mm256_castsi256_si128(
        _mm256_permute4x64_epi64(pk, _MM_SHUFFLE(3, 1, 2, 0)));
}

/* The strength kernel at 256 bits: the frame rules and the two exemptions of
 * y264_db_strength_x86, one pass per axis. */
static inline __attribute__((always_inline))
void y264_db_strength16_x86(const struct y264_bs_ctx *c,
                            uint8_t bsv[4][4], uint8_t bsh[4][4])
{
    const int8_t *nnz = c->nnz;
    const int ns = c->nnz_stride;

    __m128i co[5];
    for (int y = 0; y < 4; y++)
        co[y + 1] = y264_db_co_row(nnz, ns, y, c->tr8_cur);
    co[0] = c->have_top ? y264_db_co_row(nnz, ns, -1, c->tr8_top) : co[1];

    struct y264_db_side hi[5];
    hi[0] = y264_db_load4(c, c->have_top ? -1 : 0, 0, co[0]);
    for (int y = 0; y < 4; y++)
        hi[y + 1] = y264_db_load4(c, y, 0, co[y + 1]);

    /* With no left macroblock the p column is outside the frame and must not
     * be read, so the p side is rotated out of the row itself; lane 0 then
     * holds column 3's value and feeds an edge the caller does not filter. */
    struct y264_db_side lo[4];
    for (int y = 0; y < 4; y++) {
        if (c->have_left) {
            int l = y264_db_co_at(nnz, ns, -1, y, c->tr8_left);
            lo[y] = y264_db_load4(c, y, -1, y264_db_co_shift(co[y + 1], l));
        } else {
            lo[y] = y264_db_rot4(hi[y + 1]);
        }
    }
    {
        const __m256i ev = _mm256_setr_epi16(-1, 0, 0, 0, -1, 0, 0, 0,
                                             -1, 0, 0, 0, -1, 0, 0, 0);
        struct y264_db_side16 p = y264_db_quad(&lo[0], &lo[1], &lo[2], &lo[3]);
        struct y264_db_side16 q = y264_db_quad(&hi[1], &hi[2], &hi[3], &hi[4]);
        __m128i rows = y264_db_bs16(&p, &q, ev);
        if (!c->have_left)
            rows = _mm_and_si128(rows, _mm_set1_epi32((int)0xffffff00u));
        _mm_storeu_si128((__m128i *)(void *)&bsv[0][0],
                         _mm_shuffle_epi8(rows, Y264_DB_TRV));
    }
    {
        const __m256i eh = _mm256_setr_epi16(-1, -1, -1, -1, 0, 0, 0, 0,
                                             0, 0, 0, 0, 0, 0, 0, 0);
        struct y264_db_side16 p = y264_db_quad(&hi[0], &hi[1], &hi[2], &hi[3]);
        struct y264_db_side16 q = y264_db_quad(&hi[1], &hi[2], &hi[3], &hi[4]);
        __m128i r = y264_db_bs16(&p, &q, eh);
        if (!c->have_top)
            r = _mm_and_si128(r, _mm_set_epi32(-1, -1, -1, 0));
        _mm_storeu_si128((__m128i *)(void *)&bsh[0][0], r);
    }
}
#endif /* __AVX2__ */

/* ---- whole edges: per-lane strengths -------------------------------------- *
 *
 * The forms below take the whole edge with its four bS values: bS becomes
 * per-LANE values. A lane whose bS is 0 is masked out of the filter decision,
 * a bS 4 lane takes the strong filter and a bS 1-3 lane the normal one with
 * its own tc0; both filters are computed and selected per lane, and a filter
 * no lane of the edge needs is skipped.
 *
 * The bodies are generated per register width (P is the intrinsic prefix, V
 * the register type): eight lanes at 128 bits and sixteen at 256.
 */

/* The luma filter over the lanes of one register. `s` is p3 p2 p1 p0 q0 q1 q2
 * q3; `out` receives p2 p1 p0 q0 q1 q2. tc0 is 0 on a lane that is not bS
 * 1-3. alpha, beta and tc0 arrive scaled to the bit depth. */
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
        const V pmax = P##_set1_epi16(PIXEL_MAX);                             \
        V p0n = P##_min_epi16(P##_max_epi16(P##_add_epi16(p0, d), z), pmax);  \
        V q0n = P##_min_epi16(P##_max_epi16(P##_sub_epi16(q0, d), z), pmax);  \
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
 * the bS 4 select per lane: only p0 and q0 move and the bS < 4 clip is
 * tc0 + 1 with no ap/aq term. A bS 0 lane runs with tc 0 and the strong
 * select clear, so its delta clips to zero and it keeps its own samples. */
#define Y264_DB_CHROMA_LANES(NAME, P, V)                                      \
static inline void NAME(V p1, V p0, V q0, V q1, int alpha, int beta, V tc,    \
                        V bs4, V *po, V *qo)                                  \
{                                                                             \
    const V vb = P##_set1_epi16((short)beta);                                 \
    const V z = Y264_DB_ZERO_##P;                                      \
    const V c2 = P##_set1_epi16(2), pmax = P##_set1_epi16(PIXEL_MAX);         \
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
    V p0w = P##_min_epi16(P##_max_epi16(P##_add_epi16(p0, d), z), pmax);      \
    V q0w = P##_min_epi16(P##_max_epi16(P##_sub_epi16(q0, d), z), pmax);      \
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
 * of tc0tab are read and no fourth. A scaled tc0 is at most 25 * 4, still a
 * byte. */
static inline void y264_db_luma16_params(const uint8_t bs[4],
                                         const uint8_t tc0tab[3],
                                         __m128i *tc0, __m128i *bs4,
                                         __m128i *bsnz)
{
    int32_t w;
    memcpy(&w, bs, 4);
    __m128i b = _mm_shuffle_epi8(_mm_cvtsi32_si128(w),
        _mm_setr_epi8(0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3));
    __m128i tab = _mm_cvtsi32_si128((int)((uint32_t)(tc0tab[0] << Y264_DB_SH) << 8 |
                                          (uint32_t)(tc0tab[1] << Y264_DB_SH) << 16 |
                                          (uint32_t)(tc0tab[2] << Y264_DB_SH) << 24));
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
    __m128i tab = _mm_cvtsi32_si128((int)((uint32_t)((tc0tab[0] << Y264_DB_SH) + 1) << 8 |
                                          (uint32_t)((tc0tab[1] << Y264_DB_SH) + 1) << 16 |
                                          (uint32_t)((tc0tab[2] << Y264_DB_SH) + 1) << 24));
    *tc = _mm_shuffle_epi8(tab, b);
    *bs4 = _mm_cmpgt_epi8(b, _mm_set1_epi8(3));
}

/* Per-lane tc and bS 4 mask of an eight-line chroma edge, each of the four bS
 * entries covering `span` consecutive lines (2 for 4:2:0, 4 for 4:2:2), lanes
 * [8g, 8g + 8). */
static inline void y264_db_chroma8_params(const uint8_t bs[4],
                                          const uint8_t tc0tab[3],
                                          int span, int g,
                                          __m128i *tc, __m128i *bs4)
{
    int16_t t[8], m[8];
    for (int i = 0; i < 8; i++) {
        int b = bs[(g * 8 + i) / span];
        t[i] = (int16_t)(b == 0 || b >= 4 ? 0 : (tc0tab[b - 1] << Y264_DB_SH) + 1);
        m[i] = (int16_t)(b >= 4 ? -1 : 0);
    }
    *tc  = _mm_loadu_si128((const __m128i *)(const void *)t);
    *bs4 = _mm_loadu_si128((const __m128i *)(const void *)m);
}

#endif /* YAH264_DSP_X86_LOOPFILTER_X86_CORE_H */
