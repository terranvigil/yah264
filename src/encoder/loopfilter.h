/*
 * loopfilter.h - in-loop deblocking filter
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef YAH264_DEBLOCK_H
#define YAH264_DEBLOCK_H

#include "mbcode.h"

/* qPav of a chroma edge (8.7.2.2): the mean of the QPc each side's QPY maps
 * to. Above 8 bits a QPc goes below zero, and it must reach the mean as it
 * is: the only clip is the one on indexA/indexB, after the filter offset. */
static inline int y264_deblock_chroma_qpav(int qpy_p, int qpy_q, int cqo)
{
    return (y264_chroma_qp(qpy_p, cqo) + y264_chroma_qp(qpy_q, cqo) + 1) >> 1;
}

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
