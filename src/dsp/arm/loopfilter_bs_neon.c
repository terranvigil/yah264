/*
 * loopfilter_bs_neon.c - aarch64 NEON boundary strengths (ITU-T H.264 8.7.2.1)
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The strength derivation reads the motion and coefficient grids and never a
 * sample, so one source serves both libraries: the 8-bit one compiles it as
 * y264_deblock_strength_neon and the 10-bit one, through symbols10.h, as
 * y264_deblock_strength_neon_10.
 */
#if defined(__aarch64__)
#include <arm_neon.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "../loopfilter.h"

/* ------------------------------------------------------------------ *
 * Boundary strengths, one macroblock per call (8.7.2.1).
 *
 * The scalar shape derived one edge at a time behind a lazy memo: a call, six
 * strided grid reads and a chain of early-outs, up to thirty-two times per
 * macroblock. Every test in 8.7.2.1 is a lane-wise compare, so all sixteen
 * vertical strengths fall out of two eight-lane passes and the sixteen
 * horizontal ones out of two more. The eight lanes of a pass are two
 * macroblock rows of four edges, which is why the results narrow straight into
 * a single eight-byte store.
 *
 * The only part that is not a plain grid read is the coefficient flag: under
 * the 8x8 transform a 4x4 block's coded status is its whole quadrant's, so the
 * mask is folded with vrev32 (which swaps the lanes inside each 32-bit pair)
 * before it is used, and the left column and top row fold with their OWN
 * macroblock's flag rather than this one's.
 * ------------------------------------------------------------------ */

/* Four bytes of an int8 grid widened to int16 lanes, without reading a fifth:
 * the grids are exactly wide enough for the macroblock and an 8-byte load off
 * the last one would run past the row. */
static inline int16x4_t db_ld4_s8(const int8_t *p)
{
    int32_t w;
    memcpy(&w, p, 4);
    return vget_low_s16(vmovl_s8(vreinterpret_s8_s32(vdup_n_s32(w))));
}

/* "Has coefficients" for four consecutive 4x4 blocks of row `dy`, under the
 * transform flag of the macroblock those blocks belong to. */
static inline uint16x4_t db_co_row(const int8_t *nnz, int s, int dy, int tr8)
{
    const int16x4_t z = vdup_n_s16(0);
    if (!tr8)
        return vcgt_s16(db_ld4_s8(nnz + dy * s), z);
    int qy = dy & ~1;                       /* -1 -> -2: the quadrant's first row */
    uint16x4_t t = vorr_u16(vcgt_s16(db_ld4_s8(nnz + qy * s), z),
                            vcgt_s16(db_ld4_s8(nnz + (qy + 1) * s), z));
    return vorr_u16(t, vrev32_u16(t));      /* pair the columns */
}

/* The same for one block, used for the left neighbour column. */
static inline int db_co_at(const int8_t *nnz, int s, int dx, int dy, int tr8)
{
    if (!tr8)
        return nnz[dy * s + dx] > 0;
    int qx = dx & ~1, qy = dy & ~1;
    return nnz[qy * s + qx] > 0 || nnz[qy * s + qx + 1] > 0
        || nnz[(qy + 1) * s + qx] > 0 || nnz[(qy + 1) * s + qx + 1] > 0;
}

struct db_side4 {
    int16x4_t r0, r1, x0, y0, x1, y1;
    uint16x4_t co;
};
struct db_side8 {
    int16x8_t r0, r1, x0, y0, x1, y1;
    uint16x8_t co;
};

static inline struct db_side4 db_load4(const struct y264_bs_ctx *c, int dy,
                                       int dx, uint16x4_t co)
{
    int i = dy * c->mv_stride + dx;
    struct db_side4 s;
    s.r0 = db_ld4_s8(c->ref0 + i);
    s.r1 = db_ld4_s8(c->ref1 + i);
    s.x0 = vld1_s16(c->mvx0 + i);
    s.y0 = vld1_s16(c->mvy0 + i);
    s.x1 = vld1_s16(c->mvx1 + i);
    s.y1 = vld1_s16(c->mvy1 + i);
    s.co = co;
    return s;
}

/* Rotate a row's lanes right by one: (a0,a1,a2,a3) -> (a3,a0,a1,a2). Lane 0 is
 * then the wrong block, and only feeds edges the caller discards. */
static inline struct db_side4 db_rot4(struct db_side4 a)
{
    struct db_side4 s;
    s.r0 = vext_s16(a.r0, a.r0, 3);
    s.r1 = vext_s16(a.r1, a.r1, 3);
    s.x0 = vext_s16(a.x0, a.x0, 3);
    s.y0 = vext_s16(a.y0, a.y0, 3);
    s.x1 = vext_s16(a.x1, a.x1, 3);
    s.y1 = vext_s16(a.y1, a.y1, 3);
    s.co = vext_u16(a.co, a.co, 3);
    return s;
}

static inline struct db_side8 db_pair(struct db_side4 a, struct db_side4 b)
{
    struct db_side8 s;
    s.r0 = vcombine_s16(a.r0, b.r0);
    s.r1 = vcombine_s16(a.r1, b.r1);
    s.x0 = vcombine_s16(a.x0, b.x0);
    s.y0 = vcombine_s16(a.y0, b.y0);
    s.x1 = vcombine_s16(a.x1, b.x1);
    s.y1 = vcombine_s16(a.y1, b.y1);
    s.co = vcombine_u16(a.co, b.co);
    return s;
}

/* Eight edges at once. `edge` selects the macroblock-edge lanes (bS 4 rather
 * than 3 on the intra branch). */
static inline uint8x8_t db_bs8(const struct db_side8 *p, const struct db_side8 *q,
                               uint16x8_t edge)
{
    const int16x8_t z = vdupq_n_s16(0), four = vdupq_n_s16(4);
    /* "used in list n" is the complement of "negative refIdx", so one compare
     * per side per list answers both questions: the eight compares this used
     * to take are four, and the intra test reuses them too. */
    uint16x8_t pn0 = vcltq_s16(p->r0, z), pn1 = vcltq_s16(p->r1, z);
    uint16x8_t qn0 = vcltq_s16(q->r0, z), qn1 = vcltq_s16(q->r1, z);
    uint16x8_t intra = vorrq_u16(vandq_u16(pn0, pn1), vandq_u16(qn0, qn1));
    uint16x8_t coeff = vorrq_u16(p->co, q->co);

    uint16x8_t p0 = vmvnq_u16(pn0), p1 = vmvnq_u16(pn1);
    /* different list membership: the XOR of the two "negative" masks is the
     * XOR of their complements, so the mvn is not on this path. */
    uint16x8_t one = veorq_u16(pn0, qn0);
    one = vorrq_u16(one, veorq_u16(pn1, qn1));
    /* multi-ref: same list, different list-0 picture */
    one = vorrq_u16(one, vandq_u16(p0, vmvnq_u16(vceqq_s16(p->r0, q->r0))));
    one = vorrq_u16(one, vandq_u16(p1, vmvnq_u16(vceqq_s16(p->r1, q->r1))));   /* and list 1 */
    /* The motion test is one compare per list, not two: the two components'
     * absolute differences are folded with a max before the threshold, which
     * is the same predicate as OR-ing two thresholds and one op cheaper. */
    uint16x8_t d0 = vcgeq_s16(vmaxq_s16(vabdq_s16(p->x0, q->x0),
                                        vabdq_s16(p->y0, q->y0)), four);
    one = vorrq_u16(one, vandq_u16(p0, d0));
    uint16x8_t d1 = vcgeq_s16(vmaxq_s16(vabdq_s16(p->x1, q->x1),
                                        vabdq_s16(p->y1, q->y1)), four);
    one = vorrq_u16(one, vandq_u16(p1, d1));

    int16x8_t bs = vreinterpretq_s16_u16(vandq_u16(one, vdupq_n_u16(1)));
    bs = vbslq_s16(coeff, vdupq_n_s16(2), bs);
    bs = vbslq_s16(intra, vbslq_s16(edge, vdupq_n_s16(4), vdupq_n_s16(3)), bs);
    return vmovn_u16(vreinterpretq_u16_s16(bs));
}

void y264_deblock_strength_neon(const struct y264_bs_ctx *c,
                                uint8_t bsv[4][4], uint8_t bsh[4][4])
{
    const int8_t *nnz = c->nnz;
    const int ns = c->nnz_stride;

    /* Columns 0..3 of rows -1..3; hi[0] is the top neighbour's row. */
    uint16x4_t co[5];
    for (int y = 0; y < 4; y++)
        co[y + 1] = db_co_row(nnz, ns, y, c->tr8_cur);
    co[0] = c->have_top ? db_co_row(nnz, ns, -1, c->tr8_top) : co[1];
    struct db_side4 hi[5];
    hi[0] = db_load4(c, c->have_top ? -1 : 0, 0, co[0]);
    for (int y = 0; y < 4; y++)
        hi[y + 1] = db_load4(c, y, 0, co[y + 1]);

    /* Vertical edges: the p side is the same row shifted one column left, and
 * its coefficient lane comes from the LEFT macroblock's fold. With no left
 * macroblock that column is outside the frame and must not be read, so the
 * p side is rotated out of the row itself -- lane 0 then holds column 3's
 * value, and the xb == 0 edge it feeds is one the caller does not filter
 * and is zeroed below. */
    struct db_side4 lo[4];
    for (int y = 0; y < 4; y++) {
        if (c->have_left) {
            int l = db_co_at(nnz, ns, -1, y, c->tr8_left);
            uint16x4_t colo = vext_u16(vdup_n_u16(l ? 0xffff : 0), co[y + 1], 3);
            lo[y] = db_load4(c, y, -1, colo);
        } else {
            lo[y] = db_rot4(hi[y + 1]);
        }
    }
    {
        static const uint16_t EDGEV[8] = { 0xffff, 0, 0, 0, 0xffff, 0, 0, 0 };
        /* the kernel produces a row of four edges at a time and bsv is stored
 * edge-major, so the 4x4 result matrix is transposed on the way out --
 * one table lookup, against a 32-bit skip test per edge in the caller */
        static const uint8_t TR[16] = { 0, 4,  8, 12, 1, 5,  9, 13,
                                        2, 6, 10, 14, 3, 7, 11, 15 };
        uint16x8_t ev = vld1q_u16(EDGEV);
        struct db_side8 p = db_pair(lo[0], lo[1]), q = db_pair(hi[1], hi[2]);
        uint8x8_t r01 = db_bs8(&p, &q, ev);
        p = db_pair(lo[2], lo[3]); q = db_pair(hi[3], hi[4]);
        uint8x8_t r23 = db_bs8(&p, &q, ev);
        uint8x16_t rows = vcombine_u8(r01, r23);
        if (!c->have_left)
            rows = vandq_u8(rows, vreinterpretq_u8_u32(vdupq_n_u32(0xffffff00)));
        vst1q_u8(&bsv[0][0], vqtbl1q_u8(rows, vld1q_u8(TR)));
    }

    /* Horizontal edges: the p side is the row above, so no shift is needed. */
    {
        struct db_side8 p = db_pair(hi[0], hi[1]), q = db_pair(hi[1], hi[2]);
        uint8x8_t r = db_bs8(&p, &q, vcombine_u16(vdup_n_u16(0xffff), vdup_n_u16(0)));
        if (!c->have_top)
            r = vreinterpret_u8_u32(vset_lane_u32(0, vreinterpret_u32_u8(r), 0));
        vst1_u8(&bsh[0][0], r);
        p = db_pair(hi[2], hi[3]); q = db_pair(hi[3], hi[4]);
        vst1_u8(&bsh[2][0], db_bs8(&p, &q, vdupq_n_u16(0)));
    }
}

#endif /* __aarch64__ */
