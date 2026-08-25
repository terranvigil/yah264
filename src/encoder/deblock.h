/*
 * deblock.h - in-loop deblocking filter
 * Copyright (c) 2026, the next264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef NEXT264_DEBLOCK_H
#define NEXT264_DEBLOCK_H

#include "macroblock.h"

/* Apply the in-loop deblocking filter to f->rec in place (luma and chroma),
 * using the frame's per-4x4 intra/nnz/motion grids for boundary strength. */
void n264_deblock_frame(n264_frame_t *f);

/* Deblock MB rows [mby0, mby1) serially, in raster order -- the staircase's
 * trailing per-row filter. Requires: analysis of every row < mby1 is complete
 * (bS inputs + this row's pixels), and rows < mby0 are already deblocked.
 * Byte-identical to the corresponding rows of n264_deblock_frame (same per-MB
 * order; the filter has only left/top dependencies). Never uses f->pool. */
void n264_deblock_rows(n264_frame_t *f, int mby0, int mby1);

#endif /* NEXT264_DEBLOCK_H */
