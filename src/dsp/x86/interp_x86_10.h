/*
 * interp_x86_10.h - the 10-bit mc and hpel kernels' shared 128-bit bodies
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The 16-bit-sample twins of interp_x86.h. Every body here is `static inline`
 * and 128-bit: interp_sse4_10.c wraps each one under its SSE4.2 name, and
 * interp_avx2_10.c wraps the narrow shapes the same way (VEX-encoded there)
 * and writes its own 256-bit forms where a 16-wide row is one register.
 *
 * A row of eight samples is one register. What moves from 8 bits are the
 * lane bounds, restated at each kernel; the three that shape the code:
 *
 *   - the six-tap of 10-bit samples spans [-10230, 42966], past both int16
 *     and u16. It is formed in modular 16-bit arithmetic with 10240 = 320 * 32
 *     added, which puts the exact sum in [10, 53206], inside u16. A half-pel
 *     sample is then ((sum + 16) >> 5) - 320, saturated at 0 and clipped at
 *     PIXEL_MAX; the offset is a multiple of 32, so the shift stays exact.
 *   - the centre plane filters those offset sums vertically in int32, where
 *     the offset has grown to 32 * 10240 = 320 * 1024 and comes off after
 *     the >> 10 the same way.
 *   - the chroma bilinear weights sum to 64, so a weighted sum is at most
 *     64 * 1023 = 65472 and the rounding add stays inside u16.
 *
 * No byte pack is a clip here: every output clips to [0, 1023] through
 * packus_epi32 and min_epu16, or subs_epu16 and min_epu16.
 *
 * Loads are sized to the block, never to the register: checkasm maps each
 * kernel's window against a PROT_NONE page at both ends.
 */
#ifndef YAH264_DSP_X86_INTERP_X86_10_H
#define YAH264_DSP_X86_INTERP_X86_10_H

#include <immintrin.h>
#include <stddef.h>
#include <string.h>

#include "dsp/arch.h"

#if Y264_BIT_DEPTH != 10
#error "interp_x86_10.h is the 10-bit library's x86 tier"
#endif

/* the stride of a luma kernel's own half-pel planes: room for the 17th
 * column the odd phases read */
#define I10_ST 24

/* ---- loads and stores ----------------------------------------------------- */

static inline __m128i i10_ld8(const pixel *p)
{
    return _mm_loadu_si128((const __m128i *)(const void *)p);
}

static inline __m128i i10_ld4(const pixel *p)
{
    return _mm_loadl_epi64((const __m128i *)(const void *)p);
}

static inline void i10_st8(pixel *p, __m128i v)
{
    _mm_storeu_si128((__m128i *)(void *)p, v);
}

static inline void i10_st4(pixel *p, __m128i v)
{
    _mm_storel_epi64((__m128i *)(void *)p, v);
}

static inline __m128i i10_ld32x4(const int32_t *p)
{
    return _mm_loadu_si128((const __m128i *)(const void *)p);
}

/* two int32 quads down to eight samples in [0, PIXEL_MAX] */
static inline __m128i i10_clip32(__m128i lo, __m128i hi)
{
    return _mm_min_epu16(_mm_packus_epi32(lo, hi), _mm_set1_epi16(PIXEL_MAX));
}

/* ---- the six-tap ----------------------------------------------------------- */

/* (a + f) + 10240 + 20(c + d) - 5(b + e) in modular 16-bit lanes; the exact
 * value lies in [10, 53206], so the lane read unsigned is the sum itself. */
static inline __m128i i10_tap6_off(__m128i a, __m128i b, __m128i c,
                                   __m128i d, __m128i e, __m128i f)
{
    __m128i t = _mm_add_epi16(_mm_add_epi16(a, f), _mm_set1_epi16(10240));
    t = _mm_add_epi16(t, _mm_mullo_epi16(_mm_add_epi16(c, d), _mm_set1_epi16(20)));
    return _mm_sub_epi16(t, _mm_mullo_epi16(_mm_add_epi16(b, e), _mm_set1_epi16(5)));
}

/* the half-pel sample of an offset sum: (sum + 16) <= 53222, no wrap */
static inline __m128i i10_half(__m128i sum)
{
    __m128i v = _mm_srli_epi16(_mm_add_epi16(sum, _mm_set1_epi16(16)), 5);
    return _mm_min_epu16(_mm_subs_epu16(v, _mm_set1_epi16(320)),
                         _mm_set1_epi16(PIXEL_MAX));
}

/* the same tap over int32 lanes, for values that no longer fit 16 bits */
static inline __m128i i10_tap6_32(__m128i a, __m128i b, __m128i c,
                                  __m128i d, __m128i e, __m128i f)
{
    __m128i s05 = _mm_add_epi32(a, f);
    __m128i s14 = _mm_add_epi32(b, e);
    __m128i s23 = _mm_add_epi32(c, d);
    __m128i m5  = _mm_add_epi32(_mm_slli_epi32(s14, 2), s14);
    __m128i m20 = _mm_add_epi32(_mm_slli_epi32(s23, 4), _mm_slli_epi32(s23, 2));
    return _mm_add_epi32(_mm_sub_epi32(s05, m5), m20);
}

/* the centre sample of eight columns from six rows of offset horizontal sums:
 * the vertical tap of u16 values <= 53206 reaches 42 * 53206, int32 */
static inline __m128i i10_centre(__m128i c0, __m128i c1, __m128i c2,
                                 __m128i c3, __m128i c4, __m128i c5)
{
    const __m128i z = _mm_setzero_si128();
    __m128i lo = i10_tap6_32(_mm_unpacklo_epi16(c0, z), _mm_unpacklo_epi16(c1, z),
                             _mm_unpacklo_epi16(c2, z), _mm_unpacklo_epi16(c3, z),
                             _mm_unpacklo_epi16(c4, z), _mm_unpacklo_epi16(c5, z));
    __m128i hi = i10_tap6_32(_mm_unpackhi_epi16(c0, z), _mm_unpackhi_epi16(c1, z),
                             _mm_unpackhi_epi16(c2, z), _mm_unpackhi_epi16(c3, z),
                             _mm_unpackhi_epi16(c4, z), _mm_unpackhi_epi16(c5, z));
    __m128i k = _mm_set1_epi32(512 - 320 * 1024);
    return i10_clip32(_mm_srai_epi32(_mm_add_epi32(lo, k), 10),
                      _mm_srai_epi32(_mm_add_epi32(hi, k), 10));
}

/* offset horizontal sums of eight columns; `p` addresses column x - 2, and
 * the six loads read exactly p[0 .. 12] */
static inline __m128i i10_hsum8(const pixel *p)
{
    return i10_tap6_off(i10_ld8(p), i10_ld8(p + 1), i10_ld8(p + 2),
                        i10_ld8(p + 3), i10_ld8(p + 4), i10_ld8(p + 5));
}

static inline __m128i i10_vsum8(const pixel *p, int rs)
{
    return i10_tap6_off(i10_ld8(p - 2 * rs), i10_ld8(p - rs), i10_ld8(p),
                        i10_ld8(p + rs), i10_ld8(p + 2 * rs), i10_ld8(p + 3 * rs));
}

/* the vertical half-pel sample one column past the block */
static inline pixel i10_vcol(const pixel *q, int rs)
{
    int s = q[-2 * rs] - 5 * q[-rs] + 20 * q[0] + 20 * q[rs] - 5 * q[2 * rs] + q[3 * rs];
    s = (s + 16) >> 5;
    return (pixel)(s < 0 ? 0 : s > PIXEL_MAX ? PIXEL_MAX : s);
}

/* ---- the luma interpolation -------------------------------------------------
 *
 * The 8-bit kernels' plane scheme: build whichever of G (integer), H, V and J
 * (centre) the phase reads, over the block plus one column and row, then emit
 * each row as one plane or the rounding average of two (avg_epu16 is
 * (a + b + 1) >> 1). The tiers differ only in how they filter a sixteen-column
 * row, so the body is written once and takes the row steps as macro
 * parameters, each producing n8 groups of eight columns:
 *
 *   HCL(p, out)            horizontal half-pel samples from column ix - 2
 *   HSUM(p, out)           the offset horizontal sums, unclipped
 *   VCL(p, rs, out)        vertical half-pel samples
 *   JCL(c, out)            the centre from six rows of sums at stride I10_ST
 *
 * The window is columns [ix-2, ix+w+2] and rows [iy-2, iy+h+3].
 */
#define I10_LUMA_BODY(NAME, W, HCL, HSUM, VCL, JCL)                            \
static void NAME(pixel *dst, int dstride, const pixel *ref, int rstride,       \
                 int ix, int iy, int fx, int fy, int h)                        \
{                                                                              \
    const int ST = I10_ST;                                                     \
    if (fx == 0 && fy == 0) {                                                  \
        for (int y = 0; y < h; y++)                                            \
            for (int k = 0; k < (W); k += 8)                                   \
                i10_st8(dst + (size_t)y * dstride + k,                         \
                        i10_ld8(ref + (ptrdiff_t)(iy + y) * rstride + ix + k));\
        return;                                                                \
    }                                                                          \
    pixel G[17 * I10_ST], H[17 * I10_ST], V[17 * I10_ST], J[16 * I10_ST];      \
    uint16_t cc[21 * I10_ST];                                                  \
    int needH = (fx != 0);                                                     \
    int needV = (fy != 0);                                                     \
    int needJ = (fx == 2 && fy != 0) || (fy == 2 && fx != 0);                  \
    int needG = ((fx & 1) && fy == 0) || (fx == 0 && (fy & 1));                \
    if (needG)                                                                 \
        for (int y = 0; y <= h; y++) {                                         \
            const pixel *p = ref + (ptrdiff_t)(iy + y) * rstride + ix;         \
            for (int k = 0; k < (W); k += 8)                                   \
                i10_st8(G + y * ST + k, i10_ld8(p + k));                       \
            G[y * ST + (W)] = p[W];                                            \
        }                                                                      \
    if (needH)                                                                 \
        for (int y = 0; y <= h; y++)                                           \
            HCL(ref + (ptrdiff_t)(iy + y) * rstride + ix - 2, H + y * ST);     \
    if (needV)                                                                 \
        for (int y = 0; y <= h; y++) {                                         \
            const pixel *p = ref + (ptrdiff_t)(iy + y) * rstride + ix;         \
            VCL(p, rstride, V + y * ST);                                       \
            V[y * ST + (W)] = i10_vcol(p + (W), rstride);                      \
        }                                                                      \
    if (needJ) {                                                               \
        for (int y = -2; y <= h + 2; y++)                                      \
            HSUM(ref + (ptrdiff_t)(iy + y) * rstride + ix - 2,                 \
                 cc + (y + 2) * ST);                                           \
        for (int y = 0; y < h; y++)                                            \
            JCL(cc + y * ST, J + y * ST);                                      \
    }                                                                          \
    for (int y = 0; y < h; y++) {                                              \
        const pixel *Gy = G + y * ST, *Hy = H + y * ST;                        \
        const pixel *Vy = V + y * ST, *Jy = J + y * ST;                        \
        pixel *d = dst + (size_t)y * dstride;                                  \
        for (int k = 0; k < (W); k += 8) {                                     \
            __m128i out;                                                       \
            switch (fy * 4 + fx) {                                             \
            case 0*4+1: out = _mm_avg_epu16(i10_ld8(Gy+k), i10_ld8(Hy+k)); break;\
            case 0*4+2: out = i10_ld8(Hy + k); break;                          \
            case 0*4+3: out = _mm_avg_epu16(i10_ld8(Gy+k+1), i10_ld8(Hy+k)); break;\
            case 1*4+0: out = _mm_avg_epu16(i10_ld8(Gy+k), i10_ld8(Vy+k)); break;\
            case 2*4+0: out = i10_ld8(Vy + k); break;                          \
            case 3*4+0: out = _mm_avg_epu16(i10_ld8(Gy+k+ST), i10_ld8(Vy+k)); break;\
            case 1*4+1: out = _mm_avg_epu16(i10_ld8(Hy+k), i10_ld8(Vy+k)); break;\
            case 1*4+2: out = _mm_avg_epu16(i10_ld8(Hy+k), i10_ld8(Jy+k)); break;\
            case 1*4+3: out = _mm_avg_epu16(i10_ld8(Hy+k), i10_ld8(Vy+k+1)); break;\
            case 2*4+1: out = _mm_avg_epu16(i10_ld8(Vy+k), i10_ld8(Jy+k)); break;\
            case 2*4+2: out = i10_ld8(Jy + k); break;                          \
            case 2*4+3: out = _mm_avg_epu16(i10_ld8(Jy+k), i10_ld8(Vy+k+1)); break;\
            case 3*4+1: out = _mm_avg_epu16(i10_ld8(Vy+k), i10_ld8(Hy+k+ST)); break;\
            case 3*4+2: out = _mm_avg_epu16(i10_ld8(Jy+k), i10_ld8(Hy+k+ST)); break;\
            case 3*4+3: out = _mm_avg_epu16(i10_ld8(Vy+k+1), i10_ld8(Hy+k+ST)); break;\
            default:    out = i10_ld8(Gy + k); break;                          \
            }                                                                  \
            i10_st8(d + k, out);                                               \
        }                                                                      \
    }                                                                          \
}

/* the 128-bit row steps, eight columns a group */
static inline void i10_hcl_x(const pixel *p, pixel *out, int n8)
{
    for (int k = 0; k < 8 * n8; k += 8)
        i10_st8(out + k, i10_half(i10_hsum8(p + k)));
}

static inline void i10_hsum_x(const pixel *p, uint16_t *out, int n8)
{
    for (int k = 0; k < 8 * n8; k += 8)
        i10_st8(out + k, i10_hsum8(p + k));
}

static inline void i10_vcl_x(const pixel *p, int rs, pixel *out, int n8)
{
    for (int k = 0; k < 8 * n8; k += 8)
        i10_st8(out + k, i10_half(i10_vsum8(p + k, rs)));
}

static inline void i10_jcl_x(const uint16_t *c, pixel *out, int n8)
{
    for (int k = 0; k < 8 * n8; k += 8)
        i10_st8(out + k, i10_centre(i10_ld8(c + k), i10_ld8(c + I10_ST + k),
                                    i10_ld8(c + 2 * I10_ST + k),
                                    i10_ld8(c + 3 * I10_ST + k),
                                    i10_ld8(c + 4 * I10_ST + k),
                                    i10_ld8(c + 5 * I10_ST + k)));
}

#define I10_HCL8(p, o)      i10_hcl_x(p, o, 1)
#define I10_HSUM8(p, o)     i10_hsum_x(p, o, 1)
#define I10_VCL8(p, rs, o)  i10_vcl_x(p, rs, o, 1)
#define I10_JCL8(c, o)      i10_jcl_x(c, o, 1)
#define I10_HCL16(p, o)     i10_hcl_x(p, o, 2)
#define I10_HSUM16(p, o)    i10_hsum_x(p, o, 2)
#define I10_VCL16(p, rs, o) i10_vcl_x(p, rs, o, 2)
#define I10_JCL16(c, o)     i10_jcl_x(c, o, 2)

I10_LUMA_BODY(i10_mc_luma8, 8, I10_HCL8, I10_HSUM8, I10_VCL8, I10_JCL8)

/* ---- chroma bilinear ---------------------------------------------------------
 *
 * (cA*A + cB*B + cC*C + cD*D + 32) >> 6 in u16 lanes: the weights sum to 64,
 * so the sum is at most 65472 and the rounding add at most 65504, which is
 * the modular arithmetic read unsigned, exactly. */
struct i10_cw { __m128i a, b, c, d; };

static inline struct i10_cw i10_cweights(int fx, int fy)
{
    struct i10_cw w = {
        _mm_set1_epi16((short)((8 - fx) * (8 - fy))), _mm_set1_epi16((short)(fx * (8 - fy))),
        _mm_set1_epi16((short)((8 - fx) * fy)),       _mm_set1_epi16((short)(fx * fy)),
    };
    return w;
}

static inline __m128i i10_bilin(__m128i A, __m128i B, __m128i C, __m128i D,
                                const struct i10_cw *w)
{
    __m128i v = _mm_add_epi16(_mm_mullo_epi16(A, w->a), _mm_mullo_epi16(B, w->b));
    v = _mm_add_epi16(v, _mm_mullo_epi16(C, w->c));
    v = _mm_add_epi16(v, _mm_mullo_epi16(D, w->d));
    return _mm_srli_epi16(_mm_add_epi16(v, _mm_set1_epi16(32)), 6);
}

/* eight columns of one output row; reads r and r + rs over nine columns */
static inline __m128i i10_bilin8(const pixel *r, int rs, const struct i10_cw *w)
{
    return i10_bilin(i10_ld8(r), i10_ld8(r + 1), i10_ld8(r + rs), i10_ld8(r + rs + 1), w);
}

static inline void i10_mc_chroma_w8h(pixel *dst, int dstride, const pixel *ref,
                                     int rstride, int ix, int iy, int fx, int fy, int h)
{
    const pixel *r = ref + (ptrdiff_t)iy * rstride + ix;
    if (!(fx | fy)) {              /* the zero phase is a copy */
        for (int y = 0; y < h; y++, dst += dstride, r += rstride)
            i10_st8(dst, i10_ld8(r));
        return;
    }
    struct i10_cw w = i10_cweights(fx, fy);
    for (int y = 0; y < h; y++, dst += dstride, r += rstride)
        i10_st8(dst, i10_bilin8(r, rstride, &w));
}

/* rows p and q of four samples in one register */
static inline __m128i i10_rows4(const pixel *p, const pixel *q)
{
    return _mm_unpacklo_epi64(i10_ld4(p), i10_ld4(q));
}

/* Width 4 packs two output rows per register, lanes 0-3 row y and 4-7 row
 * y + 1; the dispatcher routes even heights only. */
static inline void i10_mc_chroma_w4h(pixel *dst, int dstride, const pixel *ref,
                                     int rstride, int ix, int iy, int fx, int fy, int h)
{
    const pixel *r = ref + (ptrdiff_t)iy * rstride + ix;
    if (!(fx | fy)) {
        for (int y = 0; y < h; y++, dst += dstride, r += rstride)
            i10_st4(dst, i10_ld4(r));
        return;
    }
    struct i10_cw w = i10_cweights(fx, fy);
    for (int y = 0; y < h; y += 2, dst += 2 * dstride, r += 2 * rstride) {
        const pixel *r1 = r + rstride, *r2 = r1 + rstride;
        __m128i v = i10_bilin(i10_rows4(r, r1), i10_rows4(r + 1, r1 + 1),
                              i10_rows4(r1, r2), i10_rows4(r1 + 1, r2 + 1), &w);
        i10_st4(dst, v);
        i10_st4(dst + dstride, _mm_unpackhi_epi64(v, v));
    }
}

/* ---- the half-pel plane fetches ----------------------------------------------- */

static inline void i10_pred_copy(pixel *dst, int dstride, const pixel *s,
                                 int sstride, int w, int h)
{
    for (int y = 0; y < h; y++, dst += dstride, s += sstride) {
        if (w == 4) {
            i10_st4(dst, i10_ld4(s));
            continue;
        }
        for (int k = 0; k < w; k += 8)
            i10_st8(dst + k, i10_ld8(s + k));
    }
}

static inline void i10_pred_avg2(pixel *dst, int dstride, const pixel *s1,
                                 const pixel *s2, int sstride, int w, int h)
{
    for (int y = 0; y < h; y++, dst += dstride, s1 += sstride, s2 += sstride) {
        if (w == 4) {
            i10_st4(dst, _mm_avg_epu16(i10_ld4(s1), i10_ld4(s2)));
            continue;
        }
        for (int k = 0; k < w; k += 8)
            i10_st8(dst + k, _mm_avg_epu16(i10_ld8(s1 + k), i10_ld8(s2 + k)));
    }
}

/* ---- the weighted average --------------------------------------------------------
 *
 * Clip1((a*w0 + b*w1 + 32) >> 6). The flat (32, 32) pair is the rounding
 * average. Otherwise the two samples are interleaved against the weight pair
 * and one multiply-add forms the sum in int32: a sample is <= 1023, so any
 * int16 weight pair gives at most 2 * 1023 * 32768 in magnitude. A weight
 * outside int16 takes the scalar form, as the C does it. */
static inline int i10_wt_ok(int w0, int w1)
{
    return w0 >= -32768 && w0 <= 32767 && w1 >= -32768 && w1 <= 32767;
}

static inline __m128i i10_wpair(int w0, int w1)
{
    return _mm_set1_epi32((int)(((uint32_t)w0 & 0xffffu) | ((uint32_t)w1 << 16)));
}

static inline __m128i i10_wt8(__m128i a, __m128i b, __m128i k)
{
    __m128i r = _mm_set1_epi32(32);
    __m128i lo = _mm_add_epi32(_mm_madd_epi16(_mm_unpacklo_epi16(a, b), k), r);
    __m128i hi = _mm_add_epi32(_mm_madd_epi16(_mm_unpackhi_epi16(a, b), k), r);
    return i10_clip32(_mm_srai_epi32(lo, 6), _mm_srai_epi32(hi, 6));
}

static inline pixel i10_wt1(int a, int b, int w0, int w1)
{
    int v = (a * w0 + b * w1 + 32) >> 6;
    return (pixel)(v < 0 ? 0 : (v > PIXEL_MAX ? PIXEL_MAX : v));
}

/* one row of n samples, steps of eight and four, then scalar */
static inline void i10_wt_row(pixel *dst, const pixel *a, const pixel *b, int n,
                              int w0, int w1)
{
    int i = 0;
    if (w0 == 32 && w1 == 32) {
        for (; i + 8 <= n; i += 8)
            i10_st8(dst + i, _mm_avg_epu16(i10_ld8(a + i), i10_ld8(b + i)));
        if (i + 4 <= n) {
            i10_st4(dst + i, _mm_avg_epu16(i10_ld4(a + i), i10_ld4(b + i)));
            i += 4;
        }
    } else if (i10_wt_ok(w0, w1)) {
        __m128i k = i10_wpair(w0, w1);
        for (; i + 8 <= n; i += 8)
            i10_st8(dst + i, i10_wt8(i10_ld8(a + i), i10_ld8(b + i), k));
        if (i + 4 <= n) {
            i10_st4(dst + i, i10_wt8(i10_ld4(a + i), i10_ld4(b + i), k));
            i += 4;
        }
    }
    for (; i < n; i++)
        dst[i] = i10_wt1(a[i], b[i], w0, w1);
}

static inline void i10_avg_wt_2d(pixel *dst, int ds, const pixel *a, int as,
                                 const pixel *b, int bs, int w, int h, int w0, int w1)
{
    for (int y = 0; y < h; y++, dst += ds, a += as, b += bs)
        i10_wt_row(dst, a, b, w, w0, w1);
}

/* A 4:2:0 macroblock's flat bi-average: luma 16x16 into pred (stride 16),
 * each chroma 8x8 into its plane (stride 8). */
static inline void i10_avg_mb420_flat(pixel *pred, pixel *cu, pixel *cv,
                                      const pixel *l0, int s0, const pixel *l1, int s1,
                                      const pixel *u0, const pixel *v0, int t0,
                                      const pixel *u1, const pixel *v1, int t1)
{
    for (int y = 0; y < 16; y++, l0 += s0, l1 += s1) {
        i10_st8(pred + 16 * y, _mm_avg_epu16(i10_ld8(l0), i10_ld8(l1)));
        i10_st8(pred + 16 * y + 8, _mm_avg_epu16(i10_ld8(l0 + 8), i10_ld8(l1 + 8)));
    }
    for (int y = 0; y < 8; y++, u0 += t0, v0 += t0, u1 += t1, v1 += t1) {
        i10_st8(cu + 8 * y, _mm_avg_epu16(i10_ld8(u0), i10_ld8(u1)));
        i10_st8(cv + 8 * y, _mm_avg_epu16(i10_ld8(v0), i10_ld8(v1)));
    }
}

/* Two in-window 8x8 chroma bilinears, one per list, averaged in the same pass;
 * each list reads nine columns of nine rows. */
static inline void i10_mc_chroma_bi8(pixel *dst, int dstride,
                                     const pixel *r0, int s0, int fx0, int fy0,
                                     const pixel *r1, int s1, int fx1, int fy1,
                                     int w0, int w1)
{
    struct i10_cw a = i10_cweights(fx0, fy0), b = i10_cweights(fx1, fy1);
    int flat = w0 == 32 && w1 == 32, vec = i10_wt_ok(w0, w1);
    __m128i k = i10_wpair(vec ? w0 : 0, vec ? w1 : 0);
    for (int y = 0; y < 8; y++, r0 += s0, r1 += s1, dst += dstride) {
        __m128i p = i10_bilin8(r0, s0, &a), q = i10_bilin8(r1, s1, &b);
        if (flat) {
            i10_st8(dst, _mm_avg_epu16(p, q));
        } else if (vec) {
            i10_st8(dst, i10_wt8(p, q, k));
        } else {
            pixel pa[8], pb[8];
            i10_st8(pa, p);
            i10_st8(pb, q);
            for (int i = 0; i < 8; i++) dst[i] = i10_wt1(pa[i], pb[i], w0, w1);
        }
    }
}

/* ---- the half-pel plane build rows -------------------------------------------------
 *
 * srow[x] = the exact six-tap, int32; the offset comes off after widening.
 * Reads row[x - 2 .. x + 10]. */
static inline void i10_hrow8(int32_t *srow, const pixel *row, int x)
{
    __m128i t = i10_hsum8(row + x - 2), z = _mm_setzero_si128();
    __m128i k = _mm_set1_epi32(10240);
    _mm_storeu_si128((__m128i *)(void *)(srow + x),
                     _mm_sub_epi32(_mm_unpacklo_epi16(t, z), k));
    _mm_storeu_si128((__m128i *)(void *)(srow + x + 4),
                     _mm_sub_epi32(_mm_unpackhi_epi16(t, z), k));
}

/* H, V and C at eight columns from x: H rescales the middle int32 row (a
 * six-tap, <= 42966 in magnitude), C is the vertical tap over the six int32
 * rows (<= 42 * 42966, int32), V the vertical tap over the six sample rows. */
static inline void i10_outrow8(pixel *Hr, pixel *Vr, pixel *Cr,
                               const int32_t *s0, const int32_t *s1, const int32_t *s2,
                               const int32_t *s3, const int32_t *s4, const int32_t *s5,
                               const pixel *r0, const pixel *r1, const pixel *r2,
                               const pixel *r3, const pixel *r4, const pixel *r5, int x)
{
    __m128i k16 = _mm_set1_epi32(16), k512 = _mm_set1_epi32(512);
    __m128i a = i10_ld32x4(s2 + x), b = i10_ld32x4(s2 + x + 4);
    i10_st8(Hr + x, i10_clip32(_mm_srai_epi32(_mm_add_epi32(a, k16), 5),
                               _mm_srai_epi32(_mm_add_epi32(b, k16), 5)));
    __m128i c0 = i10_tap6_32(i10_ld32x4(s0 + x), i10_ld32x4(s1 + x), a,
                             i10_ld32x4(s3 + x), i10_ld32x4(s4 + x), i10_ld32x4(s5 + x));
    __m128i c1 = i10_tap6_32(i10_ld32x4(s0 + x + 4), i10_ld32x4(s1 + x + 4), b,
                             i10_ld32x4(s3 + x + 4), i10_ld32x4(s4 + x + 4),
                             i10_ld32x4(s5 + x + 4));
    i10_st8(Cr + x, i10_clip32(_mm_srai_epi32(_mm_add_epi32(c0, k512), 10),
                               _mm_srai_epi32(_mm_add_epi32(c1, k512), 10)));
    i10_st8(Vr + x, i10_half(i10_tap6_off(i10_ld8(r0 + x), i10_ld8(r1 + x),
                                          i10_ld8(r2 + x), i10_ld8(r3 + x),
                                          i10_ld8(r4 + x), i10_ld8(r5 + x))));
}

/* ---- the half-pel plane build, a whole band ------------------------------------------
 *
 * The 8-bit band's scheme on 16-bit samples: each source row clamped into a
 * padded copy once, its offset horizontal sums kept in u16, and a ring of six
 * of each feeding every output row:
 *
 *   H = the half-pel sample of the centre sum row
 *   V = the same of the vertical offset tap over the padded rows
 *   C = the centre over the six sum rows (i10_centre)
 *
 * `mem` holds 6 * (W + 32) samples and 6 * W sums, 12 * (W + 32) + 12 * W
 * bytes, W = pw + 2 * border >= 16. STEP is the tier's column step (8 or
 * 16), and the three macro parameters its steps:
 *
 *   HSUM(pad, t)            offset sums of pad[0 .. STEP + 4]
 *   HV(t2, p0 .. p5, H, V)  H and V of one step
 *   C(t0 .. t5, out)        the centre of one step
 *
 * The last pass of every row steps back to end on the span; each step is a
 * pure function of the padded rows, so the overlap recomputes the same values.
 */
#define I10_BAND_BODY(NAME, STEP, HSUM, HV, CEN)                               \
void NAME(pixel *Hp, pixel *Vp, pixel *Cp, int stride,                         \
          const pixel *ref, int rstride, int pw, int ph, int border,           \
          uint8_t *mem, int y0, int y1)                                        \
{                                                                              \
    const int W = pw + 2 * border, PW = W + 32;                                \
    pixel *P[6];                                                               \
    uint16_t *T[6];                                                            \
    for (int k = 0; k < 6; k++) {                                              \
        P[k] = (pixel *)(void *)mem + (size_t)k * PW;                          \
        T[k] = (uint16_t *)(void *)(mem + (size_t)12 * PW) + (size_t)k * W;    \
    }                                                                          \
    const int sy0 = y0 - 2;                                                    \
    for (int y = sy0; y < y1 + 3; y++) {                                       \
        int k = (y - sy0) % 6;                                                 \
        int cy = y < 0 ? 0 : (y >= ph ? ph - 1 : y);                           \
        const pixel *row = ref + (ptrdiff_t)cy * rstride;                      \
        pixel *pad = P[k];        /* pad[j] = row[clamp(j - border - 2)] */    \
        for (int j = 0; j < border + 2; j++) pad[j] = row[0];                  \
        memcpy(pad + border + 2, row, (size_t)pw * sizeof(pixel));             \
        for (int j = border + 2 + pw; j < W + 5; j++) pad[j] = row[pw - 1];    \
        uint16_t *t = T[k];                                                    \
        for (int j = 0; j < W; j += (STEP)) {                                  \
            if (j > W - (STEP)) j = W - (STEP);                                \
            HSUM(pad + j, t + j);                                              \
        }                                                                      \
        if (y < y0 + 3) continue;                                              \
        int oy = y - 3;           /* rows oy-2 .. oy+3 are in the rings */     \
        int k0 = (oy - 2 - sy0) % 6;                                           \
        const pixel *p[6];                                                     \
        const uint16_t *s[6];                                                  \
        for (int i = 0; i < 6; i++) {                                          \
            p[i] = P[(k0 + i) % 6] + 2;  /* pad column j + 2 is column j */    \
            s[i] = T[(k0 + i) % 6];                                            \
        }                                                                      \
        pixel *Hr = Hp + (ptrdiff_t)oy * stride - border;                      \
        pixel *Vr = Vp + (ptrdiff_t)oy * stride - border;                      \
        pixel *Cr = Cp + (ptrdiff_t)oy * stride - border;                      \
        for (int j = 0; j < W; j += (STEP)) {                                  \
            if (j > W - (STEP)) j = W - (STEP);                                \
            HV(s[2] + j, p[0] + j, p[1] + j, p[2] + j, p[3] + j, p[4] + j,     \
               p[5] + j, Hr + j, Vr + j);                                      \
            CEN(s[0] + j, s[1] + j, s[2] + j, s[3] + j, s[4] + j, s[5] + j,    \
                Cr + j);                                                       \
        }                                                                      \
    }                                                                          \
}

/* ---- the lowres half-pel planes, on the 8-bit scale ------------------------------------
 *
 * The lowres planes hold 8-bit samples at every depth: each pair mean of
 * 10-bit samples rounds straight to the 8-bit scale, m = (a + b + 4) >> 3
 * (<= 256), and the final mean of two adjacent ones saturates at 255, as the
 * encoder's own build does. A pair sum is at most 2046, u16.
 *
 * The horizontal mean pairs ADJACENT columns of the vertical means, so the
 * eight outputs from x are the pair sums of sixteen vertical means, which is
 * what hadd_epi16 forms in order. oh pairs columns (2x+1, 2x+2) of rows 0
 * and 1, ov (2x, 2x+1) of rows 1 and 2 and oc (2x+1, 2x+2) of rows 1 and 2,
 * so the three planes read the rows from 2x and 2x + 1 only: r0[2x+1 ..
 * 2x+16] and r1, r2[2x .. 2x+16]. */
static inline __m128i i10_lr_m(__m128i a, __m128i b)
{
    return _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(a, b), _mm_set1_epi16(4)), 3);
}

/* the rounded, capped means of the eight adjacent pairs of m0:m1 */
static inline __m128i i10_lr_pairs(__m128i m0, __m128i m1)
{
    __m128i s = _mm_add_epi16(_mm_hadd_epi16(m0, m1), _mm_set1_epi16(1));
    return _mm_min_epu16(_mm_srli_epi16(s, 1), _mm_set1_epi16(255));
}

static inline void i10_lr_hp_row8(pixel *oh, pixel *ov, pixel *oc, const pixel *r0,
                                  const pixel *r1, const pixel *r2, int x)
{
    const int i = 2 * x;
    __m128i a0 = i10_ld8(r0 + i + 1), a1 = i10_ld8(r0 + i + 9);
    __m128i b0 = i10_ld8(r1 + i), b1 = i10_ld8(r1 + i + 8);
    __m128i c0 = i10_ld8(r2 + i), c1 = i10_ld8(r2 + i + 8);
    __m128i d0 = i10_ld8(r1 + i + 1), d1 = i10_ld8(r1 + i + 9);
    __m128i e0 = i10_ld8(r2 + i + 1), e1 = i10_ld8(r2 + i + 9);
    i10_st8(oh + x, i10_lr_pairs(i10_lr_m(a0, d0), i10_lr_m(a1, d1)));
    i10_st8(ov + x, i10_lr_pairs(i10_lr_m(b0, c0), i10_lr_m(b1, c1)));
    i10_st8(oc + x, i10_lr_pairs(i10_lr_m(d0, e0), i10_lr_m(d1, e1)));
}

#endif /* YAH264_DSP_X86_INTERP_X86_10_H */
