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
 * Since the x86 optimisation round there are four more, the decision-loop
 * forms the NEON twin already had or (the first) that the loop needed: the
 * edge derivation into both views of y264_i8_edge_t, the six directional
 * modes from one filter pass, the same six straight from the reconstructed
 * neighbours, and the lowres estimate's four base predictions.
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

/* The filtered edge in both views of y264_i8_edge_t, once per block for a
 * decision loop: the flat array the builders index and the t/l/tl integers the
 * scalar modes read. Same numbers as y264_intra8x8_edge_c. */
void y264_intra8x8_edge_sse4(y264_i8_edge_t *e, const pixel *rec, int stride,
                             int have_top, int have_left,
                             int have_topleft, int have_topright)
{
    __m128i tf, lf;
    int tl = y264_i8_edge_regs(rec, stride, have_top, have_left, have_topleft,
                               have_topright, &tf, &lf);
    y264_i8_edge_flat(e->f, tf, lf, tl);
    _mm_storeu_si128((__m128i *)(void *)(e->t + 0), _mm_cvtepu8_epi32(tf));
    _mm_storeu_si128((__m128i *)(void *)(e->t + 4),
                     _mm_cvtepu8_epi32(_mm_srli_si128(tf, 4)));
    _mm_storeu_si128((__m128i *)(void *)(e->t + 8),
                     _mm_cvtepu8_epi32(_mm_srli_si128(tf, 8)));
    _mm_storeu_si128((__m128i *)(void *)(e->t + 12),
                     _mm_cvtepu8_epi32(_mm_srli_si128(tf, 12)));
    _mm_storeu_si128((__m128i *)(void *)(e->l + 0), _mm_cvtepu8_epi32(lf));
    _mm_storeu_si128((__m128i *)(void *)(e->l + 4),
                     _mm_cvtepu8_epi32(_mm_srli_si128(lf, 4)));
    e->tl = tl;
}

void y264_intra8x8_dir6_sse4(pixel out[6][64], const pixel e[32])
{
    y264_i8_dir6_x86(out, e);
}

/* The six from the reconstructed neighbours directly: top, left and top-left
 * present, no top right. */
void y264_intra8x8_dir6_rec_sse4(pixel out[6][64], const pixel *rec, int stride)
{
    uint8_t e[32];
    __m128i tf, lf;
    int tl = y264_i8_edge_regs(rec, stride, 1, 1, 1, 0, &tf, &lf);
    y264_i8_edge_flat(e, tf, lf, tl);
    y264_i8_dir6_x86(out, e);
}

void y264_intra8_dvhp_sse4(pixel out[4][64], const pixel top[8],
                           const pixel left[8], int dc, int a, int b, int c)
{
    y264_i8_dvhp_x86(out, top, left, dc, a, b, c);
}

#endif /* Y264_HAVE_SSE4 */
