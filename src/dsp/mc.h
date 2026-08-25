/*
 * mc.h - motion compensation (inter prediction sample interpolation)
 * Copyright (c) 2026, the next264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Reference sample coordinates are clamped to the reference picture bounds, so
 * motion vectors may point outside the frame (unrestricted MV). `mvx`/`mvy` are
 * in quarter-pel units for luma; chroma uses the same vector interpreted as
 * eighth-pel on the half-resolution chroma plane (4:2:0).
 */
#ifndef NEXT264_MC_H
#define NEXT264_MC_H

/* Reference planes carry edge-replicated borders so motion compensation can
 * read out-of-frame positions directly (the replication equals the spec's
 * coordinate clamping). The encoder allocates with these; the MC dispatcher
 * widens its in-bounds checks by them. */
#define N264_LUMA_BORDER   32
#define N264_CHROMA_BORDER 16


#include <stdint.h>
#include "../common/bitdepth.h"

void n264_mc_luma(pixel *dst, int dstride,
                  const pixel *ref, int rstride, int pw, int ph,
                  int bx, int by, int mvx, int mvy, int w, int h);

/* Weighted average of two packed predictions :
 * dst[i] = Clip1((a[i]*w0 + b[i]*w1 + 32) >> 6) over n samples, all three
 * blocks packed. w0 + w1 == 64 and both lie in [-64, 128], which bounds the
 * weighted sum inside int16 (255*128 + 32 = 32672), so the kernel is exact in
 * 16-bit lanes; the unweighted (32, 32) case is the rounding average and takes
 * a one-instruction path. */
void n264_pixel_avg_wt(pixel *dst, const pixel *a, const pixel *b, int n,
                       int w0, int w1);
void n264_pixel_avg_wt_c(pixel *dst, const pixel *a, const pixel *b, int n,
                         int w0, int w1);

/* Half-pel plane fetch: the strided read that turns a registered half-pel plane
 * into a prediction block. Whole and half positions are a copy, quarter
 * positions the 2-tap rounding average (a+b+1)>>1 of two planes. `w` is 4, 8 or
 * 16 on the dispatched path; anything else falls to the C reference. */
void n264_pred_copy(pixel *dst, int dstride, const pixel *s, int sstride,
                    int w, int h);
void n264_pred_copy_c(pixel *dst, int dstride, const pixel *s, int sstride,
                      int w, int h);
void n264_pred_avg2(pixel *dst, int dstride, const pixel *s1, const pixel *s2,
                    int sstride, int w, int h);
void n264_pred_avg2_c(pixel *dst, int dstride, const pixel *s1, const pixel *s2,
                      int sstride, int w, int h);

/* Portable reference (checkasm baseline); n264_mc_luma dispatches to this. */
void n264_mc_luma_c(pixel *dst, int dstride,
                    const pixel *ref, int rstride, int pw, int ph,
                    int bx, int by, int mvx, int mvy, int w, int h);

/* Chroma motion compensation. sub_w/sub_h are SubWidthC/SubHeightC (2 = half
 * resolution on that axis, 1 = full): 4:2:0 = (2,2), 4:2:2 = (2,1), 4:4:4 =
 * (1,1). A half-res axis interprets the MV as eighth-pel (>>3, &7); a full-res
 * axis as quarter-pel mapped into eighth-pel space (>>2, (&3)<<1). */
void n264_mc_chroma(pixel *dst, int dstride,
                    const pixel *ref, int rstride, int cpw, int cph,
                    int cbx, int cby, int mvx, int mvy, int w, int h,
                    int sub_w, int sub_h);

void n264_mc_chroma_c(pixel *dst, int dstride,
                      const pixel *ref, int rstride, int cpw, int cph,
                      int cbx, int cby, int mvx, int mvy, int w, int h,
                      int sub_w, int sub_h);

/* Precompute the three half-pel luma interpolation planes for a reference (the
 * x264 hpel model): H = horizontal-half (spec position 'b'), V = vertical-half
 * ('h'), C = centre/diagonal-half ('j'). Each output plane shares the
 * reference's `stride` and interior origin, and is filled over the full
 * bordered extent [-border, pw+border) x [-border, ph+border) using
 * frame-bounds clamping -- identical to the R access in n264_mc_luma_c -- so
 * a later strided read of any in-allocation position, averaged per the qpel
 * position, reproduces n264_mc_luma bit-for-bit. `scratch` is a caller-owned
 * int32 buffer of at least `sstride * (ph + 2*border + 5)` elements holding the
 * unclipped horizontal 6-tap intermediates; `sstride` must be >= pw + 2*border. */
/* Resolve the MC env-gated lazy statics on the calling thread (see
 * n264_mb_warm_statics). Idempotent. */
void n264_mc_warm_statics(void);

/* Build only output rows [ry0, ry1) (in the [-border, ph+border) row space), so
 * the build can be split across workers. `scratch` must hold (ry1-ry0+5) rows of
 * `sstride` int32 each, addressed from row ry0-2. Bit-identical to the
 * whole-frame build. */
void n264_mc_build_hpel_rows(pixel *Hp, pixel *Vp, pixel *Cp, int stride,
                             const pixel *ref, int rstride, int pw, int ph, int border,
                             int32_t *scratch, int sstride, int ry0, int ry1);

void n264_mc_build_hpel(pixel *Hp, pixel *Vp, pixel *Cp, int stride,
                        const pixel *ref, int rstride, int pw, int ph, int border,
                        int32_t *scratch, int sstride);

#endif /* NEXT264_MC_H */
