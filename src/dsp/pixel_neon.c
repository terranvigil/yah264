/*
 * pixel_neon.c - aarch64 NEON SAD kernels
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Phase 0 uses NEON intrinsics to prove the dispatch and checkasm loop end to
 * end on aarch64. Per docs/plan.md the performance-critical kernels move to
 * hand-written GAS assembly in Phase 5; these keep the same ABI so the swap is
 * transparent to callers and to checkasm.
 */
#if defined(__aarch64__)
#include "pixel.h"
#include <arm_neon.h>

/* Sum of squared differences, bit-exact with the scalar ssd_block (|a-b|^2 is
 * exact integer arithmetic). Height-parameterized to cover 16x16 luma, 8x8/8x16/
 * 16x8 chroma, and 16x16 4:4:4 chroma. */
int y264_ssd_16xh_neon(const uint8_t *a, int as, const uint8_t *b, int bs, int h)
{
    uint32x4_t acc = vdupq_n_u32(0);
    for (int y = 0; y < h; y++) {
        uint8x16_t d = vabdq_u8(vld1q_u8(a), vld1q_u8(b));
        acc = vpadalq_u16(acc, vmull_u8(vget_low_u8(d), vget_low_u8(d)));
        acc = vpadalq_u16(acc, vmull_u8(vget_high_u8(d), vget_high_u8(d)));
        a += as; b += bs;
    }
    return (int)vaddvq_u32(acc);
}

int y264_ssd_8xh_neon(const uint8_t *a, int as, const uint8_t *b, int bs, int h)
{
    uint32x4_t acc = vdupq_n_u32(0);
    for (int y = 0; y < h; y++) {
        uint8x8_t d = vabd_u8(vld1_u8(a), vld1_u8(b));
        acc = vpadalq_u16(acc, vmull_u8(d, d));
        a += as; b += bs;
    }
    return (int)vaddvq_u32(acc);
}

/* SAD kernels use FOUR independent accumulator chains: a single vpadal/uabal
 * accumulator serializes on its own latency (~3c) per row, which measured ~3.6x
 * slower than x264's asm; four chains keep the abd/padal pairs pipelined.
 * Per-lane bounds: each vpadalq_u8 adds two abs-diffs (<= 510) into a u16 lane,
 * <= 4 rows per chain at h=16 -> <= 2040; the pairwise chain merge stays
 * <= 8160 and the final total <= 16*16*255 = 65280 < 65535, all exact. */
static inline int sad_16xh_neon(const uint8_t *a, int as,
                                const uint8_t *b, int bs, int h)
{
    uint16x8_t s0 = vdupq_n_u16(0), s1 = vdupq_n_u16(0);
    uint16x8_t s2 = vdupq_n_u16(0), s3 = vdupq_n_u16(0);
    for (int y = 0; y < h; y += 4) {
        s0 = vpadalq_u8(s0, vabdq_u8(vld1q_u8(a + 0 * as), vld1q_u8(b + 0 * bs)));
        s1 = vpadalq_u8(s1, vabdq_u8(vld1q_u8(a + 1 * as), vld1q_u8(b + 1 * bs)));
        s2 = vpadalq_u8(s2, vabdq_u8(vld1q_u8(a + 2 * as), vld1q_u8(b + 2 * bs)));
        s3 = vpadalq_u8(s3, vabdq_u8(vld1q_u8(a + 3 * as), vld1q_u8(b + 3 * bs)));
        a += 4 * as; b += 4 * bs;
    }
    return (int)vaddvq_u16(vaddq_u16(vaddq_u16(s0, s1), vaddq_u16(s2, s3)));
}

int y264_sad_16x16_neon(const uint8_t *a, int as, const uint8_t *b, int bs)
{
    return sad_16xh_neon(a, as, b, bs, 16);
}

int y264_sad_16x8_neon(const uint8_t *a, int as, const uint8_t *b, int bs)
{
    return sad_16xh_neon(a, as, b, bs, 8);
}

static inline int sad_8xh_neon(const uint8_t *a, int as,
                               const uint8_t *b, int bs, int h)
{
    /* Pair rows into 16-byte vectors so the 8-wide block runs the 16-wide
 * datapath (two rows per abd/padal). */
    uint16x8_t s0 = vdupq_n_u16(0), s1 = vdupq_n_u16(0);
    for (int y = 0; y < h; y += 4) {
        uint8x16_t a01 = vcombine_u8(vld1_u8(a), vld1_u8(a + as));
        uint8x16_t b01 = vcombine_u8(vld1_u8(b), vld1_u8(b + bs));
        uint8x16_t a23 = vcombine_u8(vld1_u8(a + 2 * as), vld1_u8(a + 3 * as));
        uint8x16_t b23 = vcombine_u8(vld1_u8(b + 2 * bs), vld1_u8(b + 3 * bs));
        s0 = vpadalq_u8(s0, vabdq_u8(a01, b01));
        s1 = vpadalq_u8(s1, vabdq_u8(a23, b23));
        a += 4 * as; b += 4 * bs;
    }
    return (int)vaddvq_u16(vaddq_u16(s0, s1));
}

int y264_sad_8x16_neon(const uint8_t *a, int as, const uint8_t *b, int bs)
{
    return sad_8xh_neon(a, as, b, bs, 16);
}

int y264_sad_8x8_neon(const uint8_t *a, int as, const uint8_t *b, int bs)
{
    return sad_8xh_neon(a, as, b, bs, 8);
}

/* Batched-x4 SAD : one source row load feeds four
 * abd/padal chains -- the four accumulators are independent, so the chains
 * pipeline like the multi-accumulator single-SAD kernels while the source
 * loads are amortized 4x. Per-lane bounds as in sad_16xh_neon (h <= 16 rows
 * of two abs-diffs per u16 lane <= 8160, exact). */
static inline void sad_x4_16xh_neon(const uint8_t *src, int ss,
                                    const uint8_t *r0, const uint8_t *r1,
                                    const uint8_t *r2, const uint8_t *r3,
                                    int rs, int h, int scores[4])
{
    uint16x8_t s0 = vdupq_n_u16(0), s1 = vdupq_n_u16(0);
    uint16x8_t s2 = vdupq_n_u16(0), s3 = vdupq_n_u16(0);
    for (int y = 0; y < h; y++) {
        uint8x16_t s = vld1q_u8(src);
        s0 = vpadalq_u8(s0, vabdq_u8(s, vld1q_u8(r0)));
        s1 = vpadalq_u8(s1, vabdq_u8(s, vld1q_u8(r1)));
        s2 = vpadalq_u8(s2, vabdq_u8(s, vld1q_u8(r2)));
        s3 = vpadalq_u8(s3, vabdq_u8(s, vld1q_u8(r3)));
        src += ss; r0 += rs; r1 += rs; r2 += rs; r3 += rs;
    }
    scores[0] = (int)vaddvq_u16(s0);
    scores[1] = (int)vaddvq_u16(s1);
    scores[2] = (int)vaddvq_u16(s2);
    scores[3] = (int)vaddvq_u16(s3);
}

static inline void sad_x4_8xh_neon(const uint8_t *src, int ss,
                                   const uint8_t *r0, const uint8_t *r1,
                                   const uint8_t *r2, const uint8_t *r3,
                                   int rs, int h, int scores[4])
{
    /* Pair rows into 16-byte vectors (see sad_8xh_neon). */
    uint16x8_t s0 = vdupq_n_u16(0), s1 = vdupq_n_u16(0);
    uint16x8_t s2 = vdupq_n_u16(0), s3 = vdupq_n_u16(0);
    for (int y = 0; y < h; y += 2) {
        uint8x16_t s = vcombine_u8(vld1_u8(src), vld1_u8(src + ss));
        s0 = vpadalq_u8(s0, vabdq_u8(s, vcombine_u8(vld1_u8(r0), vld1_u8(r0 + rs))));
        s1 = vpadalq_u8(s1, vabdq_u8(s, vcombine_u8(vld1_u8(r1), vld1_u8(r1 + rs))));
        s2 = vpadalq_u8(s2, vabdq_u8(s, vcombine_u8(vld1_u8(r2), vld1_u8(r2 + rs))));
        s3 = vpadalq_u8(s3, vabdq_u8(s, vcombine_u8(vld1_u8(r3), vld1_u8(r3 + rs))));
        src += 2 * ss; r0 += 2 * rs; r1 += 2 * rs; r2 += 2 * rs; r3 += 2 * rs;
    }
    scores[0] = (int)vaddvq_u16(s0);
    scores[1] = (int)vaddvq_u16(s1);
    scores[2] = (int)vaddvq_u16(s2);
    scores[3] = (int)vaddvq_u16(s3);
}

void y264_sad_x4_16x16_neon(const uint8_t *src, int ss, const uint8_t *r0,
                            const uint8_t *r1, const uint8_t *r2,
                            const uint8_t *r3, int rs, int scores[4])
{ sad_x4_16xh_neon(src, ss, r0, r1, r2, r3, rs, 16, scores); }

void y264_sad_x4_16x8_neon(const uint8_t *src, int ss, const uint8_t *r0,
                           const uint8_t *r1, const uint8_t *r2,
                           const uint8_t *r3, int rs, int scores[4])
{ sad_x4_16xh_neon(src, ss, r0, r1, r2, r3, rs, 8, scores); }

void y264_sad_x4_8x16_neon(const uint8_t *src, int ss, const uint8_t *r0,
                           const uint8_t *r1, const uint8_t *r2,
                           const uint8_t *r3, int rs, int scores[4])
{ sad_x4_8xh_neon(src, ss, r0, r1, r2, r3, rs, 16, scores); }

void y264_sad_x4_8x8_neon(const uint8_t *src, int ss, const uint8_t *r0,
                          const uint8_t *r1, const uint8_t *r2,
                          const uint8_t *r3, int rs, int scores[4])
{ sad_x4_8xh_neon(src, ss, r0, r1, r2, r3, rs, 8, scores); }

void y264_sad_x4_8x4_neon(const uint8_t *src, int ss, const uint8_t *r0,
                          const uint8_t *r1, const uint8_t *r2,
                          const uint8_t *r3, int rs, int scores[4])
{ sad_x4_8xh_neon(src, ss, r0, r1, r2, r3, rs, 4, scores); }

/* FEAT_DotProd SAD: UDOT the per-byte abs-diffs against an all-ones vector to
 * horizontally sum them into 32-bit lanes in one op, replacing the widening
 * pairwise-accumulate chain (uabal). Registered only when Y264_CPU_DOTPROD is
 * set; bit-exact with the plain-NEON SAD. x264 uses the same instruction here. */
#define Y264_DOTPROD_ATTR __attribute__((target("dotprod")))

Y264_DOTPROD_ATTR
static inline int sad_16xh_dotprod(const uint8_t *a, int as,
                                   const uint8_t *b, int bs, int h)
{
    /* Four independent udot chains (udot latency would serialize a single
 * accumulator); see the plain-NEON SAD comment. */
    const uint8x16_t ones = vdupq_n_u8(1);
    uint32x4_t s0 = vdupq_n_u32(0), s1 = vdupq_n_u32(0);
    uint32x4_t s2 = vdupq_n_u32(0), s3 = vdupq_n_u32(0);
    for (int y = 0; y < h; y += 4) {
        s0 = vdotq_u32(s0, vabdq_u8(vld1q_u8(a + 0 * as), vld1q_u8(b + 0 * bs)), ones);
        s1 = vdotq_u32(s1, vabdq_u8(vld1q_u8(a + 1 * as), vld1q_u8(b + 1 * bs)), ones);
        s2 = vdotq_u32(s2, vabdq_u8(vld1q_u8(a + 2 * as), vld1q_u8(b + 2 * bs)), ones);
        s3 = vdotq_u32(s3, vabdq_u8(vld1q_u8(a + 3 * as), vld1q_u8(b + 3 * bs)), ones);
        a += 4 * as; b += 4 * bs;
    }
    return (int)vaddvq_u32(vaddq_u32(vaddq_u32(s0, s1), vaddq_u32(s2, s3)));
}

Y264_DOTPROD_ATTR
static inline int sad_8xh_dotprod(const uint8_t *a, int as,
                                  const uint8_t *b, int bs, int h)
{
    const uint8x16_t ones = vdupq_n_u8(1);
    uint32x4_t s0 = vdupq_n_u32(0), s1 = vdupq_n_u32(0);
    for (int y = 0; y < h; y += 4) {
        uint8x16_t a01 = vcombine_u8(vld1_u8(a), vld1_u8(a + as));
        uint8x16_t b01 = vcombine_u8(vld1_u8(b), vld1_u8(b + bs));
        uint8x16_t a23 = vcombine_u8(vld1_u8(a + 2 * as), vld1_u8(a + 3 * as));
        uint8x16_t b23 = vcombine_u8(vld1_u8(b + 2 * bs), vld1_u8(b + 3 * bs));
        s0 = vdotq_u32(s0, vabdq_u8(a01, b01), ones);
        s1 = vdotq_u32(s1, vabdq_u8(a23, b23), ones);
        a += 4 * as; b += 4 * bs;
    }
    return (int)vaddvq_u32(vaddq_u32(s0, s1));
}

/* Only the tall (h=16) blocks win from UDOT in checkasm; 16x8/8x8 measure
 * neutral, so those partitions stay on the plain-NEON uabal kernels. */
Y264_DOTPROD_ATTR int y264_sad_16x16_neon_dotprod(const uint8_t *a, int as, const uint8_t *b, int bs)
{ return sad_16xh_dotprod(a, as, b, bs, 16); }
Y264_DOTPROD_ATTR int y264_sad_8x16_neon_dotprod(const uint8_t *a, int as, const uint8_t *b, int bs)
{ return sad_8xh_dotprod(a, as, b, bs, 16); }

/* Load four bytes as the low half of an int16x4 residual (a - b). */
static inline int16x4_t satd_diff4(const uint8_t *a, const uint8_t *b)
{
    uint8x8_t va = vreinterpret_u8_u32(vld1_dup_u32((const uint32_t *)(const void *)a));
    uint8x8_t vb = vreinterpret_u8_u32(vld1_dup_u32((const uint32_t *)(const void *)b));
    return vget_low_s16(vreinterpretq_s16_u16(vsubl_u8(va, vb)));
}

/* 4x4 SATD: vertical Hadamard, one horizontal stage, then the exact
 * |a+b|+|a-b| = 2*max(|a|,|b|) identity for the last stage (see
 * satd_4rows_half). Bit-exact with satd_c_4x4. */
int y264_satd_4x4_neon(const uint8_t *a, int as, const uint8_t *b, int bs)
{
    int16x4_t r0 = satd_diff4(a + 0 * as, b + 0 * bs);
    int16x4_t r1 = satd_diff4(a + 1 * as, b + 1 * bs);
    int16x4_t r2 = satd_diff4(a + 2 * as, b + 2 * bs);
    int16x4_t r3 = satd_diff4(a + 3 * as, b + 3 * bs);

    /* Vertical 4-point Hadamard across the four row vectors. */
    int16x4_t t0 = vadd_s16(r0, r1), t1 = vsub_s16(r0, r1);
    int16x4_t t2 = vadd_s16(r2, r3), t3 = vsub_s16(r2, r3);
    int16x4_t h0 = vadd_s16(t0, t2), h2 = vsub_s16(t0, t2);
    int16x4_t h1 = vadd_s16(t1, t3), h3 = vsub_s16(t1, t3);

    /* One horizontal stage on adjacent columns, then abs+max for the last. */
    int16x4x2_t p01 = vtrn_s16(h0, h1);
    int16x4x2_t p23 = vtrn_s16(h2, h3);
    int16x4_t s01 = vadd_s16(p01.val[0], p01.val[1]);
    int16x4_t d01 = vsub_s16(p01.val[0], p01.val[1]);
    int16x4_t s23 = vadd_s16(p23.val[0], p23.val[1]);
    int16x4_t d23 = vsub_s16(p23.val[0], p23.val[1]);
    int32x2x2_t q0 = vtrn_s32(vreinterpret_s32_s16(s01), vreinterpret_s32_s16(d01));
    int32x2x2_t q1 = vtrn_s32(vreinterpret_s32_s16(s23), vreinterpret_s32_s16(d23));
    uint16x4_t m0 = vmax_u16(
        vreinterpret_u16_s16(vabs_s16(vreinterpret_s16_s32(q0.val[0]))),
        vreinterpret_u16_s16(vabs_s16(vreinterpret_s16_s32(q0.val[1]))));
    uint16x4_t m1 = vmax_u16(
        vreinterpret_u16_s16(vabs_s16(vreinterpret_s16_s32(q1.val[0]))),
        vreinterpret_u16_s16(vabs_s16(vreinterpret_s16_s32(q1.val[1]))));
    return 2 * (int)vaddlv_u16(vadd_u16(m0, m1));
}

/* Half-Hadamard core for four row-vector diffs (two side-by-side 4x4 blocks:
 * cols 0-3 = block A, cols 4-7 = block B). Vertical 4-point Hadamard, 16-bit
 * pair transpose, ONE horizontal butterfly stage, then the exact identity
 * |a+b| + |a-b| = 2*max(|a|, |b|)
 * replaces the final butterfly + abs + adds with abs + umax (the x264 satd
 * mechanism, re-derived). Returns per-lane max-sums: the true SATD (which for
 * yah264 is the UN-halved Hadamard abs-sum) is 2x the reduced total, exactly.
 * Lane bound: each max <= 8160, and <= 4 folded per u16 lane -> <= 32640. */
static inline uint16x8_t satd_4rows_half(int16x8_t r0, int16x8_t r1,
                                         int16x8_t r2, int16x8_t r3)
{
    /* vertical 4-point Hadamard across the four row vectors */
    int16x8_t t0 = vaddq_s16(r0, r1), t1 = vsubq_s16(r0, r1);
    int16x8_t t2 = vaddq_s16(r2, r3), t3 = vsubq_s16(r2, r3);
    int16x8_t h0 = vaddq_s16(t0, t2), h2 = vsubq_s16(t0, t2);
    int16x8_t h1 = vaddq_s16(t1, t3), h3 = vsubq_s16(t1, t3);

    /* 16-bit transpose pairs, first horizontal stage on adjacent columns */
    int16x8x2_t p01 = vtrnq_s16(h0, h1);
    int16x8x2_t p23 = vtrnq_s16(h2, h3);
    int16x8_t s01 = vaddq_s16(p01.val[0], p01.val[1]);
    int16x8_t d01 = vsubq_s16(p01.val[0], p01.val[1]);
    int16x8_t s23 = vaddq_s16(p23.val[0], p23.val[1]);
    int16x8_t d23 = vsubq_s16(p23.val[0], p23.val[1]);

    /* The 32-bit transpose of (sum, diff) puts a row's (c01, c23) partners in
 * the same lane of val[0]/val[1]; the final stage would be u +/- v, so
 * take 2*max(|u|,|v|) per lane instead. */
    int32x4x2_t q0 = vtrnq_s32(vreinterpretq_s32_s16(s01),
                               vreinterpretq_s32_s16(d01));
    int32x4x2_t q1 = vtrnq_s32(vreinterpretq_s32_s16(s23),
                               vreinterpretq_s32_s16(d23));
    uint16x8_t m0 = vmaxq_u16(
        vreinterpretq_u16_s16(vabsq_s16(vreinterpretq_s16_s32(q0.val[0]))),
        vreinterpretq_u16_s16(vabsq_s16(vreinterpretq_s16_s32(q0.val[1]))));
    uint16x8_t m1 = vmaxq_u16(
        vreinterpretq_u16_s16(vabsq_s16(vreinterpretq_s16_s32(q1.val[0]))),
        vreinterpretq_u16_s16(vabsq_s16(vreinterpretq_s16_s32(q1.val[1]))));
    return vaddq_u16(m0, m1);
}

/* Dedicated 8x8 SATD: the H.264 8x8 SATD is the sum of its four 4x4 SATDs.
 * Bit-identical to the C reference (the max-identity is exact in integers). */
int y264_satd_8x8_neon(const uint8_t *a, int as, const uint8_t *b, int bs)
{
    int16x8_t d0 = vreinterpretq_s16_u16(vsubl_u8(vld1_u8(a+0*as), vld1_u8(b+0*bs)));
    int16x8_t d1 = vreinterpretq_s16_u16(vsubl_u8(vld1_u8(a+1*as), vld1_u8(b+1*bs)));
    int16x8_t d2 = vreinterpretq_s16_u16(vsubl_u8(vld1_u8(a+2*as), vld1_u8(b+2*bs)));
    int16x8_t d3 = vreinterpretq_s16_u16(vsubl_u8(vld1_u8(a+3*as), vld1_u8(b+3*bs)));
    int16x8_t d4 = vreinterpretq_s16_u16(vsubl_u8(vld1_u8(a+4*as), vld1_u8(b+4*bs)));
    int16x8_t d5 = vreinterpretq_s16_u16(vsubl_u8(vld1_u8(a+5*as), vld1_u8(b+5*bs)));
    int16x8_t d6 = vreinterpretq_s16_u16(vsubl_u8(vld1_u8(a+6*as), vld1_u8(b+6*bs)));
    int16x8_t d7 = vreinterpretq_s16_u16(vsubl_u8(vld1_u8(a+7*as), vld1_u8(b+7*bs)));

    uint16x8_t s = vaddq_u16(satd_4rows_half(d0, d1, d2, d3),
                             satd_4rows_half(d4, d5, d6, d7));
    return 2 * (int)vaddlvq_u16(s);
}

/* Batched 8x8 SATD against four references (see y264_satd_x4_fn). The eight
 * source rows are loaded ONCE and reused, but that is the smaller half of the
 * win: a single satd8x8 is one long serial Hadamard chain, and four of them
 * written back to back give the core four independent chains to interleave.
 * The per-reference body is y264_satd_8x8_neon verbatim, so every score is
 * bit-identical to it -- the scalar and batched forms are the same arithmetic
 * in the same order, not an equivalent reassociation. */
void y264_satd_x4_8x8_neon(const uint8_t *src, int ss,
                           const uint8_t *r0, const uint8_t *r1,
                           const uint8_t *r2, const uint8_t *r3,
                           int rs, int scores[4])
{
    const uint8x8_t s0 = vld1_u8(src + 0*ss), s1 = vld1_u8(src + 1*ss);
    const uint8x8_t s2 = vld1_u8(src + 2*ss), s3 = vld1_u8(src + 3*ss);
    const uint8x8_t s4 = vld1_u8(src + 4*ss), s5 = vld1_u8(src + 5*ss);
    const uint8x8_t s6 = vld1_u8(src + 6*ss), s7 = vld1_u8(src + 7*ss);
    const uint8_t *const r[4] = { r0, r1, r2, r3 };

    for (int k = 0; k < 4; k++) {
        const uint8_t *b = r[k];
        int16x8_t d0 = vreinterpretq_s16_u16(vsubl_u8(s0, vld1_u8(b + 0*rs)));
        int16x8_t d1 = vreinterpretq_s16_u16(vsubl_u8(s1, vld1_u8(b + 1*rs)));
        int16x8_t d2 = vreinterpretq_s16_u16(vsubl_u8(s2, vld1_u8(b + 2*rs)));
        int16x8_t d3 = vreinterpretq_s16_u16(vsubl_u8(s3, vld1_u8(b + 3*rs)));
        int16x8_t d4 = vreinterpretq_s16_u16(vsubl_u8(s4, vld1_u8(b + 4*rs)));
        int16x8_t d5 = vreinterpretq_s16_u16(vsubl_u8(s5, vld1_u8(b + 5*rs)));
        int16x8_t d6 = vreinterpretq_s16_u16(vsubl_u8(s6, vld1_u8(b + 6*rs)));
        int16x8_t d7 = vreinterpretq_s16_u16(vsubl_u8(s7, vld1_u8(b + 7*rs)));
        uint16x8_t s = vaddq_u16(satd_4rows_half(d0, d1, d2, d3),
                                 satd_4rows_half(d4, d5, d6, d7));
        scores[k] = 2 * (int)vaddlvq_u16(s);
    }
}

int y264_satd_16x16_neon_ded(const uint8_t *a, int as, const uint8_t *b, int bs)
{
    int s = 0;
    for (int by = 0; by < 16; by += 8)
        for (int bx = 0; bx < 16; bx += 8)
            s += y264_satd_8x8_neon(a + by*as + bx, as, b + by*bs + bx, bs);
    return s;
}

/* ---- SA8D / hadamard-AC: 2D 8x8 Walsh-Hadamard abs-sum ---------------------
 * Shared core: full 8-point vertical WHT across the eight row vectors, 8x8
 * 16-bit transpose, then two of the three horizontal stages -- the last stage
 * (i paired with i+4) is replaced by the exact |a+b|+|a-b| = 2*max(|a|,|b|)
 * identity, so the returned value is HALF the 2D Hadamard abs-sum. Butterfly
 * ordering differs from the scalar reference, which is fine: an abs-sum over a
 * complete WHT is invariant to stage order / output permutation / sign.
 * Bounds: |input| <= 255 (pixel diffs or pixels) -> after the vertical pass
 * <= 2040, after two horizontal stages <= 8160; four maxes fold into a u16
 * lane <= 32640, exact. */

#define SUMSUB8(p, m, x, y) do { p = vaddq_s16(x, y); m = vsubq_s16(x, y); } while (0)

static inline uint32_t sa8d_half_core(int16x8_t v0, int16x8_t v1, int16x8_t v2,
                                      int16x8_t v3, int16x8_t v4, int16x8_t v5,
                                      int16x8_t v6, int16x8_t v7)
{
    int16x8_t t0, t1, t2, t3, t4, t5, t6, t7;
    int16x8_t u0, u1, u2, u3, u4, u5, u6, u7;

    /* vertical 8-point WHT (three stages of vector add/sub) */
    SUMSUB8(t0, t1, v0, v1); SUMSUB8(t2, t3, v2, v3);
    SUMSUB8(t4, t5, v4, v5); SUMSUB8(t6, t7, v6, v7);
    SUMSUB8(u0, u2, t0, t2); SUMSUB8(u1, u3, t1, t3);
    SUMSUB8(u4, u6, t4, t6); SUMSUB8(u5, u7, t5, t7);
    SUMSUB8(v0, v4, u0, u4); SUMSUB8(v1, v5, u1, u5);
    SUMSUB8(v2, v6, u2, u6); SUMSUB8(v3, v7, u3, u7);

    /* 8x8 16-bit transpose (trn 16 / 32 / 64) */
    t0 = vtrn1q_s16(v0, v1); t1 = vtrn2q_s16(v0, v1);
    t2 = vtrn1q_s16(v2, v3); t3 = vtrn2q_s16(v2, v3);
    t4 = vtrn1q_s16(v4, v5); t5 = vtrn2q_s16(v4, v5);
    t6 = vtrn1q_s16(v6, v7); t7 = vtrn2q_s16(v6, v7);
    u0 = vreinterpretq_s16_s32(vtrn1q_s32(vreinterpretq_s32_s16(t0), vreinterpretq_s32_s16(t2)));
    u2 = vreinterpretq_s16_s32(vtrn2q_s32(vreinterpretq_s32_s16(t0), vreinterpretq_s32_s16(t2)));
    u1 = vreinterpretq_s16_s32(vtrn1q_s32(vreinterpretq_s32_s16(t1), vreinterpretq_s32_s16(t3)));
    u3 = vreinterpretq_s16_s32(vtrn2q_s32(vreinterpretq_s32_s16(t1), vreinterpretq_s32_s16(t3)));
    u4 = vreinterpretq_s16_s32(vtrn1q_s32(vreinterpretq_s32_s16(t4), vreinterpretq_s32_s16(t6)));
    u6 = vreinterpretq_s16_s32(vtrn2q_s32(vreinterpretq_s32_s16(t4), vreinterpretq_s32_s16(t6)));
    u5 = vreinterpretq_s16_s32(vtrn1q_s32(vreinterpretq_s32_s16(t5), vreinterpretq_s32_s16(t7)));
    u7 = vreinterpretq_s16_s32(vtrn2q_s32(vreinterpretq_s32_s16(t5), vreinterpretq_s32_s16(t7)));
    v0 = vreinterpretq_s16_s64(vtrn1q_s64(vreinterpretq_s64_s16(u0), vreinterpretq_s64_s16(u4)));
    v4 = vreinterpretq_s16_s64(vtrn2q_s64(vreinterpretq_s64_s16(u0), vreinterpretq_s64_s16(u4)));
    v1 = vreinterpretq_s16_s64(vtrn1q_s64(vreinterpretq_s64_s16(u1), vreinterpretq_s64_s16(u5)));
    v5 = vreinterpretq_s16_s64(vtrn2q_s64(vreinterpretq_s64_s16(u1), vreinterpretq_s64_s16(u5)));
    v2 = vreinterpretq_s16_s64(vtrn1q_s64(vreinterpretq_s64_s16(u2), vreinterpretq_s64_s16(u6)));
    v6 = vreinterpretq_s16_s64(vtrn2q_s64(vreinterpretq_s64_s16(u2), vreinterpretq_s64_s16(u6)));
    v3 = vreinterpretq_s16_s64(vtrn1q_s64(vreinterpretq_s64_s16(u3), vreinterpretq_s64_s16(u7)));
    v7 = vreinterpretq_s16_s64(vtrn2q_s64(vreinterpretq_s64_s16(u3), vreinterpretq_s64_s16(u7)));

    /* two horizontal stages; the third is the abs+max identity */
    SUMSUB8(t0, t1, v0, v1); SUMSUB8(t2, t3, v2, v3);
    SUMSUB8(t4, t5, v4, v5); SUMSUB8(t6, t7, v6, v7);
    SUMSUB8(u0, u2, t0, t2); SUMSUB8(u1, u3, t1, t3);
    SUMSUB8(u4, u6, t4, t6); SUMSUB8(u5, u7, t5, t7);

    uint16x8_t m0 = vmaxq_u16(vreinterpretq_u16_s16(vabsq_s16(u0)),
                              vreinterpretq_u16_s16(vabsq_s16(u4)));
    uint16x8_t m1 = vmaxq_u16(vreinterpretq_u16_s16(vabsq_s16(u1)),
                              vreinterpretq_u16_s16(vabsq_s16(u5)));
    uint16x8_t m2 = vmaxq_u16(vreinterpretq_u16_s16(vabsq_s16(u2)),
                              vreinterpretq_u16_s16(vabsq_s16(u6)));
    uint16x8_t m3 = vmaxq_u16(vreinterpretq_u16_s16(vabsq_s16(u3)),
                              vreinterpretq_u16_s16(vabsq_s16(u7)));
    return vaddlvq_u16(vaddq_u16(vaddq_u16(m0, m1), vaddq_u16(m2, m3)));
}

/* SA8D of an 8x8 diff, x264-normalised like the C reference: (sum + 2) >> 2
 * with sum = the full 2D Hadamard abs-sum = 2 * the half-core total. */
int y264_sa8d_8x8_neon(const uint8_t *a, int as, const uint8_t *b, int bs)
{
    int16x8_t d0 = vreinterpretq_s16_u16(vsubl_u8(vld1_u8(a+0*as), vld1_u8(b+0*bs)));
    int16x8_t d1 = vreinterpretq_s16_u16(vsubl_u8(vld1_u8(a+1*as), vld1_u8(b+1*bs)));
    int16x8_t d2 = vreinterpretq_s16_u16(vsubl_u8(vld1_u8(a+2*as), vld1_u8(b+2*bs)));
    int16x8_t d3 = vreinterpretq_s16_u16(vsubl_u8(vld1_u8(a+3*as), vld1_u8(b+3*bs)));
    int16x8_t d4 = vreinterpretq_s16_u16(vsubl_u8(vld1_u8(a+4*as), vld1_u8(b+4*bs)));
    int16x8_t d5 = vreinterpretq_s16_u16(vsubl_u8(vld1_u8(a+5*as), vld1_u8(b+5*bs)));
    int16x8_t d6 = vreinterpretq_s16_u16(vsubl_u8(vld1_u8(a+6*as), vld1_u8(b+6*bs)));
    int16x8_t d7 = vreinterpretq_s16_u16(vsubl_u8(vld1_u8(a+7*as), vld1_u8(b+7*bs)));
    uint32_t half = sa8d_half_core(d0, d1, d2, d3, d4, d5, d6, d7);
    return (int)((2 * half + 2) >> 2);
}

int y264_sa8d_16x16_neon(const uint8_t *a, int as, const uint8_t *b, int bs)
{
    return y264_sa8d_8x8_neon(a, as, b, bs)
         + y264_sa8d_8x8_neon(a + 8, as, b + 8, bs)
         + y264_sa8d_8x8_neon(a + 8*as, as, b + 8*bs, bs)
         + y264_sa8d_8x8_neon(a + 8*as + 8, as, b + 8*bs + 8, bs);
}

/* AC magnitude of one 8x8 via the 8x8 Hadamard: full abs-sum minus |DC|,
 * DC = the plain pixel sum. Matches the scalar hadamard_ac_8x8 exactly. */
long y264_hadamard_ac_8x8_neon(const uint8_t *p, int stride)
{
    uint8x8_t r0 = vld1_u8(p + 0*stride), r1 = vld1_u8(p + 1*stride);
    uint8x8_t r2 = vld1_u8(p + 2*stride), r3 = vld1_u8(p + 3*stride);
    uint8x8_t r4 = vld1_u8(p + 4*stride), r5 = vld1_u8(p + 5*stride);
    uint8x8_t r6 = vld1_u8(p + 6*stride), r7 = vld1_u8(p + 7*stride);

    uint16x8_t dcv = vaddl_u8(r0, r1);
    dcv = vaddq_u16(dcv, vaddl_u8(r2, r3));
    dcv = vaddq_u16(dcv, vaddl_u8(r4, r5));
    dcv = vaddq_u16(dcv, vaddl_u8(r6, r7));
    uint32_t dc = vaddlvq_u16(dcv);

    uint32_t half = sa8d_half_core(
        vreinterpretq_s16_u16(vmovl_u8(r0)), vreinterpretq_s16_u16(vmovl_u8(r1)),
        vreinterpretq_s16_u16(vmovl_u8(r2)), vreinterpretq_s16_u16(vmovl_u8(r3)),
        vreinterpretq_s16_u16(vmovl_u8(r4)), vreinterpretq_s16_u16(vmovl_u8(r5)),
        vreinterpretq_s16_u16(vmovl_u8(r6)), vreinterpretq_s16_u16(vmovl_u8(r7)));
    return (long)(2 * half - dc);
}

/* psy-RD texture energy of a 16x16, SATD-support term: the sum over sixteen
 * 4x4 tiles of SATD(tile, flat block of the tile's rounded mean). Two tiles per
 * pass through satd_4rows_half (lanes 0-3 are the left tile, 4-7 the right --
 * every transpose there stays inside its 4-lane half), whose per-lane maxes sum
 * to HALF the true Hadamard abs-sum. The tile's DC is its pixel sum, taken from
 * the same loads. Bit-exact with texture_ac4_c_16x16. */
long y264_texture_ac4_16x16_neon(const uint8_t *p, int stride)
{
    long e = 0;
    for (int by = 0; by < 16; by += 4)
        for (int bx = 0; bx < 16; bx += 8) {
            const uint8_t *q = p + by * stride + bx;
            uint8x8_t a0 = vld1_u8(q), a1 = vld1_u8(q + stride);
            uint8x8_t a2 = vld1_u8(q + 2 * stride), a3 = vld1_u8(q + 3 * stride);
            uint16x8_t dcv = vaddq_u16(vaddl_u8(a0, a1), vaddl_u8(a2, a3));
            int dcl = (int)vaddlv_u16(vget_low_u16(dcv));
            int dch = (int)vaddlv_u16(vget_high_u16(dcv));
            uint16x8_t m = satd_4rows_half(
                vreinterpretq_s16_u16(vmovl_u8(a0)), vreinterpretq_s16_u16(vmovl_u8(a1)),
                vreinterpretq_s16_u16(vmovl_u8(a2)), vreinterpretq_s16_u16(vmovl_u8(a3)));
            long sl = 2 * (long)vaddlv_u16(vget_low_u16(m));
            long sh = 2 * (long)vaddlv_u16(vget_high_u16(m));
            int fl = 16 * ((dcl + 8) >> 4), fh = 16 * ((dch + 8) >> 4);
            e += sl - dcl + (dcl < fl ? fl - dcl : dcl - fl);
            e += sh - dch + (dch < fh ? fh - dch : dch - fh);
        }
    return e;
}

/* Full 4x4 Hadamard of two side-by-side tiles (lanes 0-3 = left tile, 4-7 =
 * right), i.e. satd_4rows_half's dataflow with the final butterfly actually
 * taken instead of collapsed into the max identity. The returned four vectors
 * hold each tile's sixteen coefficients in a fixed permutation -- which one is
 * irrelevant, but it is the SAME permutation for every call, which is what lets
 * the 8x8 combine below pair lanes across tiles.
 *
 * Lane 0 (and lane 4) of val[0] is the tile's DC, == its pixel sum.
 * Bounds for 8-bit input: |vertical| <= 1020, |stage 1| <= 2040, |coef| <= 4080. */
static inline int16x8x4_t tile4_coefs(uint8x8_t r0, uint8x8_t r1,
                                      uint8x8_t r2, uint8x8_t r3)
{
    /* the widening lives in the first butterfly: a-b in u16 wraparound IS the
 * two's-complement int16 of the signed difference for 8-bit inputs */
    int16x8_t t0 = vreinterpretq_s16_u16(vaddl_u8(r0, r1));
    int16x8_t t1 = vreinterpretq_s16_u16(vsubl_u8(r0, r1));
    int16x8_t t2 = vreinterpretq_s16_u16(vaddl_u8(r2, r3));
    int16x8_t t3 = vreinterpretq_s16_u16(vsubl_u8(r2, r3));
    int16x8_t h0 = vaddq_s16(t0, t2), h2 = vsubq_s16(t0, t2);
    int16x8_t h1 = vaddq_s16(t1, t3), h3 = vsubq_s16(t1, t3);

    int16x8x2_t p01 = vtrnq_s16(h0, h1);
    int16x8x2_t p23 = vtrnq_s16(h2, h3);
    int16x8_t s01 = vaddq_s16(p01.val[0], p01.val[1]);
    int16x8_t d01 = vsubq_s16(p01.val[0], p01.val[1]);
    int16x8_t s23 = vaddq_s16(p23.val[0], p23.val[1]);
    int16x8_t d23 = vsubq_s16(p23.val[0], p23.val[1]);

    int32x4x2_t q0 = vtrnq_s32(vreinterpretq_s32_s16(s01),
                               vreinterpretq_s32_s16(d01));
    int32x4x2_t q1 = vtrnq_s32(vreinterpretq_s32_s16(s23),
                               vreinterpretq_s32_s16(d23));
    int16x8_t a0 = vreinterpretq_s16_s32(q0.val[0]);
    int16x8_t a1 = vreinterpretq_s16_s32(q0.val[1]);
    int16x8_t b0 = vreinterpretq_s16_s32(q1.val[0]);
    int16x8_t b1 = vreinterpretq_s16_s32(q1.val[1]);

    int16x8x4_t c;
    c.val[0] = vaddq_s16(a0, a1); c.val[1] = vsubq_s16(a0, a1);
    c.val[2] = vaddq_s16(b0, b1); c.val[3] = vsubq_s16(b0, b1);
    return c;
}

#define TEX_ABS(v) vreinterpretq_u16_s16(vabsq_s16(v))
/* the two vectors' low / high 64-bit halves gathered into one register each */
#define TEX_LO2(a, b) vreinterpretq_u16_u64(vtrn1q_u64(vreinterpretq_u64_u16(a), \
                                                       vreinterpretq_u64_u16(b)))
#define TEX_HI2(a, b) vreinterpretq_u16_u64(vtrn2q_u64(vreinterpretq_u64_u16(a), \
                                                       vreinterpretq_u64_u16(b)))

/* Both psy-RD texture terms of a 16x16 in one pass over the pixels.
 *
 * The 8x8 Walsh-Hadamard factors as W2 (x) W4: transform the four 4x4 quadrant
 * tiles, then apply one 2x2 butterfly per coefficient position across them. The
 * tile pass here keeps its final butterfly (tile4_coefs) so those coefficients
 * exist; the tile-row butterfly is then a plain vector add/sub, and the
 * tile-COLUMN butterfly pairs lane l with lane l+4 of the same vector -- where
 * the |a+b| + |a-b| = 2*max(|a|,|b|) identity comes back, applied to a vector
 * and its own half-swap. Summing max(|x|, |swap x|) over all eight lanes yields
 * exactly the eight coefficient magnitudes of that butterfly (each lane pair
 * contributes its sum and its difference once), so the four (row-sum, row-diff)
 * vector pairs cover all 64 coefficients of the 8x8.
 *
 * Net: the pixels are loaded and transformed once for both terms instead of
 * twice, and the 8x8's DC (its pixel sum) falls out as the four tile DCs the
 * 4x4 term already extracted. Bit-exact with texture_ac48_c_16x16.
 *
 * Both magnitude sums are whole-block quantities -- e4 subtracts each tile's DC
 * and e8 subtracts each 8x8's DC, and the sixteen tile DCs ARE the four 8x8 DCs
 * regrouped -- so the per-tile and per-quadrant horizontal reductions collapse
 * into two whole-block accumulators plus one scalar DC total. Only the flat-mean
 * correction stays per tile, and it needs the DC alone, not the tile's sum.
 *
 * Two shapes keep the instruction count down: SABA folds the tile term's abs and
 * accumulate into one op, and the column maxes of two butterfly vectors are
 * taken together by pairing their 64-bit halves (trn1/trn2 .2d) instead of
 * half-swapping each vector against itself -- which leaves each lane pair
 * counted once rather than twice, hence the x2 on the 8x8 total.
 *
 * Accumulator bounds, 8-bit: a tile coefficient is <= 4080, so a quadrant's
 * eight tile-coefficient vectors fold to <= 32640 per s16 SABA lane; a tile-row
 * butterfly result is <= 8160, so the four paired max vectors fold to <= 32640
 * likewise. Each quadrant's lanes then widen into the u32 block accumulators. */
void y264_texture_ac48_16x16_neon(const uint8_t *p, int stride, long out[2])
{
    const int16x8_t zero = vdupq_n_s16(0);
    uint32x4_t a4 = vdupq_n_u32(0), a8 = vdupq_n_u32(0);
    long dcsum = 0, corr = 0;

    for (int qy = 0; qy < 16; qy += 8) {
        for (int qx = 0; qx < 16; qx += 8) {
            const uint8_t *q = p + qy * stride + qx;
            uint8x8_t v[8];
            for (int i = 0; i < 8; i++) v[i] = vld1_u8(q + i * stride);

            int16x8x4_t ct = tile4_coefs(v[0], v[1], v[2], v[3]);
            int16x8x4_t cb = tile4_coefs(v[4], v[5], v[6], v[7]);

            /* SATD-support term: every tile coefficient's magnitude, plus each
 * tile's DC re-scored against its own rounded mean. */
            int16x8_t t4 = zero;
            for (int k = 0; k < 4; k++) {
                t4 = vabaq_s16(t4, ct.val[k], zero);
                t4 = vabaq_s16(t4, cb.val[k], zero);
            }
            a4 = vpadalq_u16(a4, vreinterpretq_u16_s16(t4));

            int dc[4] = { vgetq_lane_s16(ct.val[0], 0), vgetq_lane_s16(ct.val[0], 4),
                          vgetq_lane_s16(cb.val[0], 0), vgetq_lane_s16(cb.val[0], 4) };
            for (int t = 0; t < 4; t++) {
                int f = 16 * ((dc[t] + 8) >> 4);
                corr += dc[t] < f ? f - dc[t] : dc[t] - f;
                dcsum += dc[t];
            }

            /* SA8D-support term: the 2x2 tile butterfly on those coefficients. */
            uint16x8_t m8 = vdupq_n_u16(0);
            for (int k = 0; k < 4; k += 2) {
                uint16x8_t s0 = TEX_ABS(vaddq_s16(ct.val[k],   cb.val[k]));
                uint16x8_t s1 = TEX_ABS(vaddq_s16(ct.val[k+1], cb.val[k+1]));
                uint16x8_t d0 = TEX_ABS(vsubq_s16(ct.val[k],   cb.val[k]));
                uint16x8_t d1 = TEX_ABS(vsubq_s16(ct.val[k+1], cb.val[k+1]));
                m8 = vaddq_u16(m8, vmaxq_u16(TEX_LO2(s0, s1), TEX_HI2(s0, s1)));
                m8 = vaddq_u16(m8, vmaxq_u16(TEX_LO2(d0, d1), TEX_HI2(d0, d1)));
            }
            a8 = vpadalq_u16(a8, m8);
        }
    }
    out[0] = (long)vaddvq_u32(a4) - dcsum + corr;
    out[1] = 2 * (long)vaddvq_u32(a8) - dcsum;
}

/* Pixel sum and sum of squares of a 16x16 , for the AQ and
 * mb-tree variance grids. Both sums are exact integers so lane order is free.
 * Bounds: the u16 sum lanes hold <= 16*2*255 = 8160 and the u32 square lanes
 * <= 16*2*65025 = 2080800, well inside their types. */
void y264_var_16x16_neon(const uint8_t *p, int stride, uint32_t out[2])
{
    uint16x8_t s1 = vdupq_n_u16(0);
    uint32x4_t s2a = vdupq_n_u32(0), s2b = vdupq_n_u32(0);
    for (int y = 0; y < 16; y++) {
        uint8x16_t v = vld1q_u8(p + y * stride);
        s1  = vpadalq_u8(s1, v);
        s2a = vpadalq_u16(s2a, vmull_u8(vget_low_u8(v), vget_low_u8(v)));
        s2b = vpadalq_u16(s2b, vmull_high_u8(v, v));
    }
    out[0] = vaddlvq_u16(s1);
    out[1] = vaddvq_u32(vaddq_u32(s2a, s2b));
}

/* FEAT_DotProd: UDOT against all-ones sums the bytes and UDOT against itself
 * sums the squares, two ops per row. Bit-exact with the plain-NEON kernel. */
Y264_DOTPROD_ATTR
void y264_var_16x16_neon_dotprod(const uint8_t *p, int stride, uint32_t out[2])
{
    const uint8x16_t ones = vdupq_n_u8(1);
    uint32x4_t s1a = vdupq_n_u32(0), s1b = vdupq_n_u32(0);
    uint32x4_t s2a = vdupq_n_u32(0), s2b = vdupq_n_u32(0);
    for (int y = 0; y < 16; y += 2) {
        uint8x16_t v0 = vld1q_u8(p + y * stride);
        uint8x16_t v1 = vld1q_u8(p + (y + 1) * stride);
        s1a = vdotq_u32(s1a, v0, ones); s2a = vdotq_u32(s2a, v0, v0);
        s1b = vdotq_u32(s1b, v1, ones); s2b = vdotq_u32(s2b, v1, v1);
    }
    out[0] = vaddvq_u32(vaddq_u32(s1a, s1b));
    out[1] = vaddvq_u32(vaddq_u32(s2a, s2b));
}

/* ---- Fused Intra4x4 all-modes cost --------------------------------------
 *
 * The nine-mode search builds nine 4x4 predictions and SATDs each against the
 * source. Both halves collapse.
 *
 * PREDICTION. Every directional Intra4x4 sample is one of three filters of the
 * flattened edge array E = { l3, l3, l2, l1, l0, tl, t0..t7, t7, t7, ... }:
 *
 * F[j] = (E[j] + 2*E[j+1] + E[j+2] + 2) >> 2 (the 121 filter)
 * H[j] = (E[j] + E[j+1] + 1) >> 1 (the pairwise average)
 * E[j] (the raw sample)
 *
 * computed once, sixteen lanes wide, via the exact identity
 * (a + 2b + c + 2) >> 2 == ((a + c) >> 1 + b + 1) >> 1 (uhadd then urhadd),
 * as in predict_neon.c. Which of the three, at which index, is then a pure
 * function of (mode, x, y) -- so one TBL over { F, H, E, dc } builds a whole
 * prediction per mode, and the two positional special cases fall out of the
 * array's padding instead of a branch: the duplicated l3 at E[0] makes F[0]
 * the (l2 + 3*l3 + 2) >> 2 that HU's z==5 sample wants, and the replicated t7
 * tail makes F[12] the (t6 + 3*t7 + 2) >> 2 that DDL's corner wants. The index
 * tables were generated from the reference formulas and checked against them
 * over every edge configuration, then checked again exhaustively by checkasm
 * against the per-mode C builder.
 *
 * COST. satd_4rows_half scores two side-by-side 4x4 blocks per 8-lane
 * Hadamard, so the modes go through in pairs: a 32-bit TRN of two predictions
 * interleaves their rows (a prediction's four u32 lanes ARE its four rows),
 * and the same TRN of the source against itself duplicates the source row
 * under both. Nine modes cost five passes, four rows of subtract each. */
static const uint8_t i4_x9_idx[9][16] = {
    { 38, 39, 40, 41, 38, 39, 40, 41, 38, 39, 40, 41, 38, 39, 40, 41 }, /* V */
    { 36, 36, 36, 36, 35, 35, 35, 35, 34, 34, 34, 34, 33, 33, 33, 33 }, /* H */
    { 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48 }, /* DC */
    {  6,  7,  8,  9,  7,  8,  9, 10,  8,  9, 10, 11,  9, 10, 11, 12 }, /* DDL */
    {  4,  5,  6,  7,  3,  4,  5,  6,  2,  3,  4,  5,  1,  2,  3,  4 }, /* DDR */
    { 21, 22, 23, 24,  4,  5,  6,  7,  3, 21, 22, 23,  2,  4,  5,  6 }, /* VR */
    { 20,  4,  5,  6, 19,  3, 20,  4, 18,  2, 19,  3, 17,  1, 18,  2 }, /* HD */
    { 22, 23, 24, 25,  6,  7,  8,  9, 23, 24, 25, 26,  7,  8,  9, 10 }, /* VL */
    { 19,  2, 18,  1, 18,  1, 17,  0, 17,  0, 33, 33, 33, 33, 33, 33 }, /* HU */
};

/* One 4x4 source block as four u32 lanes (one row per lane). */
static inline uint32x4_t load_rows4(const uint8_t *p, int stride)
{
    uint32x4_t v = vdupq_n_u32(0);
    v = vld1q_lane_u32((const uint32_t *)(const void *)(p + 0 * stride), v, 0);
    v = vld1q_lane_u32((const uint32_t *)(const void *)(p + 1 * stride), v, 1);
    v = vld1q_lane_u32((const uint32_t *)(const void *)(p + 2 * stride), v, 2);
    v = vld1q_lane_u32((const uint32_t *)(const void *)(p + 3 * stride), v, 3);
    return v;
}

/* SATD of two predictions against one source, in one 8-lane pass. */
static inline void satd_pair4x4(uint8x16_t pa, uint8x16_t pb,
                                uint8x16_t sx, uint8x16_t sy,
                                int *ca, int *cb)
{
    uint32x4_t a = vreinterpretq_u32_u8(pa), b = vreinterpretq_u32_u8(pb);
    uint8x16_t x = vreinterpretq_u8_u32(vtrn1q_u32(a, b));   /* rows 0 and 2 */
    uint8x16_t y = vreinterpretq_u8_u32(vtrn2q_u32(a, b));   /* rows 1 and 3 */
    int16x8_t d0 = vreinterpretq_s16_u16(vsubl_u8(vget_low_u8(x), vget_low_u8(sx)));
    int16x8_t d1 = vreinterpretq_s16_u16(vsubl_u8(vget_low_u8(y), vget_low_u8(sy)));
    int16x8_t d2 = vreinterpretq_s16_u16(vsubl_high_u8(x, sx));
    int16x8_t d3 = vreinterpretq_s16_u16(vsubl_high_u8(y, sy));
    uint16x8_t m = satd_4rows_half(d0, d1, d2, d3);
    *ca = 2 * (int)vaddlv_u16(vget_low_u16(m));
    *cb = 2 * (int)vaddlv_u16(vget_high_u16(m));
}

void y264_intra4x4_x9_neon(const uint8_t *src, int ss, const uint8_t *rec, int rs,
                           int ht, int hl, int htl, int htr, int costs[9])
{
    uint8_t t[8], l[4], E[24];
    int tl = 0, st = 0, sl = 0;

    for (int i = 0; i < 4; i++) {
        t[i] = ht ? rec[-rs + i] : 0;
        l[i] = hl ? rec[i * rs - 1] : 0;
        st += t[i];
        sl += l[i];
    }
    for (int i = 4; i < 8; i++)
        t[i] = htr ? rec[-rs + i] : t[3];
    if (htl)
        tl = rec[-rs - 1];

    int dc;
    if (ht && hl) dc = (st + sl + 4) >> 3;
    else if (ht)  dc = (st + 2) >> 2;
    else if (hl)  dc = (sl + 2) >> 2;
    else          dc = 1 << (Y264_BIT_DEPTH - 1);

    E[0] = l[3]; E[1] = l[3]; E[2] = l[2]; E[3] = l[1]; E[4] = l[0];
    E[5] = (uint8_t)tl;
    for (int i = 0; i < 8; i++)
        E[6 + i] = t[i];
    for (int i = 14; i < 24; i++)
        E[i] = t[7];

    uint8x16_t e0 = vld1q_u8(E), e1 = vld1q_u8(E + 1), e2 = vld1q_u8(E + 2);
    uint8x16x4_t tab;
    tab.val[0] = vrhaddq_u8(vhaddq_u8(e0, e2), e1);     /* F[0..15] -> 0..15 */
    tab.val[1] = vrhaddq_u8(e0, e1);                    /* H[0..15] -> 16..31 */
    tab.val[2] = e0;                                    /* E[0..15] -> 32..47 */
    tab.val[3] = vdupq_n_u8((uint8_t)dc);               /* DC -> 48..63 */

    uint8x16_t p[9];
    for (int m = 0; m < 9; m++)
        p[m] = vqtbl4q_u8(tab, vld1q_u8(i4_x9_idx[m]));

    uint32x4_t s = load_rows4(src, ss);
    uint8x16_t sx = vreinterpretq_u8_u32(vtrn1q_u32(s, s));   /* rows 0, 2 */
    uint8x16_t sy = vreinterpretq_u8_u32(vtrn2q_u32(s, s));   /* rows 1, 3 */

    satd_pair4x4(p[0], p[1], sx, sy, &costs[0], &costs[1]);
    satd_pair4x4(p[2], p[3], sx, sy, &costs[2], &costs[3]);
    satd_pair4x4(p[4], p[5], sx, sy, &costs[4], &costs[5]);
    satd_pair4x4(p[6], p[7], sx, sy, &costs[6], &costs[7]);
    satd_pair4x4(p[8], p[8], sx, sy, &costs[8], &costs[8]);
}


/* ---- fused I16x16 V/H/DC cost -----------------
 * The screen builds a 256-byte prediction per mode and runs a full 16x16 SATD
 * over it. Every one of those predictions is a broadcast: V repeats the top
 * row down the block, H repeats one left sample across a row, DC is a single
 * value. So none of them has to reach memory -- the residual for all three
 * modes is formed in registers off ONE pass of source loads, and the three
 * SATDs share the loads and the loop.
 *
 * Exactness is structural: satd_4rows_half is the same reduction the shipped
 * 8x8 kernel uses, fed the same residual values the materialised prediction
 * would have produced, and the 16x16 SATD is the sum over eight (4 rows x 8
 * columns) groups exactly as y264_satd_16x16_neon_ded sums four 8x8 blocks.
 * Callers that do not want a mode (no top row, no left column) pass any valid
 * 16-byte pointer and ignore that output; nothing is read out of bounds.
 */
void y264_intra_satd_x3_16x16_neon(const uint8_t *src, int ss, const uint8_t *top,
                                   const uint8_t *left, int dc, int costs[3])
{
    uint8x16_t vt = vld1q_u8(top);
    uint8x16_t vdc = vdupq_n_u8((uint8_t)dc);
    /* satd_4rows_half returns up to 16320 per lane, so at most two may be
 * folded in u16 before widening -- one (4 rows x 8 columns) pair. */
    uint32x4_t av = vdupq_n_u32(0), ah = vdupq_n_u32(0), ad = vdupq_n_u32(0);

    for (int y = 0; y < 16; y += 4) {
        uint8x16_t s0 = vld1q_u8(src + (y + 0) * ss);
        uint8x16_t s1 = vld1q_u8(src + (y + 1) * ss);
        uint8x16_t s2 = vld1q_u8(src + (y + 2) * ss);
        uint8x16_t s3 = vld1q_u8(src + (y + 3) * ss);
        uint8x16_t h0 = vdupq_n_u8(left[y + 0]), h1 = vdupq_n_u8(left[y + 1]);
        uint8x16_t h2 = vdupq_n_u8(left[y + 2]), h3 = vdupq_n_u8(left[y + 3]);

#define ROWD(lo_, s_, p_) (lo_ ? vreinterpretq_s16_u16(vsubl_u8(vget_low_u8(s_), vget_low_u8(p_))) \
                               : vreinterpretq_s16_u16(vsubl_high_u8(s_, p_)))
        /* left 8 columns, then right 8: two 4x4 tiles per satd_4rows_half */
        av = vpadalq_u16(av,
            vaddq_u16(satd_4rows_half(ROWD(1, s0, vt), ROWD(1, s1, vt),
                                      ROWD(1, s2, vt), ROWD(1, s3, vt)),
                      satd_4rows_half(ROWD(0, s0, vt), ROWD(0, s1, vt),
                                      ROWD(0, s2, vt), ROWD(0, s3, vt))));
        ah = vpadalq_u16(ah,
            vaddq_u16(satd_4rows_half(ROWD(1, s0, h0), ROWD(1, s1, h1),
                                      ROWD(1, s2, h2), ROWD(1, s3, h3)),
                      satd_4rows_half(ROWD(0, s0, h0), ROWD(0, s1, h1),
                                      ROWD(0, s2, h2), ROWD(0, s3, h3))));
        ad = vpadalq_u16(ad,
            vaddq_u16(satd_4rows_half(ROWD(1, s0, vdc), ROWD(1, s1, vdc),
                                      ROWD(1, s2, vdc), ROWD(1, s3, vdc)),
                      satd_4rows_half(ROWD(0, s0, vdc), ROWD(0, s1, vdc),
                                      ROWD(0, s2, vdc), ROWD(0, s3, vdc))));
#undef ROWD
    }
    costs[0] = 2 * (int)vaddvq_u32(av);
    costs[1] = 2 * (int)vaddvq_u32(ah);
    costs[2] = 2 * (int)vaddvq_u32(ad);
}

#endif /* __aarch64__ */
