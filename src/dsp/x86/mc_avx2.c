/*
 * mc_avx2.c - AVX2 kernels for the mc and hpel families
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Wave 2 of docs/x86-plan.md, the wide half. The same nine kernels as the
 * SSE4.2 file and the same values out of them, bit for bit: the two tiers are
 * an A/B axis for speed and never for output, which is what the kit's identity
 * leg exists to hold them to.
 *
 * WHERE A SECOND LANE IS WORTH SOMETHING HERE, AND WHERE IT IS WORTH NOTHING.
 *
 * This family's shapes are ROWS, and a row's width decides everything. Sixteen
 * columns of 16-bit lanes is exactly one 256-bit register, so a 16-wide luma
 * row -- the horizontal tap, the vertical tap, the centre tap -- goes from two
 * registers to one and the whole plane build halves its instruction count.
 * A half-pel plane build row is unbounded in width, so it simply steps sixteen
 * columns at a time instead of eight. The weighted average is a PACKED run
 * with no stride in it at all, which is the friendliest shape in the encoder:
 * sixteen samples widen into one register and the multiply chain runs once
 * where SSE4.2 runs it twice. The chroma bilinear has only eight or four
 * columns, so the upper lane takes the NEXT ROW instead: the 8-wide form does
 * two output rows per pass and the 4-wide form four, and both get their extra
 * source row for free because a row pair's lower samples are the next pair's
 * upper ones.
 *
 * Where a shape has no second half to offer, this file calls the shared
 * 128-bit body in mc_x86.h rather than pretend. The 8-wide luma kernel is one:
 * its rows are eight lanes and its plane buffers are eight columns, so a
 * 256-bit register would carry a zeroed upper lane through a whole plane
 * build. Both plane fetches are another: a prediction row is at most sixteen
 * BYTES, and the source and destination strides differ, so a row is the
 * largest unit there is and PAVGB on it is already the whole kernel. Compiled
 * here those bodies are VEX-encoded and get the three-operand form, and that
 * is the whole of what AVX2 has to give them.
 *
 * C11 intrinsics only. There is no assembly in this tree, and hygiene_check
 * refuses any inline form under this directory.
 */
#include "dsp/x86/mc_x86.h"

#if Y264_HAVE_AVX2

/* ---- 256-bit plumbing ---------------------------------------------------- */

static inline __m256i y256_2x128(__m128i lo, __m128i hi)
{
    return _mm256_inserti128_si256(_mm256_castsi128_si256(lo), hi, 1);
}

static inline __m256i y256_ld256(const void *p)
{
    return _mm256_loadu_si256((const __m256i *)p);
}

static inline void y256_st256(void *p, __m256i v)
{
    _mm256_storeu_si256((__m256i *)p, v);
}

/* Sixteen samples, zero-extended to 16-bit lanes: one row of a 16-wide block. */
static inline __m256i y256_ld16w(const uint8_t *p)
{
    return _mm256_cvtepu8_epi16(y264_ld128(p));
}

static inline __m256i y256_tap6_16(__m256i a, __m256i b, __m256i c,
                                   __m256i d, __m256i e, __m256i f)
{
    __m256i s05 = _mm256_add_epi16(a, f);
    __m256i s14 = _mm256_add_epi16(b, e);
    __m256i s23 = _mm256_add_epi16(c, d);
    __m256i t = _mm256_sub_epi16(s05, _mm256_mullo_epi16(s14, _mm256_set1_epi16(5)));
    return _mm256_add_epi16(t, _mm256_mullo_epi16(s23, _mm256_set1_epi16(20)));
}

static inline __m256i y256_tap6_32(__m256i a, __m256i b, __m256i c,
                                   __m256i d, __m256i e, __m256i f)
{
    __m256i s05 = _mm256_add_epi32(a, f);
    __m256i s14 = _mm256_add_epi32(b, e);
    __m256i s23 = _mm256_add_epi32(c, d);
    __m256i m5  = _mm256_add_epi32(_mm256_slli_epi32(s14, 2), s14);
    __m256i m20 = _mm256_add_epi32(_mm256_slli_epi32(s23, 4),
                                   _mm256_slli_epi32(s23, 2));
    return _mm256_add_epi32(_mm256_sub_epi32(s05, m5), m20);
}

/* Sixteen 16-bit lanes down to sixteen bytes, saturating at both ends -- the
 * Clip1 -- and in order. The pack is done on the two halves by hand rather
 * than with the 256-bit form, because that one packs WITHIN each 128-bit lane
 * and would interleave the halves of the row. */
static inline __m128i y256_packu8(__m256i x)
{
    return _mm_packus_epi16(_mm256_castsi256_si128(x),
                            _mm256_extracti128_si256(x, 1));
}

/* clip1((t + 16) >> 5) for sixteen lanes. */
static inline __m128i y256_clip5(__m256i t)
{
    return y256_packu8(_mm256_srai_epi16(_mm256_add_epi16(t, _mm256_set1_epi16(16)), 5));
}

/* Eight 32-bit lanes down to eight 16-bit ones, in order. */
static inline __m128i y256_pack32(__m256i v)
{
    return _mm_packs_epi32(_mm256_castsi256_si128(v),
                           _mm256_extracti128_si256(v, 1));
}

/* ---- the sixteen-column row filters -------------------------------------- */

static inline __m256i mcl16_h_avx2(const uint8_t *p)
{
    return y256_tap6_16(y256_ld16w(p + 0), y256_ld16w(p + 1),
                        y256_ld16w(p + 2), y256_ld16w(p + 3),
                        y256_ld16w(p + 4), y256_ld16w(p + 5));
}

static inline void mcl16_hclip_avx2(const uint8_t *p, uint8_t *out)
{
    _mm_storeu_si128((__m128i *)(void *)out, y256_clip5(mcl16_h_avx2(p)));
}

static inline void mcl16_hraw_avx2(const uint8_t *p, int16_t *out)
{
    y256_st256(out, mcl16_h_avx2(p));
}

static inline void mcl16_v_avx2(const uint8_t *p, int rstride, uint8_t *out)
{
    __m256i t = y256_tap6_16(y256_ld16w(p - 2 * rstride), y256_ld16w(p - rstride),
                             y256_ld16w(p), y256_ld16w(p + rstride),
                             y256_ld16w(p + 2 * rstride), y256_ld16w(p + 3 * rstride));
    _mm_storeu_si128((__m128i *)(void *)out, y256_clip5(t));
}

static inline void mcl16_j_avx2(const int16_t *c0, const int16_t *c1,
                                const int16_t *c2, const int16_t *c3,
                                const int16_t *c4, const int16_t *c5,
                                uint8_t *out)
{
    __m256i r0 = y256_ld256(c0), r1 = y256_ld256(c1), r2 = y256_ld256(c2);
    __m256i r3 = y256_ld256(c3), r4 = y256_ld256(c4), r5 = y256_ld256(c5);
    __m256i lo = y256_tap6_32(_mm256_cvtepi16_epi32(_mm256_castsi256_si128(r0)),
                              _mm256_cvtepi16_epi32(_mm256_castsi256_si128(r1)),
                              _mm256_cvtepi16_epi32(_mm256_castsi256_si128(r2)),
                              _mm256_cvtepi16_epi32(_mm256_castsi256_si128(r3)),
                              _mm256_cvtepi16_epi32(_mm256_castsi256_si128(r4)),
                              _mm256_cvtepi16_epi32(_mm256_castsi256_si128(r5)));
    __m256i hi = y256_tap6_32(_mm256_cvtepi16_epi32(_mm256_extracti128_si256(r0, 1)),
                              _mm256_cvtepi16_epi32(_mm256_extracti128_si256(r1, 1)),
                              _mm256_cvtepi16_epi32(_mm256_extracti128_si256(r2, 1)),
                              _mm256_cvtepi16_epi32(_mm256_extracti128_si256(r3, 1)),
                              _mm256_cvtepi16_epi32(_mm256_extracti128_si256(r4, 1)),
                              _mm256_cvtepi16_epi32(_mm256_extracti128_si256(r5, 1)));
    __m256i k = _mm256_set1_epi32(512);
    __m128i a = y256_pack32(_mm256_srai_epi32(_mm256_add_epi32(lo, k), 10));
    __m128i b = y256_pack32(_mm256_srai_epi32(_mm256_add_epi32(hi, k), 10));
    _mm_storeu_si128((__m128i *)(void *)out, _mm_packus_epi16(a, b));
}

Y264_MC_LUMA16_BODY(mcl16_avx2, mcl16_hclip_avx2, mcl16_hraw_avx2,
                    mcl16_v_avx2, mcl16_j_avx2)

/* ---- the exported kernels ------------------------------------------------ */

void y264_mc_luma16_avx2(pixel *dst, int dstride, const pixel *ref, int rstride,
                         int ix, int iy, int fx, int fy, int h)
{
    mcl16_avx2(dst, dstride, ref, rstride, ix, iy, fx, fy, h);
}

void y264_mc_luma8_avx2(pixel *dst, int dstride, const pixel *ref, int rstride,
                        int ix, int iy, int fx, int fy, int h)
{
    y264_mc_luma8_x86(dst, dstride, ref, rstride, ix, iy, fx, fy, h);
}

/* Two output rows per pass. The three source rows a pair needs overlap: the
 * lower pair of row y IS the upper pair of row y+1, so two interleaves serve
 * both rows and the odd height, which the dispatcher does not produce but
 * checkasm is free to ask for, falls to the 128-bit row. */
void y264_mc_chroma_w8h_avx2(pixel *dst, int dstride, const pixel *ref,
                             int rstride, int ix, int iy, int fx, int fy, int h)
{
    __m256i wab = _mm256_set1_epi16((short)(((8 - fx) * (8 - fy)) |
                                            ((fx * (8 - fy)) << 8)));
    __m256i wcd = _mm256_set1_epi16((short)(((8 - fx) * fy) | ((fx * fy) << 8)));
    __m256i r32 = _mm256_set1_epi16(32);
    int y = 0;
    for (; y + 2 <= h; y += 2) {
        const uint8_t *r0 = ref + (size_t)(iy + y) * rstride + ix;
        const uint8_t *r1 = r0 + rstride, *r2 = r1 + rstride;
        __m128i p0 = _mm_unpacklo_epi8(y264_ld64(r0), y264_ld64(r0 + 1));
        __m128i p1 = _mm_unpacklo_epi8(y264_ld64(r1), y264_ld64(r1 + 1));
        __m128i p2 = _mm_unpacklo_epi8(y264_ld64(r2), y264_ld64(r2 + 1));
        __m256i acc = _mm256_add_epi16(
            _mm256_maddubs_epi16(y256_2x128(p0, p1), wab),
            _mm256_maddubs_epi16(y256_2x128(p1, p2), wcd));
        acc = _mm256_srli_epi16(_mm256_add_epi16(acc, r32), 6);
        __m128i o = y256_packu8(acc);
        _mm_storel_epi64((__m128i *)(void *)(dst + (size_t)y * dstride), o);
        _mm_storel_epi64((__m128i *)(void *)(dst + (size_t)(y + 1) * dstride),
                         _mm_srli_si128(o, 8));
    }
    for (; y < h; y++) {
        __m128i wab8 = y264_mcc_w((8 - fx) * (8 - fy), fx * (8 - fy));
        __m128i wcd8 = y264_mcc_w((8 - fx) * fy, fx * fy);
        _mm_storel_epi64((__m128i *)(void *)(dst + (size_t)y * dstride),
                         y264_mcc_row8(ref + (size_t)(iy + y) * rstride + ix,
                                       rstride, wab8, wcd8));
    }
}

/* Four output rows per pass: four columns is a quarter of a 128-bit register,
 * so the packing that makes a 4-wide row worth vectorising at all simply goes
 * one step further here. */
void y264_mc_chroma_w4h_avx2(pixel *dst, int dstride, const pixel *ref,
                             int rstride, int ix, int iy, int fx, int fy, int h)
{
    __m256i wab = _mm256_set1_epi16((short)(((8 - fx) * (8 - fy)) |
                                            ((fx * (8 - fy)) << 8)));
    __m256i wcd = _mm256_set1_epi16((short)(((8 - fx) * fy) | ((fx * fy) << 8)));
    __m256i r32 = _mm256_set1_epi16(32);
    int y = 0;
    for (; y + 4 <= h; y += 4) {
        const uint8_t *r0 = ref + (size_t)(iy + y) * rstride + ix;
        const uint8_t *r1 = r0 + rstride, *r2 = r1 + rstride;
        const uint8_t *r3 = r2 + rstride, *r4 = r3 + rstride;
        __m256i ab = y256_2x128(y264_mcc_pair4(r0, r1), y264_mcc_pair4(r2, r3));
        __m256i cd = y256_2x128(y264_mcc_pair4(r1, r2), y264_mcc_pair4(r3, r4));
        __m256i acc = _mm256_add_epi16(_mm256_maddubs_epi16(ab, wab),
                                       _mm256_maddubs_epi16(cd, wcd));
        acc = _mm256_srli_epi16(_mm256_add_epi16(acc, r32), 6);
        __m128i o = y256_packu8(acc);
        int32_t w0 = _mm_cvtsi128_si32(o),      w1 = _mm_extract_epi32(o, 1);
        int32_t w2 = _mm_extract_epi32(o, 2),   w3 = _mm_extract_epi32(o, 3);
        memcpy(dst + (size_t)(y + 0) * dstride, &w0, 4);
        memcpy(dst + (size_t)(y + 1) * dstride, &w1, 4);
        memcpy(dst + (size_t)(y + 2) * dstride, &w2, 4);
        memcpy(dst + (size_t)(y + 3) * dstride, &w3, 4);
    }
    if (y < h)
        y264_mc_chroma_w4h_x86(dst + (size_t)y * dstride, dstride, ref, rstride,
                               ix, iy + y, fx, fy, h - y);
}

void y264_pred_copy_avx2(pixel *dst, int dstride, const pixel *s, int sstride,
                         int w, int h)
{
    y264_pred_copy_x86(dst, dstride, s, sstride, w, h);
}

void y264_pred_avg2_avx2(pixel *dst, int dstride, const pixel *s1,
                         const pixel *s2, int sstride, int w, int h)
{
    y264_pred_avg2_x86(dst, dstride, s1, s2, sstride, w, h);
}

void y264_pixel_avg_wt_avx2(pixel *dst, const pixel *a, const pixel *b, int n,
                            int w0, int w1)
{
    int i = 0;
    if (w0 == 32 && w1 == 32) {
        for (; i + 32 <= n; i += 32)
            y256_st256(dst + i, _mm256_avg_epu8(y256_ld256(a + i),
                                                y256_ld256(b + i)));
        for (; i + 16 <= n; i += 16)
            _mm_storeu_si128((__m128i *)(void *)(dst + i),
                             _mm_avg_epu8(y264_ld128(a + i), y264_ld128(b + i)));
    } else {
        __m256i k0 = _mm256_set1_epi16((short)w0);
        __m256i k1 = _mm256_set1_epi16((short)w1);
        __m256i r = _mm256_set1_epi16(32);
        for (; i + 16 <= n; i += 16) {
            __m256i va = y256_ld16w(a + i), vb = y256_ld16w(b + i);
            __m256i t = _mm256_add_epi16(_mm256_add_epi16(
                            _mm256_mullo_epi16(va, k0),
                            _mm256_mullo_epi16(vb, k1)), r);
            _mm_storeu_si128((__m128i *)(void *)(dst + i),
                             y256_packu8(_mm256_srai_epi16(t, 6)));
        }
    }
    for (; i < n; i++) {
        int v = (a[i] * w0 + b[i] * w1 + 32) >> 6;
        dst[i] = (pixel)(v < 0 ? 0 : (v > PIXEL_MAX ? PIXEL_MAX : v));
    }
}

/* Sixteen columns per pass, then the 128-bit form steps back onto the span --
 * the caller guarantees eight columns, not sixteen, so the wide loop may not
 * run at all. Both forms are pure functions of their inputs, so the overlap
 * recomputes identical values. */
void y264_hpel_hrow_avx2(int32_t *srow, const pixel *row, int x0, int x1)
{
    int x = x0;
    for (; x + 16 <= x1; x += 16) {
        const uint8_t *p = row + x - 2;
        __m256i t = y256_tap6_16(y256_ld16w(p + 0), y256_ld16w(p + 1),
                                 y256_ld16w(p + 2), y256_ld16w(p + 3),
                                 y256_ld16w(p + 4), y256_ld16w(p + 5));
        y256_st256(srow + x,
                   _mm256_cvtepi16_epi32(_mm256_castsi256_si128(t)));
        y256_st256(srow + x + 8,
                   _mm256_cvtepi16_epi32(_mm256_extracti128_si256(t, 1)));
    }
    for (; x < x1; x += 8) {
        if (x > x1 - 8) x = x1 - 8;
        y264_hpel_hrow8(srow, row, x);
    }
}

void y264_hpel_outrow_avx2(pixel *Hr, pixel *Vr, pixel *Cr,
                           const int32_t *s0, const int32_t *s1, const int32_t *s2,
                           const int32_t *s3, const int32_t *s4, const int32_t *s5,
                           const pixel *r0, const pixel *r1, const pixel *r2,
                           const pixel *r3, const pixel *r4, const pixel *r5,
                           int x0, int x1)
{
    __m256i k16 = _mm256_set1_epi32(16), k512 = _mm256_set1_epi32(512);
    int x = x0;
    for (; x + 16 <= x1; x += 16) {
        /* H: the middle intermediate rescaled */
        __m256i ha = _mm256_srai_epi32(_mm256_add_epi32(y256_ld256(s2 + x), k16), 5);
        __m256i hb = _mm256_srai_epi32(_mm256_add_epi32(y256_ld256(s2 + x + 8), k16), 5);
        _mm_storeu_si128((__m128i *)(void *)(Hr + x),
                         _mm_packus_epi16(y256_pack32(ha), y256_pack32(hb)));

        /* C: the vertical tap over the six intermediate rows */
        __m256i ca = y256_tap6_32(y256_ld256(s0 + x), y256_ld256(s1 + x),
                                  y256_ld256(s2 + x), y256_ld256(s3 + x),
                                  y256_ld256(s4 + x), y256_ld256(s5 + x));
        __m256i cb = y256_tap6_32(y256_ld256(s0 + x + 8), y256_ld256(s1 + x + 8),
                                  y256_ld256(s2 + x + 8), y256_ld256(s3 + x + 8),
                                  y256_ld256(s4 + x + 8), y256_ld256(s5 + x + 8));
        ca = _mm256_srai_epi32(_mm256_add_epi32(ca, k512), 10);
        cb = _mm256_srai_epi32(_mm256_add_epi32(cb, k512), 10);
        _mm_storeu_si128((__m128i *)(void *)(Cr + x),
                         _mm_packus_epi16(y256_pack32(ca), y256_pack32(cb)));

        /* V: the vertical tap over the six integer rows */
        __m256i v = y256_tap6_16(y256_ld16w(r0 + x), y256_ld16w(r1 + x),
                                 y256_ld16w(r2 + x), y256_ld16w(r3 + x),
                                 y256_ld16w(r4 + x), y256_ld16w(r5 + x));
        _mm_storeu_si128((__m128i *)(void *)(Vr + x), y256_clip5(v));
    }
    for (; x < x1; x += 8) {
        if (x > x1 - 8) x = x1 - 8;
        y264_hpel_outrow8(Hr, Vr, Cr, s0, s1, s2, s3, s4, s5,
                          r0, r1, r2, r3, r4, r5, x);
    }
}

#endif /* Y264_HAVE_AVX2 */
