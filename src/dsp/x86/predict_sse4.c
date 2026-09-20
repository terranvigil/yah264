/*
 * predict_sse4.c - SSE4.2 kernels for intra prediction (ITU-T H.264 8.3)
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Wave 3b of docs/x86-plan.md. Four kernels: the 16x16 builder, the cw == 8
 * chroma builder, the 8x8 builder, and the 8x8 builder's from-edge form -- the
 * shape a nine-mode decision loop wants, because the 8.3.2.2.1 reference
 * filter depends on the NEIGHBOURHOOD and not on the mode, so a loop that
 * derived the edge once feeds every mode from it.
 *
 * The bodies live in predict_x86.h and are shared with the AVX2 tier. The
 * ROUTING is the NEON twin's, unchanged and for its recorded reasons: 16x16
 * and chroma route every mode, 8x8 routes VERT/DDR/VR/HD/VL, and 4x4 routes
 * nothing. Those were measurements about SHAPE -- sixteen samples do not
 * amortize an edge-filter precompute, and the simple fills auto-vectorize
 * about as well as anything hand-written -- so they are not re-argued per
 * instruction set. Nothing in this wave is timed, which is the other reason
 * the routing is inherited rather than re-decided: a new route would be a
 * claim with no number behind it.
 *
 * C11 intrinsics only.
 */
#include "dsp/arch.h"

#if Y264_HAVE_SSE4

#include "dsp/x86/predict_x86.h"

void y264_intra16x16_sse4(pixel pred[256], const pixel *rec, int stride,
                          int mode, int have_top, int have_left)
{
    y264_i16_x86(pred, rec, stride, mode, have_top, have_left);
}

void y264_intra_chroma_sse4(pixel *pred, const pixel *rec, int stride,
                            int mode, int have_top, int have_left,
                            int cw, int ch)
{
    y264_ichroma_x86(pred, rec, stride, mode, have_top, have_left, cw, ch);
}

void y264_intra8x8_from_edge_sse4(pixel pred[64], const pixel e[32], int mode)
{
    y264_i8_from_edge_x86(pred, e, mode);
}

void y264_intra8x8_sse4(pixel pred[64], const pixel *rec, int stride,
                        int mode, int have_top, int have_left,
                        int have_topleft, int have_topright)
{
    uint8_t e[32];
    y264_i8_edge_x86(e, rec, stride, have_top, have_left,
                     have_topleft, have_topright);
    y264_i8_from_edge_x86(pred, e, mode);
}

#endif /* Y264_HAVE_SSE4 */
