/*
 * cavlc.h - Context-Adaptive Variable Length Coding of residual blocks (9.2)
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef YAH264_CAVLC_H
#define YAH264_CAVLC_H

#include "../common/bitstream.h"
#include <stdint.h>
#include "../common/bitdepth.h"

/* Block category maxNumCoeff values. */
enum {
    Y264_CAVLC_LUMA_DC   = 16,   /* Intra16x16 luma DC block */
    Y264_CAVLC_LUMA_AC   = 15,   /* Intra16x16 luma AC block (positions 1..15) */
    Y264_CAVLC_LUMA_4x4  = 16,   /* Intra4x4 / inter 4x4 block */
    Y264_CAVLC_CHROMA_AC = 15,   /* chroma AC block */
    Y264_CAVLC_CHROMA_DC = 4,    /* chroma DC block (4:2:0), coded with nC == -1 */
};

/* Encode one residual block. `coeff` holds maxNumCoeff coefficients already in
 * the block's scan order (low to high frequency). `nC` is the neighbour context
 * (>= 0 for luma/chroma AC; pass -1 for 4:2:0 chroma DC). Returns TotalCoeff,
 * the number of non-zero coefficients, which the caller records as this block's
 * nnz for neighbouring blocks' context. */
int y264_cavlc_residual(y264_bs_t *bs, const dctcoef *coeff,
                        int maxNumCoeff, int nC);

/* Bit length y264_cavlc_residual would emit for the same block, without a
 * bitstream (the RD/est pricing path). `stride` selects an interleaved
 * sub-block in place: the 8x8 CAVLC split prices scan8+j with stride 4. */
int y264_cavlc_residual_len(const dctcoef *coeff, int maxNumCoeff, int nC,
                            int stride);

/* Parse the VLC tables now, on the calling thread. Called at encoder open so
 * worker threads never build them concurrently. Idempotent. */
void y264_cavlc_warm(void);
void y264_cavlc_set_prefix15(int on);

/* Accessors used by the CAVLC self-test to validate table structure. Return the
 * codeword (length in *len, value in *code) or set *len = 0 for invalid combos. */
void y264_cavlc_coeff_token(int col, int total_coeff, int trailing_ones,
                            int *len, int *code);
void y264_cavlc_total_zeros(int max_num_coeff, int total_coeff, int total_zeros,
                            int *len, int *code);
void y264_cavlc_run_before(int zeros_left, int run, int *len, int *code);

#endif /* YAH264_CAVLC_H */
