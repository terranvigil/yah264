/*
 * pixel.h - pixel block metrics (SAD) with runtime-dispatched kernels
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef YAH264_PIXEL_H
#define YAH264_PIXEL_H

#include <stdint.h>
#include "../common/bitdepth.h"

/* Block partition sizes used by H.264 inter prediction, indexed compactly. */
enum {
    Y264_PU_16x16 = 0,
    Y264_PU_16x8,
    Y264_PU_8x16,
    Y264_PU_8x8,
    Y264_PU_8x4,
    Y264_PU_4x8,
    Y264_PU_4x4,
    Y264_PU_COUNT
};

/* Sum of absolute differences between two blocks. */
typedef int (*y264_sad_fn)(const pixel *a, int a_stride,
                           const pixel *b, int b_stride);

/* Sum of absolute Hadamard-transformed differences of a 4x4 block. */
typedef int (*y264_satd_fn)(const pixel *a, int a_stride,
                            const pixel *b, int b_stride);

/* Batched SAD: one source block against four candidate blocks sharing one
 * stride (the x264 <reference-internal> shape -- ME probe loops score 4 candidates
 * off a single source load). scores[i] == sad(src, r_i) exactly. */
typedef void (*y264_sad_x4_fn)(const pixel *src, int s_stride,
                               const pixel *r0, const pixel *r1,
                               const pixel *r2, const pixel *r3,
                               int r_stride, int scores[4]);

/* Batched 8x8 SATD, the sad_x4 shape one metric up: one source block against
 * four candidate blocks sharing a stride. `scores[i] == satd8x8(src, r_i)`
 * exactly, so a caller may compute a whole search ring before its ordered
 * strict-< comparisons without changing a decision or a tie-break -- the same
 * argument probe_int_list makes for SAD, and the reason this is byte-identical.
 *
 * Where the win comes from is NOT the same as sad_x4's, and it is worth being
 * precise because it bounds the payoff: SAD is load-bound, so sharing one
 * source load across four references is most of its 1.6-1.8x. SATD does ~5x
 * more arithmetic per byte loaded, so shared loads are worth much less; what
 * this shape actually buys is INDEPENDENT DEPENDENCY CHAINS. A single 8x8 SATD
 * is a long serial Hadamard chain that leaves a wide out-of-order core with
 * nothing to overlap. Four of them interleave. */
typedef void (*y264_satd_x4_fn)(const pixel *src, int s_stride,
                                const pixel *r0, const pixel *r1,
                                const pixel *r2, const pixel *r3,
                                int r_stride, int scores[4]);

/* Fused all-modes Intra4x4 cost (x264's <reference-internal> shape, taken to all nine
 * modes): costs[m] == satd4x4(src, prediction of mode m) for every m, computed
 * in one pass. `rec` points at the block's top-left sample in the recon plane,
 * the availability flags are the ones y264_intra4x4 takes. Modes the caller's
 * availability rules forbid are still filled, from the same zero-filled
 * neighbour samples the per-mode builder would use -- well-defined but not
 * meaningful; the caller skips them exactly as it skips the builder. */
typedef void (*y264_intra4x4_x9_fn)(const pixel *src, int s_stride,
                                    const pixel *rec, int r_stride,
                                    int have_top, int have_left,
                                    int have_topleft, int have_topright,
                                    int costs[9]);

/* Fused I16x16 V/H/DC cost : costs[0..2] ==
 * satd16x16(src, prediction of VERT / HORIZ / DC). `top` is the 16 samples
 * above the block and `left` the 16 to its left; `dc` is the mode's already
 * derived DC value, which depends on which of the two edges exist. A caller
 * whose availability rules forbid a mode passes any readable 16-byte pointer
 * for that edge and ignores that cost, exactly as it skips the builder. */
typedef void (*y264_intra_satd_x3_16_fn)(const pixel *src, int s_stride,
                                         const pixel *top, const pixel *left,
                                         int dc, int costs[3]);

typedef struct {
    y264_sad_fn  sad[Y264_PU_COUNT];
    y264_sad_x4_fn sad_x4[Y264_PU_COUNT];
    y264_satd_fn satd4x4;
    /* Fused SATD over larger blocks (== sum of the 4x4 SATDs, bit-identical). The
 * C kernels inline the 4x4 Hadamard to avoid per-4x4 indirect calls; NEON uses
 * loop wrappers over the NEON satd4x4. Callers loop these for other sizes. */
    y264_satd_fn satd8x8;
    y264_satd_x4_fn satd_x4_8x8;    /* NULL is legal: callers fall back to satd8x8 */
    y264_satd_fn satd16x16;
    /* SA8D (8x8 Hadamard sum-abs, x264-normalised) for the transform-size
 * pre-decision: SA8D(residual) < SATD(residual) => the 8x8 transform. */
    y264_satd_fn sa8d8x8;
    y264_satd_fn sa8d16x16;
    /* Single-plane 8x8 Hadamard AC energy (sum|coef| - |DC|), the psy-RD
 * texture metric's SA8D-support term. */
    long (*hadamard_ac8x8)(const pixel *p, int stride);
    /* Single-plane 16x16 psy-RD texture energy, SATD-support term: the sum over
 * the sixteen 4x4 tiles of SATD(tile, flat block of the tile's rounded
 * mean). Fused because the caller's per-tile form paid a pixel-sum pass, a
 * flat-block fill and a second operand load per tile; here the tile's DC
 * falls out of the Hadamard itself. */
    long (*texture_ac4_16x16)(const pixel *p, int stride);
    /* BOTH psy-RD texture terms of one 16x16 in a single pass: out[0] is the
 * SATD-support term above, out[1] the SA8D-support term (the four 8x8
 * hadamard_ac8x8 values summed). The RD metric always wants the pair for
 * the same block, and a 2D 8x8 Walsh-Hadamard is the four quadrant 4x4
 * transforms combined by one 2x2 butterfly per coefficient position -- so
 * the 8x8 term reads off the coefficients the 4x4 term already produced
 * instead of transforming every pixel a second time. */
    void (*texture_ac48_16x16)(const pixel *p, int stride, long out[2]);
    /* Pixel sum and sum of squares of a 16x16 : the AQ /
 * mb-tree variance grids want both for every macroblock of a full-res
 * plane. out[0] = sum, out[1] = sum of squares; both exact integers, so
 * summation order is free. */
    void (*var16x16)(const pixel *p, int stride, uint32_t out[2]);
    /* All nine Intra4x4 mode costs of one block in a single pass; see the
 * typedef above. The C reference is the per-mode builder plus satd4x4, so
 * the kernel is bit-exact with the loop it replaces. */
    y264_intra4x4_x9_fn intra4x4_x9;
    y264_intra_satd_x3_16_fn intra_satd_x3_16;
} y264_pixel_fn_t;

/* Fill `pf` with portable C kernels only. Used as the checkasm reference. */
void y264_pixel_init_c(y264_pixel_fn_t *pf);

/* Fill `pf` with the best kernels available for `cpu` (C, then SIMD overrides). */
void y264_pixel_init(uint32_t cpu, y264_pixel_fn_t *pf);

/* Process-wide dispatched kernel table, auto-selected for the detected CPU.
 * Call y264_dsp_init once before use (idempotent). */
extern y264_pixel_fn_t y264_dsp;
void y264_dsp_init(void);

#if defined(__aarch64__) && Y264_BIT_DEPTH == 8
/* NEON sum-of-squared-differences, bit-exact with scalar; width 16 or 8, any h. */
int y264_ssd_16xh_neon(const uint8_t *a, int as, const uint8_t *b, int bs, int h);
int y264_ssd_8xh_neon(const uint8_t *a, int as, const uint8_t *b, int bs, int h);
#endif

/* Width/height in pixels for each partition index, for tests and callers. */
extern const uint8_t y264_pu_width[Y264_PU_COUNT];
extern const uint8_t y264_pu_height[Y264_PU_COUNT];
extern const char *const y264_pu_name[Y264_PU_COUNT];

#endif /* YAH264_PIXEL_H */
