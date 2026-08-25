/*
 * deblock.c - boundary-strength derivation (ITU-T H.264 8.7.2.1), whole
 * macroblock at a time.
 * Copyright (c) 2026, the next264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * WHY THIS IS A KERNEL AND NOT A HELPER. deblock.c used to derive each edge's
 * bS on demand behind a lazy memo: a call, six strided grid reads and a chain
 * of early-outs, up to thirty-two times per macroblock. Measured on the
 * as-shipped t1 profile that was 1.1-1.2% of wall in `strength` alone, inside
 * a deblock stage costing 2.2-3.3% against x264's ~0.5%
 * (docs/archive/goal3-coverage-ranking.md). x264 vectorises exactly this job and we
 * had no kernel for it, which is what put it at the top of the coverage list.
 *
 * The whole-macroblock shape is what makes a kernel possible: all sixteen
 * vertical and sixteen horizontal strengths come out of one pass over a 5x5
 * window of the motion and coefficient grids, and every test in 8.7.2.1 is a
 * lane-wise compare. Deriving all thirty-two is more arithmetic than the memo
 * did when the 8x8 transform lets the luma loops skip the odd internal edges,
 * and still cheaper, because the memo's cost was per-edge overhead rather
 * than the derivation.
 */
#include "deblock.h"
#include <stdlib.h>
#include <string.h>

/* Does the 4x4 block at (bx,by) -- relative to the macroblock's top-left --
 * carry coefficients, for bS? Under the 8x8 transform the coded status is the
 * whole 8x8 quadrant's, the same fold the decoder does. `tr8` is the flag of
 * the macroblock that OWNS the block, which for the left column and top row is
 * the neighbour's. Negative coordinates round down (-1 & ~1 == -2), which is
 * the neighbour quadrant's first column, so the fold needs no special case. */
static inline int bs_coeff(const int8_t *nnz, int s, int bx, int by, int tr8)
{
    if (!tr8)
        return nnz[by * s + bx] > 0;
    int qx = bx & ~1, qy = by & ~1;
    return nnz[qy * s + qx] > 0 || nnz[qy * s + qx + 1] > 0
        || nnz[(qy + 1) * s + qx] > 0 || nnz[(qy + 1) * s + qx + 1] > 0;
}

/* One edge's strength from the two blocks' attributes (8.7.2.1).
 *
 * B slices: within a frame list0[0] and list1[0] are two distinct pictures, so
 * "same reference pictures used" reduces to the two blocks using the same set
 * of lists, and the mixed-case MV pairing is unique (L0<->L0, L1<->L1). On P/I
 * slices the list-1 field is all -1 and this collapses to the list-0 test. */
static inline uint8_t bs_of(int intra_p, int intra_q, int coeff_p, int coeff_q,
                            int r0p, int r0q, int r1p, int r1q,
                            int x0p, int x0q, int y0p, int y0q,
                            int x1p, int x1q, int y1p, int y1q, int mb_edge)
{
    if (intra_p || intra_q)
        return mb_edge ? 4 : 3;
    if (coeff_p || coeff_q)
        return 2;
    int p0 = r0p >= 0, q0 = r0q >= 0;
    int p1 = r1p >= 0, q1 = r1q >= 0;
    if (p0 != q0 || p1 != q1)                   /* different list membership */
        return 1;
    if (p0 && r0p != r0q)                       /* multi-ref: different L0 picture */
        return 1;
    if (p0 && (abs(x0p - x0q) >= 4 || abs(y0p - y0q) >= 4))
        return 1;
    if (p1 && (abs(x1p - x1q) >= 4 || abs(y1p - y1q) >= 4))
        return 1;
    return 0;
}

void n264_deblock_strength_c(const struct n264_bs_ctx *c,
                             uint8_t bsv[4][4], uint8_t bsh[4][4])
{
    const int ms = c->mv_stride, ns = c->nnz_stride;
    /* Cell attributes over rows -1..3 x columns -1..3, indexed [dy+1][dx+1].
 * The (-1,-1) corner belongs to no edge pair and is never filled. */
    uint8_t intra[5][5], coeff[5][5];
    int8_t  r0[5][5], r1[5][5];
    int16_t x0[5][5], y0[5][5], x1[5][5], y1[5][5];

    int dy0 = c->have_top ? -1 : 0, dx0 = c->have_left ? -1 : 0;
    for (int dy = dy0; dy < 4; dy++) {
        int tr8_row = dy < 0 ? c->tr8_top : c->tr8_cur;
        for (int dx = (dy < 0 ? 0 : dx0); dx < 4; dx++) {
            int i = dy * ms + dx, k = dy + 1, l = dx + 1;
            int tr8 = dx < 0 ? c->tr8_left : tr8_row;
            int a = c->ref0[i], b = c->ref1[i];
            r0[k][l] = (int8_t)a;
            r1[k][l] = (int8_t)b;
            intra[k][l] = (uint8_t)(a < 0 && b < 0);
            coeff[k][l] = (uint8_t)bs_coeff(c->nnz, ns, dx, dy, tr8);
            x0[k][l] = c->mvx0[i]; y0[k][l] = c->mvy0[i];
            x1[k][l] = c->mvx1[i]; y1[k][l] = c->mvy1[i];
        }
    }

    for (int yb = 0; yb < 4; yb++) {
        int k = yb + 1;
        for (int xb = 0; xb < 4; xb++) {
            if (xb == 0 && !c->have_left) { bsv[0][yb] = 0; continue; }
            int lp = xb, lq = xb + 1;           /* columns xb-1 and xb */
            bsv[xb][yb] = bs_of(intra[k][lp], intra[k][lq],
                                coeff[k][lp], coeff[k][lq],
                                r0[k][lp], r0[k][lq], r1[k][lp], r1[k][lq],
                                x0[k][lp], x0[k][lq], y0[k][lp], y0[k][lq],
                                x1[k][lp], x1[k][lq], y1[k][lp], y1[k][lq],
                                xb == 0);
        }
    }
    for (int yb = 0; yb < 4; yb++) {
        if (yb == 0 && !c->have_top) { memset(bsh[0], 0, 4); continue; }
        int kp = yb, kq = yb + 1;               /* rows yb-1 and yb */
        for (int xb = 0; xb < 4; xb++) {
            int l = xb + 1;
            bsh[yb][xb] = bs_of(intra[kp][l], intra[kq][l],
                                coeff[kp][l], coeff[kq][l],
                                r0[kp][l], r0[kq][l], r1[kp][l], r1[kq][l],
                                x0[kp][l], x0[kq][l], y0[kp][l], y0[kq][l],
                                x1[kp][l], x1[kq][l], y1[kp][l], y1[kq][l],
                                yb == 0);
        }
    }
}
