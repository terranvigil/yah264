/*
 * metrics_sse4_10.c - SSE4.2 pixel kernels for the 10-bit library
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * each kernel is a metrics_x86_10.h body under its SSE4.2 name. the names are
 * the 8-bit tier's; src/common/symbols10.h adds the _10 suffix, so arch.h
 * keeps one prototype set and the dispatch sites are shared.
 */
#include "dsp/x86/metrics_x86_10.h"

#if Y264_HAVE_SSE4_10

M10_SAD(sse4, 16, 16)
M10_SAD(sse4, 16, 8)
M10_SAD(sse4, 8, 16)
M10_SAD(sse4, 8, 8)

M10_SADN(sse4, 16, 16)
M10_SADN(sse4, 16, 8)
M10_SADN(sse4, 8, 16)
M10_SADN(sse4, 8, 8)
M10_SADN(sse4, 8, 4)

int y264_satd_4x4_sse4(const pixel *a, int as, const pixel *b, int bs)
{ return m10_satd_4x4(a, as, b, bs); }
M10_SATD(sse4, 8, 8)
M10_SATD(sse4, 16, 16)
M10_SATD(sse4, 16, 8)
M10_SATD(sse4, 8, 16)

void y264_satd_quad_8x8_sse4(const pixel *src, int ss, const pixel *r0,
                             const pixel *r1, const pixel *r2, const pixel *r3,
                             int rs, int scores[4])
{ m10_satd_quad_8x8(src, ss, r0, r1, r2, r3, rs, scores); }

M10_SATD_AVG(sse4, 16, 16)
M10_SATD_AVG(sse4, 16, 8)
M10_SATD_AVG(sse4, 8, 16)
M10_SATD_AVG(sse4, 8, 8)

int y264_sa8d_8x8_sse4(const pixel *a, int as, const pixel *b, int bs)
{ return m10_sa8d_8x8(a, as, b, bs); }

int y264_sa8d_16x16_sse4(const pixel *a, int as, const pixel *b, int bs)
{
    return m10_sa8d_8x8(a, as, b, bs)
         + m10_sa8d_8x8(a + 8, as, b + 8, bs)
         + m10_sa8d_8x8(a + 8 * as, as, b + 8 * bs, bs)
         + m10_sa8d_8x8(a + 8 * as + 8, as, b + 8 * bs + 8, bs);
}

long y264_hadamard_ac_8x8_sse4(const pixel *p, int stride)
{ return m10_hadamard_ac_8x8(p, stride); }

long y264_texture_ac4_16x16_sse4(const pixel *p, int stride)
{ return m10_texture_ac4_16x16(p, stride); }

void y264_texture_ac48_16x16_sse4(const pixel *p, int stride, long out[2])
{ m10_texture_ac48_16x16(p, stride, out); }

void y264_var_16x16_sse4(const pixel *p, int stride, uint32_t out[2])
{ m10_var_16x16(p, stride, out); }

void y264_intra4_mode_search_sse4(const pixel *src, int ss, const pixel *rec, int rs,
                                  int ht, int hl, int htl, int htr, int costs[9])
{ m10_intra4_mode_search(src, ss, rec, rs, ht, hl, htl, htr, costs); }

void y264_intra16_satd_trio_sse4(const pixel *src, int ss, const pixel *top,
                                 const pixel *left, int dc, int costs[3])
{ m10_intra16_satd_trio(src, ss, top, left, dc, costs); }

int y264_ssd_16xh_sse4(const pixel *a, int as, const pixel *b, int bs, int h)
{ return m10_ssd_16xh(a, as, b, bs, h); }

int y264_ssd_8xh_sse4(const pixel *a, int as, const pixel *b, int bs, int h)
{ return m10_ssd_8xh(a, as, b, bs, h); }

void y264_ssdsum4x4_16_sse4(const pixel *a, int as, const pixel *b, int bs,
                            uint32_t e[16], int32_t sum[16])
{ m10_ssdsum4x4_16(a, as, b, bs, e, sum); }

void y264_ssdsum4x4_8_sse4(const pixel *a, int as, const pixel *b, int bs,
                           uint32_t e[4], int32_t sum[4])
{ m10_ssdsum4x4_8(a, as, b, bs, e, sum); }

void y264_ssd4x4_map16_sse4(const pixel *a, int as, const pixel *b, int bs,
                            uint32_t out[16])
{ m10_ssd4x4_map16(a, as, b, bs, out); }

#endif /* Y264_HAVE_SSE4_10 */
