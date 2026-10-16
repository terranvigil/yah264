/*
 * pixel_avx512.c - AVX-512 kernels for the pixel family
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Built by default (-Davx512=false leaves it out), and only for the shapes where
 * a 512-bit register removes instructions rather than moving zeros. Every
 * kernel returns exactly what its C reference and its AVX2 twin return.
 *
 * WHAT THE WIDER REGISTER BUYS, AND WHAT IT DOES NOT.
 *
 * A zmm holds four 16-byte rows, so a 16-wide SAD takes one PSADBW per four
 * rows where AVX2 took two, and a batch of four candidates folds its four
 * accumulators into one register before the horizontal sum. The loads do not
 * shrink: each row is its own address, so a four-row register costs four
 * loads at any width. That is why the single 8x8 SATD stays on AVX2: its
 * folded 256-bit form is already one pass, and a 512-bit one would gather
 * eight 8-byte rows for the same arithmetic.
 *
 * AVX-512 HAS NO HORIZONTAL ADD, so the SATD here is a different sequence and
 * not the AVX2 one widened. Two identities replace the hadd pairs:
 *
 *   - the first horizontal stage happens in the byte domain. PMADDUBSW of the
 *     source bytes against (1, 1) gives s0 + s1 per neighbour pair and against
 *     (1, -1) gives s0 - s1; the same on the reference, subtracted, gives
 *     d0 + d1 and d0 - d1 directly, so the residual is never formed on its
 *     own. Both products stay inside 16 bits (|s0 +- s1| <= 510);
 *   - the last stage is never materialised: |x + y| + |x - y| = 2 max(|x|, |y|).
 *     A 32-bit rotate by 16 swaps each word pair, so abs, rotate and max leave
 *     the pair's max in both words, and the sum of every word is the
 *     un-halved Hadamard magnitude sum the C reference returns.
 *
 * The vertical stages run between registers, as everywhere else in this tree.
 * A 16x16 block packs rows i, i + 4, i + 8, i + 12 into the four lanes of
 * register i, so every vertical butterfly is a plain add or subtract. Word
 * sums stay below 2^15: a coefficient is at most 16 * 255 = 4080 and no word
 * sums more than eight of them before the 32-bit widening.
 *
 * C11 intrinsics only. There is no assembly in this tree, and
 * hygiene_check refuses any inline form under this directory.
 */
#include "dsp/x86/pixel_x86.h"

#if Y264_HAVE_AVX512

/* ---- loads --------------------------------------------------------------- */

/* Four sixteen-byte rows, one per 128-bit lane. */
static inline __m512i z_rows4(const uint8_t *p0, const uint8_t *p1,
                              const uint8_t *p2, const uint8_t *p3)
{
    __m512i v = _mm512_castsi128_si512(y264_ld128(p0));
    v = _mm512_inserti32x4(v, y264_ld128(p1), 1);
    v = _mm512_inserti32x4(v, y264_ld128(p2), 2);
    return _mm512_inserti32x4(v, y264_ld128(p3), 3);
}

static inline __m512i z_rows4x16(const uint8_t *p, int s)
{
    return z_rows4(p, p + s, p + 2 * s, p + 3 * s);
}

/* Two eight-byte rows as one 128-bit lane, low then high. */
static inline __m128i x_rows2x8(const uint8_t *p, const uint8_t *q)
{
    return _mm_castpd_si128(_mm_loadh_pd(_mm_castsi128_pd(y264_ld64(p)),
                                         (const double *)(const void *)q));
}

/* Eight rows of eight, two per lane: one register per 8x8 block. */
static inline __m512i z_rows8x8(const uint8_t *p, int s)
{
    __m512i v = _mm512_castsi128_si512(x_rows2x8(p, p + s));
    v = _mm512_inserti32x4(v, x_rows2x8(p + 2 * s, p + 3 * s), 1);
    v = _mm512_inserti32x4(v, x_rows2x8(p + 4 * s, p + 5 * s), 2);
    return _mm512_inserti32x4(v, x_rows2x8(p + 6 * s, p + 7 * s), 3);
}

/* ---- the SAD reductions --------------------------------------------------
 *
 * PSADBW leaves a partial sum below 2^16 in the low half of every qword. Two
 * accumulators share a register by shifting the second into the high half,
 * two such registers interleave into one, and the per-lane pair add leaves
 * (a0, a1, a2, a3) in every 128-bit lane; the lanes then fold to one xmm of
 * the four scores, in order. */
static inline __m128i z_sad_fold4(__m512i a0, __m512i a1, __m512i a2, __m512i a3)
{
    __m512i x01 = _mm512_or_si512(a0, _mm512_slli_epi64(a1, 32));
    __m512i x23 = _mm512_or_si512(a2, _mm512_slli_epi64(a3, 32));
    __m512i y = _mm512_add_epi32(_mm512_unpacklo_epi64(x01, x23),
                                 _mm512_unpackhi_epi64(x01, x23));
    __m256i h = _mm256_add_epi32(_mm512_castsi512_si256(y),
                                 _mm512_extracti64x4_epi64(y, 1));
    return _mm_add_epi32(_mm256_castsi256_si128(h),
                         _mm256_extracti128_si256(h, 1));
}

/* Two accumulators: (a0, a1) in dwords 0 and 1 of the result. */
static inline __m128i z_sad_fold2(__m512i a0, __m512i a1)
{
    __m512i x = _mm512_or_si512(a0, _mm512_slli_epi64(a1, 32));
    __m256i h = _mm256_add_epi32(_mm512_castsi512_si256(x),
                                 _mm512_extracti64x4_epi64(x, 1));
    __m128i v = _mm_add_epi32(_mm256_castsi256_si128(h),
                              _mm256_extracti128_si256(h, 1));
    return _mm_add_epi32(v, _mm_unpackhi_epi64(v, v));
}

/* Exactly three ints: the caller's array has no fourth slot. */
static inline void st_scores3(int scores[3], __m128i v)
{
    _mm_storel_epi64((__m128i *)(void *)scores, v);
    scores[2] = _mm_extract_epi32(v, 2);
}

/* ---- SAD, four, three and two candidates ---------------------------------
 *
 * Sixteen-wide: four rows per register, h / 4 passes. Eight-wide: a whole
 * 8x8 per register, h / 8 passes. One pass of source loads serves every
 * candidate, as in the AVX2 twins. */
#define Y264_SADX_AVX512(w, h, step, ROWS)                                     \
void y264_sad_x4_##w##x##h##_avx512(const pixel *src, int ss, const pixel *r0, \
                                    const pixel *r1, const pixel *r2,          \
                                    const pixel *r3, int rs, int scores[4])    \
{                                                                              \
    __m512i a0 = _mm512_setzero_si512(), a1 = a0, a2 = a0, a3 = a0;            \
    for (int y = 0; y < (h); y += (step)) {                                    \
        __m512i s = ROWS(src + y * ss, ss);                                    \
        a0 = _mm512_add_epi64(a0, _mm512_sad_epu8(s, ROWS(r0 + y * rs, rs)));  \
        a1 = _mm512_add_epi64(a1, _mm512_sad_epu8(s, ROWS(r1 + y * rs, rs)));  \
        a2 = _mm512_add_epi64(a2, _mm512_sad_epu8(s, ROWS(r2 + y * rs, rs)));  \
        a3 = _mm512_add_epi64(a3, _mm512_sad_epu8(s, ROWS(r3 + y * rs, rs)));  \
    }                                                                          \
    _mm_storeu_si128((__m128i *)(void *)scores, z_sad_fold4(a0, a1, a2, a3));  \
}                                                                              \
void y264_sad_x3_##w##x##h##_avx512(const pixel *src, int ss, const pixel *r0, \
                                    const pixel *r1, const pixel *r2, int rs,  \
                                    int scores[3])                             \
{                                                                              \
    __m512i a0 = _mm512_setzero_si512(), a1 = a0, a2 = a0;                     \
    for (int y = 0; y < (h); y += (step)) {                                    \
        __m512i s = ROWS(src + y * ss, ss);                                    \
        a0 = _mm512_add_epi64(a0, _mm512_sad_epu8(s, ROWS(r0 + y * rs, rs)));  \
        a1 = _mm512_add_epi64(a1, _mm512_sad_epu8(s, ROWS(r1 + y * rs, rs)));  \
        a2 = _mm512_add_epi64(a2, _mm512_sad_epu8(s, ROWS(r2 + y * rs, rs)));  \
    }                                                                          \
    st_scores3(scores, z_sad_fold4(a0, a1, a2, _mm512_setzero_si512()));       \
}
#define Y264_SADX2_AVX512(w, h, step, ROWS)                                    \
void y264_sad_x2_##w##x##h##_avx512(const pixel *src, int ss, const pixel *r0, \
                                    const pixel *r1, int rs, int scores[2])    \
{                                                                              \
    __m512i a0 = _mm512_setzero_si512(), a1 = a0;                              \
    for (int y = 0; y < (h); y += (step)) {                                    \
        __m512i s = ROWS(src + y * ss, ss);                                    \
        a0 = _mm512_add_epi64(a0, _mm512_sad_epu8(s, ROWS(r0 + y * rs, rs)));  \
        a1 = _mm512_add_epi64(a1, _mm512_sad_epu8(s, ROWS(r1 + y * rs, rs)));  \
    }                                                                          \
    _mm_storel_epi64((__m128i *)(void *)scores, z_sad_fold2(a0, a1));          \
}
Y264_SADX_AVX512(16, 16, 4, z_rows4x16)
Y264_SADX_AVX512(16, 8, 4, z_rows4x16)
Y264_SADX_AVX512(8, 16, 8, z_rows8x8)
Y264_SADX_AVX512(8, 8, 8, z_rows8x8)
/* No 8x8 pair: one register per candidate leaves nothing for the fold to
 * share, and it counts the same as the AVX2 twin. */
Y264_SADX2_AVX512(16, 16, 4, z_rows4x16)
Y264_SADX2_AVX512(16, 8, 4, z_rows4x16)
Y264_SADX2_AVX512(8, 16, 8, z_rows8x8)

/* ---- the SATD core -------------------------------------------------------- */

/* Source and reference bytes -> (d0 + d1, d2 + d3, ...) in *a and
 * (d0 - d1, d2 - d3, ...) in *b: the first horizontal stage, done before the
 * residual exists. */
static inline void z_hstage1(__m512i s, __m512i r, __m512i *a, __m512i *b)
{
    const __m512i pp = _mm512_set1_epi8(1);
    const __m512i pm = _mm512_set1_epi16((short)0xff01);  /* bytes (1, -1) */
    *a = _mm512_sub_epi16(_mm512_maddubs_epi16(s, pp), _mm512_maddubs_epi16(r, pp));
    *b = _mm512_sub_epi16(_mm512_maddubs_epi16(s, pm), _mm512_maddubs_epi16(r, pm));
}

/* The last horizontal stage as 2 max(|x|, |y|) over each word pair, both
 * words of the pair carrying the max. */
static inline __m512i z_pairmax(__m512i v)
{
    v = _mm512_abs_epi16(v);
    return _mm512_max_epi16(v, _mm512_rol_epi32(v, 16));
}

/* Tile rows 0-3 in four registers, after the first horizontal stage: the two
 * vertical stages, the last horizontal one, and the word sum. */
static inline __m512i z_satd_col4(__m512i r0, __m512i r1, __m512i r2, __m512i r3)
{
    __m512i t0 = _mm512_add_epi16(r0, r1), t1 = _mm512_sub_epi16(r0, r1);
    __m512i t2 = _mm512_add_epi16(r2, r3), t3 = _mm512_sub_epi16(r2, r3);
    return _mm512_add_epi16(
        _mm512_add_epi16(z_pairmax(_mm512_add_epi16(t0, t2)),
                         z_pairmax(_mm512_sub_epi16(t0, t2))),
        _mm512_add_epi16(z_pairmax(_mm512_add_epi16(t1, t3)),
                         z_pairmax(_mm512_sub_epi16(t1, t3))));
}

static inline __m512i z_widen(__m512i w)
{
    return _mm512_madd_epi16(w, _mm512_set1_epi16(1));
}

/* Sixteen rows of sixteen from four registers per side, register i holding
 * rows i, i + 4, i + 8, i + 12. */
static inline __m512i z_satd16_regs(const __m512i s[4], const __m512i r[4])
{
    __m512i a[4], b[4];
    for (int i = 0; i < 4; i++)
        z_hstage1(s[i], r[i], &a[i], &b[i]);
    return z_widen(_mm512_add_epi16(z_satd_col4(a[0], a[1], a[2], a[3]),
                                    z_satd_col4(b[0], b[1], b[2], b[3])));
}

/* Eight rows of sixteen from two registers, rows (0, 2, 4, 6) and
 * (1, 3, 5, 7). The first vertical stage is between the two registers; the
 * second pairs lanes (0, 1) and (2, 3) of each result, which a 128-bit
 * shuffle of the two results turns back into a pair of registers. */
static inline __m512i z_satd8_half(__m512i p, __m512i q)
{
    __m512i u = _mm512_add_epi16(p, q), v = _mm512_sub_epi16(p, q);
    __m512i e = _mm512_shuffle_i64x2(u, v, _MM_SHUFFLE(2, 0, 2, 0));
    __m512i f = _mm512_shuffle_i64x2(u, v, _MM_SHUFFLE(3, 1, 3, 1));
    return _mm512_add_epi16(z_pairmax(_mm512_add_epi16(e, f)),
                            z_pairmax(_mm512_sub_epi16(e, f)));
}

static inline __m512i z_satd16x8_regs(__m512i se, __m512i so,
                                      __m512i re, __m512i ro)
{
    __m512i ae, be, ao, bo;
    z_hstage1(se, re, &ae, &be);
    z_hstage1(so, ro, &ao, &bo);
    return z_widen(_mm512_add_epi16(z_satd8_half(ae, ao), z_satd8_half(be, bo)));
}

static inline __m512i z_rows_even8(const uint8_t *p, int s)
{
    return z_rows4(p, p + 2 * s, p + 4 * s, p + 6 * s);
}

static inline __m512i z_rows_odd8(const uint8_t *p, int s)
{
    return z_rows4(p + s, p + 3 * s, p + 5 * s, p + 7 * s);
}

/* Register i of a sixteen-row block: rows i, i + 4, i + 8, i + 12, read
 * from the four quarter bases so every address is a base plus 0, s, 2s or
 * 3s and needs no arithmetic of its own. */
static inline void z_rows_q16(__m512i v[4], const uint8_t *p, int s)
{
    const uint8_t *p4 = p + 4 * s, *p8 = p + 8 * s, *p12 = p + 12 * s;
    for (int i = 0; i < 4; i++)
        v[i] = z_rows4(p + i * s, p4 + i * s, p8 + i * s, p12 + i * s);
}

/* ---- SATD ---------------------------------------------------------------- */

int y264_satd_16x16_avx512(const pixel *a, int as, const pixel *b, int bs)
{
    __m512i s[4], r[4];
    z_rows_q16(s, a, as);
    z_rows_q16(r, b, bs);
    return _mm512_reduce_add_epi32(z_satd16_regs(s, r));
}

int y264_satd_16x8_avx512(const pixel *a, int as, const pixel *b, int bs)
{
    return _mm512_reduce_add_epi32(z_satd16x8_regs(
        z_rows_even8(a, as), z_rows_odd8(a, as),
        z_rows_even8(b, bs), z_rows_odd8(b, bs)));
}

/* Against the rounded average of p and q, formed in the register: PAVGB is
 * (p + q + 1) >> 1 exactly. p and q share one stride. */
int y264_satd_avg_16x16_avx512(const pixel *src, int ss, const pixel *p,
                               const pixel *q, int ps)
{
    __m512i s[4], r[4], u[4];
    z_rows_q16(s, src, ss);
    z_rows_q16(r, p, ps);
    z_rows_q16(u, q, ps);
    for (int i = 0; i < 4; i++)
        r[i] = _mm512_avg_epu8(r[i], u[i]);
    return _mm512_reduce_add_epi32(z_satd16_regs(s, r));
}

int y264_satd_avg_16x8_avx512(const pixel *src, int ss, const pixel *p,
                              const pixel *q, int ps)
{
    return _mm512_reduce_add_epi32(z_satd16x8_regs(
        z_rows_even8(src, ss), z_rows_odd8(src, ss),
        _mm512_avg_epu8(z_rows_even8(p, ps), z_rows_even8(q, ps)),
        _mm512_avg_epu8(z_rows_odd8(p, ps), z_rows_odd8(q, ps))));
}

/* Four 8x8 candidates, one per 128-bit lane: register i holds rows i and
 * i + 4 of every candidate, so the vertical stages are between registers and
 * the lanes never meet until the four scores are read out. The source is the
 * same 128 bits broadcast to every lane. */
void y264_satd_x4_8x8_avx512(const pixel *src, int ss, const pixel *r0,
                             const pixel *r1, const pixel *r2, const pixel *r3,
                             int rs, int scores[4])
{
    __m512i a[4], b[4];
    for (int i = 0; i < 4; i++) {
        __m512i s = _mm512_broadcast_i32x4(x_rows2x8(src + i * ss,
                                                     src + (i + 4) * ss));
        __m512i r = _mm512_castsi128_si512(x_rows2x8(r0 + i * rs, r0 + (i + 4) * rs));
        r = _mm512_inserti32x4(r, x_rows2x8(r1 + i * rs, r1 + (i + 4) * rs), 1);
        r = _mm512_inserti32x4(r, x_rows2x8(r2 + i * rs, r2 + (i + 4) * rs), 2);
        r = _mm512_inserti32x4(r, x_rows2x8(r3 + i * rs, r3 + (i + 4) * rs), 3);
        z_hstage1(s, r, &a[i], &b[i]);
    }
    {
        __m512i w = z_widen(_mm512_add_epi16(z_satd_col4(a[0], a[1], a[2], a[3]),
                                             z_satd_col4(b[0], b[1], b[2], b[3])));
        /* lane k is candidate k's four partial sums; two hadds leave
         * (c0, c2, ...) in the low half and (c1, c3, ...) in the high one */
        __m256i h = _mm256_hadd_epi32(_mm512_castsi512_si256(w),
                                      _mm512_extracti64x4_epi64(w, 1));
        h = _mm256_hadd_epi32(h, h);
        _mm_storeu_si128((__m128i *)(void *)scores,
                         _mm_unpacklo_epi32(_mm256_castsi256_si128(h),
                                            _mm256_extracti128_si256(h, 1)));
    }
}


/* ---- the psy texture terms ------------------------------------------------
 *
 * Both terms of the fused pass come out of one transform of the 16x16 block,
 * on pixels rather than a residual. Register i holds rows i, i + 4, i + 8,
 * i + 12, so after the byte-domain first stage and the two vertical stages
 * every word pair holds a 4x4 tile's partial row, and one more in-register
 * butterfly finishes all sixteen 4x4 transforms.
 *
 * The 8x8 term needs those coefficients as values, not magnitudes, because an
 * 8-point Walsh-Hadamard is the 4-point one on each half followed by one more
 * butterfly between the halves: vertically between lanes (0, 1) and (2, 3),
 * which is rows r and r + 4 of a quadrant, and horizontally between the two
 * dwords of a qword, which is a quadrant's two tiles side by side. The last of
 * those is the pair-max form again, on a 64-bit rotate. The term subtracts
 * each quadrant's pixel sum, and because the four quadrants are summed, the
 * block's pixel sum -- the sum of the sixteen tile DCs -- is subtracted once.
 *
 * The 4x4 term replaces each tile's DC magnitude with its distance from the
 * nearest multiple of 16 (the C reference's flat-DC correction); with all
 * sixteen DCs in one register of dwords that is five vector instructions. */

/* (x + y, x - y) in each word pair, low word first. */
static inline __m512i z_pairbfly(__m512i v)
{
    __m512i r = _mm512_rol_epi32(v, 16);
    return _mm512_mask_sub_epi16(_mm512_add_epi16(v, r), 0xaaaaaaaau, r, v);
}

/* Rows r and r + 4 of each quadrant are lanes (0, 1) and (2, 3) of every
 * register: line them up as two registers, add and subtract. */
static inline void z_lanebfly(__m512i *x, __m512i *y)
{
    __m512i e = _mm512_shuffle_i64x2(*x, *y, _MM_SHUFFLE(2, 0, 2, 0));
    __m512i f = _mm512_shuffle_i64x2(*x, *y, _MM_SHUFFLE(3, 1, 3, 1));
    *x = _mm512_add_epi16(e, f);
    *y = _mm512_sub_epi16(e, f);
}

/* 2 max(|x|, |y|) over dword pairs within each qword, both places. */
static inline __m512i z_dwpairmax(__m512i v)
{
    v = _mm512_abs_epi16(v);
    return _mm512_max_epi16(v, _mm512_rol_epi64(v, 32));
}

void y264_texture_ac48_16x16_avx512(const pixel *p, int stride, long out[2])
{
    const __m512i one = _mm512_set1_epi16(1);
    __m512i s[4], c[8], dc, corr, a4, a8;
    z_rows_q16(s, p, stride);
    {
        const __m512i pp = _mm512_set1_epi8(1);
        const __m512i pm = _mm512_set1_epi16((short)0xff01);
        for (int h = 0; h < 2; h++) {
            const __m512i w = h ? pm : pp;
            __m512i r0 = _mm512_maddubs_epi16(s[0], w), r1 = _mm512_maddubs_epi16(s[1], w);
            __m512i r2 = _mm512_maddubs_epi16(s[2], w), r3 = _mm512_maddubs_epi16(s[3], w);
            __m512i t0 = _mm512_add_epi16(r0, r1), t1 = _mm512_sub_epi16(r0, r1);
            __m512i t2 = _mm512_add_epi16(r2, r3), t3 = _mm512_sub_epi16(r2, r3);
            c[4 * h + 0] = _mm512_add_epi16(t0, t2);
            c[4 * h + 1] = _mm512_sub_epi16(t0, t2);
            c[4 * h + 2] = _mm512_add_epi16(t1, t3);
            c[4 * h + 3] = _mm512_sub_epi16(t1, t3);
        }
    }
    /* the tile DCs, one per dword, before the last 4x4 stage folds them */
    dc = _mm512_madd_epi16(c[0], one);
    {
        __m512i flat = _mm512_and_si512(_mm512_add_epi32(dc, _mm512_set1_epi32(8)),
                                        _mm512_set1_epi32(~15));
        corr = _mm512_sub_epi32(_mm512_abs_epi32(_mm512_sub_epi32(dc, flat)), dc);
    }
    {
        __m512i m = _mm512_setzero_si512();
        for (int i = 0; i < 8; i++) {
            c[i] = z_pairbfly(c[i]);
            m = _mm512_add_epi16(m, _mm512_abs_epi16(c[i]));
        }
        a4 = _mm512_add_epi32(_mm512_madd_epi16(m, one), corr);
    }
    for (int i = 0; i < 8; i += 2)
        z_lanebfly(&c[i], &c[i + 1]);
    a8 = _mm512_add_epi32(
        _mm512_madd_epi16(_mm512_add_epi16(
            _mm512_add_epi16(z_dwpairmax(c[0]), z_dwpairmax(c[1])),
            _mm512_add_epi16(z_dwpairmax(c[2]), z_dwpairmax(c[3]))), one),
        _mm512_madd_epi16(_mm512_add_epi16(
            _mm512_add_epi16(z_dwpairmax(c[4]), z_dwpairmax(c[5])),
            _mm512_add_epi16(z_dwpairmax(c[6]), z_dwpairmax(c[7]))), one));
    out[0] = (long)_mm512_reduce_add_epi32(a4);
    out[1] = (long)_mm512_reduce_add_epi32(_mm512_sub_epi32(a8, dc));
}

/* ---- SA8D 16x16 -----------------------------------------------------------
 *
 * The texture pass's 8x8 transform on a residual: byte-domain first stage,
 * two vertical stages, the in-pair butterfly, the lane butterfly, and the
 * dword pair-max. Each quadrant is rounded on its own, (q + 2) >> 2, so the
 * per-dword sums are folded only within a quadrant. The lane butterfly writes
 * its sums to lanes (0, 1) and its differences to lanes (2, 3), so a
 * quadrant's top or bottom half is lane parity: fold the two dwords of a
 * qword, then lanes (0, 2) and (1, 3), and every quadrant's sum sits in four
 * dwords; the rounded total is the reduction over four. */
int y264_sa8d_16x16_avx512(const pixel *a, int as, const pixel *b, int bs)
{
    const __m512i one = _mm512_set1_epi16(1);
    __m512i s[4], r[4], c[8], v;
    z_rows_q16(s, a, as);
    z_rows_q16(r, b, bs);
    {
        __m512i x[4], y[4];
        for (int i = 0; i < 4; i++)
            z_hstage1(s[i], r[i], &x[i], &y[i]);
        for (int h = 0; h < 2; h++) {
            const __m512i *q = h ? y : x;
            __m512i t0 = _mm512_add_epi16(q[0], q[1]), t1 = _mm512_sub_epi16(q[0], q[1]);
            __m512i t2 = _mm512_add_epi16(q[2], q[3]), t3 = _mm512_sub_epi16(q[2], q[3]);
            c[4 * h + 0] = z_pairbfly(_mm512_add_epi16(t0, t2));
            c[4 * h + 1] = z_pairbfly(_mm512_sub_epi16(t0, t2));
            c[4 * h + 2] = z_pairbfly(_mm512_add_epi16(t1, t3));
            c[4 * h + 3] = z_pairbfly(_mm512_sub_epi16(t1, t3));
        }
    }
    for (int i = 0; i < 8; i += 2)
        z_lanebfly(&c[i], &c[i + 1]);
    v = _mm512_add_epi32(
        _mm512_madd_epi16(_mm512_add_epi16(
            _mm512_add_epi16(z_dwpairmax(c[0]), z_dwpairmax(c[1])),
            _mm512_add_epi16(z_dwpairmax(c[2]), z_dwpairmax(c[3]))), one),
        _mm512_madd_epi16(_mm512_add_epi16(
            _mm512_add_epi16(z_dwpairmax(c[4]), z_dwpairmax(c[5])),
            _mm512_add_epi16(z_dwpairmax(c[6]), z_dwpairmax(c[7]))), one));
    v = _mm512_add_epi32(v, _mm512_rol_epi64(v, 32));
    v = _mm512_add_epi32(v, _mm512_shuffle_i64x2(v, v, _MM_SHUFFLE(1, 0, 3, 2)));
    v = _mm512_srai_epi32(_mm512_add_epi32(v, _mm512_set1_epi32(2)), 2);
    return _mm512_reduce_add_epi32(v) >> 2;
}

#endif /* Y264_HAVE_AVX512 */
