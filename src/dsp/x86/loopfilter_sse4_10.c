/*
 * loopfilter_sse4_10.c - SSE4.2 deblocking kernels for the 10-bit library
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * each kernel is a loopfilter_x86_10.h body (for the strength derivation,
 * loopfilter_x86_core.h's) under its SSE4.2 name. the names are the 8-bit
 * tier's; src/common/symbols10.h adds the _10 suffix, so arch.h keeps one
 * prototype set and the dispatch sites are shared.
 *
 * the four-line luma forms have no 10-bit twin: the whole-edge ones take
 * every x86 luma edge before the four-line sites are reached.
 */
#include "dsp/x86/loopfilter_x86_10.h"

#if Y264_HAVE_SSE4_10

void y264_deblock_strength_sse4(const struct y264_bs_ctx *c,
                                uint8_t bsv[4][4], uint8_t bsh[4][4])
{ y264_db_strength_x86(c, bsv, bsh); }

void y264_deblock_luma_v16_sse4(pixel *q0p, int stride, int alpha, int beta,
                                const uint8_t bs[4], const uint8_t tc0tab[3])
{ lf10_luma_v16(q0p, stride, alpha, beta, bs, tc0tab); }

void y264_deblock_luma_h16_sse4(pixel *q0p, int stride, int alpha, int beta,
                                const uint8_t bs[4], const uint8_t tc0tab[3])
{ lf10_luma_h16(q0p, stride, alpha, beta, bs, tc0tab); }

void y264_deblock_chroma8_h_sse4(pixel *q0p, int stride, int alpha, int beta,
                                 const uint8_t bs[4], const uint8_t tc0tab[3],
                                 int span, int g)
{ lf10_chroma8_h(q0p, stride, alpha, beta, bs, tc0tab, span, g); }

void y264_deblock_chroma2_v_sse4(pixel *cbq0, pixel *crq0, int cbs, int crs,
                                 int alpha, int beta, const uint8_t bs[4],
                                 const uint8_t tc0tab[3])
{ lf10_chroma2_v(cbq0, crq0, cbs, crs, alpha, beta, bs, tc0tab); }

void y264_deblock_chroma2_h_sse4(pixel *cbq0, pixel *crq0, int cbs, int crs,
                                 int alpha, int beta, const uint8_t bs[4],
                                 const uint8_t tc0tab[3])
{ lf10_chroma2_h(cbq0, crq0, cbs, crs, alpha, beta, bs, tc0tab); }

#endif /* Y264_HAVE_SSE4_10 */
