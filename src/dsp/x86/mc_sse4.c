/*
 * mc_sse4.c - SSE4.2 kernels for the mc and hpel families
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Wave 2 of docs/x86-plan.md: the SSE4.2 twin of every mc and hpel kernel the
 * NEON twin covers. Each one is bit-exact with the portable reference in
 * src/dsp/mc.c -- the luma half-pel planes, the chroma bilinear, the two plane
 * fetches, the weighted average and the two half-pel plane build rows -- which
 * is what lets the tier be a runtime axis that cannot move a byte of output.
 *
 * The shared building blocks and the two plane-scheme bodies are in mc_x86.h.
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
#include "dsp/x86/mc_x86.h"

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

#endif /* Y264_HAVE_SSE4 */
