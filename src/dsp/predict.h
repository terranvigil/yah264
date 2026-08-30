/*
 * predict.h - H.264 intra prediction
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Prediction reads reconstructed neighbour samples directly from the recon
 * plane: `rec` points at the top-left sample of the current block, so the row
 * above is rec[-stride + x], the column to the left is rec[y*stride - 1], and
 * the corner is rec[-stride - 1]. Predicted samples are written to `pred`,
 * which is a tightly packed block (stride == block width).
 */
#ifndef YAH264_PREDICT_H
#define YAH264_PREDICT_H

#include <stdint.h>
#include "../common/bitdepth.h"

/* Intra16x16 prediction modes (ITU-T H.264 8.3.3). */
enum { Y264_I16_VERT = 0, Y264_I16_HORIZ = 1, Y264_I16_DC = 2, Y264_I16_PLANE = 3 };

/* Intra chroma prediction modes (8.3.4). Note the numbering differs from luma. */
enum { Y264_IC_DC = 0, Y264_IC_HORIZ = 1, Y264_IC_VERT = 2, Y264_IC_PLANE = 3 };

/* Intra4x4 prediction modes (8.3.1). */
enum {
    Y264_I4_VERT = 0, Y264_I4_HORIZ = 1, Y264_I4_DC = 2, Y264_I4_DDL = 3,
    Y264_I4_DDR = 4, Y264_I4_VR = 5, Y264_I4_HD = 6, Y264_I4_VL = 7, Y264_I4_HU = 8
};

void y264_intra16x16(pixel pred[256], const pixel *rec, int stride,
                     int mode, int have_top, int have_left);

/* Intra chroma prediction into a cw x ch block (stride cw). cw/ch = MbWidthC /
 * MbHeightC: 8x8 (4:2:0), 8x16 (4:2:2), 16x16 (4:4:4). */
void y264_intra_chroma(pixel *pred, const pixel *rec, int stride,
                       int mode, int have_top, int have_left, int cw, int ch);

/* Intra4x4 for one 4x4 block. `have_topright` covers the four samples above and
 * to the right; when false they are replicated from the last top sample per the
 * spec. */
void y264_intra4x4(pixel pred[16], const pixel *rec, int stride,
                   int mode, int have_top, int have_left,
                   int have_topleft, int have_topright);

/* Intra8x8 for one 8x8 block (8.3.2), High profile. Same prediction modes as
 * Intra4x4 but preceded by the reference-sample low-pass filter (8.3.2.2.1);
 * `have_topright` covers the eight samples above and to the right. */
void y264_intra8x8(pixel pred[64], const pixel *rec, int stride,
                   int mode, int have_top, int have_left,
                   int have_topleft, int have_topright);

/* The filtered reference samples of one Intra8x8 block, split out because the
 * 8.3.2.2.1 low-pass depends on the NEIGHBOURHOOD and not on the mode: a
 * nine-mode decision loop was running it nine times per block. Unavailable
 * sides read as zero, which is what the whole-function form produced too (the
 * DC mode sums both edges before it branches on availability, and T(-1)/L(-1)
 * read `tl`, which stays 0 without a top-left).
 *
 * Two views of the same numbers, so one derivation feeds both builders:
 * `t`/`l`/`tl` for the C switch, and `f` -- the flat array the NEON builders
 * index, laid out { l7..l0, tl, t0..t15, t15, t15 } with the two tail
 * replications that fold DDL's endpoint special into the ordinary 121 filter. */
typedef struct { int t[16], l[8], tl; pixel f[32]; } y264_i8_edge_t;
void y264_intra8x8_edge_c(y264_i8_edge_t *e, const pixel *rec, int stride,
                          int have_top, int have_left,
                          int have_topleft, int have_topright);
/* Build one mode's prediction from those samples. `have_top`/`have_left` are
 * still needed: they select the DC mode's divisor. `_c` is the portable
 * reference; the unsuffixed entry dispatches per mode exactly as
 * y264_intra8x8 does. */
void y264_intra8x8_from_edge_c(pixel pred[64], const y264_i8_edge_t *e,
                               int mode, int have_top, int have_left);
void y264_intra8x8_from_edge(pixel pred[64], const y264_i8_edge_t *e,
                             int mode, int have_top, int have_left);

/* Portable references (checkasm baselines); the public entry points above
 * dispatch to NEON builders where available. */
void y264_intra16x16_c(pixel pred[256], const pixel *rec, int stride,
                       int mode, int have_top, int have_left);
void y264_intra_chroma_c(pixel *pred, const pixel *rec, int stride,
                         int mode, int have_top, int have_left, int cw, int ch);
void y264_intra4x4_c(pixel pred[16], const pixel *rec, int stride,
                     int mode, int have_top, int have_left,
                     int have_topleft, int have_topright);
void y264_intra8x8_c(pixel pred[64], const pixel *rec, int stride,
                     int mode, int have_top, int have_left,
                     int have_topleft, int have_topright);

#endif /* YAH264_PREDICT_H */
