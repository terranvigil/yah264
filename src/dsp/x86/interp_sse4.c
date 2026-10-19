/*
 * interp_sse4.c - SSE4.2 kernels for the mc and hpel families
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Wave 2 of docs/simd.md: the SSE4.2 twin of every mc and hpel kernel the
 * NEON twin covers. Each one is bit-exact with the portable reference in
 * src/dsp/interp.c -- the luma half-pel planes, the chroma bilinear, the two plane
 * fetches, the weighted average and the two half-pel plane build rows -- which
 * is what lets the tier be a runtime axis that cannot move a byte of output.
 *
 * The shared building blocks and the two plane-scheme bodies are in interp_x86.h.
 * What is decided HERE is how a sixteen-column row is filtered at 128 bits:
 * as two halves of eight 16-bit lanes, low eight columns and high eight, from
 * SIX loads of sixteen bytes at offsets 0 to 5. Six narrow loads rather than
 * two wide ones and a shuffle is not a lazy choice. The window the dispatcher
 * promises is [ix-2, ix+18], the sixth load ends exactly on it, and a wider
 * load would declare columns the kernel never uses -- which the checkasm page
 * guard, mapped to that window, would fault on.
 *
 * C11 intrinsics only. There is no assembly in this tree, and hygiene_check
 * refuses any inline form under this directory.
 */
#include "dsp/x86/interp_x86.h"

#if Y264_HAVE_SSE4

/* ---- the sixteen-column row filters -------------------------------------- */

/* The horizontal six-tap over sixteen columns, as two registers of eight
 * 16-bit lanes. `p` addresses sample (ix-2, row). */
static inline void mcl16_h(const uint8_t *p, __m128i *lo, __m128i *hi)
{
    __m128i l0 = y264_ld128(p + 0), l1 = y264_ld128(p + 1);
    __m128i l2 = y264_ld128(p + 2), l3 = y264_ld128(p + 3);
    __m128i l4 = y264_ld128(p + 4), l5 = y264_ld128(p + 5);
    *lo = y264_tap6_16(_mm_cvtepu8_epi16(l0), _mm_cvtepu8_epi16(l1),
                       _mm_cvtepu8_epi16(l2), _mm_cvtepu8_epi16(l3),
                       _mm_cvtepu8_epi16(l4), _mm_cvtepu8_epi16(l5));
    *hi = y264_tap6_16(_mm_cvtepu8_epi16(_mm_srli_si128(l0, 8)),
                       _mm_cvtepu8_epi16(_mm_srli_si128(l1, 8)),
                       _mm_cvtepu8_epi16(_mm_srli_si128(l2, 8)),
                       _mm_cvtepu8_epi16(_mm_srli_si128(l3, 8)),
                       _mm_cvtepu8_epi16(_mm_srli_si128(l4, 8)),
                       _mm_cvtepu8_epi16(_mm_srli_si128(l5, 8)));
}

static inline void mcl16_hclip_sse4(const uint8_t *p, uint8_t *out)
{
    __m128i lo, hi;
    mcl16_h(p, &lo, &hi);
    _mm_storeu_si128((__m128i *)(void *)out, y264_clip5_16(lo, hi));
}

static inline void mcl16_hraw_sse4(const uint8_t *p, int16_t *out)
{
    __m128i lo, hi;
    mcl16_h(p, &lo, &hi);
    _mm_storeu_si128((__m128i *)(void *)out, lo);
    _mm_storeu_si128((__m128i *)(void *)(out + 8), hi);
}

static inline void mcl16_v_sse4(const uint8_t *p, int rstride, uint8_t *out)
{
    __m128i r0 = y264_ld128(p - 2 * rstride), r1 = y264_ld128(p - rstride);
    __m128i r2 = y264_ld128(p), r3 = y264_ld128(p + rstride);
    __m128i r4 = y264_ld128(p + 2 * rstride), r5 = y264_ld128(p + 3 * rstride);
    __m128i lo = y264_tap6_16(_mm_cvtepu8_epi16(r0), _mm_cvtepu8_epi16(r1),
                              _mm_cvtepu8_epi16(r2), _mm_cvtepu8_epi16(r3),
                              _mm_cvtepu8_epi16(r4), _mm_cvtepu8_epi16(r5));
    __m128i hi = y264_tap6_16(_mm_cvtepu8_epi16(_mm_srli_si128(r0, 8)),
                              _mm_cvtepu8_epi16(_mm_srli_si128(r1, 8)),
                              _mm_cvtepu8_epi16(_mm_srli_si128(r2, 8)),
                              _mm_cvtepu8_epi16(_mm_srli_si128(r3, 8)),
                              _mm_cvtepu8_epi16(_mm_srli_si128(r4, 8)),
                              _mm_cvtepu8_epi16(_mm_srli_si128(r5, 8)));
    _mm_storeu_si128((__m128i *)(void *)out, y264_clip5_16(lo, hi));
}

static inline void mcl16_j_sse4(const int16_t *c0, const int16_t *c1,
                                const int16_t *c2, const int16_t *c3,
                                const int16_t *c4, const int16_t *c5,
                                uint8_t *out)
{
    __m128i a = y264_jfilt8(c0, c1, c2, c3, c4, c5);
    __m128i b = y264_jfilt8(c0 + 8, c1 + 8, c2 + 8, c3 + 8, c4 + 8, c5 + 8);
    _mm_storeu_si128((__m128i *)(void *)out, _mm_unpacklo_epi64(a, b));
}

Y264_MC_LUMA16_BODY(mcl16_sse4, mcl16_hclip_sse4, mcl16_hraw_sse4,
                    mcl16_v_sse4, mcl16_j_sse4)

/* ---- the exported kernels ------------------------------------------------ */

void y264_mc_luma16_sse4(pixel *dst, int dstride, const pixel *ref, int rstride,
                         int ix, int iy, int fx, int fy, int h)
{
    mcl16_sse4(dst, dstride, ref, rstride, ix, iy, fx, fy, h);
}

void y264_mc_luma8_sse4(pixel *dst, int dstride, const pixel *ref, int rstride,
                        int ix, int iy, int fx, int fy, int h)
{
    y264_mc_luma8_x86(dst, dstride, ref, rstride, ix, iy, fx, fy, h);
}

void y264_mc_chroma_w8h_sse4(pixel *dst, int dstride, const pixel *ref,
                             int rstride, int ix, int iy, int fx, int fy, int h)
{
    y264_mc_chroma_w8h_x86(dst, dstride, ref, rstride, ix, iy, fx, fy, h);
}

void y264_mc_chroma_w4h_sse4(pixel *dst, int dstride, const pixel *ref,
                             int rstride, int ix, int iy, int fx, int fy, int h)
{
    y264_mc_chroma_w4h_x86(dst, dstride, ref, rstride, ix, iy, fx, fy, h);
}

void y264_pred_copy_sse4(pixel *dst, int dstride, const pixel *s, int sstride,
                         int w, int h)
{
    y264_pred_copy_x86(dst, dstride, s, sstride, w, h);
}

void y264_pred_avg2_sse4(pixel *dst, int dstride, const pixel *s1,
                         const pixel *s2, int sstride, int w, int h)
{
    y264_pred_avg2_x86(dst, dstride, s1, s2, sstride, w, h);
}

void y264_pixel_avg_wt_sse4(pixel *dst, const pixel *a, const pixel *b, int n,
                            int w0, int w1)
{
    int i = 0;
    if (w0 == 32 && w1 == 32) {
        for (; i + 16 <= n; i += 16)
            _mm_storeu_si128((__m128i *)(void *)(dst + i),
                             _mm_avg_epu8(y264_ld128(a + i), y264_ld128(b + i)));
    } else {
        __m128i k0 = _mm_set1_epi16((short)w0), k1 = _mm_set1_epi16((short)w1);
        for (; i + 16 <= n; i += 16)
            _mm_storeu_si128((__m128i *)(void *)(dst + i),
                             y264_avgwt16(a + i, b + i, k0, k1));
    }
    for (; i < n; i++) {
        int v = (a[i] * w0 + b[i] * w1 + 32) >> 6;
        dst[i] = (pixel)(v < 0 ? 0 : (v > PIXEL_MAX ? PIXEL_MAX : v));
    }
}

/* The interior of one half-pel scratch row. The caller guarantees x0 >= 2,
 * x1 <= pw - 3 and x1 - x0 >= 8; the last pass steps back to end ON the span,
 * which recomputes columns rather than skipping them and stays bit-exact
 * because every column is a pure function of the source. */
void y264_hpel_hrow_sse4(int32_t *srow, const pixel *row, int x0, int x1)
{
    for (int x = x0; x < x1; x += 8) {
        if (x > x1 - 8) x = x1 - 8;
        y264_hpel_hrow8(srow, row, x);
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
        y264_hpel_outrow8(Hr, Vr, Cr, s0, s1, s2, s3, s4, s5,
                          r0, r1, r2, r3, r4, r5, x);
    }
}

void y264_pixel_avg_wt_2d_sse4(pixel *dst, int ds, const pixel *a, int as,
                               const pixel *b, int bs, int w, int h, int w0, int w1)
{
    y264_avg_wt_2d_x86(dst, ds, a, as, b, bs, w, h, w0, w1);
}

void y264_avg_mb420_flat_sse4(pixel *pred, pixel *cu, pixel *cv,
                              const pixel *l0, int s0, const pixel *l1, int s1,
                              const pixel *u0, const pixel *v0, int t0,
                              const pixel *u1, const pixel *v1, int t1)
{
    y264_avg_mb420_flat_x86(pred, cu, cv, l0, s0, l1, s1, u0, v0, t0, u1, v1, t1);
}

void y264_mc_chroma_bi8_sse4(pixel *dst, int dstride,
                             const pixel *r0, int s0, int fx0, int fy0,
                             const pixel *r1, int s1, int fx1, int fy1,
                             int w0, int w1)
{
    y264_mc_chroma_bi8_x86(dst, dstride, r0, s0, fx0, fy0, r1, s1, fx1, fy1, w0, w1);
}

/* ---- the half-pel band steps at 128 bits ---------------------------------- */

static inline __m128i ld16s(const int16_t *p)
{
    return _mm_loadu_si128((const __m128i *)(const void *)p);
}

static inline void hpb_htap_sse4(const uint8_t *pad, int16_t *t)
{
    mcl16_hraw_sse4(pad, t);
}

static inline void hpb_hout_sse4(const int16_t *t, uint8_t *out)
{
    _mm_storeu_si128((__m128i *)(void *)out, y264_clip5_16(ld16s(t), ld16s(t + 8)));
}

/* `p` rows are already at the output column, so this is the 16-wide vertical
 * filter with the six rows named instead of strided. */
static inline void hpb_vout_sse4(const uint8_t *p0, const uint8_t *p1,
                                 const uint8_t *p2, const uint8_t *p3,
                                 const uint8_t *p4, const uint8_t *p5,
                                 uint8_t *out)
{
    __m128i lo = y264_vfilt8(p0, p1, p2, p3, p4, p5);
    __m128i hi = y264_vfilt8(p0 + 8, p1 + 8, p2 + 8, p3 + 8, p4 + 8, p5 + 8);
    _mm_storeu_si128((__m128i *)(void *)out, y264_clip5_16(lo, hi));
}

/* Eight columns of the centre plane from six rows of int16 taps. The pair
 * sums fit int16; (20, -5) against the interleaved (s23, s14) is one PMADDWD
 * and (1, 1) against (s05, 512) is the other, rounding included. */
static inline __m128i hpb_c8_sse4(const int16_t *t0, const int16_t *t1,
                                  const int16_t *t2, const int16_t *t3,
                                  const int16_t *t4, const int16_t *t5)
{
    __m128i s05 = _mm_add_epi16(ld16s(t0), ld16s(t5));
    __m128i s14 = _mm_add_epi16(ld16s(t1), ld16s(t4));
    __m128i s23 = _mm_add_epi16(ld16s(t2), ld16s(t3));
    __m128i kw = _mm_set1_epi32((int)(0xfffb0014u));     /* (20, -5) */
    __m128i k1 = _mm_set1_epi16(1), k512 = _mm_set1_epi16(512);
    __m128i lo = _mm_add_epi32(_mm_madd_epi16(_mm_unpacklo_epi16(s23, s14), kw),
                               _mm_madd_epi16(_mm_unpacklo_epi16(s05, k512), k1));
    __m128i hi = _mm_add_epi32(_mm_madd_epi16(_mm_unpackhi_epi16(s23, s14), kw),
                               _mm_madd_epi16(_mm_unpackhi_epi16(s05, k512), k1));
    return _mm_packs_epi32(_mm_srai_epi32(lo, 10), _mm_srai_epi32(hi, 10));
}

static inline void hpb_cout_sse4(const int16_t *t0, const int16_t *t1,
                                 const int16_t *t2, const int16_t *t3,
                                 const int16_t *t4, const int16_t *t5,
                                 uint8_t *out)
{
    __m128i a = hpb_c8_sse4(t0, t1, t2, t3, t4, t5);
    __m128i b = hpb_c8_sse4(t0 + 8, t1 + 8, t2 + 8, t3 + 8, t4 + 8, t5 + 8);
    _mm_storeu_si128((__m128i *)(void *)out, _mm_packus_epi16(a, b));
}

Y264_HPEL_BAND_BODY(y264_hpel_build_band_sse4, hpb_htap_sse4, hpb_hout_sse4,
                    hpb_vout_sse4, hpb_cout_sse4)

/* One row of a lowres bilinear phase plane, n samples. Sixteen columns per
 * pass with the last pass stepped back onto the span; a row shorter than one
 * register is the scalar formula. The output never aliases the two rows. */
void y264_bilin4_row_sse4(pixel *o, const pixel *r0, const pixel *r1, int n, int ax,
                          int w00, int w10, int w01, int w11)
{
    if (n >= 16) {
        __m128i wa = y264_mcc_w(w00, w10), wb = y264_mcc_w(w01, w11);
        for (int X = 0; X < n; X += 16) {
            if (X > n - 16) X = n - 16;
            _mm_storeu_si128((__m128i *)(void *)(o + X),
                             y264_bilin4_16(r0 + X, r1 + X, ax, wa, wb));
        }
        return;
    }
    for (int X = 0; X < n; X++) {
        int v = w00 * r0[X] + w10 * r0[X + ax] + w01 * r1[X] + w11 * r1[X + ax];
        o[X] = (pixel)((v + 8) >> 4);
    }
}

/* Eight columns per pass, the last pass stepped back to end on the span. The
 * outputs never alias the source rows, so the overlap recomputes the same
 * values. */
void y264_lr_hp_row_sse4(pixel *oh, pixel *ov, pixel *oc, const pixel *r0,
                         const pixel *r1, const pixel *r2, int n)
{
    for (int x = 0; x < n; x += 8) {
        if (x > n - 8) x = n - 8;
        y264_lr_hp_row8(oh, ov, oc, r0, r1, r2, x);
    }
}

#endif /* Y264_HAVE_SSE4 */
