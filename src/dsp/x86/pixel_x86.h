/*
 * pixel_x86.h - the 128-bit building blocks the pixel kernels share
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Every helper here is `static inline` and 128-bit, so the SSE4.2 file gets
 * SSE encodings and the AVX2 file gets VEX encodings of the same source. The
 * AVX2 file includes this header for exactly that: the narrow shapes (a 4x4
 * SATD, an 8-wide tail) have nothing for a 256-bit register to do, and a tail
 * written twice is a tail that can disagree with itself.
 *
 * TWO IDENTITIES CARRY MOST OF THIS FILE.
 *
 * The first is that a Hadamard butterfly's last stage never has to be
 * materialised when only |coefficients| are wanted. `hadd`/`hsub` of the same
 * two registers produce the sums and the differences of adjacent lanes in one
 * pair of instructions, and the sum of their magnitudes over both results IS
 * the sum of the magnitudes of that stage's outputs. Nothing has to be put
 * back in coefficient order, which is what lets a 4x4 SATD finish in two
 * hadd/hsub pairs with no transpose anywhere.
 *
 * The second is that the 2-D transform is separable, so the pass across rows
 * (plain adds between registers) and the pass along a row (the hadd pairs
 * above) can run in either order, and the sum of absolute coefficients does
 * not care which -- the two orders differ by a transpose. That is why no
 * kernel here transposes: the vertical pass runs between whole-row registers
 * and the horizontal pass runs inside them.
 *
 * LANE DISCIPLINE. `_mm_hadd_epi16` pairs lanes (0,1), (2,3), (4,5), (6,7),
 * so a register holding eight columns holds two independent 4-wide tiles and
 * the butterflies never cross between them. The 256-bit twins keep the same
 * property because `_mm256_hadd_epi16` is two 128-bit halves side by side --
 * which is the reason a 16-wide row expands to one ymm and yields four tiles
 * with the identical sequence.
 *
 * WINDOWS. checkasm maps each kernel's declared w x h window against a
 * PROT_NONE page at both tails, so a load is sized to the block and never to
 * the register: an 8-wide block is loaded eight bytes at a time and a 4-wide
 * one four. Planes in this encoder are plain malloc with no alignment
 * guarantee (docs/dsp-coverage-inventory.md section 3), so every load here is
 * an unaligned one.
 */
#ifndef YAH264_DSP_X86_PIXEL_X86_H
#define YAH264_DSP_X86_PIXEL_X86_H

#include <immintrin.h>
#include <string.h>

#include "dsp/arch.h"

/* ---- loads sized to the block, never to the register --------------------- */

static inline __m128i y264_ld32(const uint8_t *p)
{
    int32_t v;
    memcpy(&v, p, 4);
    return _mm_cvtsi32_si128(v);
}

static inline __m128i y264_ld64(const uint8_t *p)
{
    return _mm_loadl_epi64((const __m128i *)(const void *)p);
}

static inline __m128i y264_ld128(const uint8_t *p)
{
    return _mm_loadu_si128((const __m128i *)(const void *)p);
}

/* ---- reductions ---------------------------------------------------------- */

static inline int y264_hsum_epi32(__m128i v)
{
    v = _mm_add_epi32(v, _mm_shuffle_epi32(v, 0x4e));
    v = _mm_add_epi32(v, _mm_shuffle_epi32(v, 0xb1));
    return _mm_cvtsi128_si32(v);
}

/* PSADBW leaves its two sums in lanes 0 and 2. */
static inline int y264_hsum_sad(__m128i v)
{
    return _mm_cvtsi128_si32(v) + _mm_extract_epi32(v, 2);
}

/* ---- the SATD core ------------------------------------------------------- */

/* Four residual rows of eight columns -> the 32 coefficient magnitudes of the
 * two 4x4 tiles they span, as four partial int32 sums.
 *
 * The vertical butterfly is four adds and four subtracts between whole rows.
 * The horizontal one is the hadd/hsub identity twice: the first pair leaves
 * (x0+x1, x2+x3) beside (x0-x1, x2-x3) for every row, the second pair combines
 * those into the four coefficients of that row without ever ordering them.
 *
 * Range, 8-bit: a residual is within +-255 and a 2-D 4x4 Hadamard coefficient
 * is at most 16*255 = 4080, so four of them add to 16320 and stay inside a
 * signed 16-bit lane; the widening to int32 happens in the final madd. */
static inline __m128i y264_satd_rows8(__m128i d0, __m128i d1,
                                      __m128i d2, __m128i d3)
{
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
    __m128i s = _mm_add_epi16(_mm_add_epi16(a, b), _mm_add_epi16(c, d));
    return _mm_madd_epi16(s, _mm_set1_epi16(1));
}

/* One 4x4 tile from its residual held as two rows per register.
 *
 * The vertical butterfly has to cross the halves of a register here rather
 * than between registers, which `_mm_shuffle_epi32` by 0x4e does: adding a
 * register to its own half-swap leaves row0+row1 in the low half, subtracting
 * leaves row0-row1 there, and one `unpacklo_epi64` picks both out. */
static inline __m128i y264_satd_tile4(__m128i d01, __m128i d23)
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

/* Two rows of four samples, zero-extended into one register. */
static inline __m128i y264_rows2x4(const uint8_t *p, int stride)
{
    return _mm_cvtepu8_epi16(_mm_unpacklo_epi32(y264_ld32(p),
                                                y264_ld32(p + stride)));
}

static inline int y264_satd_4x4_x86(const uint8_t *a, int as,
                                    const uint8_t *b, int bs)
{
    __m128i d01 = _mm_sub_epi16(y264_rows2x4(a, as), y264_rows2x4(b, bs));
    __m128i d23 = _mm_sub_epi16(y264_rows2x4(a + 2 * as, as),
                                y264_rows2x4(b + 2 * bs, bs));
    return y264_hsum_epi32(y264_satd_tile4(d01, d23));
}

/* ---- the 8-point Walsh-Hadamard along a row ------------------------------
 *
 * Three stages inside one register of eight lanes, which is what SA8D, the
 * 8x8 AC magnitude and the psy 8x8 term all need after their column pass.
 * Stages one and two pair lanes four and two apart, so each is a shuffle, an
 * add, a subtract and a select; stage three pairs neighbours, which is the
 * hadd/hsub identity again -- so this returns the last stage UNRESOLVED, as
 * the pair (sums, differences) whose magnitudes the caller adds up. */
static inline void y264_had8_row(__m128i v, __m128i *sum, __m128i *dif)
{
    __m128i s4 = _mm_shuffle_epi32(v, 0x4e);
    __m128i a = _mm_unpacklo_epi64(_mm_add_epi16(v, s4), _mm_sub_epi16(v, s4));
    __m128i s2 = _mm_shufflehi_epi16(_mm_shufflelo_epi16(a, 0x4e), 0x4e);
    __m128i p = _mm_add_epi16(a, s2), q = _mm_sub_epi16(a, s2);
    /* q's wanted lanes are 0,1 and 4,5; the shuffle moves them to 2,3 and 6,7
     * where the blend takes them, so the pair keeps its sign. */
    __m128i qs = _mm_shufflehi_epi16(_mm_shufflelo_epi16(q, 0x4e), 0x4e);
    __m128i b = _mm_blend_epi16(p, qs, 0xcc);
    *sum = _mm_hadd_epi16(b, b);
    *dif = _mm_hsub_epi16(b, b);
}

/* The eight magnitudes of one row's 8-point transform.
 *
 * `hadd(b, b)` writes its four results into BOTH halves of the register, so
 * the four sums and the four differences are taken from the low halves of the
 * two results and joined: the register that comes back holds eight distinct
 * magnitudes and not four of them twice. */
static inline __m128i y264_had8_absrow(__m128i v)
{
    __m128i s, d;
    y264_had8_row(v, &s, &d);
    return _mm_abs_epi16(_mm_unpacklo_epi64(s, d));
}

/* The column pass of an 8x8 Walsh-Hadamard, between eight whole-row
 * registers. Ordering follows the portable reference exactly, though only the
 * multiset of outputs matters to every caller here. */
#define Y264_HAD8_COLS(v, o) do {                                            \
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

/* Sum of the 64 coefficient magnitudes of an 8x8 Walsh-Hadamard of `d`,
 * eight rows of eight int16. The column pass runs between the registers and
 * the row pass inside them; a coefficient is at most 64*255 = 16320 and eight
 * magnitudes add to 130560, so the accumulation widens to int32 per row. */
static inline int y264_had8x8_absum(const __m128i d[8])
{
    __m128i c[8];
    __m128i acc = _mm_setzero_si128();
    Y264_HAD8_COLS(d, c);
    for (int i = 0; i < 8; i++)
        acc = _mm_add_epi32(acc, _mm_madd_epi16(y264_had8_absrow(c[i]),
                                                _mm_set1_epi16(1)));
    return y264_hsum_epi32(acc);
}

/* Eight rows of an 8x8 block as residual against `b`, or against nothing. */
static inline void y264_load8x8_diff(__m128i d[8], const uint8_t *a, int as,
                                     const uint8_t *b, int bs)
{
    for (int i = 0; i < 8; i++)
        d[i] = _mm_sub_epi16(_mm_cvtepu8_epi16(y264_ld64(a + i * as)),
                             _mm_cvtepu8_epi16(y264_ld64(b + i * bs)));
}

static inline void y264_load8x8(__m128i d[8], const uint8_t *p, int stride)
{
    for (int i = 0; i < 8; i++)
        d[i] = _mm_cvtepu8_epi16(y264_ld64(p + i * stride));
}

/* ---- the fused Intra4x4 edge table ---------------------------------------
 *
 * Every directional Intra4x4 sample is one of three filters of the flattened
 * edge array E = { l3, l3, l2, l1, l0, tl, t0..t7, t7, t7, ... }: the 121
 * filter F, the pairwise average H, or the raw sample. Which one, at which
 * index, is a pure function of (mode, x, y), so one 64-entry byte lookup over
 * { F, H, E, dc } builds a whole prediction and the two positional special
 * cases fall out of the array's padding rather than a branch -- the duplicated
 * l3 at E[0] gives HU its (l2 + 3*l3 + 2) >> 2 corner, and the replicated t7
 * tail gives DDL its own. The index rows were derived from the standard's
 * per-mode formulas; checkasm checks the result against the portable builder
 * for every mode under every availability combination, which is what makes
 * that derivation an assertion rather than a claim. */
static const uint8_t y264_i4_x9_idx[9][16] = {
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

/* A 64-entry byte table lives in four registers; PSHUFB indexes sixteen at a
 * time, so the selection between them is two blends on bits 4 and 5 of the
 * index. */
static inline __m128i y264_tbl64(const __m128i t[4], __m128i idx)
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

/* The truncating average x86 has no instruction for: PAVGB rounds up, and
 * (a + b) >> 1 is that result minus the carry the rounding added. */
static inline __m128i y264_avg_trunc(__m128i a, __m128i b)
{
    return _mm_sub_epi8(_mm_avg_epu8(a, b),
                        _mm_and_si128(_mm_xor_si128(a, b), _mm_set1_epi8(1)));
}

/* The edge array and the three filters of it, shared by both tiers. Returns
 * the four table registers the index rows above are written against. */
static inline void y264_i4_edge_tab(__m128i t[4], const uint8_t *rec, int rs,
                                    int ht, int hl, int htl, int htr)
{
    uint8_t e[24], top[8], left[4];
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
    e[3] = left[1]; e[4] = left[0]; e[5] = (uint8_t)tl;
    for (int i = 0; i < 8; i++)
        e[6 + i] = top[i];
    for (int i = 14; i < 24; i++)
        e[i] = top[7];

    {
        __m128i e0 = y264_ld128(e), e1 = y264_ld128(e + 1), e2 = y264_ld128(e + 2);
        /* (a + 2b + c + 2) >> 2 == (((a + c) >> 1) + b + 1) >> 1 */
        t[0] = _mm_avg_epu8(y264_avg_trunc(e0, e2), e1);
        t[1] = _mm_avg_epu8(e0, e1);
        t[2] = e0;
        t[3] = _mm_set1_epi8((char)dc);
    }
}

#endif /* YAH264_DSP_X86_PIXEL_X86_H */
