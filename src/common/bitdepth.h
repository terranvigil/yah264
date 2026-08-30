/*
 * bitdepth.h - pixel sample type and bit-depth constants.
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Compile-time bit depth (x264-style). At Y264_BIT_DEPTH 8 `pixel` is uint8_t
 * and everything is byte-identical to the pre-abstraction encoder; a 10/12-bit
 * build sets Y264_BIT_DEPTH and `pixel` becomes uint16_t. Only TRUE image
 * samples use `pixel` -- bitstream bytes, nnz counts, mode ids and flags stay
 * uint8_t. See docs/high-bit-depth-plan.md.
 */
#ifndef YAH264_BITDEPTH_H
#define YAH264_BITDEPTH_H

#include <stdint.h>
#include "yah264.h"   /* pixel and Y264_BIT_DEPTH: defined once, in the public
                        * header, so that header stands alone for consumers. */

#if Y264_BIT_DEPTH > 8
/* Residuals/DCT intermediates overflow int16 at BD>8, so widen them. */
typedef int32_t  dctcoef;
#else
typedef int16_t  dctcoef;
#endif

#define PIXEL_MAX      ((1 << Y264_BIT_DEPTH) - 1)
#define Y264_QP_BD_OFFSET (6 * (Y264_BIT_DEPTH - 8))

static inline pixel y264_clip_pixel(int x)
{
    return (pixel)(x < 0 ? 0 : (x > PIXEL_MAX ? PIXEL_MAX : x));
}

#endif /* YAH264_BITDEPTH_H */
