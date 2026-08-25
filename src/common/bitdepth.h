/*
 * bitdepth.h - pixel sample type and bit-depth constants.
 * Copyright (c) 2026, the next264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Compile-time bit depth (x264-style). At N264_BIT_DEPTH 8 `pixel` is uint8_t
 * and everything is byte-identical to the pre-abstraction encoder; a 10/12-bit
 * build sets N264_BIT_DEPTH and `pixel` becomes uint16_t. Only TRUE image
 * samples use `pixel` -- bitstream bytes, nnz counts, mode ids and flags stay
 * uint8_t. See docs/high-bit-depth-plan.md.
 */
#ifndef NEXT264_BITDEPTH_H
#define NEXT264_BITDEPTH_H

#include <stdint.h>

#ifndef N264_BIT_DEPTH
#define N264_BIT_DEPTH 8
#endif

#if N264_BIT_DEPTH > 8
typedef uint16_t pixel;
/* Residuals/DCT intermediates overflow int16 at BD>8, so widen them. */
typedef int32_t  dctcoef;
#else
typedef uint8_t  pixel;
typedef int16_t  dctcoef;
#endif

#define PIXEL_MAX      ((1 << N264_BIT_DEPTH) - 1)
#define N264_QP_BD_OFFSET (6 * (N264_BIT_DEPTH - 8))

static inline pixel n264_clip_pixel(int x)
{
    return (pixel)(x < 0 ? 0 : (x > PIXEL_MAX ? PIXEL_MAX : x));
}

#endif /* NEXT264_BITDEPTH_H */
