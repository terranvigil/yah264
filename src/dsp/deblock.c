/*
 * deblock.c - boundary-strength derivation (ITU-T H.264 8.7.2.1), whole
 * macroblock at a time.
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * WHY THIS IS A KERNEL AND NOT A HELPER. deblock.c used to derive each edge's
 * bS on demand behind a lazy memo: a call, six strided grid reads and a chain
 * of early-outs, up to thirty-two times per macroblock. Measured on the
 * as-shipped t1 profile that was 1.1-1.2% of wall in `strength` alone, inside
 * a deblock stage costing 2.2-3.3% of wall. That job vectorises and we had no kernel
 * for it, which is what put it at the top of the coverage list.
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

/* Each cell's attributes are gathered once into a 5x8 window (rows -1..3,
 * columns -1..3, padded to a stride of 8), and every edge's strength is then a
 * branch-free select over its two cells. The per-edge chain of early-outs the
 * rule reads as is data-dependent in exactly the way a branch predictor
 * cannot follow -- which test decides an edge changes from one edge to the
 * next. On the pure-C 4K profile the selects take about a quarter less time
 * than the chain did. */
enum { BSW = 8 };

/* One edge's strength from the two blocks' attributes (8.7.2.1).
 *
 * B slices: within a frame list0[0] and list1[0] are two distinct pictures, so
 * "same reference pictures used" reduces to the two blocks using the same set
 * of lists, and the mixed-case MV pairing is unique (L0<->L0, L1<->L1). On P/I
 * slices the list-1 field is all -1 and this collapses to the list-0 test. */
/* `ibs` is the strength an intra block gives this edge (4 on a frame
 * picture's macroblock edges and a field picture's vertical ones, 3 on a
 * field picture's horizontal macroblock edges, 3 inside the macroblock);
 * `ythr` is the vertical motion threshold, 4 in quarter frame samples and 2
 * in quarter field ones. */
static inline uint8_t bs_sel(int ip, int iq, int cp, int cq, int kp, int kq,
                             int x0p, int x0q, int y0p, int y0q,
                             int x1p, int x1q, int y1p, int y1q,
                             int r0p, int r0q, int r1p, int r1q,
                             int ibs, int ythr)
{
    /* kp/kq: bit 0 = uses list 0, bit 1 = uses list 1 */
    int p0 = kp & 1, p1 = kp >> 1;
    int mv = (kp != kq)
           | (p0 & ((r0p != r0q) | (abs(x0p - x0q) >= 4) | (abs(y0p - y0q) >= ythr)))
           | (p1 & ((r1p != r1q) | (abs(x1p - x1q) >= 4) | (abs(y1p - y1q) >= ythr)));
    int coded = cp | cq;
    int v = coded ? 2 : mv;
    return (uint8_t)((ip | iq) ? ibs : v);
}

void y264_deblock_strength_c(const struct y264_bs_ctx *c,
                             uint8_t bsv[4][4], uint8_t bsh[4][4])
{
    const int ms = c->mv_stride, ns = c->nnz_stride;
    const int ythr = c->field ? 2 : 4;          /* see y264_bs_ctx.field */
    const int hbs  = c->field ? 3 : 4;
    /* [dy+1][dx+1] at a stride of BSW. The (-1,-1) corner belongs to no edge
     * pair; cells outside the frame are zeroed rather than skipped so the
     * selects never read an indeterminate value, and their edges are
     * overwritten below. */
    uint8_t intra[5 * BSW], coeff[5 * BSW], lists[5 * BSW];
    int8_t  r0[5 * BSW], r1[5 * BSW];
    int16_t x0[5 * BSW], y0[5 * BSW], x1[5 * BSW], y1[5 * BSW];

    int dy0 = c->have_top ? -1 : 0, dx0 = c->have_left ? -1 : 0;
    if (dy0 == 0 || dx0 == 0) {
        memset(intra, 0, sizeof intra); memset(coeff, 0, sizeof coeff);
        memset(lists, 0, sizeof lists);
        memset(r0, 0, sizeof r0); memset(r1, 0, sizeof r1);
        memset(x0, 0, sizeof x0); memset(y0, 0, sizeof y0);
        memset(x1, 0, sizeof x1); memset(y1, 0, sizeof y1);
    }
    for (int dy = dy0; dy < 4; dy++) {
        int tr8_row = dy < 0 ? c->tr8_top : c->tr8_cur;
        for (int dx = (dy < 0 ? 0 : dx0); dx < 4; dx++) {
            int i = dy * ms + dx, k = (dy + 1) * BSW + dx + 1;
            int tr8 = dx < 0 ? c->tr8_left : tr8_row;
            int a = c->ref0[i], b = c->ref1[i];
            if (c->refmap && a >= 0) a = c->refmap[a];
            r0[k] = (int8_t)a;
            r1[k] = (int8_t)b;
            lists[k] = (uint8_t)((a >= 0) | ((b >= 0) << 1));
            intra[k] = (uint8_t)(a < 0 && b < 0);
            coeff[k] = (uint8_t)bs_coeff(c->nnz, ns, dx, dy, tr8);
            x0[k] = c->mvx0[i]; y0[k] = c->mvy0[i];
            x1[k] = c->mvx1[i]; y1[k] = c->mvy1[i];
        }
    }

#define BS_EDGE(P, Q, IBS) bs_sel(intra[P], intra[Q], coeff[P], coeff[Q],   \
                                  lists[P], lists[Q], x0[P], x0[Q],         \
                                  y0[P], y0[Q], x1[P], x1[Q], y1[P], y1[Q], \
                                  r0[P], r0[Q], r1[P], r1[Q], IBS, ythr)
    for (int yb = 0; yb < 4; yb++)
        for (int xb = 0; xb < 4; xb++) {
            int q = (yb + 1) * BSW + xb + 1;
            bsv[xb][yb] = BS_EDGE(q - 1, q, xb == 0 ? 4 : 3);
        }
    for (int yb = 0; yb < 4; yb++)
        for (int xb = 0; xb < 4; xb++) {
            int q = (yb + 1) * BSW + xb + 1;
            bsh[yb][xb] = BS_EDGE(q - BSW, q, yb == 0 ? hbs : 3);
        }
#undef BS_EDGE
    if (!c->have_left) memset(bsv[0], 0, 4);
    if (!c->have_top)  memset(bsh[0], 0, 4);
}
