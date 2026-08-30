/*
 * deblock.h - in-loop deblocking filter
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef YAH264_DEBLOCK_H
#define YAH264_DEBLOCK_H

#include "macroblock.h"

/* Apply the in-loop deblocking filter to f->rec in place (luma and chroma),
 * using the frame's per-4x4 intra/nnz/motion grids for boundary strength. */
void y264_deblock_frame(y264_frame_t *f);

/* Deblock MB rows [mby0, mby1) serially, in raster order -- the staircase's
 * trailing per-row filter. Requires: analysis of every row < mby1 is complete
 * (bS inputs + this row's pixels), and rows < mby0 are already deblocked.
 * Byte-identical to the corresponding rows of y264_deblock_frame (same per-MB
 * order; the filter has only left/top dependencies). Never uses f->pool. */
void y264_deblock_rows(y264_frame_t *f, int mby0, int mby1);

#endif /* YAH264_DEBLOCK_H */
