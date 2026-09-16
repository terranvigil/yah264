/*
 * bitdepth.h - pixel sample type and bit-depth constants.
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
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
#include "yah264.h"   /* Y264_BIT_DEPTH, and the public API's own suffixing. */

/* `pixel` is the LIBRARY's type and lives here rather than in the public
 * header: both libraries are linked into one program now (item C3-10bit), so a
 * header that typed the samples for its caller would have to be two headers.
 * A consumer sees void* planes and a stride in samples. */
#if Y264_BIT_DEPTH > 8
typedef uint16_t pixel;
#else
typedef uint8_t  pixel;
#endif

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
