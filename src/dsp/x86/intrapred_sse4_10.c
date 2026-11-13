/*
 * intrapred_sse4_10.c - SSE4.2 intra prediction kernels for the 10-bit library
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * each kernel is an intrapred_x86_10.h body under its SSE4.2 name. the names
 * are the 8-bit tier's; src/common/symbols10.h adds the _10 suffix, so
 * arch.h and intrapred.h keep one prototype set and the dispatch sites are
 * shared.
 */
#include "dsp/x86/intrapred_x86_10.h"

#if Y264_HAVE_SSE4_10

void y264_intra16x16_sse4(pixel pred[256], const pixel *rec, int stride,
                          int mode, int have_top, int have_left)
{ ip10_i16(pred, rec, stride, mode, have_top, have_left); }

void y264_intra_chroma_sse4(pixel *pred, const pixel *rec, int stride,
                            int mode, int have_top, int have_left,
                            int cw, int ch)
{
    (void)cw;                                   /* dispatcher guarantees 8 */
    ip10_ichroma(pred, rec, stride, mode, have_top, have_left, ch);
}

void y264_intra8x8_from_edge_sse4(pixel pred[64], const pixel e[32], int mode)
{ ip10_i8_from_edge(pred, e, mode); }

void y264_intra8x8_sse4(pixel pred[64], const pixel *rec, int stride,
                        int mode, int have_top, int have_left,
                        int have_topleft, int have_topright)
{
    pixel e[32];
    __m128i t0, t1, lf;
    int tl = ip10_edge_regs(rec, stride, have_top, have_left, have_topleft,
                            have_topright, &t0, &t1, &lf);
    ip10_edge_flat(e, t0, t1, lf, tl);
    ip10_i8_from_edge(pred, e, mode);
}

/* both views of y264_i8_edge_t: the flat array and the t/l/tl integers */
void y264_intra8x8_edge_sse4(y264_i8_edge_t *e, const pixel *rec, int stride,
                             int have_top, int have_left,
                             int have_topleft, int have_topright)
{
    __m128i t0, t1, lf;
    int tl = ip10_edge_regs(rec, stride, have_top, have_left, have_topleft,
                            have_topright, &t0, &t1, &lf);
    ip10_edge_flat(e->f, t0, t1, lf, tl);
    _mm_storeu_si128((__m128i *)(void *)(e->t + 0), _mm_cvtepu16_epi32(t0));
    _mm_storeu_si128((__m128i *)(void *)(e->t + 4),
                     _mm_cvtepu16_epi32(_mm_srli_si128(t0, 8)));
    _mm_storeu_si128((__m128i *)(void *)(e->t + 8), _mm_cvtepu16_epi32(t1));
    _mm_storeu_si128((__m128i *)(void *)(e->t + 12),
                     _mm_cvtepu16_epi32(_mm_srli_si128(t1, 8)));
    _mm_storeu_si128((__m128i *)(void *)(e->l + 0), _mm_cvtepu16_epi32(lf));
    _mm_storeu_si128((__m128i *)(void *)(e->l + 4),
                     _mm_cvtepu16_epi32(_mm_srli_si128(lf, 8)));
    e->tl = tl;
}

void y264_intra8x8_dir6_sse4(pixel out[6][64], const pixel e[32])
{ ip10_i8_dir6(out, e); }

/* top, left and top-left present, no top right */
void y264_intra8x8_dir6_rec_sse4(pixel out[6][64], const pixel *rec, int stride)
{
    pixel e[32];
    __m128i t0, t1, lf;
    int tl = ip10_edge_regs(rec, stride, 1, 1, 1, 0, &t0, &t1, &lf);
    ip10_edge_flat(e, t0, t1, lf, tl);
    ip10_i8_dir6(out, e);
}

void y264_intra8_dvhp_sse4(pixel out[4][64], const pixel top[8],
                           const pixel left[8], int dc, int a, int b, int c)
{ ip10_i8_dvhp(out, top, left, dc, a, b, c); }

#endif /* Y264_HAVE_SSE4_10 */
