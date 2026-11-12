/*
 * interp_neon10.c - aarch64 NEON motion compensation for the 10-bit library
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The 10-bit twins of part of src/dsp/interp_neon.c, under the 8-bit kernels'
 * names (src/common/symbols10.h suffixes them): the half-pel plane fetch,
 * the weighted average, chroma MC at widths 8 and 4, and the two row kernels
 * of the half-pel plane build. It also holds the row of the lowres half-pel
 * planes on the 8-bit scale, which the 8-bit library leaves to the compiler.
 * The six-tap luma kernels, the fused band build and the two fused
 * bi-prediction forms are here too.
 *
 * A sample is a u16 lane. The six-tap filter of 10-bit samples reaches
 * 42*1023 = 42966, past int16, so the filters widen to int32 where the 8-bit
 * kernels keep int16.
 */
#if defined(__aarch64__)
#include "../interp.h"
#include "../arch.h"
#include <arm_neon.h>
#include <string.h>

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

/* ---- the fused bi-prediction forms -----------------------------------------
 *
 * A 4:2:0 macroblock's flat bi-average from whole-sample positions in one
 * call: luma 16x16 into pred (stride 16), each chroma 8x8 into its plane
 * (stride 8), every sample (a + b + 1) >> 1. */
void y264_avg_mb420_flat_neon(pixel *pred, pixel *cu, pixel *cv,
                              const pixel *l0, int s0, const pixel *l1, int s1,
                              const pixel *u0, const pixel *v0, int t0,
                              const pixel *u1, const pixel *v1, int t1)
{
    for (int y = 0; y < 16; y++, l0 += s0, l1 += s1) {
        uint16x8x2_t a = vld1q_u16_x2(l0), b = vld1q_u16_x2(l1);
        a.val[0] = vrhaddq_u16(a.val[0], b.val[0]);
        a.val[1] = vrhaddq_u16(a.val[1], b.val[1]);
        vst1q_u16_x2(pred + 16 * y, a);
    }
    for (int y = 0; y < 8; y++, u0 += t0, v0 += t0, u1 += t1, v1 += t1) {
        vst1q_u16(cu + 8 * y, vrhaddq_u16(vld1q_u16(u0), vld1q_u16(u1)));
        vst1q_u16(cv + 8 * y, vrhaddq_u16(vld1q_u16(v0), vld1q_u16(v1)));
    }
}

/* Two in-bounds 8x8 chroma predictions, one per list, averaged into dst in
 * the same pass: each list's rows are the width-8 bilinear above, with each
 * source row loaded once for two output rows, then wt_row's average. Each
 * list reads nine columns of nine rows. A weight outside int16 averages
 * through the scalar form, as wt_row does. */
void y264_mc_chroma_bi8_neon(pixel *dst, int dstride,
                             const pixel *r0, int s0, int fx0, int fy0,
                             const pixel *r1, int s1, int fx1, int fy1,
                             int w0, int w1)
{
    const uint16_t a0 = (uint16_t)((8 - fx0) * (8 - fy0)), a1 = (uint16_t)(fx0 * (8 - fy0));
    const uint16_t a2 = (uint16_t)((8 - fx0) * fy0),       a3 = (uint16_t)(fx0 * fy0);
    const uint16_t b0 = (uint16_t)((8 - fx1) * (8 - fy1)), b1 = (uint16_t)(fx1 * (8 - fy1));
    const uint16_t b2 = (uint16_t)((8 - fx1) * fy1),       b3 = (uint16_t)(fx1 * fy1);
    const int flat = w0 == 32 && w1 == 32, vec = wt_ok(w0, w1);
    const int16x4_t k0 = vdup_n_s16((int16_t)(vec ? w0 : 0));
    const int16x4_t k1 = vdup_n_s16((int16_t)(vec ? w1 : 0));
    uint16x8_t t0 = vld1q_u16(r0), t1 = vld1q_u16(r0 + 1);
    uint16x8_t u0 = vld1q_u16(r1), u1 = vld1q_u16(r1 + 1);
    for (int y = 0; y < 8; y++, dst += dstride) {
        r0 += s0; r1 += s1;
        uint16x8_t n0 = vld1q_u16(r0), n1 = vld1q_u16(r0 + 1);
        uint16x8_t m0 = vld1q_u16(r1), m1 = vld1q_u16(r1 + 1);
        /* the weights sum to 64, so the sum stays within u16 as in mcc8 */
        uint16x8_t p = vmulq_n_u16(t0, a0);
        p = vmlaq_n_u16(p, t1, a1); p = vmlaq_n_u16(p, n0, a2); p = vmlaq_n_u16(p, n1, a3);
        uint16x8_t q = vmulq_n_u16(u0, b0);
        q = vmlaq_n_u16(q, u1, b1); q = vmlaq_n_u16(q, m0, b2); q = vmlaq_n_u16(q, m1, b3);
        p = vrshrq_n_u16(p, 6);
        q = vrshrq_n_u16(q, 6);
        if (flat) {
            vst1q_u16(dst, vrhaddq_u16(p, q));
        } else if (vec) {
            vst1q_u16(dst, wt8(p, q, k0, k1));
        } else {
            pixel pa[8], pb[8];
            vst1q_u16(pa, p);
            vst1q_u16(pb, q);
            for (int i = 0; i < 8; i++) dst[i] = wt1(pa[i], pb[i], w0, w1);
        }
        t0 = n0; t1 = n1; u0 = m0; u1 = m1;
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

/* ---- six-tap luma ----------------------------------------------------------
 *
 * The 8-bit kernels' plane scheme: the half-pel planes G (whole), H, V and J
 * (centre) over the block plus one column and row, then each quarter-pel
 * phase as one plane or the rounding average of two. A six-tap of 10-bit
 * samples spans [-10230, 42966], 53196 values, so it is formed in modular
 * u16 arithmetic with 10240 = 320 * 32 added, which puts the exact sum in
 * [10, 53206]. A half-pel sample is then ((sum + 16) >> 5) - 320, saturated
 * at 0 and clipped at PIXEL_MAX. The centre keeps the offset horizontal
 * sums in u16 and runs its vertical six-tap on int32 lanes, where the
 * offset has grown to 32 * 10240 = 320 * 1024 and comes off after the
 * >> 10 the same way. */
#define MCL_ST 24   /* plane row stride */

static inline uint16x8_t tap6_off(uint16x8_t a, uint16x8_t b, uint16x8_t c,
                                  uint16x8_t d, uint16x8_t e, uint16x8_t f)
{
    uint16x8_t t = vaddq_u16(vaddq_u16(a, f), vdupq_n_u16(10240));
    t = vmlaq_n_u16(t, vaddq_u16(c, d), 20);
    return vmlsq_n_u16(t, vaddq_u16(b, e), 5);
}

static inline uint16x8_t half_clip(uint16x8_t sum)
{
    return vminq_u16(vqsubq_u16(vrshrq_n_u16(sum, 5), vdupq_n_u16(320)),
                     vdupq_n_u16(PIXEL_MAX));
}

static inline uint16x8_t centre_clip(int32x4_t lo, int32x4_t hi)
{
    const int32x4_t k = vdupq_n_s32(320);
    return clip_s32x2(vsubq_s32(vrshrq_n_s32(lo, 10), k),
                      vsubq_s32(vrshrq_n_s32(hi, 10), k));
}

/* the offset horizontal sums of the eight samples from p + 2 */
static inline uint16x8_t hsum8(const pixel *p)
{
    return tap6_off(vld1q_u16(p), vld1q_u16(p + 1), vld1q_u16(p + 2),
                    vld1q_u16(p + 3), vld1q_u16(p + 4), vld1q_u16(p + 5));
}

/* the same from one sixteen-sample window, for the 8-wide kernel */
static inline uint16x8_t hsum8_win(const pixel *p)
{
    uint16x8_t lo = vld1q_u16(p), hi = vld1q_u16(p + 8);
    return tap6_off(lo, vextq_u16(lo, hi, 1), vextq_u16(lo, hi, 2),
                    vextq_u16(lo, hi, 3), vextq_u16(lo, hi, 4), vextq_u16(lo, hi, 5));
}

static inline uint16x8_t vsum8(const pixel *p, int rs)
{
    return tap6_off(vld1q_u16(p - 2 * rs), vld1q_u16(p - rs), vld1q_u16(p),
                    vld1q_u16(p + rs), vld1q_u16(p + 2 * rs), vld1q_u16(p + 3 * rs));
}

static inline uint16x8_t jsum8(const uint16_t *c, int k)
{
    int32x4_t lo, hi;
    tap6_u16(vld1q_u16(c + k), vld1q_u16(c + MCL_ST + k), vld1q_u16(c + 2 * MCL_ST + k),
             vld1q_u16(c + 3 * MCL_ST + k), vld1q_u16(c + 4 * MCL_ST + k),
             vld1q_u16(c + 5 * MCL_ST + k), &lo, &hi);
    return centre_clip(lo, hi);
}

/* the vertical half-pel sample one column past the block, which the
 * shifted phases read */
static inline pixel vcol(const pixel *q, int rs)
{
    int s = q[-2 * rs] - 5 * q[-rs] + 20 * q[0] + 20 * q[rs] - 5 * q[2 * rs] + q[3 * rs];
    s = (s + 16) >> 5;
    return (pixel)(s < 0 ? 0 : s > PIXEL_MAX ? PIXEL_MAX : s);
}

/* Eight output samples of a phase, from the planes at the same column. */
static inline uint16x8_t mcl_out8(int fx, int fy, const pixel *G, const pixel *H,
                                  const pixel *V, const pixel *J)
{
    switch (fy * 4 + fx) {
    case 0*4+1: return vrhaddq_u16(vld1q_u16(G), vld1q_u16(H));
    case 0*4+2: return vld1q_u16(H);
    case 0*4+3: return vrhaddq_u16(vld1q_u16(G + 1), vld1q_u16(H));
    case 1*4+0: return vrhaddq_u16(vld1q_u16(G), vld1q_u16(V));
    case 2*4+0: return vld1q_u16(V);
    case 3*4+0: return vrhaddq_u16(vld1q_u16(G + MCL_ST), vld1q_u16(V));
    case 1*4+1: return vrhaddq_u16(vld1q_u16(H), vld1q_u16(V));
    case 1*4+2: return vrhaddq_u16(vld1q_u16(H), vld1q_u16(J));
    case 1*4+3: return vrhaddq_u16(vld1q_u16(H), vld1q_u16(V + 1));
    case 2*4+1: return vrhaddq_u16(vld1q_u16(V), vld1q_u16(J));
    case 2*4+2: return vld1q_u16(J);
    case 2*4+3: return vrhaddq_u16(vld1q_u16(J), vld1q_u16(V + 1));
    case 3*4+1: return vrhaddq_u16(vld1q_u16(V), vld1q_u16(H + MCL_ST));
    case 3*4+2: return vrhaddq_u16(vld1q_u16(J), vld1q_u16(H + MCL_ST));
    case 3*4+3: return vrhaddq_u16(vld1q_u16(V + 1), vld1q_u16(H + MCL_ST));
    default:    return vld1q_u16(G);
    }
}

/* The planes for a block of w = 8 * nh columns (nh = 1 or 2) and h rows.
 * The 16-wide form reads [ix-2, ix+18] with unaligned eight-sample loads; the
 * 8-wide form reads [ix-2, ix+13], one sixteen-sample window a row. Both read
 * rows [iy-2, iy+h+3]. */
static inline void mcl_luma(pixel *dst, int dstride, const pixel *ref, int rstride,
                            int ix, int iy, int fx, int fy, int h, int nh)
{
    const int w = 8 * nh;
    if (fx == 0 && fy == 0) {
        for (int y = 0; y < h; y++)
            for (int k = 0; k < w; k += 8)
                vst1q_u16(dst + y * dstride + k, vld1q_u16(ref + (iy + y) * rstride + ix + k));
        return;
    }

    pixel G[17 * MCL_ST], H[17 * MCL_ST], V[17 * MCL_ST], J[16 * MCL_ST];
    uint16_t cc[21 * MCL_ST];

    int needH = (fx != 0);
    int needV = (fy != 0);
    int needJ = (fx == 2 && fy != 0) || (fy == 2 && fx != 0);
    int needG = ((fx & 1) && fy == 0) || (fx == 0 && (fy & 1));

    if (needG)
        for (int y = 0; y <= h; y++) {
            const pixel *p = ref + (iy + y) * rstride + ix;
            for (int k = 0; k < w; k += 8) vst1q_u16(G + y * MCL_ST + k, vld1q_u16(p + k));
            G[y * MCL_ST + w] = p[w];
        }
    if (needH)
        for (int y = 0; y <= h; y++) {
            const pixel *p = ref + (iy + y) * rstride + ix - 2;
            if (nh == 2) {
                vst1q_u16(H + y * MCL_ST, half_clip(hsum8(p)));
                vst1q_u16(H + y * MCL_ST + 8, half_clip(hsum8(p + 8)));
            } else {
                vst1q_u16(H + y * MCL_ST, half_clip(hsum8_win(p)));
            }
        }
    if (needV)
        for (int y = 0; y <= h; y++) {
            const pixel *p = ref + (iy + y) * rstride + ix;
            for (int k = 0; k < w; k += 8)
                vst1q_u16(V + y * MCL_ST + k, half_clip(vsum8(p + k, rstride)));
            V[y * MCL_ST + w] = vcol(p + w, rstride);
        }
    if (needJ) {
        for (int y = -2; y <= h + 2; y++) {
            const pixel *p = ref + (iy + y) * rstride + ix - 2;
            uint16_t *c = cc + (y + 2) * MCL_ST;
            if (nh == 2) {
                vst1q_u16(c, hsum8(p));
                vst1q_u16(c + 8, hsum8(p + 8));
            } else {
                vst1q_u16(c, hsum8_win(p));
            }
        }
        for (int y = 0; y < h; y++)
            for (int k = 0; k < w; k += 8)
                vst1q_u16(J + y * MCL_ST + k, jsum8(cc + y * MCL_ST, k));
    }

    for (int y = 0; y < h; y++) {
        const int o = y * MCL_ST;
        for (int k = 0; k < w; k += 8)
            vst1q_u16(dst + y * dstride + k,
                      mcl_out8(fx, fy, G + o + k, H + o + k, V + o + k, J + o + k));
    }
}

void y264_mc_luma_neon16(pixel *dst, int dstride, const pixel *ref, int rstride,
                         int ix, int iy, int fx, int fy, int h)
{
    mcl_luma(dst, dstride, ref, rstride, ix, iy, fx, fy, h, 2);
}

void y264_mc_luma_neon8(pixel *dst, int dstride, const pixel *ref, int rstride,
                        int ix, int iy, int fx, int fy, int h)
{
    mcl_luma(dst, dstride, ref, rstride, ix, iy, fx, fy, h, 1);
}

/* The whole half-pel build for a band of output rows, borders included, in
 * one pass. Each source row is clamped into a padded copy once and its
 * horizontal six-tap kept in u16 with the six-tap section's 10240 offset; a
 * ring of six of each feeds the output rows:
 *   H = the offset tap's half-pel clip
 *   V = the same of the vertical tap over the padded rows
 *   C = the vertical tap over the offset horizontal taps on int32 lanes,
 *       less the offset after the >> 10
 * Bit-exact with y264_mc_build_hpel_rows. `mem` must hold
 * 12 * (W + 32) + 12 * W bytes, W = pw + 2 * border, and W >= 16. */
void y264_hpel_build_band_neon(pixel *Hp, pixel *Vp, pixel *Cp, int stride,
                               const pixel *ref, int rstride, int pw, int ph,
                               int border, uint8_t *mem, int y0, int y1)
{
    const int W = pw + 2 * border, PW = W + 32;
    pixel *P[6];
    uint16_t *T[6];
    for (int k = 0; k < 6; k++) {
        P[k] = (pixel *)(void *)mem + (size_t)k * PW;
        T[k] = (uint16_t *)(void *)(mem + (size_t)12 * PW) + (size_t)k * W;
    }
    const int sy0 = y0 - 2;
    for (int y = sy0; y < y1 + 3; y++) {
        int k = (y - sy0) % 6;
        int cy = y < 0 ? 0 : (y >= ph ? ph - 1 : y);
        const pixel *row = ref + (size_t)cy * rstride;
        pixel *pad = P[k];                   /* pad[j] = row[clamp(j - border - 2)] */
        for (int j = 0; j < border + 2; j++) pad[j] = row[0];
        memcpy(pad + border + 2, row, (size_t)pw * sizeof(pixel));
        for (int j = border + 2 + pw; j < W + 5; j++) pad[j] = row[pw - 1];
        uint16_t *t = T[k];
        for (int j = 0; j < W; j += 8) {
            if (j > W - 8) j = W - 8;        /* overlap tail */
            /* only pad[j .. j + 12] reach a live lane (<= W + 4 at the last
             * step); the load's last three samples lie in the pad's slack */
            vst1q_u16(t + j, hsum8_win(pad + j));
        }
        if (y < y0 + 3) continue;
        int oy = y - 3;                      /* rows oy-2 .. oy+3 are in the rings */
        int k0 = (oy - 2 - sy0) % 6;
        const pixel *p[6];
        const uint16_t *s[6];
        for (int i = 0; i < 6; i++) {
            p[i] = P[(k0 + i) % 6] + 2;      /* pad column j + 2 is output column j */
            s[i] = T[(k0 + i) % 6];
        }
        pixel *Hr = Hp + (ptrdiff_t)oy * stride - border;
        pixel *Vr = Vp + (ptrdiff_t)oy * stride - border;
        pixel *Cr = Cp + (ptrdiff_t)oy * stride - border;
        for (int j = 0; j < W; j += 8) {
            if (j > W - 8) j = W - 8;
            uint16x8_t s2 = vld1q_u16(s[2] + j);
            vst1q_u16(Hr + j, half_clip(s2));
            vst1q_u16(Vr + j, half_clip(tap6_off(vld1q_u16(p[0] + j), vld1q_u16(p[1] + j),
                                                 vld1q_u16(p[2] + j), vld1q_u16(p[3] + j),
                                                 vld1q_u16(p[4] + j), vld1q_u16(p[5] + j))));
            int32x4_t lo, hi;
            tap6_u16(vld1q_u16(s[0] + j), vld1q_u16(s[1] + j), s2,
                     vld1q_u16(s[3] + j), vld1q_u16(s[4] + j), vld1q_u16(s[5] + j),
                     &lo, &hi);
            vst1q_u16(Cr + j, centre_clip(lo, hi));
        }
    }
}

/* ---- the lowres half-pel planes ---------------------------------------------
 *
 * The lowres planes hold 8-bit samples at every depth. Each output is the
 * rounded mean of two vertical pair means, where a pair mean of 10-bit
 * samples rounds straight to the 8-bit scale, (a + b + 4) >> 3, and the
 * final mean saturates at 255:
 *
 *   oh[x] = min(avg(m(r0[2x+1], r1[2x+1]), m(r0[2x+2], r1[2x+2])), 255)
 *   ov[x] = min(avg(m(r1[2x],   r2[2x]),   m(r1[2x+1], r2[2x+1])), 255)
 *   oc[x] = min(avg(m(r1[2x+1], r2[2x+1]), m(r1[2x+2], r2[2x+2])), 255)
 *
 * A pair sum is at most 2046, so it stays in u16. Eight outputs a step read
 * a row deinterleaved from 2x (r1, r2) and from 2x + 1 (all three), which
 * is r0[1 .. 2n] and r1, r2[0 .. 2n] over the row; the last step steps back
 * to end on the span, so the caller hands it n >= 8. */
static inline uint16x8_t lr_m(uint16x8_t a, uint16x8_t b)
{
    return vrshrq_n_u16(vaddq_u16(a, b), 3);
}

void y264_lr_hp_row_neon(pixel *oh, pixel *ov, pixel *oc, const pixel *r0,
                         const pixel *r1, const pixel *r2, int n)
{
    const uint16x8_t cap = vdupq_n_u16(255);
    for (int x = 0; x < n; x += 8) {
        if (x > n - 8) x = n - 8;
        const int i = 2 * x;
        /* from 2x + 1: the odd samples and the even ones a column on */
        uint16x8x2_t a = vld2q_u16(r0 + i + 1);
        uint16x8x2_t b = vld2q_u16(r1 + i), c = vld2q_u16(r2 + i);
        uint16x8_t b2 = vld2q_u16(r1 + i + 1).val[1], c2 = vld2q_u16(r2 + i + 1).val[1];
        uint16x8_t m01o = lr_m(a.val[0], b.val[1]), m01e2 = lr_m(a.val[1], b2);
        uint16x8_t m12e = lr_m(b.val[0], c.val[0]), m12o = lr_m(b.val[1], c.val[1]);
        uint16x8_t m12e2 = lr_m(b2, c2);
        vst1q_u16(oh + x, vminq_u16(vrhaddq_u16(m01o, m01e2), cap));
        vst1q_u16(ov + x, vminq_u16(vrhaddq_u16(m12e, m12o), cap));
        vst1q_u16(oc + x, vminq_u16(vrhaddq_u16(m12o, m12e2), cap));
    }
}

#endif /* __aarch64__ */
