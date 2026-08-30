/*
 * deblock.h - in-loop deblocking kernels shared by the encoder and checkasm
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef YAH264_DSP_DEBLOCK_H
#define YAH264_DSP_DEBLOCK_H

#include <stdint.h>

/* Everything one macroblock's boundary-strength derivation (8.7.2.1) reads.
 * The pointers all address the macroblock's TOP-LEFT 4x4 block; the derivation
 * also reads the column to its left and the row above it, so a caller that
 * sets have_left / have_top is promising those are inside the frame.
 *
 * The nnz fold needs one more column and row than the strengths do (an 8x8
 * transform's coded status is its whole quadrant's, and the neighbour column's
 * quadrant partner sits at bx0-2), which is why this takes the grids and their
 * strides rather than a pre-gathered window. */
struct y264_bs_ctx {
    const int8_t  *ref0, *ref1;         /* per-4x4 list-0 / list-1 refIdx */
    const int16_t *mvx0, *mvy0;
    const int16_t *mvx1, *mvy1;
    int  mv_stride;
    const int8_t  *nnz;                 /* luma per-4x4 coefficient counts */
    int  nnz_stride;
    uint8_t tr8_cur, tr8_left, tr8_top; /* transform_size_8x8_flag per MB */
    uint8_t have_left, have_top;
};

/* bsv[xb][yb] = strength of the vertical edge between (xb-1,yb) and (xb,yb).
 * bsh[yb][xb] = strength of the horizontal edge between (xb,yb-1) and (xb,yb).
 * Both are indexed EDGE-major, so one edge's four strengths are four adjacent
 * bytes and the filter loop can skip a dead edge with a single 32-bit test --
 * which matters, because only 15% of edges carry any strength at all.
 * bsv[0] is left 0 when !have_left, bsh[0] when !have_top: the caller does not
 * filter the frame border. */
void y264_deblock_strength_c(const struct y264_bs_ctx *c,
                             uint8_t bsv[4][4], uint8_t bsh[4][4]);

#if defined(__aarch64__) && Y264_BIT_DEPTH == 8
void y264_deblock_strength_neon(const struct y264_bs_ctx *c,
                                uint8_t bsv[4][4], uint8_t bsh[4][4]);
#endif

#endif
