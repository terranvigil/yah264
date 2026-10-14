/*
 * pixel_sve.c - SVE pixel metrics (docs/x86-plan.md, "The ARM SVE tier")
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Every kernel here is vector-length agnostic: it is exact at any SVE vector
 * length, 128 bits through 2048, and checkasm runs it at whatever length the
 * CPU (or QEMU) has. What depends on the length is the DISPATCH. These kernels
 * spend one 16-bit lane per pixel column, so at 256 bits a 16-wide block row
 * is one register where NEON needs two, and the transform work halves; at 128
 * bits they do NEON's work with more loads, so y264_pixel_init installs them
 * only on a part whose vectors are at least 256 bits (Y264_CPU_SVE256).
 *
 * The Hadamard shapes need no permute that crosses a 128-bit segment. A
 * horizontal butterfly between neighbouring columns is TRN1/TRN2 at 16 bits
 * between two rows, the next one TRN at 32 bits, the one after that TRN at 64
 * bits; each pairs elements 2k and 2k+1 of its width and nothing else, so the
 * columns of one 4x4 or 8x8 block never leave their group of lanes whatever
 * the vector length. The last butterfly of each transform is folded into a
 * max: |a + b| + |a - b| == 2 * max(|a|, |b|).
 *
 * Columns past the block are loaded as zero (an inactive lane of a predicated
 * load reads nothing and holds zero) and transform to zero, so a register
 * wider than the block costs lanes and never correctness.
 *
 * The feature is selected for this file only, with a target pragma, so no SVE
 * instruction can reach a baseline translation unit.
 */
#include <stdint.h>

#include "../arch.h"

#if defined(__clang__)
#pragma clang attribute push(__attribute__((target("sve"))), apply_to = function)
#else
#pragma GCC target("+sve")
#endif
#include <arm_sve.h>

/* The vector length in bytes, for detection (src/common/cpu.c). Called only
 * once the hwcaps have said SVE exists. */
int y264_sve_cntb(void)
{
    return (int)svcntb();
}

/* ---- SATD ----------------------------------------------------------------
 *
 * One 16-bit lane per column. A band is four rows: the vertical 4-point
 * transform is lane-wise, then each pair of vertical-frequency rows takes the
 * horizontal one inside its groups of four lanes. Per lane, a band adds at
 * most 2 * 2040, so a u16 accumulator holds sixteen bands (a 16x16 at 128 bits
 * puts two column chunks through each lane: eight bands, 32640). */

static inline svint16_t diff_row(svbool_t pg, svbool_t pt,
                                 const uint8_t *a, const uint8_t *b)
{
    return svreinterpret_s16_u16(svsub_u16_x(pt, svld1ub_u16(pg, a),
                                             svld1ub_u16(pg, b)));
}

/* (p + q + 1) >> 1 of two rows, the bi-prediction average, then a - avg. */
static inline svint16_t diff_avg_row(svbool_t pg, svbool_t pt, const uint8_t *a,
                                     const uint8_t *p, const uint8_t *q)
{
    svuint16_t s = svadd_u16_x(pt, svld1ub_u16(pg, p), svld1ub_u16(pg, q));
    svuint16_t avg = svlsr_n_u16_x(pt, svadd_n_u16_x(pt, s, 1), 1);
    return svreinterpret_s16_u16(svsub_u16_x(pt, svld1ub_u16(pg, a), avg));
}

/* The horizontal 4-point transform of rows x and y, groups of four lanes,
 * last stage folded: the result's lanes sum to half of sum |coefficient|. */
static inline svuint16_t had4h_max(svbool_t pt, svint16_t x, svint16_t y)
{
    svint16_t a = svtrn1_s16(x, y), b = svtrn2_s16(x, y);
    svint32_t s = svreinterpret_s32_s16(svadd_s16_x(pt, a, b));
    svint32_t d = svreinterpret_s32_s16(svsub_s16_x(pt, a, b));
    svint16_t c = svreinterpret_s16_s32(svtrn1_s32(s, d));
    svint16_t e = svreinterpret_s16_s32(svtrn2_s32(s, d));
    return svmax_u16_x(pt, svreinterpret_u16_s16(svabs_s16_x(pt, c)),
                           svreinterpret_u16_s16(svabs_s16_x(pt, e)));
}

static inline svuint16_t satd_band(svbool_t pt, svint16_t r0, svint16_t r1,
                                   svint16_t r2, svint16_t r3)
{
    svint16_t s01 = svadd_s16_x(pt, r0, r1), d01 = svsub_s16_x(pt, r0, r1);
    svint16_t s23 = svadd_s16_x(pt, r2, r3), d23 = svsub_s16_x(pt, r2, r3);
    svint16_t v0 = svadd_s16_x(pt, s01, s23), v1 = svsub_s16_x(pt, s01, s23);
    svint16_t v2 = svadd_s16_x(pt, d01, d23), v3 = svsub_s16_x(pt, d01, d23);
    return svadd_u16_x(pt, had4h_max(pt, v0, v1), had4h_max(pt, v2, v3));
}

/* The rows of one column chunk, pointers stepped a row at a time: SVE loads
 * have no post-increment form, and stepping keeps it to one add per stream per
 * row where precomputed row addresses cost a register and an add each. */
static inline svuint16_t satd_cols(svbool_t pg, svbool_t pt, const uint8_t *a,
                                   int as, const uint8_t *b, int bs, int h)
{
    svuint16_t acc = svdup_n_u16(0);
    for (int y = 0; y < h; y += 4) {
        svint16_t r0 = diff_row(pg, pt, a, b); a += as; b += bs;
        svint16_t r1 = diff_row(pg, pt, a, b); a += as; b += bs;
        svint16_t r2 = diff_row(pg, pt, a, b); a += as; b += bs;
        svint16_t r3 = diff_row(pg, pt, a, b); a += as; b += bs;
        acc = svadd_u16_x(pt, acc, satd_band(pt, r0, r1, r2, r3));
    }
    return acc;
}

static inline svuint16_t satd_avg_cols(svbool_t pg, svbool_t pt, const uint8_t *a,
                                       int as, const uint8_t *p, const uint8_t *q,
                                       int ps, int h)
{
    svuint16_t acc = svdup_n_u16(0);
    for (int y = 0; y < h; y += 4) {
        svint16_t r0 = diff_avg_row(pg, pt, a, p, q); a += as; p += ps; q += ps;
        svint16_t r1 = diff_avg_row(pg, pt, a, p, q); a += as; p += ps; q += ps;
        svint16_t r2 = diff_avg_row(pg, pt, a, p, q); a += as; p += ps; q += ps;
        svint16_t r3 = diff_avg_row(pg, pt, a, p, q); a += as; p += ps; q += ps;
        acc = svadd_u16_x(pt, acc, satd_band(pt, r0, r1, r2, r3));
    }
    return acc;
}

/* SATD of a w x h block, w a multiple of 4 and h of 4. A block that fits one
 * register's lanes takes the straight path; narrower vectors walk it in
 * column chunks. Both are exact; only the first is ever dispatched. */
static inline int satd_wxh(const uint8_t *a, int as, const uint8_t *b, int bs,
                           int w, int h)
{
    const svbool_t pt = svptrue_b16();
    if (w <= (int)svcnth())
        return 2 * (int)svaddv_u16(pt, satd_cols(svwhilelt_b16_s32(0, w), pt,
                                                 a, as, b, bs, h));
    svuint16_t acc = svdup_n_u16(0);
    for (int x = 0; x < w; x += (int)svcnth())
        acc = svadd_u16_x(pt, acc, satd_cols(svwhilelt_b16_s32(x, w), pt,
                                             a + x, as, b + x, bs, h));
    return 2 * (int)svaddv_u16(pt, acc);
}

static inline int satd_avg_wxh(const uint8_t *a, int as, const uint8_t *p,
                               const uint8_t *q, int ps, int w, int h)
{
    const svbool_t pt = svptrue_b16();
    if (w <= (int)svcnth())
        return 2 * (int)svaddv_u16(pt, satd_avg_cols(svwhilelt_b16_s32(0, w), pt,
                                                     a, as, p, q, ps, h));
    svuint16_t acc = svdup_n_u16(0);
    for (int x = 0; x < w; x += (int)svcnth())
        acc = svadd_u16_x(pt, acc, satd_avg_cols(svwhilelt_b16_s32(x, w), pt,
                                                 a + x, as, p + x, q + x, ps, h));
    return 2 * (int)svaddv_u16(pt, acc);
}

int y264_satd_16x16_sve(const uint8_t *a, int as, const uint8_t *b, int bs)
{
    return satd_wxh(a, as, b, bs, 16, 16);
}

int y264_satd_16x8_sve(const uint8_t *a, int as, const uint8_t *b, int bs)
{
    return satd_wxh(a, as, b, bs, 16, 8);
}

int y264_satd_avg_16x16_sve(const uint8_t *a, int as, const uint8_t *p,
                            const uint8_t *q, int ps)
{
    return satd_avg_wxh(a, as, p, q, ps, 16, 16);
}

int y264_satd_avg_16x8_sve(const uint8_t *a, int as, const uint8_t *p,
                           const uint8_t *q, int ps)
{
    return satd_avg_wxh(a, as, p, q, ps, 16, 8);
}

/* ---- the batched 8x8 SATD ------------------------------------------------
 *
 * Two candidates side by side at 256 bits: lanes 0..7 hold reference k's row
 * and lanes 8..15 reference k+1's, joined by an OR from two predicated loads
 * (the second from 8 bytes before its row, so lane 8 lands on its column 0),
 * against the source row loaded once and copied into the upper half by a
 * quadword DUP. The 4x4 transforms never cross a group of four lanes, so the
 * two candidates stay apart and reduce from their own halves. Lanes past 16
 * may hold the source's copy on wider vectors; they are never reduced, and
 * no butterfly carries them into the low sixteen. Below 16 lanes each
 * candidate takes a register of its own. */

static inline svint16_t diff_pair_row(svbool_t pt, svbool_t pl, svbool_t ph,
                                      svuint16_t f, const uint8_t *ra,
                                      const uint8_t *rb8)
{
    svuint16_t r = svorr_u16_x(pt, svld1ub_u16(pl, ra), svld1ub_u16(ph, rb8));
    return svreinterpret_s16_u16(svsub_u16_x(pt, f, r));
}

void y264_satd_x4_8x8_sve(const uint8_t *src, int ss, const uint8_t *r0,
                          const uint8_t *r1, const uint8_t *r2,
                          const uint8_t *r3, int rs, int scores[4])
{
    const svbool_t pt = svptrue_b16();
    const svbool_t pl = svwhilelt_b16_s32(0, 8);
    if (svcnth() < 16) {
        scores[0] = 2 * (int)svaddv_u16(pt, satd_cols(pl, pt, src, ss, r0, rs, 8));
        scores[1] = 2 * (int)svaddv_u16(pt, satd_cols(pl, pt, src, ss, r1, rs, 8));
        scores[2] = 2 * (int)svaddv_u16(pt, satd_cols(pl, pt, src, ss, r2, rs, 8));
        scores[3] = 2 * (int)svaddv_u16(pt, satd_cols(pl, pt, src, ss, r3, rs, 8));
        return;
    }
    const svbool_t ph = svbic_b_z(pt, svwhilelt_b16_s32(0, 16), pl);
    const uint8_t *q1 = r1 - 8, *q3 = r3 - 8;
    svuint16_t acc01 = svdup_n_u16(0), acc23 = acc01;
    ptrdiff_t os = 0, orf = 0;
#define PAIR_ROWS(av, bv) do {                                             \
        svuint16_t f = svdupq_lane_u16(svld1ub_u16(pl, src + os), 0);       \
        av = diff_pair_row(pt, pl, ph, f, r0 + orf, q1 + orf);              \
        bv = diff_pair_row(pt, pl, ph, f, r2 + orf, q3 + orf);              \
        os += ss; orf += rs; } while (0)
    for (int y = 0; y < 8; y += 4) {
        svint16_t a0, a1, a2, a3, b0, b1, b2, b3;
        PAIR_ROWS(a0, b0); PAIR_ROWS(a1, b1); PAIR_ROWS(a2, b2); PAIR_ROWS(a3, b3);
        acc01 = svadd_u16_x(pt, acc01, satd_band(pt, a0, a1, a2, a3));
        acc23 = svadd_u16_x(pt, acc23, satd_band(pt, b0, b1, b2, b3));
    }
#undef PAIR_ROWS
    scores[0] = 2 * (int)svaddv_u16(pl, acc01);
    scores[1] = 2 * (int)svaddv_u16(ph, acc01);
    scores[2] = 2 * (int)svaddv_u16(pl, acc23);
    scores[3] = 2 * (int)svaddv_u16(ph, acc23);
}

/* ---- SA8D ----------------------------------------------------------------
 *
 * The 8x8 transform the same way: eight rows, the vertical 8-point transform
 * lane-wise, then the horizontal one inside groups of eight lanes with TRN at
 * 16, 32 and 64 bits, the last stage folded into a max. A group of eight
 * lanes is one 8x8 block's columns, so the two quadrants of a 16-wide band are
 * the low and high halves of the band's lanes and reduce apart: each 8x8 is
 * rounded on its own, as the reference rounds it. Per lane a band adds four
 * maxes of at most 8160, 32640 in all, so it reduces before the next band. */

static inline svuint16_t had8h_max(svbool_t pt, svint16_t x, svint16_t y)
{
    svint16_t a = svtrn1_s16(x, y), b = svtrn2_s16(x, y);
    svint32_t s = svreinterpret_s32_s16(svadd_s16_x(pt, a, b));
    svint32_t d = svreinterpret_s32_s16(svsub_s16_x(pt, a, b));
    svint16_t c = svreinterpret_s16_s32(svtrn1_s32(s, d));
    svint16_t e = svreinterpret_s16_s32(svtrn2_s32(s, d));
    svint64_t p = svreinterpret_s64_s16(svadd_s16_x(pt, c, e));
    svint64_t q = svreinterpret_s64_s16(svsub_s16_x(pt, c, e));
    svint16_t f = svreinterpret_s16_s64(svtrn1_s64(p, q));
    svint16_t g = svreinterpret_s16_s64(svtrn2_s64(p, q));
    return svmax_u16_x(pt, svreinterpret_u16_s16(svabs_s16_x(pt, f)),
                           svreinterpret_u16_s16(svabs_s16_x(pt, g)));
}

/* Half the 2-D 8x8 abs-sum of every block in an eight-row band, per lane. */
static inline svuint16_t sa8d_band(svbool_t pg, svbool_t pt, const uint8_t *a,
                                   int as, const uint8_t *b, int bs)
{
    svint16_t r0 = diff_row(pg, pt, a, b); a += as; b += bs;
    svint16_t r1 = diff_row(pg, pt, a, b); a += as; b += bs;
    svint16_t r2 = diff_row(pg, pt, a, b); a += as; b += bs;
    svint16_t r3 = diff_row(pg, pt, a, b); a += as; b += bs;
    svint16_t r4 = diff_row(pg, pt, a, b); a += as; b += bs;
    svint16_t r5 = diff_row(pg, pt, a, b); a += as; b += bs;
    svint16_t r6 = diff_row(pg, pt, a, b); a += as; b += bs;
    svint16_t r7 = diff_row(pg, pt, a, b);
    svint16_t a0 = svadd_s16_x(pt, r0, r1), a1 = svsub_s16_x(pt, r0, r1);
    svint16_t a2 = svadd_s16_x(pt, r2, r3), a3 = svsub_s16_x(pt, r2, r3);
    svint16_t a4 = svadd_s16_x(pt, r4, r5), a5 = svsub_s16_x(pt, r4, r5);
    svint16_t a6 = svadd_s16_x(pt, r6, r7), a7 = svsub_s16_x(pt, r6, r7);
    svint16_t b0 = svadd_s16_x(pt, a0, a2), b2 = svsub_s16_x(pt, a0, a2);
    svint16_t b1 = svadd_s16_x(pt, a1, a3), b3 = svsub_s16_x(pt, a1, a3);
    svint16_t b4 = svadd_s16_x(pt, a4, a6), b6 = svsub_s16_x(pt, a4, a6);
    svint16_t b5 = svadd_s16_x(pt, a5, a7), b7 = svsub_s16_x(pt, a5, a7);
    svint16_t c0 = svadd_s16_x(pt, b0, b4), c4 = svsub_s16_x(pt, b0, b4);
    svint16_t c1 = svadd_s16_x(pt, b1, b5), c5 = svsub_s16_x(pt, b1, b5);
    svint16_t c2 = svadd_s16_x(pt, b2, b6), c6 = svsub_s16_x(pt, b2, b6);
    svint16_t c3 = svadd_s16_x(pt, b3, b7), c7 = svsub_s16_x(pt, b3, b7);
    return svadd_u16_x(pt, svadd_u16_x(pt, had8h_max(pt, c0, c1), had8h_max(pt, c2, c3)),
                           svadd_u16_x(pt, had8h_max(pt, c4, c5), had8h_max(pt, c6, c7)));
}

/* The two 8x8 SA8Ds of one band whose columns x.. sit in the lanes, rounded
 * each: lanes of column < 8 are the left block, 8..15 the right. */
static inline int sa8d_band_pair(svbool_t pt, svuint16_t acc, int x)
{
    svbool_t pl = svwhilelt_b16_s32(x, 8);
    svbool_t pr = svbic_b_z(pt, svwhilelt_b16_s32(x, 16), pl);
    return (int)((2 * svaddv_u16(pl, acc) + 2) >> 2)
         + (int)((2 * svaddv_u16(pr, acc) + 2) >> 2);
}

int y264_sa8d_16x16_sve(const uint8_t *a, int as, const uint8_t *b, int bs)
{
    const svbool_t pt = svptrue_b16();
    if (16 <= (int)svcnth()) {
        const svbool_t pg = svwhilelt_b16_s32(0, 16);
        return sa8d_band_pair(pt, sa8d_band(pg, pt, a, as, b, bs), 0)
             + sa8d_band_pair(pt, sa8d_band(pg, pt, a + 8 * as, as, b + 8 * bs, bs), 0);
    }
    /* narrower vectors: one chunk of columns at a time, and a chunk never
     * straddles the two blocks because the lane count is a multiple of 8 */
    int sum = 0;
    for (int x = 0; x < 16; x += (int)svcnth()) {
        const svbool_t pg = svwhilelt_b16_s32(x, 16);
        sum += sa8d_band_pair(pt, sa8d_band(pg, pt, a + x, as, b + x, bs), x);
        sum += sa8d_band_pair(pt, sa8d_band(pg, pt, a + 8 * as + x, as,
                                            b + 8 * bs + x, bs), x);
    }
    return sum;
}

/* ---- the psy texture terms -----------------------------------------------
 *
 * Both texture terms of a 16x16 from one transform of the pixels. Each 4x4
 * tile's Hadamard is taken in full (no folded last stage, because the 8x8
 * term is built from it): a band's vertical transform lane-wise, the
 * horizontal one by TRN at 16 and 32 bits, leaving a tile's sixteen
 * coefficients in its four lanes of four registers, its DC in lane 4k of the
 * first. An 8x8 Hadamard is the four tile transforms of its quadrant combined
 * by one 2x2 butterfly per coefficient position, so the 8x8 term is a band
 * pair's lane-wise sum and difference, then TRN at 64 bits across the two
 * tiles of each group of eight lanes, folded into a max.
 *
 * Ranges per lane (pixels, not differences): a tile coefficient is at most
 * 4080 and a band adds four, so four bands reach 65280 in a u16; an 8x8
 * coefficient is at most 16320 and a band pair adds four maxes, 65280 again,
 * so each band pair reduces on its own. */

/* The horizontal 4-point transform of rows x and y in full: f and g hold the
 * eight coefficients, f's lane 4k the DC of x's group k. */
static inline void had4h_full(svbool_t pt, svint16_t x, svint16_t y,
                              svint16_t *f, svint16_t *g)
{
    svint16_t a = svtrn1_s16(x, y), b = svtrn2_s16(x, y);
    svint32_t sm = svreinterpret_s32_s16(svadd_s16_x(pt, a, b));
    svint32_t df = svreinterpret_s32_s16(svsub_s16_x(pt, a, b));
    svint16_t c = svreinterpret_s16_s32(svtrn1_s32(sm, df));
    svint16_t e = svreinterpret_s16_s32(svtrn2_s32(sm, df));
    *f = svadd_s16_x(pt, c, e);
    *g = svsub_s16_x(pt, c, e);
}

/* Every tile coefficient of a four-row band of pixels; f0's lane 4k is tile
 * k's DC (its pixel sum). */
static inline void tile_band(svbool_t pg, svbool_t pt, const uint8_t *p, int s,
                             svint16_t *f0, svint16_t *g0, svint16_t *f1, svint16_t *g1)
{
    svint16_t r0 = svreinterpret_s16_u16(svld1ub_u16(pg, p)); p += s;
    svint16_t r1 = svreinterpret_s16_u16(svld1ub_u16(pg, p)); p += s;
    svint16_t r2 = svreinterpret_s16_u16(svld1ub_u16(pg, p)); p += s;
    svint16_t r3 = svreinterpret_s16_u16(svld1ub_u16(pg, p));
    svint16_t s01 = svadd_s16_x(pt, r0, r1), d01 = svsub_s16_x(pt, r0, r1);
    svint16_t s23 = svadd_s16_x(pt, r2, r3), d23 = svsub_s16_x(pt, r2, r3);
    had4h_full(pt, svadd_s16_x(pt, s01, s23), svsub_s16_x(pt, s01, s23), f0, g0);
    had4h_full(pt, svadd_s16_x(pt, d01, d23), svsub_s16_x(pt, d01, d23), f1, g1);
}

static inline svuint16_t abs_u16(svbool_t pt, svint16_t x)
{
    return svreinterpret_u16_s16(svabs_s16_x(pt, x));
}

/* The 8x8 term's contribution of one pair of vertically combined tile
 * coefficient registers: TRN at 64 bits pairs each tile with its neighbour in
 * the group of eight lanes, and the last butterfly folds into a max. */
static inline svuint16_t had8_join_max(svbool_t pt, svint16_t x, svint16_t y)
{
    svint64_t x64 = svreinterpret_s64_s16(x), y64 = svreinterpret_s64_s16(y);
    return svmax_u16_x(pt, abs_u16(pt, svreinterpret_s16_s64(svtrn1_s64(x64, y64))),
                           abs_u16(pt, svreinterpret_s16_s64(svtrn2_s64(x64, y64))));
}

/* The texture terms of the columns in pg's lanes (a whole number of 8x8
 * blocks), rows 0..15: e4 is the 4x4 term, t8 the 8x8 abs-sum, dcs the sum
 * of the tile DCs. */
__attribute__((always_inline))
static inline void tex48_cols(svbool_t pg, svbool_t pt, svbool_t pdc,
                              const uint8_t *p, int s, long *e4, long *t8, long *dcs)
{
    svuint16_t acc4 = svdup_n_u16(0), acct = acc4, accd = acc4;
    uint64_t t8s = 0;
    for (int bp = 0; bp < 2; bp++) {
        svint16_t f0, g0, f1, g1, h0, k0, h1, k1;
        tile_band(pg, pt, p, s, &f0, &g0, &f1, &g1);
        tile_band(pg, pt, p + 4 * s, s, &h0, &k0, &h1, &k1);
        acc4 = svadd_u16_x(pt, acc4, svadd_u16_x(pt,
                   svadd_u16_x(pt, abs_u16(pt, f0), abs_u16(pt, g0)),
                   svadd_u16_x(pt, abs_u16(pt, f1), abs_u16(pt, g1))));
        acc4 = svadd_u16_x(pt, acc4, svadd_u16_x(pt,
                   svadd_u16_x(pt, abs_u16(pt, h0), abs_u16(pt, k0)),
                   svadd_u16_x(pt, abs_u16(pt, h1), abs_u16(pt, k1))));
        /* each tile's DC against a flat block of its own rounded mean */
        svuint16_t d0 = svreinterpret_u16_s16(f0), d1 = svreinterpret_u16_s16(h0);
        svuint16_t fl0 = svand_n_u16_x(pt, svadd_n_u16_x(pt, d0, 8), 0xfff0);
        svuint16_t fl1 = svand_n_u16_x(pt, svadd_n_u16_x(pt, d1, 8), 0xfff0);
        acct = svadd_u16_m(pdc, acct, svabd_u16_x(pt, d0, fl0));
        acct = svadd_u16_m(pdc, acct, svabd_u16_x(pt, d1, fl1));
        accd = svadd_u16_m(pdc, accd, svadd_u16_x(pt, d0, d1));
        /* the 8x8 term: quadrant tiles above and below, then left and right */
        svuint16_t m = svadd_u16_x(pt,
            had8_join_max(pt, svadd_s16_x(pt, f0, h0), svsub_s16_x(pt, f0, h0)),
            had8_join_max(pt, svadd_s16_x(pt, g0, k0), svsub_s16_x(pt, g0, k0)));
        m = svadd_u16_x(pt, m, svadd_u16_x(pt,
            had8_join_max(pt, svadd_s16_x(pt, f1, h1), svsub_s16_x(pt, f1, h1)),
            had8_join_max(pt, svadd_s16_x(pt, g1, k1), svsub_s16_x(pt, g1, k1))));
        t8s += svaddv_u16(pt, m);
        p += 8 * s;
    }
    uint64_t d = svaddv_u16(pt, accd);
    *e4 += (long)svaddv_u16(pt, acc4) + (long)svaddv_u16(pt, acct) - (long)d;
    *t8 += 2 * (long)t8s;
    *dcs += (long)d;
}

void y264_texture_ac48_16x16_sve(const uint8_t *p, int stride, long out[2])
{
    const svbool_t pt = svptrue_b16();
    /* lane 4k of every group: where a tile's DC lands */
    const svbool_t pdc = svcmpeq_n_u16(pt, svand_n_u16_x(pt, svindex_u16(0, 1), 3), 0);
    long e4 = 0, t8 = 0, dcs = 0;
    if (16 <= (int)svcnth()) {
        tex48_cols(svwhilelt_b16_s32(0, 16), pt, pdc, p, stride, &e4, &t8, &dcs);
    } else {
        for (int x = 0; x < 16; x += (int)svcnth())
            tex48_cols(svwhilelt_b16_s32(x, 16), pt, pdc, p + x, stride, &e4, &t8, &dcs);
    }
    out[0] = e4;
    out[1] = t8 - dcs;
}

#if defined(__clang__)
#pragma clang attribute pop
#endif
