/*
 * metrics_avx2_10.c - AVX2 pixel kernels for the 10-bit library
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * At 16 bits a sample, a 16-wide row is exactly one 256-bit register, which
 * is the shape the wide forms here are built on: its low lane is columns 0-7
 * and its high lane columns 8-15, and every hadd, hsub, shuffle and blend this
 * family uses works on the two lanes independently. So the 128-bit sequences
 * of metrics_x86_10.h lift unchanged and the high lane simply carries the
 * right-hand tiles. An 8-wide block folds rows i and i + 4 (or i + 8) into the
 * two lanes for the same effect.
 *
 * Shapes with no second half to fill (a lone 4x4 SATD, one 8x8 transform) are
 * the shared 128-bit bodies, VEX-encoded here.
 */
#include "dsp/x86/metrics_x86_10.h"

#if Y264_HAVE_AVX2_10

/* ---- 256-bit plumbing ---------------------------------------------------- */

static inline __m256i m10_ld16(const pixel *p)
{
    return _mm256_loadu_si256((const __m256i *)(const void *)p);
}

static inline __m256i m10_2x128(__m128i lo, __m128i hi)
{
    return _mm256_inserti128_si256(_mm256_castsi128_si256(lo), hi, 1);
}

/* rows p and p + d of an 8-wide block, one per lane */
static inline __m256i m10_fold8(const pixel *p, int d)
{
    return m10_2x128(m10_ld8(p), m10_ld8(p + d));
}

static inline int m10_hsum32_256(__m256i v)
{
    return m10_hsum32(_mm_add_epi32(_mm256_castsi256_si128(v),
                                    _mm256_extracti128_si256(v, 1)));
}

static inline int m10_hsum16_256(__m256i v)
{
    return m10_hsum32_256(_mm256_madd_epi16(v, _mm256_set1_epi16(1)));
}

static inline __m256i m10_absdiff256(__m256i a, __m256i b)
{
    return _mm256_abs_epi16(_mm256_sub_epi16(a, b));
}

/* ---- SAD -----------------------------------------------------------------
 *
 * One row per register at width 16 (<= 1023 per lane per row, <= 16368 after
 * sixteen); two rows per register at width 8. */
#define M10_SAD16_AVX2(h)                                                       \
int y264_sad_16x##h##_avx2(const pixel *a, int as, const pixel *b, int bs)      \
{                                                                               \
    __m256i acc = _mm256_setzero_si256();                                       \
    for (int y = 0; y < (h); y++)                                               \
        acc = _mm256_add_epi16(acc, m10_absdiff256(m10_ld16(a + y * as),        \
                                                   m10_ld16(b + y * bs)));      \
    return m10_hsum16_256(acc);                                                 \
}
M10_SAD16_AVX2(16)
M10_SAD16_AVX2(8)

#define M10_SAD8_AVX2(h)                                                        \
int y264_sad_8x##h##_avx2(const pixel *a, int as, const pixel *b, int bs)       \
{                                                                               \
    __m256i acc = _mm256_setzero_si256();                                       \
    for (int y = 0; y < (h); y += 2)                                            \
        acc = _mm256_add_epi16(acc, m10_absdiff256(m10_fold8(a + y * as, as),   \
                                                   m10_fold8(b + y * bs, bs))); \
    return m10_hsum16_256(acc);                                                 \
}
M10_SAD8_AVX2(16)
M10_SAD8_AVX2(8)

static inline void sad_n_16xh(const pixel *src, int ss, const pixel *const *r,
                              int rs, int h, int n, int *scores)
{
    __m256i acc[4] = { _mm256_setzero_si256(), _mm256_setzero_si256(),
                       _mm256_setzero_si256(), _mm256_setzero_si256() };
    for (int y = 0; y < h; y++) {
        __m256i s = m10_ld16(src + y * ss);
        for (int k = 0; k < n; k++)
            acc[k] = _mm256_add_epi16(acc[k], m10_absdiff256(s, m10_ld16(r[k] + y * rs)));
    }
    for (int k = 0; k < n; k++)
        scores[k] = m10_hsum16_256(acc[k]);
}

static inline void sad_n_8xh(const pixel *src, int ss, const pixel *const *r,
                             int rs, int h, int n, int *scores)
{
    __m256i acc[4] = { _mm256_setzero_si256(), _mm256_setzero_si256(),
                       _mm256_setzero_si256(), _mm256_setzero_si256() };
    for (int y = 0; y < h; y += 2) {
        __m256i s = m10_fold8(src + y * ss, ss);
        for (int k = 0; k < n; k++)
            acc[k] = _mm256_add_epi16(acc[k],
                                      m10_absdiff256(s, m10_fold8(r[k] + y * rs, rs)));
    }
    for (int k = 0; k < n; k++)
        scores[k] = m10_hsum16_256(acc[k]);
}

#define M10_SADN_AVX2(w, h)                                                     \
void y264_sad_quad_##w##x##h##_avx2(const pixel *src, int ss, const pixel *r0,  \
                                    const pixel *r1, const pixel *r2,           \
                                    const pixel *r3, int rs, int scores[4])     \
{ const pixel *const r[4] = { r0, r1, r2, r3 };                                 \
  sad_n_##w##xh(src, ss, r, rs, h, 4, scores); }                                \
void y264_sad_trio_##w##x##h##_avx2(const pixel *src, int ss, const pixel *r0,  \
                                    const pixel *r1, const pixel *r2, int rs,   \
                                    int scores[3])                              \
{ const pixel *const r[3] = { r0, r1, r2 };                                     \
  sad_n_##w##xh(src, ss, r, rs, h, 3, scores); }                                \
void y264_sad_pair_##w##x##h##_avx2(const pixel *src, int ss, const pixel *r0,  \
                                    const pixel *r1, int rs, int scores[2])     \
{ const pixel *const r[2] = { r0, r1 };                                         \
  sad_n_##w##xh(src, ss, r, rs, h, 2, scores); }
M10_SADN_AVX2(16, 16)
M10_SADN_AVX2(16, 8)
M10_SADN_AVX2(8, 16)
M10_SADN_AVX2(8, 8)

/* 8x4 is one pair of folds; the narrow body serves it */
M10_SADN(avx2, 8, 4)

/* ---- SATD ----------------------------------------------------------------
 *
 * m10_satd_rows8 at twice the width: four 4x4 tiles per pass, the same
 * bounds per lane. */
static inline __m256i satd_rows16(__m256i d0, __m256i d1, __m256i d2, __m256i d3)
{
    const __m256i one = _mm256_set1_epi16(1);
    __m256i t0 = _mm256_add_epi16(d0, d1), t1 = _mm256_sub_epi16(d0, d1);
    __m256i t2 = _mm256_add_epi16(d2, d3), t3 = _mm256_sub_epi16(d2, d3);
    __m256i o0 = _mm256_add_epi16(t0, t2), o2 = _mm256_sub_epi16(t0, t2);
    __m256i o1 = _mm256_add_epi16(t1, t3), o3 = _mm256_sub_epi16(t1, t3);
    __m256i h01 = _mm256_hadd_epi16(o0, o1), g01 = _mm256_hsub_epi16(o0, o1);
    __m256i h23 = _mm256_hadd_epi16(o2, o3), g23 = _mm256_hsub_epi16(o2, o3);
    __m256i a = _mm256_abs_epi16(_mm256_hadd_epi16(h01, g01));
    __m256i b = _mm256_abs_epi16(_mm256_hsub_epi16(h01, g01));
    __m256i c = _mm256_abs_epi16(_mm256_hadd_epi16(h23, g23));
    __m256i d = _mm256_abs_epi16(_mm256_hsub_epi16(h23, g23));
    return _mm256_madd_epi16(_mm256_add_epi16(_mm256_add_epi16(a, b),
                                              _mm256_add_epi16(c, d)), one);
}

/* four rows of sixteen as residual */
static inline __m256i satd_group16(const pixel *a, int as, const pixel *b, int bs)
{
    __m256i d[4];
    for (int i = 0; i < 4; i++)
        d[i] = _mm256_sub_epi16(m10_ld16(a + i * as), m10_ld16(b + i * bs));
    return satd_rows16(d[0], d[1], d[2], d[3]);
}

/* four rows of eight, folded with the four rows `fold` below them */
static inline __m256i satd_fold8(const pixel *a, int as, const pixel *b, int bs,
                                 int fold)
{
    __m256i d[4];
    for (int i = 0; i < 4; i++)
        d[i] = _mm256_sub_epi16(m10_fold8(a + i * as, fold * as),
                                m10_fold8(b + i * bs, fold * bs));
    return satd_rows16(d[0], d[1], d[2], d[3]);
}

int y264_satd_4x4_avx2(const pixel *a, int as, const pixel *b, int bs)
{ return m10_satd_4x4(a, as, b, bs); }

int y264_satd_8x8_avx2(const pixel *a, int as, const pixel *b, int bs)
{ return m10_hsum32_256(satd_fold8(a, as, b, bs, 4)); }

int y264_satd_16x16_avx2(const pixel *a, int as, const pixel *b, int bs)
{
    __m256i acc = _mm256_setzero_si256();
    for (int y = 0; y < 16; y += 4)
        acc = _mm256_add_epi32(acc, satd_group16(a + y * as, as, b + y * bs, bs));
    return m10_hsum32_256(acc);
}

int y264_satd_16x8_avx2(const pixel *a, int as, const pixel *b, int bs)
{
    return m10_hsum32_256(_mm256_add_epi32(
        satd_group16(a, as, b, bs), satd_group16(a + 4 * as, as, b + 4 * bs, bs)));
}

int y264_satd_8x16_avx2(const pixel *a, int as, const pixel *b, int bs)
{
    return m10_hsum32_256(_mm256_add_epi32(
        satd_fold8(a, as, b, bs, 8), satd_fold8(a + 4 * as, as, b + 4 * bs, bs, 8)));
}

void y264_satd_quad_8x8_avx2(const pixel *src, int ss, const pixel *r0,
                             const pixel *r1, const pixel *r2, const pixel *r3,
                             int rs, int scores[4])
{
    /* the folded source is built once for the four candidates */
    const pixel *const r[4] = { r0, r1, r2, r3 };
    __m256i s[4];
    for (int i = 0; i < 4; i++)
        s[i] = m10_fold8(src + i * ss, 4 * ss);
    for (int k = 0; k < 4; k++) {
        __m256i d[4];
        for (int i = 0; i < 4; i++)
            d[i] = _mm256_sub_epi16(s[i], m10_fold8(r[k] + i * rs, 4 * rs));
        scores[k] = m10_hsum32_256(satd_rows16(d[0], d[1], d[2], d[3]));
    }
}

/* SATD against (p + q + 1) >> 1; p and q share a stride */
#define M10_SATD_AVG16_AVX2(h)                                                  \
int y264_satd_avg_16x##h##_avx2(const pixel *src, int ss, const pixel *p,       \
                                const pixel *q, int ps)                         \
{                                                                               \
    __m256i acc = _mm256_setzero_si256();                                       \
    for (int y = 0; y < (h); y += 4) {                                          \
        __m256i d[4];                                                           \
        for (int i = 0; i < 4; i++)                                             \
            d[i] = _mm256_sub_epi16(m10_ld16(src + (y + i) * ss),               \
                                    _mm256_avg_epu16(m10_ld16(p + (y + i) * ps),\
                                                     m10_ld16(q + (y + i) * ps)));\
        acc = _mm256_add_epi32(acc, satd_rows16(d[0], d[1], d[2], d[3]));       \
    }                                                                           \
    return m10_hsum32_256(acc);                                                 \
}
M10_SATD_AVG16_AVX2(16)
M10_SATD_AVG16_AVX2(8)

#define M10_SATD_AVG8_AVX2(h)                                                   \
int y264_satd_avg_8x##h##_avx2(const pixel *src, int ss, const pixel *p,        \
                               const pixel *q, int ps)                          \
{                                                                               \
    __m256i acc = _mm256_setzero_si256();                                       \
    for (int y = 0; y < (h); y += 8) {                                          \
        __m256i d[4];                                                           \
        for (int i = 0; i < 4; i++) {                                           \
            const int o = (y + i) * ps;                                         \
            d[i] = _mm256_sub_epi16(m10_fold8(src + (y + i) * ss, 4 * ss),      \
                                    _mm256_avg_epu16(m10_fold8(p + o, 4 * ps),  \
                                                     m10_fold8(q + o, 4 * ps)));\
        }                                                                       \
        acc = _mm256_add_epi32(acc, satd_rows16(d[0], d[1], d[2], d[3]));       \
    }                                                                           \
    return m10_hsum32_256(acc);                                                 \
}
M10_SATD_AVG8_AVX2(16)
M10_SATD_AVG8_AVX2(8)

/* ---- the 8x8 Walsh-Hadamard, two blocks at a time ------------------------
 *
 * A 16-wide row carries the left block's row in its low lane and the right
 * block's in its high lane. The bounds are m10_had8_rowmax's. */
static inline __m256i had8_rowmax256(__m256i v)
{
    __m256i s4 = _mm256_shuffle_epi32(v, 0x4e);
    __m256i a = _mm256_unpacklo_epi64(_mm256_add_epi16(v, s4), _mm256_sub_epi16(v, s4));
    __m256i s2 = _mm256_shufflehi_epi16(_mm256_shufflelo_epi16(a, 0x4e), 0x4e);
    __m256i p = _mm256_add_epi16(a, s2), q = _mm256_sub_epi16(a, s2);
    __m256i qs = _mm256_shufflehi_epi16(_mm256_shufflelo_epi16(q, 0x4e), 0x4e);
    __m256i b = _mm256_abs_epi16(_mm256_blend_epi16(p, qs, 0xcc));
    return _mm256_max_epi16(b, _mm256_srli_epi32(b, 16));
}

/* half of each block's magnitude sum, kept apart */
static inline void had8x8_half_pair(const __m256i d[8], int *lo, int *hi)
{
    const __m256i keep = _mm256_set1_epi32(1);
    __m256i c[8], acc = _mm256_setzero_si256();
    {
        __m256i a0 = _mm256_add_epi16(d[0], d[4]), a1 = _mm256_add_epi16(d[1], d[5]);
        __m256i a2 = _mm256_add_epi16(d[2], d[6]), a3 = _mm256_add_epi16(d[3], d[7]);
        __m256i a4 = _mm256_sub_epi16(d[0], d[4]), a5 = _mm256_sub_epi16(d[1], d[5]);
        __m256i a6 = _mm256_sub_epi16(d[2], d[6]), a7 = _mm256_sub_epi16(d[3], d[7]);
        __m256i b0 = _mm256_add_epi16(a0, a2), b1 = _mm256_add_epi16(a1, a3);
        __m256i b2 = _mm256_sub_epi16(a0, a2), b3 = _mm256_sub_epi16(a1, a3);
        __m256i b4 = _mm256_add_epi16(a4, a6), b5 = _mm256_add_epi16(a5, a7);
        __m256i b6 = _mm256_sub_epi16(a4, a6), b7 = _mm256_sub_epi16(a5, a7);
        c[0] = _mm256_add_epi16(b0, b1); c[1] = _mm256_sub_epi16(b0, b1);
        c[2] = _mm256_add_epi16(b2, b3); c[3] = _mm256_sub_epi16(b2, b3);
        c[4] = _mm256_add_epi16(b4, b5); c[5] = _mm256_sub_epi16(b4, b5);
        c[6] = _mm256_add_epi16(b6, b7); c[7] = _mm256_sub_epi16(b6, b7);
    }
    for (int i = 0; i < 8; i++)
        acc = _mm256_add_epi32(acc, _mm256_madd_epi16(had8_rowmax256(c[i]), keep));
    *lo = m10_hsum32(_mm256_castsi256_si128(acc));
    *hi = m10_hsum32(_mm256_extracti128_si256(acc, 1));
}

int y264_sa8d_8x8_avx2(const pixel *a, int as, const pixel *b, int bs)
{ return m10_sa8d_8x8(a, as, b, bs); }

int y264_sa8d_16x16_avx2(const pixel *a, int as, const pixel *b, int bs)
{
    int q[4];
    for (int h = 0; h < 2; h++) {
        __m256i d[8];
        for (int i = 0; i < 8; i++)
            d[i] = _mm256_sub_epi16(m10_ld16(a + (8 * h + i) * as),
                                    m10_ld16(b + (8 * h + i) * bs));
        had8x8_half_pair(d, &q[2 * h], &q[2 * h + 1]);
    }
    /* each quadrant carries its own rounding, as the reference's does */
    return ((2 * q[0] + 2) >> 2) + ((2 * q[1] + 2) >> 2)
         + ((2 * q[2] + 2) >> 2) + ((2 * q[3] + 2) >> 2);
}

long y264_hadamard_ac_8x8_avx2(const pixel *p, int stride)
{ return m10_hadamard_ac_8x8(p, stride); }

/* ---- the psy texture terms ----------------------------------------------- */

/* four rows of sixteen samples as four 4x4 tiles, with their DCs: lanes 0 and
 * 1 of each 128-bit half of the first hadd pair */
static inline __m256i tex_group16(const pixel *p, int stride, int dc[4])
{
    const __m256i one = _mm256_set1_epi16(1);
    __m256i d0 = m10_ld16(p), d1 = m10_ld16(p + stride);
    __m256i d2 = m10_ld16(p + 2 * stride), d3 = m10_ld16(p + 3 * stride);
    __m256i t0 = _mm256_add_epi16(d0, d1), t1 = _mm256_sub_epi16(d0, d1);
    __m256i t2 = _mm256_add_epi16(d2, d3), t3 = _mm256_sub_epi16(d2, d3);
    __m256i o0 = _mm256_add_epi16(t0, t2), o2 = _mm256_sub_epi16(t0, t2);
    __m256i o1 = _mm256_add_epi16(t1, t3), o3 = _mm256_sub_epi16(t1, t3);
    __m256i h01 = _mm256_hadd_epi16(o0, o1), g01 = _mm256_hsub_epi16(o0, o1);
    __m256i h23 = _mm256_hadd_epi16(o2, o3), g23 = _mm256_hsub_epi16(o2, o3);
    __m256i c0 = _mm256_hadd_epi16(h01, g01);
    __m256i a = _mm256_abs_epi16(c0);
    __m256i b = _mm256_abs_epi16(_mm256_hsub_epi16(h01, g01));
    __m256i c = _mm256_abs_epi16(_mm256_hadd_epi16(h23, g23));
    __m256i e = _mm256_abs_epi16(_mm256_hsub_epi16(h23, g23));
    __m128i lo = _mm256_castsi256_si128(c0), hi = _mm256_extracti128_si256(c0, 1);
    dc[0] = (int16_t)_mm_extract_epi16(lo, 0);
    dc[1] = (int16_t)_mm_extract_epi16(lo, 1);
    dc[2] = (int16_t)_mm_extract_epi16(hi, 0);
    dc[3] = (int16_t)_mm_extract_epi16(hi, 1);
    return _mm256_madd_epi16(_mm256_add_epi16(_mm256_add_epi16(a, b),
                                              _mm256_add_epi16(c, e)), one);
}

long y264_texture_ac4_16x16_avx2(const pixel *p, int stride)
{
    __m256i acc = _mm256_setzero_si256();
    long corr = 0;
    for (int by = 0; by < 16; by += 4) {
        int dc[4];
        acc = _mm256_add_epi32(acc, tex_group16(p + by * stride, stride, dc));
        for (int k = 0; k < 4; k++)
            corr += m10_tex_corr(dc[k]);
    }
    return (long)m10_hsum32_256(acc) + corr;
}

void y264_texture_ac48_16x16_avx2(const pixel *p, int stride, long out[2])
{
    __m256i a4 = _mm256_setzero_si256();
    long corr = 0, e8 = 0;
    for (int qy = 0; qy < 16; qy += 8) {
        const pixel *q = p + qy * stride;
        __m256i d[8], s = _mm256_setzero_si256();
        int h0, h1;
        for (int i = 0; i < 8; i++) {
            d[i] = m10_ld16(q + i * stride);
            s = _mm256_add_epi32(s, _mm256_madd_epi16(d[i], _mm256_set1_epi16(1)));
        }
        had8x8_half_pair(d, &h0, &h1);
        e8 += 2 * (long)h0 - m10_hsum32(_mm256_castsi256_si128(s))
            + 2 * (long)h1 - m10_hsum32(_mm256_extracti128_si256(s, 1));
        for (int i = 0; i < 8; i += 4) {
            int dc[4];
            a4 = _mm256_add_epi32(a4, tex_group16(q + i * stride, stride, dc));
            for (int k = 0; k < 4; k++)
                corr += m10_tex_corr(dc[k]);
        }
    }
    out[0] = (long)m10_hsum32_256(a4) + corr;
    out[1] = e8;
}

/* ---- variance: a sum lane takes a sample a row (<= 16368) ---------------- */

void y264_var_16x16_avx2(const pixel *p, int stride, uint32_t out[2])
{
    __m256i s1 = _mm256_setzero_si256(), s2 = s1;
    for (int y = 0; y < 16; y++, p += stride) {
        __m256i v = m10_ld16(p);
        s1 = _mm256_add_epi16(s1, v);
        s2 = _mm256_add_epi32(s2, _mm256_madd_epi16(v, v));
    }
    out[0] = (uint32_t)m10_hsum16_256(s1);
    out[1] = (uint32_t)m10_hsum32_256(s2);
}

/* ---- the fused intra costs ----------------------------------------------- */

/* two 4x4 tiles, one per lane: m10_satd_tile4 at twice the width */
static inline void satd_tile4_pair(__m256i d01, __m256i d23, int *lo, int *hi)
{
    __m256i w01 = _mm256_shuffle_epi32(d01, 0x4e);
    __m256i w23 = _mm256_shuffle_epi32(d23, 0x4e);
    __m256i t01 = _mm256_unpacklo_epi64(_mm256_add_epi16(d01, w01),
                                        _mm256_sub_epi16(d01, w01));
    __m256i t23 = _mm256_unpacklo_epi64(_mm256_add_epi16(d23, w23),
                                        _mm256_sub_epi16(d23, w23));
    __m256i x = _mm256_add_epi16(t01, t23), y = _mm256_sub_epi16(t01, t23);
    __m256i h = _mm256_hadd_epi16(x, y), g = _mm256_hsub_epi16(x, y);
    __m256i a = _mm256_abs_epi16(_mm256_hadd_epi16(h, g));
    __m256i b = _mm256_abs_epi16(_mm256_hsub_epi16(h, g));
    __m256i s = _mm256_madd_epi16(_mm256_add_epi16(a, b), _mm256_set1_epi16(1));
    *lo = m10_hsum32(_mm256_castsi256_si128(s));
    *hi = m10_hsum32(_mm256_extracti128_si256(s, 1));
}

void y264_intra4_mode_search_avx2(const pixel *src, int ss, const pixel *rec, int rs,
                                  int ht, int hl, int htl, int htr, int costs[9])
{
    __m128i p01[9], p23[9];
    __m128i s01 = m10_rows2x4(src, ss), s23 = m10_rows2x4(src + 2 * ss, ss);
    __m256i w01 = m10_2x128(s01, s01), w23 = m10_2x128(s23, s23);
    m10_i4_preds(p01, p23, rec, rs, ht, hl, htl, htr);
    /* modes in pairs, one per lane; the ninth takes the narrow form */
    for (int m = 0; m < 8; m += 2)
        satd_tile4_pair(_mm256_sub_epi16(m10_2x128(p01[m], p01[m + 1]), w01),
                        _mm256_sub_epi16(m10_2x128(p23[m], p23[m + 1]), w23),
                        &costs[m], &costs[m + 1]);
    costs[8] = m10_hsum32(m10_satd_tile4(_mm_sub_epi16(p01[8], s01),
                                         _mm_sub_epi16(p23[8], s23)));
}

void y264_intra16_satd_trio_avx2(const pixel *src, int ss, const pixel *top,
                                 const pixel *left, int dc, int costs[3])
{
    __m256i vt = m10_ld16(top), vd = _mm256_set1_epi16((short)dc);
    __m256i av = _mm256_setzero_si256(), ah = av, ad = av;
    for (int y = 0; y < 16; y += 4) {
        __m256i s[4], hb[4];
        for (int i = 0; i < 4; i++) {
            s[i] = m10_ld16(src + (y + i) * ss);
            hb[i] = _mm256_set1_epi16((short)left[y + i]);
        }
        av = _mm256_add_epi32(av, satd_rows16(_mm256_sub_epi16(s[0], vt),
                                              _mm256_sub_epi16(s[1], vt),
                                              _mm256_sub_epi16(s[2], vt),
                                              _mm256_sub_epi16(s[3], vt)));
        ah = _mm256_add_epi32(ah, satd_rows16(_mm256_sub_epi16(s[0], hb[0]),
                                              _mm256_sub_epi16(s[1], hb[1]),
                                              _mm256_sub_epi16(s[2], hb[2]),
                                              _mm256_sub_epi16(s[3], hb[3])));
        ad = _mm256_add_epi32(ad, satd_rows16(_mm256_sub_epi16(s[0], vd),
                                              _mm256_sub_epi16(s[1], vd),
                                              _mm256_sub_epi16(s[2], vd),
                                              _mm256_sub_epi16(s[3], vd)));
    }
    costs[0] = m10_hsum32_256(av);
    costs[1] = m10_hsum32_256(ah);
    costs[2] = m10_hsum32_256(ad);
}

/* ---- SSD and the per-4x4 maps (bounds as in metrics_x86_10.h) ----------- */

int y264_ssd_16xh_avx2(const pixel *a, int as, const pixel *b, int bs, int h)
{
    __m256i acc = _mm256_setzero_si256();
    for (int y = 0; y < h; y++) {
        __m256i d = _mm256_sub_epi16(m10_ld16(a + y * as), m10_ld16(b + y * bs));
        acc = _mm256_add_epi32(acc, _mm256_madd_epi16(d, d));
    }
    return m10_hsum32_256(acc);
}

int y264_ssd_8xh_avx2(const pixel *a, int as, const pixel *b, int bs, int h)
{
    __m256i acc = _mm256_setzero_si256();
    int y = 0;
    for (; y + 2 <= h; y += 2) {
        __m256i d = _mm256_sub_epi16(m10_fold8(a + y * as, as), m10_fold8(b + y * bs, bs));
        acc = _mm256_add_epi32(acc, _mm256_madd_epi16(d, d));
    }
    int s = m10_hsum32_256(acc);
    /* every caller's height is even; an odd tail is not dropped */
    if (y < h)
        s += m10_ssd_8xh(a + y * as, as, b + y * bs, bs, h - y);
    return s;
}

/* A 16-wide row's pair sums already sit two blocks per lane; one hadd of the
 * squares against the widened sums leaves (e, e, s, s) per lane, and a 64-bit
 * permute orders them for the two stores. */
void y264_ssdsum4x4_16_avx2(const pixel *a, int as, const pixel *b, int bs,
                            uint32_t e[16], int32_t sum[16])
{
    const __m256i one = _mm256_set1_epi16(1);
    for (int r = 0; r < 4; r++) {
        __m256i ea = _mm256_setzero_si256(), sa = ea;
        for (int y = 0; y < 4; y++) {
            __m256i d = _mm256_sub_epi16(m10_ld16(a + (4 * r + y) * as),
                                         m10_ld16(b + (4 * r + y) * bs));
            ea = _mm256_add_epi32(ea, _mm256_madd_epi16(d, d));
            sa = _mm256_add_epi16(sa, d);
        }
        __m256i v = _mm256_permute4x64_epi64(
            _mm256_hadd_epi32(ea, _mm256_madd_epi16(sa, one)), 0xd8);
        _mm_storeu_si128((__m128i *)(void *)(e + 4 * r), _mm256_castsi256_si128(v));
        _mm_storeu_si128((__m128i *)(void *)(sum + 4 * r), _mm256_extracti128_si256(v, 1));
    }
}

/* rows 0-3 in the low lane and 4-7 in the high one */
void y264_ssdsum4x4_8_avx2(const pixel *a, int as, const pixel *b, int bs,
                           uint32_t e[4], int32_t sum[4])
{
    __m256i ea = _mm256_setzero_si256(), sa = ea;
    for (int y = 0; y < 4; y++) {
        __m256i d = _mm256_sub_epi16(m10_fold8(a + y * as, 4 * as),
                                     m10_fold8(b + y * bs, 4 * bs));
        ea = _mm256_add_epi32(ea, _mm256_madd_epi16(d, d));
        sa = _mm256_add_epi16(sa, d);
    }
    __m256i v = _mm256_permute4x64_epi64(
        _mm256_hadd_epi32(ea, _mm256_madd_epi16(sa, _mm256_set1_epi16(1))), 0xd8);
    _mm_storeu_si128((__m128i *)(void *)e, _mm256_castsi256_si128(v));
    _mm_storeu_si128((__m128i *)(void *)sum, _mm256_extracti128_si256(v, 1));
}

/* two block rows per pass: the hadd of their pair sums leaves (upper, upper,
 * lower, lower) per lane, and the permute orders eight map entries */
void y264_ssd4x4_map16_avx2(const pixel *a, int as, const pixel *b, int bs,
                            uint32_t out[16])
{
    for (int r = 0; r < 4; r += 2) {
        __m256i e0 = _mm256_setzero_si256(), e1 = e0;
        for (int y = 0; y < 4; y++) {
            __m256i d0 = _mm256_sub_epi16(m10_ld16(a + (4 * r + y) * as),
                                          m10_ld16(b + (4 * r + y) * bs));
            __m256i d1 = _mm256_sub_epi16(m10_ld16(a + (4 * r + 4 + y) * as),
                                          m10_ld16(b + (4 * r + 4 + y) * bs));
            e0 = _mm256_add_epi32(e0, _mm256_madd_epi16(d0, d0));
            e1 = _mm256_add_epi32(e1, _mm256_madd_epi16(d1, d1));
        }
        _mm256_storeu_si256((__m256i *)(void *)(out + 4 * r),
                            _mm256_permute4x64_epi64(_mm256_hadd_epi32(e0, e1), 0xd8));
    }
}

#endif /* Y264_HAVE_AVX2_10 */
