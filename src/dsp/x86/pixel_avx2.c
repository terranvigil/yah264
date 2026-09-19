/*
 * pixel_avx2.c - AVX2 kernels for the pixel family
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Wave 1 of docs/x86-plan.md, the wide half. Same kernels as the SSE4.2 file
 * and the same values out of them, bit for bit: the two tiers are an A/B axis
 * for speed and never for output, which is what the kit's identity leg exists
 * to hold them to.
 *
 * WHAT A SECOND LANE IS WORTH HERE, AND WHEN IT IS WORTH NOTHING.
 *
 * Every 256-bit shuffle this family needs -- hadd, hsub, the 32-bit and 16-bit
 * shuffles, unpack, blend -- is TWO INDEPENDENT 128-BIT HALVES, with no lane
 * crossing at all. That is a gift rather than a limitation: it means the
 * 128-bit sequences in pixel_x86.h lift to 256 bits unchanged, and what the
 * upper lane holds is simply a second, independent block. So the wide kernels
 * here differ from the narrow ones only in what is PACKED into the register:
 *
 *   - a 16-wide row becomes one register and yields four 4x4 tiles per pass
 *     instead of two (SATD 16x16, the I16x16 costs, the psy 4x4 term);
 *   - an 8x8 block becomes rows 0-3 in the low lane and rows 4-7 in the high
 *     one, so its four tiles fall out of a single pass (SATD 8x8);
 *   - an 8x8 Walsh-Hadamard runs on TWO blocks at once, one per lane, which is
 *     what the 16x16 SA8D and the psy 8x8 term are made of.
 *
 * Where a shape has no second half to offer -- a lone 4x4 SATD, a lone 8x8
 * transform -- this file calls the shared 128-bit helper rather than pretend.
 * Compiled here it is VEX-encoded and gets the three-operand form and the free
 * upper-zeroing, and that is the whole of what AVX2 has to give those shapes.
 * A wide kernel that only moved zeros through its top lane would cost a
 * vzeroupper and buy nothing.
 *
 * C11 intrinsics only. There is no assembly in this tree, and hygiene_check
 * refuses any inline form under this directory.
 */
#include "dsp/x86/pixel_x86.h"

#if Y264_HAVE_AVX2

/* ---- 256-bit plumbing ---------------------------------------------------- */

static inline __m256i y256_2x128(__m128i lo, __m128i hi)
{
    return _mm256_inserti128_si256(_mm256_castsi128_si256(lo), hi, 1);
}

static inline __m128i y256_fold(__m256i v)
{
    return _mm_add_epi32(_mm256_castsi256_si128(v),
                         _mm256_extracti128_si256(v, 1));
}

static inline int y256_hsum32(__m256i v)
{
    return y264_hsum_epi32(y256_fold(v));
}

static inline int y256_hsum_sad(__m256i v)
{
    return y264_hsum_sad(y256_fold(v));
}

/* Two rows of sixteen, or four rows of eight, as one register. */
static inline __m256i y256_rows2x16(const uint8_t *p, int stride)
{
    return y256_2x128(y264_ld128(p), y264_ld128(p + stride));
}

static inline __m256i y256_rows4x8(const uint8_t *p, int stride)
{
    return y256_2x128(_mm_unpacklo_epi64(y264_ld64(p),
                                         y264_ld64(p + stride)),
                      _mm_unpacklo_epi64(y264_ld64(p + 2 * stride),
                                         y264_ld64(p + 3 * stride)));
}

/* ---- SAD ----------------------------------------------------------------- */

#define Y264_SAD16_AVX2(h)                                                     \
int y264_sad_16x##h##_avx2(const pixel *a, int as, const pixel *b, int bs)     \
{                                                                              \
    __m256i acc = _mm256_setzero_si256();                                      \
    for (int y = 0; y < (h); y += 2)                                           \
        acc = _mm256_add_epi32(acc,                                            \
            _mm256_sad_epu8(y256_rows2x16(a + y * as, as),                     \
                            y256_rows2x16(b + y * bs, bs)));                   \
    return y256_hsum_sad(acc);                                                 \
}
Y264_SAD16_AVX2(16)
Y264_SAD16_AVX2(8)

#define Y264_SAD8_AVX2(h)                                                      \
int y264_sad_8x##h##_avx2(const pixel *a, int as, const pixel *b, int bs)      \
{                                                                              \
    __m256i acc = _mm256_setzero_si256();                                      \
    for (int y = 0; y < (h); y += 4)                                           \
        acc = _mm256_add_epi32(acc,                                            \
            _mm256_sad_epu8(y256_rows4x8(a + y * as, as),                      \
                            y256_rows4x8(b + y * bs, bs)));                    \
    return y256_hsum_sad(acc);                                                 \
}
Y264_SAD8_AVX2(16)
Y264_SAD8_AVX2(8)

#define Y264_SADX4_16_AVX2(h)                                                  \
void y264_sad_x4_16x##h##_avx2(const pixel *src, int ss, const pixel *r0,      \
                               const pixel *r1, const pixel *r2,               \
                               const pixel *r3, int rs, int scores[4])         \
{                                                                              \
    __m256i a0 = _mm256_setzero_si256(), a1 = a0, a2 = a0, a3 = a0;            \
    for (int y = 0; y < (h); y += 2) {                                         \
        __m256i s = y256_rows2x16(src + y * ss, ss);                           \
        a0 = _mm256_add_epi32(a0, _mm256_sad_epu8(s, y256_rows2x16(r0 + y * rs, rs))); \
        a1 = _mm256_add_epi32(a1, _mm256_sad_epu8(s, y256_rows2x16(r1 + y * rs, rs))); \
        a2 = _mm256_add_epi32(a2, _mm256_sad_epu8(s, y256_rows2x16(r2 + y * rs, rs))); \
        a3 = _mm256_add_epi32(a3, _mm256_sad_epu8(s, y256_rows2x16(r3 + y * rs, rs))); \
    }                                                                          \
    scores[0] = y256_hsum_sad(a0); scores[1] = y256_hsum_sad(a1);              \
    scores[2] = y256_hsum_sad(a2); scores[3] = y256_hsum_sad(a3);              \
}
Y264_SADX4_16_AVX2(16)
Y264_SADX4_16_AVX2(8)

/* Four rows of an 8-wide candidate fill one register, so a 8x4 batch is one
 * iteration and an 8x16 is four. */
#define Y264_SADX4_8_AVX2(h)                                                   \
void y264_sad_x4_8x##h##_avx2(const pixel *src, int ss, const pixel *r0,       \
                              const pixel *r1, const pixel *r2,                \
                              const pixel *r3, int rs, int scores[4])          \
{                                                                              \
    __m256i a0 = _mm256_setzero_si256(), a1 = a0, a2 = a0, a3 = a0;            \
    for (int y = 0; y < (h); y += 4) {                                         \
        __m256i s = y256_rows4x8(src + y * ss, ss);                            \
        a0 = _mm256_add_epi32(a0, _mm256_sad_epu8(s, y256_rows4x8(r0 + y * rs, rs))); \
        a1 = _mm256_add_epi32(a1, _mm256_sad_epu8(s, y256_rows4x8(r1 + y * rs, rs))); \
        a2 = _mm256_add_epi32(a2, _mm256_sad_epu8(s, y256_rows4x8(r2 + y * rs, rs))); \
        a3 = _mm256_add_epi32(a3, _mm256_sad_epu8(s, y256_rows4x8(r3 + y * rs, rs))); \
    }                                                                          \
    scores[0] = y256_hsum_sad(a0); scores[1] = y256_hsum_sad(a1);              \
    scores[2] = y256_hsum_sad(a2); scores[3] = y256_hsum_sad(a3);              \
}
Y264_SADX4_8_AVX2(16)
Y264_SADX4_8_AVX2(8)
Y264_SADX4_8_AVX2(4)

/* ---- the SATD core, four tiles at a time --------------------------------- */

/* Exactly y264_satd_rows8 with the registers twice as wide. Every operation in
 * it is per-128-bit-lane, so the upper lane carries two more independent 4x4
 * tiles and nothing in the sequence has to change. */
static inline __m256i satd_rows16(__m256i d0, __m256i d1,
                                  __m256i d2, __m256i d3)
{
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
    __m256i s = _mm256_add_epi16(_mm256_add_epi16(a, b), _mm256_add_epi16(c, d));
    return _mm256_madd_epi16(s, _mm256_set1_epi16(1));
}

/* Four rows of sixteen columns as residual: four 4x4 tiles. */
static inline __m256i satd_group16(const uint8_t *a, int as,
                                   const uint8_t *b, int bs)
{
    __m256i d[4];
    for (int i = 0; i < 4; i++)
        d[i] = _mm256_sub_epi16(
            _mm256_cvtepu8_epi16(y264_ld128(a + i * as)),
            _mm256_cvtepu8_epi16(y264_ld128(b + i * bs)));
    return satd_rows16(d[0], d[1], d[2], d[3]);
}

int y264_satd_4x4_avx2(const pixel *a, int as, const pixel *b, int bs)
{
    /* Nothing to put in the second lane: one 4x4 is one tile. */
    return y264_satd_4x4_x86(a, as, b, bs);
}

/* An 8x8 is four tiles, and they fit one register when the block is folded at
 * its own half: rows 0-3 in the low lane, rows 4-7 in the high one. The
 * butterflies never cross a lane, so the fold is free. */
int y264_satd_8x8_avx2(const pixel *a, int as, const pixel *b, int bs)
{
    __m256i d[4];
    for (int i = 0; i < 4; i++) {
        __m256i sa = y256_2x128(_mm_cvtepu8_epi16(y264_ld64(a + i * as)),
                                _mm_cvtepu8_epi16(y264_ld64(a + (i + 4) * as)));
        __m256i sb = y256_2x128(_mm_cvtepu8_epi16(y264_ld64(b + i * bs)),
                                _mm_cvtepu8_epi16(y264_ld64(b + (i + 4) * bs)));
        d[i] = _mm256_sub_epi16(sa, sb);
    }
    return y256_hsum32(satd_rows16(d[0], d[1], d[2], d[3]));
}

int y264_satd_16x16_avx2(const pixel *a, int as, const pixel *b, int bs)
{
    __m256i acc = _mm256_setzero_si256();
    for (int y = 0; y < 16; y += 4)
        acc = _mm256_add_epi32(acc, satd_group16(a + y * as, as, b + y * bs, bs));
    return y256_hsum32(acc);
}

void y264_satd_x4_8x8_avx2(const pixel *src, int ss, const pixel *r0,
                           const pixel *r1, const pixel *r2, const pixel *r3,
                           int rs, int scores[4])
{
    const pixel *const r[4] = { r0, r1, r2, r3 };
    for (int k = 0; k < 4; k++)
        scores[k] = y264_satd_8x8_avx2(src, ss, r[k], rs);
}

/* ---- the 8x8 Walsh-Hadamard, two blocks at a time ------------------------ */

/* The 128-bit row transform, lane for lane. */
static inline void had8_row256(__m256i v, __m256i *sum, __m256i *dif)
{
    __m256i s4 = _mm256_shuffle_epi32(v, 0x4e);
    __m256i a = _mm256_unpacklo_epi64(_mm256_add_epi16(v, s4),
                                      _mm256_sub_epi16(v, s4));
    __m256i s2 = _mm256_shufflehi_epi16(_mm256_shufflelo_epi16(a, 0x4e), 0x4e);
    __m256i p = _mm256_add_epi16(a, s2), q = _mm256_sub_epi16(a, s2);
    __m256i qs = _mm256_shufflehi_epi16(_mm256_shufflelo_epi16(q, 0x4e), 0x4e);
    __m256i b = _mm256_blend_epi16(p, qs, 0xcc);
    *sum = _mm256_hadd_epi16(b, b);
    *dif = _mm256_hsub_epi16(b, b);
}

/* Sum of the 64 coefficient magnitudes for EACH lane's 8x8, kept apart: the
 * SA8D rounds per block and the psy term subtracts a per-quadrant DC, so the
 * two answers must not be folded together. */
static inline void had8x8_absum_pair(const __m256i d[8], int *lo, int *hi)
{
    __m256i c[8];
    __m256i acc = _mm256_setzero_si256();
    {
        __m256i a0 = _mm256_add_epi16(d[0], d[4]);
        __m256i a1 = _mm256_add_epi16(d[1], d[5]);
        __m256i a2 = _mm256_add_epi16(d[2], d[6]);
        __m256i a3 = _mm256_add_epi16(d[3], d[7]);
        __m256i a4 = _mm256_sub_epi16(d[0], d[4]);
        __m256i a5 = _mm256_sub_epi16(d[1], d[5]);
        __m256i a6 = _mm256_sub_epi16(d[2], d[6]);
        __m256i a7 = _mm256_sub_epi16(d[3], d[7]);
        __m256i b0 = _mm256_add_epi16(a0, a2), b1 = _mm256_add_epi16(a1, a3);
        __m256i b2 = _mm256_sub_epi16(a0, a2), b3 = _mm256_sub_epi16(a1, a3);
        __m256i b4 = _mm256_add_epi16(a4, a6), b5 = _mm256_add_epi16(a5, a7);
        __m256i b6 = _mm256_sub_epi16(a4, a6), b7 = _mm256_sub_epi16(a5, a7);
        c[0] = _mm256_add_epi16(b0, b1); c[1] = _mm256_sub_epi16(b0, b1);
        c[2] = _mm256_add_epi16(b2, b3); c[3] = _mm256_sub_epi16(b2, b3);
        c[4] = _mm256_add_epi16(b4, b5); c[5] = _mm256_sub_epi16(b4, b5);
        c[6] = _mm256_add_epi16(b6, b7); c[7] = _mm256_sub_epi16(b6, b7);
    }
    for (int i = 0; i < 8; i++) {
        __m256i s, f;
        had8_row256(c[i], &s, &f);
        acc = _mm256_add_epi32(acc, _mm256_madd_epi16(
            _mm256_abs_epi16(_mm256_unpacklo_epi64(s, f)),
            _mm256_set1_epi16(1)));
    }
    *lo = y264_hsum_epi32(_mm256_castsi256_si128(acc));
    *hi = y264_hsum_epi32(_mm256_extracti128_si256(acc, 1));
}

/* Two 8x8 residual blocks, one per lane. */
static inline void load8x8_pair_diff(__m256i d[8], const uint8_t *a0,
                                     const uint8_t *a1, int as,
                                     const uint8_t *b0, const uint8_t *b1,
                                     int bs)
{
    for (int i = 0; i < 8; i++) {
        __m256i va = y256_2x128(_mm_cvtepu8_epi16(y264_ld64(a0 + i * as)),
                                _mm_cvtepu8_epi16(y264_ld64(a1 + i * as)));
        __m256i vb = y256_2x128(_mm_cvtepu8_epi16(y264_ld64(b0 + i * bs)),
                                _mm_cvtepu8_epi16(y264_ld64(b1 + i * bs)));
        d[i] = _mm256_sub_epi16(va, vb);
    }
}

static inline void load8x8_pair(__m256i d[8], const uint8_t *p0,
                                const uint8_t *p1, int stride)
{
    for (int i = 0; i < 8; i++)
        d[i] = y256_2x128(_mm_cvtepu8_epi16(y264_ld64(p0 + i * stride)),
                          _mm_cvtepu8_epi16(y264_ld64(p1 + i * stride)));
}

int y264_sa8d_8x8_avx2(const pixel *a, int as, const pixel *b, int bs)
{
    /* One block, one lane's worth of work: the shared 128-bit form, here in
     * its VEX encoding. */
    __m128i d[8];
    y264_load8x8_diff(d, a, as, b, bs);
    return (y264_had8x8_absum(d) + 2) >> 2;
}

int y264_sa8d_16x16_avx2(const pixel *a, int as, const pixel *b, int bs)
{
    __m256i d[8];
    int q0, q1, q2, q3;
    load8x8_pair_diff(d, a, a + 8, as, b, b + 8, bs);
    had8x8_absum_pair(d, &q0, &q1);
    load8x8_pair_diff(d, a + 8 * as, a + 8 * as + 8, as,
                      b + 8 * bs, b + 8 * bs + 8, bs);
    had8x8_absum_pair(d, &q2, &q3);
    /* Each quadrant carries the reference's own rounding. */
    return ((q0 + 2) >> 2) + ((q1 + 2) >> 2)
         + ((q2 + 2) >> 2) + ((q3 + 2) >> 2);
}

static inline int sum8x8_avx2(const uint8_t *p, int stride)
{
    __m256i acc = _mm256_setzero_si256();
    for (int y = 0; y < 8; y += 4)
        acc = _mm256_add_epi32(acc, _mm256_sad_epu8(y256_rows4x8(p + y * stride,
                                                                 stride),
                                                    _mm256_setzero_si256()));
    return y256_hsum_sad(acc);
}

long y264_hadamard_ac_8x8_avx2(const pixel *p, int stride)
{
    __m128i d[8];
    y264_load8x8(d, p, stride);
    return (long)y264_had8x8_absum(d) - (long)sum8x8_avx2(p, stride);
}

/* ---- the psy texture terms ----------------------------------------------- */

/* Four rows of sixteen as four 4x4 tiles, with the four tile DCs handed back.
 * The DCs are lanes 0 and 1 of each 128-bit half of the first hadd pair. */
static inline __m256i tex_group16(const uint8_t *p, int stride, int dc[4])
{
    __m256i d[4];
    for (int i = 0; i < 4; i++)
        d[i] = _mm256_cvtepu8_epi16(y264_ld128(p + i * stride));
    {
        __m256i t0 = _mm256_add_epi16(d[0], d[1]), t1 = _mm256_sub_epi16(d[0], d[1]);
        __m256i t2 = _mm256_add_epi16(d[2], d[3]), t3 = _mm256_sub_epi16(d[2], d[3]);
        __m256i o0 = _mm256_add_epi16(t0, t2), o2 = _mm256_sub_epi16(t0, t2);
        __m256i o1 = _mm256_add_epi16(t1, t3), o3 = _mm256_sub_epi16(t1, t3);
        __m256i h01 = _mm256_hadd_epi16(o0, o1), g01 = _mm256_hsub_epi16(o0, o1);
        __m256i h23 = _mm256_hadd_epi16(o2, o3), g23 = _mm256_hsub_epi16(o2, o3);
        __m256i c0 = _mm256_hadd_epi16(h01, g01);
        __m256i a = _mm256_abs_epi16(c0);
        __m256i b = _mm256_abs_epi16(_mm256_hsub_epi16(h01, g01));
        __m256i c = _mm256_abs_epi16(_mm256_hadd_epi16(h23, g23));
        __m256i e = _mm256_abs_epi16(_mm256_hsub_epi16(h23, g23));
        __m256i s = _mm256_add_epi16(_mm256_add_epi16(a, b), _mm256_add_epi16(c, e));
        __m128i lo = _mm256_castsi256_si128(c0);
        __m128i hi = _mm256_extracti128_si256(c0, 1);
        dc[0] = (int16_t)_mm_extract_epi16(lo, 0);
        dc[1] = (int16_t)_mm_extract_epi16(lo, 1);
        dc[2] = (int16_t)_mm_extract_epi16(hi, 0);
        dc[3] = (int16_t)_mm_extract_epi16(hi, 1);
        return _mm256_madd_epi16(s, _mm256_set1_epi16(1));
    }
}

static inline long tex_corr(int dc)
{
    int flat = 16 * ((dc + 8) >> 4);
    return (long)(dc < flat ? flat - dc : dc - flat) - (long)dc;
}

long y264_texture_ac4_16x16_avx2(const pixel *p, int stride)
{
    __m256i acc = _mm256_setzero_si256();
    long corr = 0;
    for (int by = 0; by < 16; by += 4) {
        int dc[4];
        acc = _mm256_add_epi32(acc, tex_group16(p + by * stride, stride, dc));
        for (int k = 0; k < 4; k++)
            corr += tex_corr(dc[k]);
    }
    return (long)y256_hsum32(acc) + corr;
}

void y264_texture_ac48_16x16_avx2(const pixel *p, int stride, long out[2])
{
    __m256i a4 = _mm256_setzero_si256();
    __m256i d[8];
    long corr = 0, e8 = 0;
    int q0, q1;

    /* The SA8D-support term takes the two quadrants of a row band together,
     * one per lane; the 4x4-support term takes the band's four tile rows
     * sixteen columns at a time. */
    for (int qy = 0; qy < 16; qy += 8) {
        const uint8_t *q = p + qy * stride;
        load8x8_pair(d, q, q + 8, stride);
        had8x8_absum_pair(d, &q0, &q1);
        e8 += (long)q0 - (long)sum8x8_avx2(q, stride)
            + (long)q1 - (long)sum8x8_avx2(q + 8, stride);
        for (int i = 0; i < 8; i += 4) {
            int dc[4];
            a4 = _mm256_add_epi32(a4, tex_group16(q + i * stride, stride, dc));
            for (int k = 0; k < 4; k++)
                corr += tex_corr(dc[k]);
        }
    }
    out[0] = (long)y256_hsum32(a4) + corr;
    out[1] = e8;
}

/* ---- variance ------------------------------------------------------------ */

void y264_var_16x16_avx2(const pixel *p, int stride, uint32_t out[2])
{
    const __m256i z = _mm256_setzero_si256();
    __m256i s1 = z, s2 = z;
    for (int y = 0; y < 16; y += 2) {
        __m256i v = y256_rows2x16(p + y * stride, stride);
        __m256i lo = _mm256_unpacklo_epi8(v, z), hi = _mm256_unpackhi_epi8(v, z);
        s1 = _mm256_add_epi32(s1, _mm256_sad_epu8(v, z));
        s2 = _mm256_add_epi32(s2, _mm256_add_epi32(_mm256_madd_epi16(lo, lo),
                                                   _mm256_madd_epi16(hi, hi)));
    }
    out[0] = (uint32_t)y256_hsum_sad(s1);
    out[1] = (uint32_t)y256_hsum32(s2);
}

/* ---- the fused intra costs ----------------------------------------------- */

/* Two 4x4 tiles, one per lane: y264_satd_tile4 at twice the width. */
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
    *lo = y264_hsum_epi32(_mm256_castsi256_si128(s));
    *hi = y264_hsum_epi32(_mm256_extracti128_si256(s, 1));
}

void y264_intra4x4_x9_avx2(const pixel *src, int ss, const pixel *rec, int rs,
                           int ht, int hl, int htl, int htr, int costs[9])
{
    __m128i tab[4], pr[9];
    __m128i s01 = y264_rows2x4(src, ss), s23 = y264_rows2x4(src + 2 * ss, ss);
    __m256i ss01 = y256_2x128(s01, s01), ss23 = y256_2x128(s23, s23);

    y264_i4_edge_tab(tab, rec, rs, ht, hl, htl, htr);
    for (int m = 0; m < 9; m++)
        pr[m] = y264_tbl64(tab, y264_ld128(y264_i4_x9_idx[m]));

    /* The modes go through in pairs, one per lane, against the same source in
     * both. The ninth has no partner and takes the narrow form. */
    for (int m = 0; m < 8; m += 2) {
        __m256i p01 = y256_2x128(_mm_cvtepu8_epi16(pr[m]),
                                 _mm_cvtepu8_epi16(pr[m + 1]));
        __m256i p23 = y256_2x128(_mm_cvtepu8_epi16(_mm_srli_si128(pr[m], 8)),
                                 _mm_cvtepu8_epi16(_mm_srli_si128(pr[m + 1], 8)));
        satd_tile4_pair(_mm256_sub_epi16(p01, ss01),
                        _mm256_sub_epi16(p23, ss23), &costs[m], &costs[m + 1]);
    }
    costs[8] = y264_hsum_epi32(y264_satd_tile4(
        _mm_sub_epi16(_mm_cvtepu8_epi16(pr[8]), s01),
        _mm_sub_epi16(_mm_cvtepu8_epi16(_mm_srli_si128(pr[8], 8)), s23)));
}

/* All three I16x16 predictions are broadcasts, so one pass of source loads
 * forms all three residuals in registers; a whole 16-wide row is one register
 * here, which halves the passes the SSE4.2 twin needs. */
void y264_intra_satd_x3_16x16_avx2(const pixel *src, int ss, const pixel *top,
                                   const pixel *left, int dc, int costs[3])
{
    const __m256i z = _mm256_setzero_si256();
    __m256i vt = _mm256_cvtepu8_epi16(y264_ld128(top));
    __m256i vd = _mm256_set1_epi16((short)dc);
    __m256i av = z, ah = z, ad = z;

    for (int y = 0; y < 16; y += 4) {
        __m256i s[4], hb[4];
        for (int i = 0; i < 4; i++) {
            s[i] = _mm256_cvtepu8_epi16(y264_ld128(src + (y + i) * ss));
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
    costs[0] = y256_hsum32(av);
    costs[1] = y256_hsum32(ah);
    costs[2] = y256_hsum32(ad);
}

/* ---- SSD ----------------------------------------------------------------- */

int y264_ssd_16xh_avx2(const uint8_t *a, int as, const uint8_t *b, int bs, int h)
{
    __m256i acc = _mm256_setzero_si256();
    for (int y = 0; y < h; y++) {
        __m256i d = _mm256_sub_epi16(
            _mm256_cvtepu8_epi16(y264_ld128(a + y * as)),
            _mm256_cvtepu8_epi16(y264_ld128(b + y * bs)));
        acc = _mm256_add_epi32(acc, _mm256_madd_epi16(d, d));
    }
    return y256_hsum32(acc);
}

int y264_ssd_8xh_avx2(const uint8_t *a, int as, const uint8_t *b, int bs, int h)
{
    __m256i acc = _mm256_setzero_si256();
    int y = 0;
    for (; y + 2 <= h; y += 2) {
        __m256i va = y256_2x128(_mm_cvtepu8_epi16(y264_ld64(a + y * as)),
                                _mm_cvtepu8_epi16(y264_ld64(a + (y + 1) * as)));
        __m256i vb = y256_2x128(_mm_cvtepu8_epi16(y264_ld64(b + y * bs)),
                                _mm_cvtepu8_epi16(y264_ld64(b + (y + 1) * bs)));
        __m256i d = _mm256_sub_epi16(va, vb);
        acc = _mm256_add_epi32(acc, _mm256_madd_epi16(d, d));
    }
    {
        int s = y256_hsum32(acc);
        /* The caller's heights are all even today; the odd tail is here so a
         * future one does not silently drop a row. */
        for (; y < h; y++) {
            __m128i d = _mm_sub_epi16(_mm_cvtepu8_epi16(y264_ld64(a + y * as)),
                                      _mm_cvtepu8_epi16(y264_ld64(b + y * bs)));
            s += y264_hsum_epi32(_mm_madd_epi16(d, d));
        }
        return s;
    }
}

#endif /* Y264_HAVE_AVX2 */
