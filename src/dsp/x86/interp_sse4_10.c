/*
 * interp_sse4_10.c - SSE4.2 mc and hpel kernels for the 10-bit library
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * each kernel is an interp_x86_10.h body under its SSE4.2 name. the names are
 * the 8-bit tier's; src/common/symbols10.h adds the _10 suffix, so arch.h
 * keeps one prototype set and the dispatch sites are shared. the lowres
 * bilinear row has no twin: no 10-bit caller builds a bilinear lowres plane.
 */
#include "dsp/x86/interp_x86_10.h"

#if Y264_HAVE_SSE4_10

I10_LUMA_BODY(i10_mc_luma16, 16, I10_HCL16, I10_HSUM16, I10_VCL16, I10_JCL16)

void y264_mc_luma16_sse4(pixel *dst, int dstride, const pixel *ref, int rstride,
                         int ix, int iy, int fx, int fy, int h)
{ i10_mc_luma16(dst, dstride, ref, rstride, ix, iy, fx, fy, h); }

void y264_mc_luma8_sse4(pixel *dst, int dstride, const pixel *ref, int rstride,
                        int ix, int iy, int fx, int fy, int h)
{ i10_mc_luma8(dst, dstride, ref, rstride, ix, iy, fx, fy, h); }

void y264_mc_chroma_w8h_sse4(pixel *dst, int dstride, const pixel *ref,
                             int rstride, int ix, int iy, int fx, int fy, int h)
{ i10_mc_chroma_w8h(dst, dstride, ref, rstride, ix, iy, fx, fy, h); }

void y264_mc_chroma_w4h_sse4(pixel *dst, int dstride, const pixel *ref,
                             int rstride, int ix, int iy, int fx, int fy, int h)
{ i10_mc_chroma_w4h(dst, dstride, ref, rstride, ix, iy, fx, fy, h); }

void y264_pred_copy_sse4(pixel *dst, int dstride, const pixel *s, int sstride,
                         int w, int h)
{ i10_pred_copy(dst, dstride, s, sstride, w, h); }

void y264_pred_avg2_sse4(pixel *dst, int dstride, const pixel *s1,
                         const pixel *s2, int sstride, int w, int h)
{ i10_pred_avg2(dst, dstride, s1, s2, sstride, w, h); }

void y264_pixel_avg_wt_sse4(pixel *dst, const pixel *a, const pixel *b, int n,
                            int w0, int w1)
{ i10_wt_row(dst, a, b, n, w0, w1); }

void y264_pixel_avg_wt_2d_sse4(pixel *dst, int ds, const pixel *a, int as,
                               const pixel *b, int bs, int w, int h, int w0, int w1)
{ i10_avg_wt_2d(dst, ds, a, as, b, bs, w, h, w0, w1); }

void y264_avg_mb420_flat_sse4(pixel *pred, pixel *cu, pixel *cv,
                              const pixel *l0, int s0, const pixel *l1, int s1,
                              const pixel *u0, const pixel *v0, int t0,
                              const pixel *u1, const pixel *v1, int t1)
{ i10_avg_mb420_flat(pred, cu, cv, l0, s0, l1, s1, u0, v0, t0, u1, v1, t1); }

void y264_mc_chroma_bi8_sse4(pixel *dst, int dstride,
                             const pixel *r0, int s0, int fx0, int fy0,
                             const pixel *r1, int s1, int fx1, int fy1,
                             int w0, int w1)
{ i10_mc_chroma_bi8(dst, dstride, r0, s0, fx0, fy0, r1, s1, fx1, fy1, w0, w1); }

/* the row kernels need a span of eight; the last pass backs up to end on it,
 * recomputing columns that are pure functions of the source */
void y264_hpel_hrow_sse4(int32_t *srow, const pixel *row, int x0, int x1)
{
    for (int x = x0; x < x1; x += 8) {
        if (x > x1 - 8) x = x1 - 8;
        i10_hrow8(srow, row, x);
    }
}

void y264_hpel_outrow_sse4(pixel *Hr, pixel *Vr, pixel *Cr,
                           const int32_t *s0, const int32_t *s1, const int32_t *s2,
                           const int32_t *s3, const int32_t *s4, const int32_t *s5,
                           const pixel *r0, const pixel *r1, const pixel *r2,
                           const pixel *r3, const pixel *r4, const pixel *r5,
                           int x0, int x1)
{
    for (int x = x0; x < x1; x += 8) {
        if (x > x1 - 8) x = x1 - 8;
        i10_outrow8(Hr, Vr, Cr, s0, s1, s2, s3, s4, s5, r0, r1, r2, r3, r4, r5, x);
    }
}

static inline void hpb_hsum_sse4(const pixel *pad, uint16_t *t)
{
    i10_st8(t, i10_hsum8(pad));
}

static inline void hpb_hv_sse4(const uint16_t *t2, const pixel *p0, const pixel *p1,
                               const pixel *p2, const pixel *p3, const pixel *p4,
                               const pixel *p5, pixel *H, pixel *V)
{
    i10_st8(H, i10_half(i10_ld8(t2)));
    i10_st8(V, i10_half(i10_tap6_off(i10_ld8(p0), i10_ld8(p1), i10_ld8(p2),
                                     i10_ld8(p3), i10_ld8(p4), i10_ld8(p5))));
}

static inline void hpb_c_sse4(const uint16_t *t0, const uint16_t *t1,
                              const uint16_t *t2, const uint16_t *t3,
                              const uint16_t *t4, const uint16_t *t5, pixel *out)
{
    i10_st8(out, i10_centre(i10_ld8(t0), i10_ld8(t1), i10_ld8(t2),
                            i10_ld8(t3), i10_ld8(t4), i10_ld8(t5)));
}

I10_BAND_BODY(y264_hpel_build_band_sse4, 8, hpb_hsum_sse4, hpb_hv_sse4, hpb_c_sse4)

/* the lowres half-pel row on the 8-bit scale; n >= 8 */
void y264_lr_hp_row_sse4(pixel *oh, pixel *ov, pixel *oc, const pixel *r0,
                         const pixel *r1, const pixel *r2, int n)
{
    for (int x = 0; x < n; x += 8) {
        if (x > n - 8) x = n - 8;
        i10_lr_hp_row8(oh, ov, oc, r0, r1, r2, x);
    }
}

#endif /* Y264_HAVE_SSE4_10 */
