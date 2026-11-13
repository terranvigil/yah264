/*
 * loopfilter_sse4.c - SSE4.2 kernels for the deblock family (ITU-T H.264 8.7)
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Wave 3b of docs/simd.md. Four kernels: the two luma edge shapes, the
 * eight-line chroma horizontal edge, and the whole-macroblock strength
 * derivation. The filters themselves live in loopfilter_x86.h, written once at
 * 128 bits and shared with the AVX2 tier, because a four-line luma segment and
 * an eight-line chroma edge have nothing for a wider register to hold. Only
 * the strength derivation widens, and its AVX2 twin is in loopfilter_avx2.c.
 *
 * Since the x86 optimisation round there are four more, the whole-edge forms
 * the NEON twin has: a sixteen-line luma edge with its four bS values in each
 * direction, and a 4:2:0 chroma edge of BOTH planes in each direction. bS is
 * carried per lane, so an edge is one call where it was four (luma) or, for
 * the vertical chroma edge, sixteen scalar lines. The single-plane vertical
 * chroma edge stays scalar as on NEON (it measured slower there in time); the
 * two-plane form is judged on retired instructions, as its NEON twin is.
 *
 * What is NOT here, and why:
 *   - 4:4:4 chroma, which takes the LUMA-style filter on chroma planes and is
 *     therefore the scalar path's business either way, and the vertical
 *     chroma edges of 4:2:2, which the two-plane form does not cover.
 *
 * C11 intrinsics only.
 */
#include "dsp/arch.h"

#if Y264_HAVE_SSE4

#include "dsp/x86/loopfilter_x86.h"

void y264_deblock_luma_v4_sse4(pixel *q0p, int stride, int bs, int alpha,
                               int beta, int tc0)
{
    y264_db_luma_v4_x86(q0p, stride, bs, alpha, beta, tc0);
}

void y264_deblock_luma_h4_sse4(pixel *q0p, int stride, int bs, int alpha,
                               int beta, int tc0)
{
    y264_db_luma_h4_x86(q0p, stride, bs, alpha, beta, tc0);
}

void y264_deblock_chroma8_h_sse4(pixel *q0p, int stride, int alpha, int beta,
                                 const uint8_t bs[4], const uint8_t tc0tab[3],
                                 int span, int g)
{
    y264_db_chroma8_h_x86(q0p, stride, alpha, beta, bs, tc0tab, span, g);
}

/* The whole-edge forms (loopfilter_x86.h): a sixteen-line luma edge with its four
 * bS values, and a 4:2:0 chroma edge of both planes, one call each. */
void y264_deblock_luma_v16_sse4(pixel *q0p, int stride, int alpha, int beta,
                                const uint8_t bs[4], const uint8_t tc0tab[3])
{
    y264_db_luma_v16_x86(q0p, stride, alpha, beta, bs, tc0tab);
}

void y264_deblock_luma_h16_sse4(pixel *q0p, int stride, int alpha, int beta,
                                const uint8_t bs[4], const uint8_t tc0tab[3])
{
    y264_db_luma_h16_x86(q0p, stride, alpha, beta, bs, tc0tab);
}

void y264_deblock_chroma2_v_sse4(pixel *cbq0, pixel *crq0, int cbs, int crs,
                                 int alpha, int beta, const uint8_t bs[4],
                                 const uint8_t tc0tab[3])
{
    y264_db_chroma2_v_x86(cbq0, crq0, cbs, crs, alpha, beta, bs, tc0tab);
}

void y264_deblock_chroma2_h_sse4(pixel *cbq0, pixel *crq0, int cbs, int crs,
                                 int alpha, int beta, const uint8_t bs[4],
                                 const uint8_t tc0tab[3])
{
    y264_db_chroma2_h_x86(cbq0, crq0, cbs, crs, alpha, beta, bs, tc0tab);
}

/* The strength derivation, two eight-lane passes per axis
 * (loopfilter_x86_core.h, shared with the 10-bit library). */
void y264_deblock_strength_sse4(const struct y264_bs_ctx *c,
                                uint8_t bsv[4][4], uint8_t bsh[4][4])
{
    y264_db_strength_x86(c, bsv, bsh);
}

#endif /* Y264_HAVE_SSE4 */
