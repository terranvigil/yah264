/*
 * interp_avx2_10.c - AVX2 mc and hpel kernels for the 10-bit library
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * a 16-wide row of 16-bit samples is one 256-bit register, so the luma row
 * filters, the width-16 fetches and averages and the half-pel build step
 * sixteen columns a register. chroma is eight or four wide, so the upper lane
 * takes the next rows instead.
 *
 * the in-lane unpack, pack and hadd keep columns in order where a pack undoes
 * an unpack; where not (an int32 pack of two separately loaded halves, a
 * horizontal pair sum), a 64-bit permute puts the quarters back.
 *
 * shapes with no second half to fill, and the step-back row tails, use the
 * shared 128-bit bodies, VEX-encoded.
 */
#include "dsp/x86/interp_x86_10.h"

#if Y264_HAVE_AVX2_10

/* ---- 256-bit plumbing ---------------------------------------------------- */

static inline __m256i i10_ld16(const void *p)
{
    return _mm256_loadu_si256((const __m256i *)p);
}

static inline void i10_st16(void *p, __m256i v)
{
    _mm256_storeu_si256((__m256i *)p, v);
}

static inline __m256i i10_2x128(__m128i lo, __m128i hi)
{
    return _mm256_inserti128_si256(_mm256_castsi128_si256(lo), hi, 1);
}

static inline __m256i i10_tap6_off256(__m256i a, __m256i b, __m256i c,
                                      __m256i d, __m256i e, __m256i f)
{
    __m256i t = _mm256_add_epi16(_mm256_add_epi16(a, f), _mm256_set1_epi16(10240));
    t = _mm256_add_epi16(t, _mm256_mullo_epi16(_mm256_add_epi16(c, d),
                                               _mm256_set1_epi16(20)));
    return _mm256_sub_epi16(t, _mm256_mullo_epi16(_mm256_add_epi16(b, e),
                                                  _mm256_set1_epi16(5)));
}

static inline __m256i i10_half256(__m256i sum)
{
    __m256i v = _mm256_srli_epi16(_mm256_add_epi16(sum, _mm256_set1_epi16(16)), 5);
    return _mm256_min_epu16(_mm256_subs_epu16(v, _mm256_set1_epi16(320)),
                            _mm256_set1_epi16(PIXEL_MAX));
}

static inline __m256i i10_tap6_32x8(__m256i a, __m256i b, __m256i c,
                                    __m256i d, __m256i e, __m256i f)
{
    __m256i s05 = _mm256_add_epi32(a, f);
    __m256i s14 = _mm256_add_epi32(b, e);
    __m256i s23 = _mm256_add_epi32(c, d);
    __m256i m5  = _mm256_add_epi32(_mm256_slli_epi32(s14, 2), s14);
    __m256i m20 = _mm256_add_epi32(_mm256_slli_epi32(s23, 4), _mm256_slli_epi32(s23, 2));
    return _mm256_add_epi32(_mm256_sub_epi32(s05, m5), m20);
}

/* lo:hi must come from an in-lane unpack, which this pack undoes */
static inline __m256i i10_clip32_256(__m256i lo, __m256i hi)
{
    return _mm256_min_epu16(_mm256_packus_epi32(lo, hi), _mm256_set1_epi16(PIXEL_MAX));
}

/* two runs of eight int32 columns, a then b, to sixteen samples in order */
static inline __m256i i10_clip32_ab(__m256i a, __m256i b)
{
    return _mm256_permute4x64_epi64(i10_clip32_256(a, b), 0xd8);
}

static inline __m256i i10_hsum16(const pixel *p)
{
    return i10_tap6_off256(i10_ld16(p), i10_ld16(p + 1), i10_ld16(p + 2),
                           i10_ld16(p + 3), i10_ld16(p + 4), i10_ld16(p + 5));
}

static inline __m256i i10_centre256(__m256i c0, __m256i c1, __m256i c2,
                                    __m256i c3, __m256i c4, __m256i c5)
{
    const __m256i z = _mm256_setzero_si256();
    __m256i lo = i10_tap6_32x8(_mm256_unpacklo_epi16(c0, z), _mm256_unpacklo_epi16(c1, z),
                               _mm256_unpacklo_epi16(c2, z), _mm256_unpacklo_epi16(c3, z),
                               _mm256_unpacklo_epi16(c4, z), _mm256_unpacklo_epi16(c5, z));
    __m256i hi = i10_tap6_32x8(_mm256_unpackhi_epi16(c0, z), _mm256_unpackhi_epi16(c1, z),
                               _mm256_unpackhi_epi16(c2, z), _mm256_unpackhi_epi16(c3, z),
                               _mm256_unpackhi_epi16(c4, z), _mm256_unpackhi_epi16(c5, z));
    __m256i k = _mm256_set1_epi32(512 - 320 * 1024);
    return i10_clip32_256(_mm256_srai_epi32(_mm256_add_epi32(lo, k), 10),
                          _mm256_srai_epi32(_mm256_add_epi32(hi, k), 10));
}

/* ---- luma --------------------------------------------------------------- */

static inline void hcl16_avx2(const pixel *p, pixel *out)
{
    i10_st16(out, i10_half256(i10_hsum16(p)));
}

static inline void hsum16_avx2(const pixel *p, uint16_t *out)
{
    i10_st16(out, i10_hsum16(p));
}

static inline void vcl16_avx2(const pixel *p, int rs, pixel *out)
{
    i10_st16(out, i10_half256(i10_tap6_off256(i10_ld16(p - 2 * rs), i10_ld16(p - rs),
                                              i10_ld16(p), i10_ld16(p + rs),
                                              i10_ld16(p + 2 * rs),
                                              i10_ld16(p + 3 * rs))));
}

static inline void jcl16_avx2(const uint16_t *c, pixel *out)
{
    i10_st16(out, i10_centre256(i10_ld16(c), i10_ld16(c + I10_ST),
                                i10_ld16(c + 2 * I10_ST), i10_ld16(c + 3 * I10_ST),
                                i10_ld16(c + 4 * I10_ST), i10_ld16(c + 5 * I10_ST)));
}

I10_LUMA_BODY(i10_mc_luma16_avx2, 16, hcl16_avx2, hsum16_avx2, vcl16_avx2, jcl16_avx2)

void y264_mc_luma16_avx2(pixel *dst, int dstride, const pixel *ref, int rstride,
                         int ix, int iy, int fx, int fy, int h)
{ i10_mc_luma16_avx2(dst, dstride, ref, rstride, ix, iy, fx, fy, h); }

void y264_mc_luma8_avx2(pixel *dst, int dstride, const pixel *ref, int rstride,
                        int ix, int iy, int fx, int fy, int h)
{ i10_mc_luma8(dst, dstride, ref, rstride, ix, iy, fx, fy, h); }

/* ---- chroma -----------------------------------------------------------------
 *
 * the u16 bilinear of interp_x86_10.h, rows in both lanes. */
struct i10_cw256 { __m256i a, b, c, d; };

static inline struct i10_cw256 i10_cweights256(int fx, int fy)
{
    struct i10_cw256 w = {
        _mm256_set1_epi16((short)((8 - fx) * (8 - fy))),
        _mm256_set1_epi16((short)(fx * (8 - fy))),
        _mm256_set1_epi16((short)((8 - fx) * fy)),
        _mm256_set1_epi16((short)(fx * fy)),
    };
    return w;
}

static inline __m256i i10_bilin256(__m256i A, __m256i B, __m256i C, __m256i D,
                                   const struct i10_cw256 *w)
{
    __m256i v = _mm256_add_epi16(_mm256_mullo_epi16(A, w->a), _mm256_mullo_epi16(B, w->b));
    v = _mm256_add_epi16(v, _mm256_mullo_epi16(C, w->c));
    v = _mm256_add_epi16(v, _mm256_mullo_epi16(D, w->d));
    return _mm256_srli_epi16(_mm256_add_epi16(v, _mm256_set1_epi16(32)), 6);
}

/* output rows y and y + 1 from source rows r .. r + 2s: row y's lower source
 * row is row y + 1's upper */
static inline __m256i i10_bilin8x2(const pixel *r, int s, const struct i10_cw256 *w)
{
    __m128i a0 = i10_ld8(r), b0 = i10_ld8(r + 1);
    __m128i a1 = i10_ld8(r + s), b1 = i10_ld8(r + s + 1);
    __m128i a2 = i10_ld8(r + 2 * s), b2 = i10_ld8(r + 2 * s + 1);
    return i10_bilin256(i10_2x128(a0, a1), i10_2x128(b0, b1),
                        i10_2x128(a1, a2), i10_2x128(b1, b2), w);
}

static inline void i10_st8x2(pixel *dst, int ds, __m256i v)
{
    i10_st8(dst, _mm256_castsi256_si128(v));
    i10_st8(dst + ds, _mm256_extracti128_si256(v, 1));
}

void y264_mc_chroma_w8h_avx2(pixel *dst, int dstride, const pixel *ref,
                             int rstride, int ix, int iy, int fx, int fy, int h)
{
    if (!(fx | fy)) {
        i10_mc_chroma_w8h(dst, dstride, ref, rstride, ix, iy, 0, 0, h);
        return;
    }
    struct i10_cw256 w = i10_cweights256(fx, fy);
    const pixel *r = ref + (ptrdiff_t)iy * rstride + ix;
    int y = 0;
    for (; y + 2 <= h; y += 2, r += 2 * rstride)
        i10_st8x2(dst + (size_t)y * dstride, dstride, i10_bilin8x2(r, rstride, &w));
    if (y < h)
        i10_mc_chroma_w8h(dst + (size_t)y * dstride, dstride, ref, rstride,
                          ix, iy + y, fx, fy, h - y);
}

/* four output rows a pass, two a lane */
void y264_mc_chroma_w4h_avx2(pixel *dst, int dstride, const pixel *ref,
                             int rstride, int ix, int iy, int fx, int fy, int h)
{
    if (!(fx | fy)) {
        i10_mc_chroma_w4h(dst, dstride, ref, rstride, ix, iy, 0, 0, h);
        return;
    }
    struct i10_cw256 w = i10_cweights256(fx, fy);
    int y = 0;
    for (; y + 4 <= h; y += 4) {
        const pixel *r0 = ref + (ptrdiff_t)(iy + y) * rstride + ix;
        const pixel *r1 = r0 + rstride, *r2 = r1 + rstride;
        const pixel *r3 = r2 + rstride, *r4 = r3 + rstride;
        __m256i v = i10_bilin256(
            i10_2x128(i10_rows4(r0, r1), i10_rows4(r2, r3)),
            i10_2x128(i10_rows4(r0 + 1, r1 + 1), i10_rows4(r2 + 1, r3 + 1)),
            i10_2x128(i10_rows4(r1, r2), i10_rows4(r3, r4)),
            i10_2x128(i10_rows4(r1 + 1, r2 + 1), i10_rows4(r3 + 1, r4 + 1)), &w);
        __m128i lo = _mm256_castsi256_si128(v), hi = _mm256_extracti128_si256(v, 1);
        pixel *d = dst + (size_t)y * dstride;
        i10_st4(d, lo);
        i10_st4(d + dstride, _mm_unpackhi_epi64(lo, lo));
        i10_st4(d + 2 * dstride, hi);
        i10_st4(d + 3 * dstride, _mm_unpackhi_epi64(hi, hi));
    }
    if (y < h)
        i10_mc_chroma_w4h(dst + (size_t)y * dstride, dstride, ref, rstride,
                          ix, iy + y, fx, fy, h - y);
}

/* ---- fetches and averages ------------------------------------------------ */

void y264_pred_copy_avx2(pixel *dst, int dstride, const pixel *s, int sstride,
                         int w, int h)
{
    if (w != 16) {
        i10_pred_copy(dst, dstride, s, sstride, w, h);
        return;
    }
    for (int y = 0; y < h; y++, dst += dstride, s += sstride)
        i10_st16(dst, i10_ld16(s));
}

void y264_pred_avg2_avx2(pixel *dst, int dstride, const pixel *s1,
                         const pixel *s2, int sstride, int w, int h)
{
    if (w != 16) {
        i10_pred_avg2(dst, dstride, s1, s2, sstride, w, h);
        return;
    }
    for (int y = 0; y < h; y++, dst += dstride, s1 += sstride, s2 += sstride)
        i10_st16(dst, _mm256_avg_epu16(i10_ld16(s1), i10_ld16(s2)));
}

/* sixteen weighted samples: the in-lane unpack and int32 pack cancel */
static inline __m256i i10_wt16(__m256i a, __m256i b, __m256i k)
{
    __m256i r = _mm256_set1_epi32(32);
    __m256i lo = _mm256_add_epi32(_mm256_madd_epi16(_mm256_unpacklo_epi16(a, b), k), r);
    __m256i hi = _mm256_add_epi32(_mm256_madd_epi16(_mm256_unpackhi_epi16(a, b), k), r);
    return i10_clip32_256(_mm256_srai_epi32(lo, 6), _mm256_srai_epi32(hi, 6));
}

static inline __m256i i10_wpair256(int w0, int w1)
{
    return _mm256_set1_epi32((int)(((uint32_t)w0 & 0xffffu) | ((uint32_t)w1 << 16)));
}

/* sixteen per pass, the rest through the 128-bit row */
static inline void i10_wt_row16(pixel *dst, const pixel *a, const pixel *b, int n,
                                int w0, int w1)
{
    int i = 0;
    if (w0 == 32 && w1 == 32) {
        for (; i + 16 <= n; i += 16)
            i10_st16(dst + i, _mm256_avg_epu16(i10_ld16(a + i), i10_ld16(b + i)));
    } else if (i10_wt_ok(w0, w1)) {
        __m256i k = i10_wpair256(w0, w1);
        for (; i + 16 <= n; i += 16)
            i10_st16(dst + i, i10_wt16(i10_ld16(a + i), i10_ld16(b + i), k));
    }
    if (i < n)
        i10_wt_row(dst + i, a + i, b + i, n - i, w0, w1);
}

void y264_pixel_avg_wt_avx2(pixel *dst, const pixel *a, const pixel *b, int n,
                            int w0, int w1)
{ i10_wt_row16(dst, a, b, n, w0, w1); }

void y264_pixel_avg_wt_2d_avx2(pixel *dst, int ds, const pixel *a, int as,
                               const pixel *b, int bs, int w, int h, int w0, int w1)
{
    if (w != 16) {
        i10_avg_wt_2d(dst, ds, a, as, b, bs, w, h, w0, w1);
        return;
    }
    for (int y = 0; y < h; y++, dst += ds, a += as, b += bs)
        i10_wt_row16(dst, a, b, 16, w0, w1);
}

void y264_avg_mb420_flat_avx2(pixel *pred, pixel *cu, pixel *cv,
                              const pixel *l0, int s0, const pixel *l1, int s1,
                              const pixel *u0, const pixel *v0, int t0,
                              const pixel *u1, const pixel *v1, int t1)
{
    for (int y = 0; y < 16; y++, l0 += s0, l1 += s1)
        i10_st16(pred + 16 * y, _mm256_avg_epu16(i10_ld16(l0), i10_ld16(l1)));
    for (int y = 0; y < 8; y++, u0 += t0, v0 += t0, u1 += t1, v1 += t1) {
        i10_st8(cu + 8 * y, _mm_avg_epu16(i10_ld8(u0), i10_ld8(u1)));
        i10_st8(cv + 8 * y, _mm_avg_epu16(i10_ld8(v0), i10_ld8(v1)));
    }
}

/* two output rows per pass for each list, then their average */
void y264_mc_chroma_bi8_avx2(pixel *dst, int dstride,
                             const pixel *r0, int s0, int fx0, int fy0,
                             const pixel *r1, int s1, int fx1, int fy1,
                             int w0, int w1)
{
    if (!i10_wt_ok(w0, w1)) {
        i10_mc_chroma_bi8(dst, dstride, r0, s0, fx0, fy0, r1, s1, fx1, fy1, w0, w1);
        return;
    }
    struct i10_cw256 a = i10_cweights256(fx0, fy0), b = i10_cweights256(fx1, fy1);
    int flat = w0 == 32 && w1 == 32;
    __m256i k = i10_wpair256(w0, w1);
    for (int y = 0; y < 8; y += 2, r0 += 2 * s0, r1 += 2 * s1, dst += 2 * dstride) {
        __m256i p = i10_bilin8x2(r0, s0, &a), q = i10_bilin8x2(r1, s1, &b);
        i10_st8x2(dst, dstride, flat ? _mm256_avg_epu16(p, q) : i10_wt16(p, q, k));
    }
}

/* ---- the half-pel plane build -------------------------------------------- */

/* the span is only guaranteed eight columns, so the wide loop may not run;
 * the 128-bit step backs up onto the span for the tail */
void y264_hpel_hrow_avx2(int32_t *srow, const pixel *row, int x0, int x1)
{
    const __m256i k = _mm256_set1_epi32(10240);
    int x = x0;
    for (; x + 16 <= x1; x += 16) {
        __m256i t = i10_hsum16(row + x - 2);
        i10_st16(srow + x, _mm256_sub_epi32(
            _mm256_cvtepu16_epi32(_mm256_castsi256_si128(t)), k));
        i10_st16(srow + x + 8, _mm256_sub_epi32(
            _mm256_cvtepu16_epi32(_mm256_extracti128_si256(t, 1)), k));
    }
    for (; x < x1; x += 8) {
        if (x > x1 - 8) x = x1 - 8;
        i10_hrow8(srow, row, x);
    }
}

void y264_hpel_outrow_avx2(pixel *Hr, pixel *Vr, pixel *Cr,
                           const int32_t *s0, const int32_t *s1, const int32_t *s2,
                           const int32_t *s3, const int32_t *s4, const int32_t *s5,
                           const pixel *r0, const pixel *r1, const pixel *r2,
                           const pixel *r3, const pixel *r4, const pixel *r5,
                           int x0, int x1)
{
    const __m256i k16 = _mm256_set1_epi32(16), k512 = _mm256_set1_epi32(512);
    int x = x0;
    for (; x + 16 <= x1; x += 16) {
        __m256i a = i10_ld16(s2 + x), b = i10_ld16(s2 + x + 8);
        i10_st16(Hr + x, i10_clip32_ab(_mm256_srai_epi32(_mm256_add_epi32(a, k16), 5),
                                       _mm256_srai_epi32(_mm256_add_epi32(b, k16), 5)));
        __m256i ca = i10_tap6_32x8(i10_ld16(s0 + x), i10_ld16(s1 + x), a,
                                   i10_ld16(s3 + x), i10_ld16(s4 + x), i10_ld16(s5 + x));
        __m256i cb = i10_tap6_32x8(i10_ld16(s0 + x + 8), i10_ld16(s1 + x + 8), b,
                                   i10_ld16(s3 + x + 8), i10_ld16(s4 + x + 8),
                                   i10_ld16(s5 + x + 8));
        i10_st16(Cr + x, i10_clip32_ab(_mm256_srai_epi32(_mm256_add_epi32(ca, k512), 10),
                                       _mm256_srai_epi32(_mm256_add_epi32(cb, k512), 10)));
        i10_st16(Vr + x, i10_half256(i10_tap6_off256(i10_ld16(r0 + x), i10_ld16(r1 + x),
                                                     i10_ld16(r2 + x), i10_ld16(r3 + x),
                                                     i10_ld16(r4 + x), i10_ld16(r5 + x))));
    }
    for (; x < x1; x += 8) {
        if (x > x1 - 8) x = x1 - 8;
        i10_outrow8(Hr, Vr, Cr, s0, s1, s2, s3, s4, s5, r0, r1, r2, r3, r4, r5, x);
    }
}

static inline void hpb_hsum_avx2(const pixel *pad, uint16_t *t)
{
    i10_st16(t, i10_hsum16(pad));
}

static inline void hpb_hv_avx2(const uint16_t *t2, const pixel *p0, const pixel *p1,
                               const pixel *p2, const pixel *p3, const pixel *p4,
                               const pixel *p5, pixel *H, pixel *V)
{
    i10_st16(H, i10_half256(i10_ld16(t2)));
    i10_st16(V, i10_half256(i10_tap6_off256(i10_ld16(p0), i10_ld16(p1), i10_ld16(p2),
                                            i10_ld16(p3), i10_ld16(p4), i10_ld16(p5))));
}

static inline void hpb_c_avx2(const uint16_t *t0, const uint16_t *t1,
                              const uint16_t *t2, const uint16_t *t3,
                              const uint16_t *t4, const uint16_t *t5, pixel *out)
{
    i10_st16(out, i10_centre256(i10_ld16(t0), i10_ld16(t1), i10_ld16(t2),
                                i10_ld16(t3), i10_ld16(t4), i10_ld16(t5)));
}

I10_BAND_BODY(y264_hpel_build_band_avx2, 16, hpb_hsum_avx2, hpb_hv_avx2, hpb_c_avx2)

/* ---- the lowres half-pel row ------------------------------------------------
 *
 * sixteen outputs from 32 vertical means. the pair sum is per 128-bit lane,
 * so quarters come out as outputs 0-3, 8-11, 4-7, 12-15 and a 64-bit permute
 * orders them. */
static inline __m256i i10_lr_m256(__m256i a, __m256i b)
{
    return _mm256_srli_epi16(_mm256_add_epi16(_mm256_add_epi16(a, b),
                                              _mm256_set1_epi16(4)), 3);
}

static inline __m256i i10_lr_pairs256(__m256i m0, __m256i m1)
{
    __m256i s = _mm256_add_epi16(_mm256_hadd_epi16(m0, m1), _mm256_set1_epi16(1));
    s = _mm256_min_epu16(_mm256_srli_epi16(s, 1), _mm256_set1_epi16(255));
    return _mm256_permute4x64_epi64(s, 0xd8);
}

void y264_lr_hp_row_avx2(pixel *oh, pixel *ov, pixel *oc, const pixel *r0,
                         const pixel *r1, const pixel *r2, int n)
{
    int x = 0;
    for (; x + 16 <= n; x += 16) {
        const int i = 2 * x;
        __m256i a0 = i10_ld16(r0 + i + 1), a1 = i10_ld16(r0 + i + 17);
        __m256i b0 = i10_ld16(r1 + i), b1 = i10_ld16(r1 + i + 16);
        __m256i c0 = i10_ld16(r2 + i), c1 = i10_ld16(r2 + i + 16);
        __m256i d0 = i10_ld16(r1 + i + 1), d1 = i10_ld16(r1 + i + 17);
        __m256i e0 = i10_ld16(r2 + i + 1), e1 = i10_ld16(r2 + i + 17);
        i10_st16(oh + x, i10_lr_pairs256(i10_lr_m256(a0, d0), i10_lr_m256(a1, d1)));
        i10_st16(ov + x, i10_lr_pairs256(i10_lr_m256(b0, c0), i10_lr_m256(b1, c1)));
        i10_st16(oc + x, i10_lr_pairs256(i10_lr_m256(d0, e0), i10_lr_m256(d1, e1)));
    }
    for (; x < n; x += 8) {
        if (x > n - 8) x = n - 8;
        i10_lr_hp_row8(oh, ov, oc, r0, r1, r2, x);
    }
}

#endif /* Y264_HAVE_AVX2_10 */
