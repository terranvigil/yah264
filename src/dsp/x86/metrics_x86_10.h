/*
 * metrics_x86_10.h - the 10-bit pixel kernels' shared 128-bit bodies
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * 16-bit-sample twins of metrics_x86.h and metrics_sse4.c. every body is
 * static inline and 128-bit: the SSE4.2 file wraps them all, the AVX2 file
 * wraps the narrow shapes and writes its own 256-bit 16-wide forms.
 *
 * a - b in 16-bit wraparound is the exact residual, as both are <= 1023. the
 * bounds that shape the code:
 *   - a 4x4 Hadamard coefficient is <= 16 * 1023 = 16368, and the four
 *     magnitudes the SATD core adds in a lane are <= 32736, so int16 holds.
 *   - an 8x8 coefficient reaches 64 * 1023 = 65472, so the row transform
 *     stops a stage short and takes the last as 2 * max(|x|, |y|).
 *
 * x86 has no 16-bit SAD instruction: SAD is sub, abs and add, kept in 16-bit
 * lanes for as many rows as the bound allows.
 *
 * loads are sized to the block, never the register: checkasm puts a
 * PROT_NONE page at both ends of each kernel's window.
 */
#ifndef YAH264_DSP_X86_METRICS_X86_10_H
#define YAH264_DSP_X86_METRICS_X86_10_H

#include <immintrin.h>
#include <string.h>

#include "dsp/arch.h"

#if Y264_BIT_DEPTH != 10
#error "metrics_x86_10.h is the 10-bit library's x86 tier"
#endif

/* ---- loads and reductions ------------------------------------------------ */

static inline __m128i m10_ld8(const pixel *p)
{
    return _mm_loadu_si128((const __m128i *)(const void *)p);
}

static inline __m128i m10_ld4(const pixel *p)
{
    return _mm_loadl_epi64((const __m128i *)(const void *)p);
}

/* two rows of four samples in one register */
static inline __m128i m10_rows2x4(const pixel *p, int stride)
{
    return _mm_unpacklo_epi64(m10_ld4(p), m10_ld4(p + stride));
}

static inline int m10_hsum32(__m128i v)
{
    v = _mm_add_epi32(v, _mm_shuffle_epi32(v, 0x4e));
    v = _mm_add_epi32(v, _mm_shuffle_epi32(v, 0xb1));
    return _mm_cvtsi128_si32(v);
}

/* lanes must be <= 32767: the madd reads them signed */
static inline int m10_hsum16(__m128i v)
{
    return m10_hsum32(_mm_madd_epi16(v, _mm_set1_epi16(1)));
}

static inline __m128i m10_absdiff(__m128i a, __m128i b)
{
    return _mm_abs_epi16(_mm_sub_epi16(a, b));
}

/* ---- SAD -----------------------------------------------------------------
 *
 * a 16-wide row adds its two registers first: <= 2046 a lane a row, <= 32736
 * after sixteen rows (8-wide, half that), still signed for the madd. */
static inline int m10_sad_16xh(const pixel *a, int as, const pixel *b, int bs,
                               int h)
{
    __m128i acc = _mm_setzero_si128();
    for (int y = 0; y < h; y++, a += as, b += bs)
        acc = _mm_add_epi16(acc, _mm_add_epi16(m10_absdiff(m10_ld8(a), m10_ld8(b)),
                                               m10_absdiff(m10_ld8(a + 8),
                                                           m10_ld8(b + 8))));
    return m10_hsum16(acc);
}

static inline int m10_sad_8xh(const pixel *a, int as, const pixel *b, int bs,
                              int h)
{
    __m128i acc = _mm_setzero_si128();
    for (int y = 0; y < h; y++, a += as, b += bs)
        acc = _mm_add_epi16(acc, m10_absdiff(m10_ld8(a), m10_ld8(b)));
    return m10_hsum16(acc);
}

/* one source load a row feeds every candidate; n is constant at each call
 * site, so the loops unroll */
static inline void m10_sad_n_16xh(const pixel *src, int ss, const pixel *const *r,
                                  int rs, int h, int n, int *scores)
{
    __m128i acc[4] = { _mm_setzero_si128(), _mm_setzero_si128(),
                       _mm_setzero_si128(), _mm_setzero_si128() };
    for (int y = 0; y < h; y++) {
        __m128i s0 = m10_ld8(src + y * ss), s1 = m10_ld8(src + y * ss + 8);
        for (int k = 0; k < n; k++) {
            const pixel *q = r[k] + y * rs;
            acc[k] = _mm_add_epi16(acc[k],
                                   _mm_add_epi16(m10_absdiff(s0, m10_ld8(q)),
                                                 m10_absdiff(s1, m10_ld8(q + 8))));
        }
    }
    for (int k = 0; k < n; k++)
        scores[k] = m10_hsum16(acc[k]);
}

static inline void m10_sad_n_8xh(const pixel *src, int ss, const pixel *const *r,
                                 int rs, int h, int n, int *scores)
{
    __m128i acc[4] = { _mm_setzero_si128(), _mm_setzero_si128(),
                       _mm_setzero_si128(), _mm_setzero_si128() };
    for (int y = 0; y < h; y++) {
        __m128i s = m10_ld8(src + y * ss);
        for (int k = 0; k < n; k++)
            acc[k] = _mm_add_epi16(acc[k], m10_absdiff(s, m10_ld8(r[k] + y * rs)));
    }
    for (int k = 0; k < n; k++)
        scores[k] = m10_hsum16(acc[k]);
}

/* ---- the SATD core -------------------------------------------------------
 *
 * four residual rows of eight -> two 4x4 tiles' magnitude sums in four int32
 * lanes. lanes reach 4092 after the vertical butterfly, 8184 then 16368 after
 * the two hadd/hsub stages. the four magnitudes in a lane belong to one tile,
 * and by Cauchy-Schwarz they sum to <= 2 * sqrt(sum c^2) =
 * 8 * sqrt(sum d^2) <= 32736, so they add in int16 before the madd. a tile of
 * samples obeys the same bound. */
static inline __m128i m10_satd_rows8(__m128i d0, __m128i d1, __m128i d2, __m128i d3)
{
    const __m128i one = _mm_set1_epi16(1);
    __m128i t0 = _mm_add_epi16(d0, d1), t1 = _mm_sub_epi16(d0, d1);
    __m128i t2 = _mm_add_epi16(d2, d3), t3 = _mm_sub_epi16(d2, d3);
    __m128i o0 = _mm_add_epi16(t0, t2), o2 = _mm_sub_epi16(t0, t2);
    __m128i o1 = _mm_add_epi16(t1, t3), o3 = _mm_sub_epi16(t1, t3);
    __m128i h01 = _mm_hadd_epi16(o0, o1), g01 = _mm_hsub_epi16(o0, o1);
    __m128i h23 = _mm_hadd_epi16(o2, o3), g23 = _mm_hsub_epi16(o2, o3);
    __m128i a = _mm_abs_epi16(_mm_hadd_epi16(h01, g01));
    __m128i b = _mm_abs_epi16(_mm_hsub_epi16(h01, g01));
    __m128i c = _mm_abs_epi16(_mm_hadd_epi16(h23, g23));
    __m128i d = _mm_abs_epi16(_mm_hsub_epi16(h23, g23));
    return _mm_madd_epi16(_mm_add_epi16(_mm_add_epi16(a, b), _mm_add_epi16(c, d)), one);
}

/* one 4x4 tile, two rows a register: the vertical butterfly crosses the
 * register halves. two magnitudes a lane add to <= 32736. */
static inline __m128i m10_satd_tile4(__m128i d01, __m128i d23)
{
    __m128i w01 = _mm_shuffle_epi32(d01, 0x4e);
    __m128i w23 = _mm_shuffle_epi32(d23, 0x4e);
    __m128i t01 = _mm_unpacklo_epi64(_mm_add_epi16(d01, w01),
                                     _mm_sub_epi16(d01, w01));
    __m128i t23 = _mm_unpacklo_epi64(_mm_add_epi16(d23, w23),
                                     _mm_sub_epi16(d23, w23));
    __m128i x = _mm_add_epi16(t01, t23), y = _mm_sub_epi16(t01, t23);
    __m128i h = _mm_hadd_epi16(x, y), g = _mm_hsub_epi16(x, y);
    __m128i a = _mm_abs_epi16(_mm_hadd_epi16(h, g));
    __m128i b = _mm_abs_epi16(_mm_hsub_epi16(h, g));
    return _mm_madd_epi16(_mm_add_epi16(a, b), _mm_set1_epi16(1));
}

static inline int m10_satd_4x4(const pixel *a, int as, const pixel *b, int bs)
{
    __m128i d01 = _mm_sub_epi16(m10_rows2x4(a, as), m10_rows2x4(b, bs));
    __m128i d23 = _mm_sub_epi16(m10_rows2x4(a + 2 * as, as),
                                m10_rows2x4(b + 2 * bs, bs));
    return m10_hsum32(m10_satd_tile4(d01, d23));
}

static inline __m128i m10_satd_group8(const pixel *a, int as, const pixel *b, int bs)
{
    __m128i d[4];
    for (int i = 0; i < 4; i++)
        d[i] = _mm_sub_epi16(m10_ld8(a + i * as), m10_ld8(b + i * bs));
    return m10_satd_rows8(d[0], d[1], d[2], d[3]);
}

/* w x h as a sum of four-row, eight-column groups */
static inline int m10_satd_wxh(const pixel *a, int as, const pixel *b, int bs,
                               int w, int h)
{
    __m128i acc = _mm_setzero_si128();
    for (int y = 0; y < h; y += 4)
        for (int x = 0; x < w; x += 8)
            acc = _mm_add_epi32(acc, m10_satd_group8(a + y * as + x, as,
                                                     b + y * bs + x, bs));
    return m10_hsum32(acc);
}

static inline void m10_satd_quad_8x8(const pixel *src, int ss, const pixel *r0,
                                     const pixel *r1, const pixel *r2,
                                     const pixel *r3, int rs, int scores[4])
{
    /* source rows load once for all four candidates */
    const pixel *const r[4] = { r0, r1, r2, r3 };
    __m128i s[8];
    for (int i = 0; i < 8; i++)
        s[i] = m10_ld8(src + i * ss);
    for (int k = 0; k < 4; k++) {
        __m128i d[8];
        for (int i = 0; i < 8; i++)
            d[i] = _mm_sub_epi16(s[i], m10_ld8(r[k] + i * rs));
        scores[k] = m10_hsum32(_mm_add_epi32(m10_satd_rows8(d[0], d[1], d[2], d[3]),
                                             m10_satd_rows8(d[4], d[5], d[6], d[7])));
    }
}

/* against (p + q + 1) >> 1, which PAVGW is exactly; p and q share a stride */
static inline int m10_satd_avg_wxh(const pixel *src, int ss, const pixel *p,
                                   const pixel *q, int ps, int w, int h)
{
    __m128i acc = _mm_setzero_si128();
    for (int y = 0; y < h; y += 4)
        for (int x = 0; x < w; x += 8) {
            __m128i d[4];
            for (int i = 0; i < 4; i++) {
                int o = (y + i) * ps + x;
                d[i] = _mm_sub_epi16(m10_ld8(src + (y + i) * ss + x),
                                     _mm_avg_epu16(m10_ld8(p + o), m10_ld8(q + o)));
            }
            acc = _mm_add_epi32(acc, m10_satd_rows8(d[0], d[1], d[2], d[3]));
        }
    return m10_hsum32(acc);
}

/* ---- the 8x8 Walsh-Hadamard ----------------------------------------------
 *
 * the column pass is <= 8184 a lane, residual or sample. the row pass's first
 * two in-register stages reach 16368 and 32736; the third would reach 65472,
 * so it is taken as 2 * max(|x|, |y|) of each pair. the max lands in the even
 * lane; the odd lane is don't-care and the caller's madd drops it. */
static inline __m128i m10_had8_rowmax(__m128i v)
{
    __m128i s4 = _mm_shuffle_epi32(v, 0x4e);
    __m128i a = _mm_unpacklo_epi64(_mm_add_epi16(v, s4), _mm_sub_epi16(v, s4));
    __m128i s2 = _mm_shufflehi_epi16(_mm_shufflelo_epi16(a, 0x4e), 0x4e);
    __m128i p = _mm_add_epi16(a, s2), q = _mm_sub_epi16(a, s2);
    __m128i qs = _mm_shufflehi_epi16(_mm_shufflelo_epi16(q, 0x4e), 0x4e);
    __m128i b = _mm_abs_epi16(_mm_blend_epi16(p, qs, 0xcc));
    return _mm_max_epi16(b, _mm_srli_epi32(b, 16));
}

#define M10_HAD8_COLS(v, o) do {                                             \
    __m128i a0_ = _mm_add_epi16((v)[0], (v)[4]);                             \
    __m128i a1_ = _mm_add_epi16((v)[1], (v)[5]);                             \
    __m128i a2_ = _mm_add_epi16((v)[2], (v)[6]);                             \
    __m128i a3_ = _mm_add_epi16((v)[3], (v)[7]);                             \
    __m128i a4_ = _mm_sub_epi16((v)[0], (v)[4]);                             \
    __m128i a5_ = _mm_sub_epi16((v)[1], (v)[5]);                             \
    __m128i a6_ = _mm_sub_epi16((v)[2], (v)[6]);                             \
    __m128i a7_ = _mm_sub_epi16((v)[3], (v)[7]);                             \
    __m128i b0_ = _mm_add_epi16(a0_, a2_), b1_ = _mm_add_epi16(a1_, a3_);    \
    __m128i b2_ = _mm_sub_epi16(a0_, a2_), b3_ = _mm_sub_epi16(a1_, a3_);    \
    __m128i b4_ = _mm_add_epi16(a4_, a6_), b5_ = _mm_add_epi16(a5_, a7_);    \
    __m128i b6_ = _mm_sub_epi16(a4_, a6_), b7_ = _mm_sub_epi16(a5_, a7_);    \
    (o)[0] = _mm_add_epi16(b0_, b1_); (o)[1] = _mm_sub_epi16(b0_, b1_);      \
    (o)[2] = _mm_add_epi16(b2_, b3_); (o)[3] = _mm_sub_epi16(b2_, b3_);      \
    (o)[4] = _mm_add_epi16(b4_, b5_); (o)[5] = _mm_sub_epi16(b4_, b5_);      \
    (o)[6] = _mm_add_epi16(b6_, b7_); (o)[7] = _mm_sub_epi16(b6_, b7_);      \
} while (0)

/* half the 64 magnitudes' sum: the madd by (1, 0) keeps each pair's max
 * (<= 32736) and drops the don't-care lane */
static inline int m10_had8x8_half(const __m128i d[8])
{
    const __m128i keep = _mm_set1_epi32(1);
    __m128i c[8], acc = _mm_setzero_si128();
    M10_HAD8_COLS(d, c);
    for (int i = 0; i < 8; i++)
        acc = _mm_add_epi32(acc, _mm_madd_epi16(m10_had8_rowmax(c[i]), keep));
    return m10_hsum32(acc);
}

static inline int m10_sa8d_8x8(const pixel *a, int as, const pixel *b, int bs)
{
    __m128i d[8];
    for (int i = 0; i < 8; i++)
        d[i] = _mm_sub_epi16(m10_ld8(a + i * as), m10_ld8(b + i * bs));
    return (2 * m10_had8x8_half(d) + 2) >> 2;
}

/* the pixel sum (<= 65472) is the DC, so subtracting it leaves the AC */
static inline long m10_hadamard_ac_8x8(const pixel *p, int stride)
{
    __m128i d[8], s = _mm_setzero_si128();
    for (int i = 0; i < 8; i++) {
        d[i] = m10_ld8(p + i * stride);
        s = _mm_add_epi32(s, _mm_madd_epi16(d[i], _mm_set1_epi16(1)));
    }
    return 2 * (long)m10_had8x8_half(d) - (long)m10_hsum32(s);
}

/* ---- the psy texture terms -----------------------------------------------
 *
 * four rows of eight as two 4x4 tiles; lanes 0 and 1 of the first hadd pair
 * are the tiles' DCs (sample sums, <= 16368). lanes fit int16 as in the SATD
 * core. */
static inline __m128i m10_tex_group8(const pixel *p, int stride, int dc[2])
{
    const __m128i one = _mm_set1_epi16(1);
    __m128i d0 = m10_ld8(p), d1 = m10_ld8(p + stride);
    __m128i d2 = m10_ld8(p + 2 * stride), d3 = m10_ld8(p + 3 * stride);
    __m128i t0 = _mm_add_epi16(d0, d1), t1 = _mm_sub_epi16(d0, d1);
    __m128i t2 = _mm_add_epi16(d2, d3), t3 = _mm_sub_epi16(d2, d3);
    __m128i o0 = _mm_add_epi16(t0, t2), o2 = _mm_sub_epi16(t0, t2);
    __m128i o1 = _mm_add_epi16(t1, t3), o3 = _mm_sub_epi16(t1, t3);
    __m128i h01 = _mm_hadd_epi16(o0, o1), g01 = _mm_hsub_epi16(o0, o1);
    __m128i h23 = _mm_hadd_epi16(o2, o3), g23 = _mm_hsub_epi16(o2, o3);
    __m128i c0 = _mm_hadd_epi16(h01, g01);
    __m128i a = _mm_abs_epi16(c0);
    __m128i b = _mm_abs_epi16(_mm_hsub_epi16(h01, g01));
    __m128i c = _mm_abs_epi16(_mm_hadd_epi16(h23, g23));
    __m128i e = _mm_abs_epi16(_mm_hsub_epi16(h23, g23));
    dc[0] = (int16_t)_mm_extract_epi16(c0, 0);
    dc[1] = (int16_t)_mm_extract_epi16(c0, 1);
    return _mm_madd_epi16(_mm_add_epi16(_mm_add_epi16(a, b), _mm_add_epi16(c, e)), one);
}

/* a tile's SATD against a flat block of its rounded mean: the DC is replaced
 * by its distance from that mean */
static inline long m10_tex_corr(int dc)
{
    int flat = 16 * ((dc + 8) >> 4);
    return (long)(dc < flat ? flat - dc : dc - flat) - (long)dc;
}

static inline long m10_texture_ac4_16x16(const pixel *p, int stride)
{
    __m128i acc = _mm_setzero_si128();
    long corr = 0;
    for (int by = 0; by < 16; by += 4)
        for (int bx = 0; bx < 16; bx += 8) {
            int dc[2];
            acc = _mm_add_epi32(acc, m10_tex_group8(p + by * stride + bx, stride, dc));
            corr += m10_tex_corr(dc[0]) + m10_tex_corr(dc[1]);
        }
    return (long)m10_hsum32(acc) + corr;
}

static inline void m10_texture_ac48_16x16(const pixel *p, int stride, long out[2])
{
    __m128i a4 = _mm_setzero_si128();
    long corr = 0, e8 = 0;
    for (int qy = 0; qy < 16; qy += 8)
        for (int qx = 0; qx < 16; qx += 8) {
            const pixel *q = p + qy * stride + qx;
            e8 += m10_hadamard_ac_8x8(q, stride);
            for (int i = 0; i < 8; i += 4) {
                int dc[2];
                a4 = _mm_add_epi32(a4, m10_tex_group8(q + i * stride, stride, dc));
                corr += m10_tex_corr(dc[0]) + m10_tex_corr(dc[1]);
            }
        }
    out[0] = (long)m10_hsum32(a4) + corr;
    out[1] = e8;
}

/* ---- variance -------------------------------------------------------------
 *
 * a sum lane takes two samples a row, <= 32736 after sixteen rows; a square
 * lane takes four squares a row, <= 16 * 4 * 1023^2 ~= 67M. */
static inline void m10_var_16x16(const pixel *p, int stride, uint32_t out[2])
{
    __m128i s1 = _mm_setzero_si128(), s2 = s1;
    for (int y = 0; y < 16; y++, p += stride) {
        __m128i v0 = m10_ld8(p), v1 = m10_ld8(p + 8);
        s1 = _mm_add_epi16(s1, _mm_add_epi16(v0, v1));
        s2 = _mm_add_epi32(s2, _mm_add_epi32(_mm_madd_epi16(v0, v0),
                                             _mm_madd_epi16(v1, v1)));
    }
    out[0] = (uint32_t)m10_hsum16(s1);
    out[1] = (uint32_t)m10_hsum32(s2);
}

/* ---- the fused Intra4x4 nine-mode cost -----------------------------------
 *
 * every prediction sample is an entry of a 64-entry table over the flat edge
 * E = { l3, l3, l2, l1, l0, tl, t0..t7, t7 ... }: the 121 filter (0-15), the
 * pair average (16-31), E itself (32-47) and DC (48). the index rows restate
 * the C builder's per-mode formulas. a byte shuffle reaches sixteen entries,
 * so the 16-bit table is split into low and high bytes, looked up with the
 * same indices and interleaved back. */
static const uint8_t m10_i4_idx[9][16] = {
    { 38, 39, 40, 41, 38, 39, 40, 41, 38, 39, 40, 41, 38, 39, 40, 41 }, /* V  */
    { 36, 36, 36, 36, 35, 35, 35, 35, 34, 34, 34, 34, 33, 33, 33, 33 }, /* H  */
    { 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48 }, /* DC */
    {  6,  7,  8,  9,  7,  8,  9, 10,  8,  9, 10, 11,  9, 10, 11, 12 }, /* DDL*/
    {  4,  5,  6,  7,  3,  4,  5,  6,  2,  3,  4,  5,  1,  2,  3,  4 }, /* DDR*/
    { 21, 22, 23, 24,  4,  5,  6,  7,  3, 21, 22, 23,  2,  4,  5,  6 }, /* VR */
    { 20,  4,  5,  6, 19,  3, 20,  4, 18,  2, 19,  3, 17,  1, 18,  2 }, /* HD */
    { 22, 23, 24, 25,  6,  7,  8,  9, 23, 24, 25, 26,  7,  8,  9, 10 }, /* VL */
    { 19,  2, 18,  1, 18,  1, 17,  0, 17,  0, 33, 33, 33, 33, 33, 33 }, /* HU */
};

/* a 64-entry byte table in four registers, selected on bits 4 and 5 */
static inline __m128i m10_tbl64(const __m128i t[4], __m128i idx)
{
    const __m128i b16 = _mm_set1_epi8(16), b32 = _mm_set1_epi8(32);
    __m128i lo = _mm_and_si128(idx, _mm_set1_epi8(15));
    __m128i s16 = _mm_cmpeq_epi8(_mm_and_si128(idx, b16), b16);
    __m128i s32 = _mm_cmpeq_epi8(_mm_and_si128(idx, b32), b32);
    __m128i a = _mm_blendv_epi8(_mm_shuffle_epi8(t[0], lo),
                                _mm_shuffle_epi8(t[1], lo), s16);
    __m128i b = _mm_blendv_epi8(_mm_shuffle_epi8(t[2], lo),
                                _mm_shuffle_epi8(t[3], lo), s16);
    return _mm_blendv_epi8(a, b, s32);
}

/* sixteen 16-bit entries (two registers) as their low and high bytes */
static inline void m10_split_bytes(__m128i v0, __m128i v1, __m128i *lo, __m128i *hi)
{
    const __m128i m = _mm_set1_epi16(0xff);
    *lo = _mm_packus_epi16(_mm_and_si128(v0, m), _mm_and_si128(v1, m));
    *hi = _mm_packus_epi16(_mm_srli_epi16(v0, 8), _mm_srli_epi16(v1, 8));
}

/* rows 0-1 of each prediction in p01, rows 2-3 in p23 */
static inline void m10_i4_preds(__m128i p01[9], __m128i p23[9], const pixel *rec,
                                int rs, int ht, int hl, int htl, int htr)
{
    pixel e[24], top[8], left[4];
    int tl = 0, st = 0, sl = 0, dc;

    for (int i = 0; i < 4; i++) {
        top[i]  = ht ? rec[-rs + i] : 0;
        left[i] = hl ? rec[i * rs - 1] : 0;
        st += top[i];
        sl += left[i];
    }
    for (int i = 4; i < 8; i++)
        top[i] = htr ? rec[-rs + i] : top[3];
    if (htl)
        tl = rec[-rs - 1];

    if (ht && hl)   dc = (st + sl + 4) >> 3;
    else if (ht)    dc = (st + 2) >> 2;
    else if (hl)    dc = (sl + 2) >> 2;
    else            dc = 1 << (Y264_BIT_DEPTH - 1);

    e[0] = left[3]; e[1] = left[3]; e[2] = left[2];
    e[3] = left[1]; e[4] = left[0]; e[5] = (pixel)tl;
    for (int i = 0; i < 8; i++)
        e[6 + i] = top[i];
    for (int i = 14; i < 24; i++)
        e[i] = top[7];

    __m128i f[2], h[2], x[2];
    for (int k = 0; k < 2; k++) {
        __m128i e0 = m10_ld8(e + 8 * k), e1 = m10_ld8(e + 8 * k + 1);
        __m128i e2 = m10_ld8(e + 8 * k + 2);
        /* (a + 2b + c + 2) >> 2, <= 4094 before the shift */
        f[k] = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(e0, e2),
                                            _mm_add_epi16(_mm_add_epi16(e1, e1),
                                                          _mm_set1_epi16(2))), 2);
        h[k] = _mm_avg_epu16(e0, e1);
        x[k] = e0;
    }
    __m128i lo[4], hi[4];
    __m128i vdc = _mm_set1_epi16((short)dc);
    m10_split_bytes(f[0], f[1], &lo[0], &hi[0]);
    m10_split_bytes(h[0], h[1], &lo[1], &hi[1]);
    m10_split_bytes(x[0], x[1], &lo[2], &hi[2]);
    m10_split_bytes(vdc, vdc, &lo[3], &hi[3]);
    for (int m = 0; m < 9; m++) {
        __m128i ix = _mm_loadu_si128((const __m128i *)(const void *)m10_i4_idx[m]);
        __m128i pl = m10_tbl64(lo, ix), ph = m10_tbl64(hi, ix);
        p01[m] = _mm_unpacklo_epi8(pl, ph);
        p23[m] = _mm_unpackhi_epi8(pl, ph);
    }
}

static inline void m10_intra4_mode_search(const pixel *src, int ss, const pixel *rec,
                                          int rs, int ht, int hl, int htl, int htr,
                                          int costs[9])
{
    __m128i p01[9], p23[9];
    __m128i s01 = m10_rows2x4(src, ss), s23 = m10_rows2x4(src + 2 * ss, ss);
    m10_i4_preds(p01, p23, rec, rs, ht, hl, htl, htr);
    for (int m = 0; m < 9; m++)
        costs[m] = m10_hsum32(m10_satd_tile4(_mm_sub_epi16(p01[m], s01),
                                             _mm_sub_epi16(p23[m], s23)));
}

/* all three I16x16 predictions are broadcasts, so one pass of source loads
 * forms the three residuals */
static inline void m10_intra16_satd_trio(const pixel *src, int ss, const pixel *top,
                                         const pixel *left, int dc, int costs[3])
{
    __m128i tl = m10_ld8(top), th = m10_ld8(top + 8);
    __m128i vd = _mm_set1_epi16((short)dc);
    __m128i av = _mm_setzero_si128(), ah = av, ad = av;

    for (int y = 0; y < 16; y += 4) {
        __m128i sl[4], sh[4], hb[4];
        for (int i = 0; i < 4; i++) {
            sl[i] = m10_ld8(src + (y + i) * ss);
            sh[i] = m10_ld8(src + (y + i) * ss + 8);
            hb[i] = _mm_set1_epi16((short)left[y + i]);
        }
#define M10_X3(acc, p0, p1, p2, p3, q0, q1, q2, q3)                            \
        acc = _mm_add_epi32(acc,                                               \
            _mm_add_epi32(m10_satd_rows8(_mm_sub_epi16(sl[0], p0),             \
                                         _mm_sub_epi16(sl[1], p1),             \
                                         _mm_sub_epi16(sl[2], p2),             \
                                         _mm_sub_epi16(sl[3], p3)),            \
                          m10_satd_rows8(_mm_sub_epi16(sh[0], q0),             \
                                         _mm_sub_epi16(sh[1], q1),             \
                                         _mm_sub_epi16(sh[2], q2),             \
                                         _mm_sub_epi16(sh[3], q3))))
        M10_X3(av, tl, tl, tl, tl, th, th, th, th);
        M10_X3(ah, hb[0], hb[1], hb[2], hb[3], hb[0], hb[1], hb[2], hb[3]);
        M10_X3(ad, vd, vd, vd, vd, vd, vd, vd, vd);
#undef M10_X3
    }
    costs[0] = m10_hsum32(av);
    costs[1] = m10_hsum32(ah);
    costs[2] = m10_hsum32(ad);
}

/* ---- SSD and the per-4x4 maps ---------------------------------------------
 *
 * a squared residual reaches 1023^2, so a madd of the residual with itself
 * widens (two squares an int32 lane, <= 2093058). a 16x16 total is
 * <= 2.68e8, inside the callers' int. the 4x4 signed sums stay in int16 (four
 * rows of +-1023) and widen through a pair add. */
static inline int m10_ssd_16xh(const pixel *a, int as, const pixel *b, int bs, int h)
{
    __m128i acc = _mm_setzero_si128();
    for (int y = 0; y < h; y++, a += as, b += bs) {
        __m128i d0 = _mm_sub_epi16(m10_ld8(a), m10_ld8(b));
        __m128i d1 = _mm_sub_epi16(m10_ld8(a + 8), m10_ld8(b + 8));
        acc = _mm_add_epi32(acc, _mm_add_epi32(_mm_madd_epi16(d0, d0),
                                               _mm_madd_epi16(d1, d1)));
    }
    return m10_hsum32(acc);
}

static inline int m10_ssd_8xh(const pixel *a, int as, const pixel *b, int bs, int h)
{
    __m128i acc = _mm_setzero_si128();
    for (int y = 0; y < h; y++, a += as, b += bs) {
        __m128i d = _mm_sub_epi16(m10_ld8(a), m10_ld8(b));
        acc = _mm_add_epi32(acc, _mm_madd_epi16(d, d));
    }
    return m10_hsum32(acc);
}

static inline void m10_ssdsum4x4_16(const pixel *a, int as, const pixel *b, int bs,
                                    uint32_t e[16], int32_t sum[16])
{
    const __m128i one = _mm_set1_epi16(1);
    for (int r = 0; r < 4; r++) {
        __m128i el = _mm_setzero_si128(), eh = el, sl = el, sh = el;
        for (int y = 0; y < 4; y++) {
            const pixel *pa = a + (4 * r + y) * as, *pb = b + (4 * r + y) * bs;
            __m128i dl = _mm_sub_epi16(m10_ld8(pa), m10_ld8(pb));
            __m128i dh = _mm_sub_epi16(m10_ld8(pa + 8), m10_ld8(pb + 8));
            el = _mm_add_epi32(el, _mm_madd_epi16(dl, dl));
            eh = _mm_add_epi32(eh, _mm_madd_epi16(dh, dh));
            sl = _mm_add_epi16(sl, dl);
            sh = _mm_add_epi16(sh, dh);
        }
        _mm_storeu_si128((__m128i *)(void *)(e + 4 * r), _mm_hadd_epi32(el, eh));
        _mm_storeu_si128((__m128i *)(void *)(sum + 4 * r),
                         _mm_hadd_epi32(_mm_madd_epi16(sl, one),
                                        _mm_madd_epi16(sh, one)));
    }
}

static inline void m10_ssdsum4x4_8(const pixel *a, int as, const pixel *b, int bs,
                                   uint32_t e[4], int32_t sum[4])
{
    const __m128i one = _mm_set1_epi16(1);
    __m128i e0 = _mm_setzero_si128(), e1 = e0, s0 = e0, s1 = e0;
    for (int y = 0; y < 4; y++) {
        __m128i d0 = _mm_sub_epi16(m10_ld8(a + y * as), m10_ld8(b + y * bs));
        __m128i d1 = _mm_sub_epi16(m10_ld8(a + (4 + y) * as),
                                   m10_ld8(b + (4 + y) * bs));
        e0 = _mm_add_epi32(e0, _mm_madd_epi16(d0, d0));
        e1 = _mm_add_epi32(e1, _mm_madd_epi16(d1, d1));
        s0 = _mm_add_epi16(s0, d0);
        s1 = _mm_add_epi16(s1, d1);
    }
    _mm_storeu_si128((__m128i *)(void *)e, _mm_hadd_epi32(e0, e1));
    _mm_storeu_si128((__m128i *)(void *)sum,
                     _mm_hadd_epi32(_mm_madd_epi16(s0, one), _mm_madd_epi16(s1, one)));
}

static inline void m10_ssd4x4_map16(const pixel *a, int as, const pixel *b, int bs,
                                    uint32_t out[16])
{
    for (int r = 0; r < 4; r++) {
        __m128i el = _mm_setzero_si128(), eh = el;
        for (int y = 0; y < 4; y++) {
            const pixel *pa = a + (4 * r + y) * as, *pb = b + (4 * r + y) * bs;
            __m128i dl = _mm_sub_epi16(m10_ld8(pa), m10_ld8(pb));
            __m128i dh = _mm_sub_epi16(m10_ld8(pa + 8), m10_ld8(pb + 8));
            el = _mm_add_epi32(el, _mm_madd_epi16(dl, dl));
            eh = _mm_add_epi32(eh, _mm_madd_epi16(dh, dh));
        }
        _mm_storeu_si128((__m128i *)(void *)(out + 4 * r), _mm_hadd_epi32(el, eh));
    }
}

/* ---- every 128-bit kernel under one tier's names --------------------------
 *
 * the SSE4.2 file instantiates the whole list, the AVX2 file only the shapes
 * it has no wider form for. */
#define M10_SAD(T, w, h)                                                        \
int y264_sad_##w##x##h##_##T(const pixel *a, int as, const pixel *b, int bs)    \
{ return m10_sad_##w##xh(a, as, b, bs, h); }

#define M10_SADN(T, w, h)                                                       \
void y264_sad_quad_##w##x##h##_##T(const pixel *src, int ss, const pixel *r0,   \
                                   const pixel *r1, const pixel *r2,            \
                                   const pixel *r3, int rs, int scores[4])      \
{ const pixel *const r[4] = { r0, r1, r2, r3 };                                 \
  m10_sad_n_##w##xh(src, ss, r, rs, h, 4, scores); }                            \
void y264_sad_trio_##w##x##h##_##T(const pixel *src, int ss, const pixel *r0,   \
                                   const pixel *r1, const pixel *r2, int rs,    \
                                   int scores[3])                               \
{ const pixel *const r[3] = { r0, r1, r2 };                                     \
  m10_sad_n_##w##xh(src, ss, r, rs, h, 3, scores); }                            \
void y264_sad_pair_##w##x##h##_##T(const pixel *src, int ss, const pixel *r0,   \
                                   const pixel *r1, int rs, int scores[2])      \
{ const pixel *const r[2] = { r0, r1 };                                         \
  m10_sad_n_##w##xh(src, ss, r, rs, h, 2, scores); }

#define M10_SATD(T, w, h)                                                       \
int y264_satd_##w##x##h##_##T(const pixel *a, int as, const pixel *b, int bs)   \
{ return m10_satd_wxh(a, as, b, bs, w, h); }

#define M10_SATD_AVG(T, w, h)                                                   \
int y264_satd_avg_##w##x##h##_##T(const pixel *src, int ss, const pixel *p,     \
                                  const pixel *q, int ps)                       \
{ return m10_satd_avg_wxh(src, ss, p, q, ps, w, h); }

#endif /* YAH264_DSP_X86_METRICS_X86_10_H */
