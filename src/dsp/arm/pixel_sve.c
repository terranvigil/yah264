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

#if defined(__clang__)
#pragma clang attribute pop
#endif
