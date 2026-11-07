/*
 * interp_neon10.c - aarch64 NEON motion compensation for the 10-bit library
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The 10-bit twins of part of src/dsp/interp_neon.c, under the 8-bit kernels'
 * names (src/common/symbols10.h suffixes them): the half-pel plane fetch,
 * the weighted average, chroma MC at widths 8 and 4, and the two row kernels
 * of the half-pel plane build. The six-tap luma kernels, the fused band
 * build and the fused bi-prediction forms stay C at 10 bits for now.
 *
 * A sample is a u16 lane. The six-tap filter of 10-bit samples reaches
 * 42*1023 = 42966, past int16, so the filters widen to int32 where the 8-bit
 * kernels keep int16.
 */
#if defined(__aarch64__)
#include "../interp.h"
#include "../arch.h"
#include <arm_neon.h>

#if Y264_BIT_DEPTH != 10
#error "interp_neon10.c is the 10-bit library's NEON tier"
#endif

static inline uint16x8_t clip_s32x2(int32x4_t lo, int32x4_t hi)
{
    return vminq_u16(vcombine_u16(vqmovun_s32(lo), vqmovun_s32(hi)),
                     vdupq_n_u16(PIXEL_MAX));
}

/* ---- half-pel plane fetch --------------------------------------------------*/

void y264_pred_copy_neon(pixel *dst, int dstride, const pixel *s, int sstride,
                         int w, int h)
{
    if (w == 16) {
        for (int y = 0; y < h; y++)
            vst1q_u16_x2(dst + y * dstride, vld1q_u16_x2(s + y * sstride));
    } else if (w == 8) {
        for (int y = 0; y < h; y++)
            vst1q_u16(dst + y * dstride, vld1q_u16(s + y * sstride));
    } else {
        for (int y = 0; y < h; y++)
            vst1_u16(dst + y * dstride, vld1_u16(s + y * sstride));
    }
}

void y264_pred_avg2_neon(pixel *dst, int dstride, const pixel *s1,
                         const pixel *s2, int sstride, int w, int h)
{
    if (w == 16) {
        for (int y = 0; y < h; y++) {
            uint16x8x2_t a = vld1q_u16_x2(s1 + y * sstride), b = vld1q_u16_x2(s2 + y * sstride);
            a.val[0] = vrhaddq_u16(a.val[0], b.val[0]);
            a.val[1] = vrhaddq_u16(a.val[1], b.val[1]);
            vst1q_u16_x2(dst + y * dstride, a);
        }
    } else if (w == 8) {
        for (int y = 0; y < h; y++)
            vst1q_u16(dst + y * dstride,
                      vrhaddq_u16(vld1q_u16(s1 + y * sstride), vld1q_u16(s2 + y * sstride)));
    } else {
        for (int y = 0; y < h; y++)
            vst1_u16(dst + y * dstride,
                     vrhadd_u16(vld1_u16(s1 + y * sstride), vld1_u16(s2 + y * sstride)));
    }
}

/* ---- weighted average ------------------------------------------------------
 *
 * Clip1((a*w0 + b*w1 + 32) >> 6). The flat (32, 32) pair is exactly the
 * rounding average. Otherwise the products are formed in int32 from int16
 * weights, which holds any weight the encoder passes; a weight outside int16
 * takes the scalar loop. */
static inline int wt_ok(int w0, int w1)
{
    return w0 >= -32768 && w0 <= 32767 && w1 >= -32768 && w1 <= 32767;
}

static inline uint16x8_t wt8(uint16x8_t a, uint16x8_t b, int16x4_t k0, int16x4_t k1)
{
    int16x8_t sa = vreinterpretq_s16_u16(a), sb = vreinterpretq_s16_u16(b);
    int32x4_t lo = vmlal_s16(vmull_s16(vget_low_s16(sa), k0), vget_low_s16(sb), k1);
    int32x4_t hi = vmlal_s16(vmull_s16(vget_high_s16(sa), k0), vget_high_s16(sb), k1);
    return clip_s32x2(vrshrq_n_s32(lo, 6), vrshrq_n_s32(hi, 6));
}

static inline pixel wt1(int a, int b, int w0, int w1)
{
    int v = (a * w0 + b * w1 + 32) >> 6;
    return (pixel)(v < 0 ? 0 : (v > PIXEL_MAX ? PIXEL_MAX : v));
}

static void wt_row(pixel *dst, const pixel *a, const pixel *b, int n, int w0, int w1)
{
    int i = 0;
    if (w0 == 32 && w1 == 32) {
        for (; i + 8 <= n; i += 8)
            vst1q_u16(dst + i, vrhaddq_u16(vld1q_u16(a + i), vld1q_u16(b + i)));
    } else if (wt_ok(w0, w1)) {
        const int16x4_t k0 = vdup_n_s16((int16_t)w0), k1 = vdup_n_s16((int16_t)w1);
        for (; i + 8 <= n; i += 8)
            vst1q_u16(dst + i, wt8(vld1q_u16(a + i), vld1q_u16(b + i), k0, k1));
    }
    for (; i < n; i++)
        dst[i] = wt1(a[i], b[i], w0, w1);
}

void y264_pixel_avg_wt_neon(pixel *dst, const pixel *a, const pixel *b, int n,
                            int w0, int w1)
{
    wt_row(dst, a, b, n, w0, w1);
}

void y264_pixel_avg_wt_2d_neon(pixel *dst, int ds, const pixel *a, int as,
                               const pixel *b, int bs, int w, int h, int w0, int w1)
{
    for (int y = 0; y < h; y++, dst += ds, a += as, b += bs)
        wt_row(dst, a, b, w, w0, w1);
}

/* ---- chroma ----------------------------------------------------------------
 *
 * The eighth-pel bilinear in u16 lanes. The weights sum to 64, so the sum is
 * at most 64 * 1023 = 65472 and the rounding shift adds 32 without wrapping:
 * the modular u16 arithmetic of the C reference's in-bounds path, exactly. */
static inline uint16x8_t mcc8(const pixel *r0, const pixel *r1, uint16_t cA,
                              uint16_t cB, uint16_t cC, uint16_t cD)
{
    uint16x8_t v = vmulq_n_u16(vld1q_u16(r0), cA);
    v = vmlaq_n_u16(v, vld1q_u16(r0 + 1), cB);
    v = vmlaq_n_u16(v, vld1q_u16(r1), cC);
    v = vmlaq_n_u16(v, vld1q_u16(r1 + 1), cD);
    return vrshrq_n_u16(v, 6);
}

void y264_mc_chroma_neon_w8h(pixel *dst, int dstride, const pixel *ref,
                             int rstride, int ix, int iy, int fx, int fy, int h)
{
    const uint16_t cA = (uint16_t)((8 - fx) * (8 - fy)), cB = (uint16_t)(fx * (8 - fy));
    const uint16_t cC = (uint16_t)((8 - fx) * fy),       cD = (uint16_t)(fx * fy);
    const pixel *r = ref + iy * rstride + ix;
    for (int y = 0; y < h; y++, dst += dstride, r += rstride)
        vst1q_u16(dst, mcc8(r, r + rstride, cA, cB, cC, cD));
}

void y264_mc_chroma_neon8(pixel *dst, int dstride, const pixel *ref,
                          int rstride, int ix, int iy, int fx, int fy)
{
    y264_mc_chroma_neon_w8h(dst, dstride, ref, rstride, ix, iy, fx, fy, 8);
}

/* Width 4: two output rows per vector, lanes 0-3 row y and 4-7 row y + 1, so
 * each pass does a full register of work. Every load is four samples, the
 * block's own window. */
void y264_mc_chroma_neon_w4h(pixel *dst, int dstride, const pixel *ref,
                             int rstride, int ix, int iy, int fx, int fy, int h)
{
    const uint16_t cA = (uint16_t)((8 - fx) * (8 - fy)), cB = (uint16_t)(fx * (8 - fy));
    const uint16_t cC = (uint16_t)((8 - fx) * fy),       cD = (uint16_t)(fx * fy);
    for (int y = 0; y < h; y += 2) {
        const pixel *r0 = ref + (iy + y) * rstride + ix;
        const pixel *r1 = r0 + rstride, *r2 = r1 + rstride;
        uint16x8_t v = vmulq_n_u16(vcombine_u16(vld1_u16(r0), vld1_u16(r1)), cA);
        v = vmlaq_n_u16(v, vcombine_u16(vld1_u16(r0 + 1), vld1_u16(r1 + 1)), cB);
        v = vmlaq_n_u16(v, vcombine_u16(vld1_u16(r1), vld1_u16(r2)), cC);
        v = vmlaq_n_u16(v, vcombine_u16(vld1_u16(r1 + 1), vld1_u16(r2 + 1)), cD);
        v = vrshrq_n_u16(v, 6);
        vst1_u16(dst + y * dstride, vget_low_u16(v));
        vst1_u16(dst + (y + 1) * dstride, vget_high_u16(v));
    }
}

/* ---- the half-pel plane build's row kernels ---------------------------------
 *
 * tap6 = a - 5b + 20c + 20d - 5e + f in int32. Both kernels take eight
 * columns a step and end on the span by stepping back over columns already
 * written, so they need a span of at least eight. */
static inline int32x4_t tap6_s32(int32x4_t a, int32x4_t b, int32x4_t c,
                                 int32x4_t d, int32x4_t e, int32x4_t f)
{
    int32x4_t t = vaddq_s32(a, f);
    t = vmlsq_n_s32(t, vaddq_s32(b, e), 5);
    return vmlaq_n_s32(t, vaddq_s32(c, d), 20);
}

/* the same filter of six u16 vectors, its two int32 halves */
static inline void tap6_u16(uint16x8_t a, uint16x8_t b, uint16x8_t c, uint16x8_t d,
                            uint16x8_t e, uint16x8_t f, int32x4_t *lo, int32x4_t *hi)
{
    int32x4_t af = vreinterpretq_s32_u32(vaddl_u16(vget_low_u16(a), vget_low_u16(f)));
    int32x4_t be = vreinterpretq_s32_u32(vaddl_u16(vget_low_u16(b), vget_low_u16(e)));
    int32x4_t cd = vreinterpretq_s32_u32(vaddl_u16(vget_low_u16(c), vget_low_u16(d)));
    *lo = vmlaq_n_s32(vmlsq_n_s32(af, be, 5), cd, 20);
    af = vreinterpretq_s32_u32(vaddl_high_u16(a, f));
    be = vreinterpretq_s32_u32(vaddl_high_u16(b, e));
    cd = vreinterpretq_s32_u32(vaddl_high_u16(c, d));
    *hi = vmlaq_n_s32(vmlsq_n_s32(af, be, 5), cd, 20);
}

/* srow[x] = tap6(row[x-2 .. x+3]) for x in [x0, x1). The thirteen samples a
 * step reads are two eight-sample loads at -2 and +3, so nothing past
 * row[x1 + 2] is touched. */
void y264_hpel_hrow_neon(int32_t *srow, const pixel *row, int x0, int x1)
{
    for (int x = x0; x < x1; x += 8) {
        if (x > x1 - 8) x = x1 - 8;
        const pixel *p = row + x - 2;
        uint16x8_t lo = vld1q_u16(p), hi = vld1q_u16(p + 5);
        uint16x8_t xh = vextq_u16(hi, hi, 3);       /* lanes 0..4 = p[8..12] */
        int32x4_t t0, t1;
        tap6_u16(lo, vextq_u16(lo, xh, 1), vextq_u16(lo, xh, 2), vextq_u16(lo, xh, 3),
                 vextq_u16(lo, xh, 4), vextq_u16(lo, xh, 5), &t0, &t1);
        vst1q_s32(srow + x, t0);
        vst1q_s32(srow + x + 4, t1);
    }
}

/* For x in [x0, x1): Hr = Clip1((s2 + 16) >> 5), Cr = Clip1((tap6 of the six
 * int32 rows + 512) >> 10) and Vr = Clip1((tap6 of the six sample rows + 16)
 * >> 5). The caller guarantees 0 <= x0, x1 <= pw and x1 - x0 >= 8. */
void y264_hpel_outrow_neon(pixel *Hr, pixel *Vr, pixel *Cr,
                           const int32_t *s0, const int32_t *s1, const int32_t *s2,
                           const int32_t *s3, const int32_t *s4, const int32_t *s5,
                           const pixel *r0, const pixel *r1, const pixel *r2,
                           const pixel *r3, const pixel *r4, const pixel *r5,
                           int x0, int x1)
{
    for (int x = x0; x < x1; x += 8) {
        if (x > x1 - 8) x = x1 - 8;
        int32x4_t a = vld1q_s32(s2 + x), b = vld1q_s32(s2 + x + 4);
        vst1q_u16(Hr + x, clip_s32x2(vrshrq_n_s32(a, 5), vrshrq_n_s32(b, 5)));

        int32x4_t c0 = tap6_s32(vld1q_s32(s0 + x), vld1q_s32(s1 + x), a,
                                vld1q_s32(s3 + x), vld1q_s32(s4 + x), vld1q_s32(s5 + x));
        int32x4_t c1 = tap6_s32(vld1q_s32(s0 + x + 4), vld1q_s32(s1 + x + 4), b,
                                vld1q_s32(s3 + x + 4), vld1q_s32(s4 + x + 4),
                                vld1q_s32(s5 + x + 4));
        vst1q_u16(Cr + x, clip_s32x2(vrshrq_n_s32(c0, 10), vrshrq_n_s32(c1, 10)));

        int32x4_t v0, v1;
        tap6_u16(vld1q_u16(r0 + x), vld1q_u16(r1 + x), vld1q_u16(r2 + x),
                 vld1q_u16(r3 + x), vld1q_u16(r4 + x), vld1q_u16(r5 + x), &v0, &v1);
        vst1q_u16(Vr + x, clip_s32x2(vrshrq_n_s32(v0, 5), vrshrq_n_s32(v1, 5)));
    }
}

#endif /* __aarch64__ */
