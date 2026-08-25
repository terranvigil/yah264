/*
 * transform_neon.c - aarch64 NEON quantization kernels
 * Copyright (c) 2026, the next264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Bit-exact with the scalar quant/dequant in transform.c (same MF/V tables and
 * rounding), validated by tests/test_transform.c and the recon-match gate.
 */
#if defined(__aarch64__)
#include <arm_neon.h>
#include <stdint.h>

/* The flat-CQM multiplier and scale rows come in fully expanded, one entry per
 * raster position, from transform.c's per-QP row tables (built at open by the
 * same expressions the scalar path uses). Expanding the six-value category
 * tables onto the stack per call instead would cost sixteen to sixty-four
 * scalar stores in front of four to sixteen vector iterations. Only the
 * flat-CQM path dispatches here; scaling-matrix streams stay scalar. */

/* Shared 4x4 forward-quant core with an explicit rounding bias `f` (callers
 * pass the legacy (1<<qbits)/3|6 or the f64<<qbits>>6 trellis-seed bias --
 * identical arithmetic either way). Bit-exact with the scalar path: the
 * product |coef|*mf <= 32767*20972 < 2^31 stays in int32. */
void n264_quant_4x4_fneon(const int16_t coef[16], int16_t lev[16], int qp, int f,
                          const int32_t mfrow[16])
{
    int qbits = 15 + qp / 6;
    int32x4_t vf = vdupq_n_s32(f);
    int32x4_t vsh = vdupq_n_s32(-qbits);          /* negative = right shift */
    for (int g = 0; g < 16; g += 4) {
        int32x4_t c = vmovl_s16(vld1_s16(coef + g));
        int32x4_t a = vabsq_s32(c);
        int32x4_t mf = vld1q_s32(mfrow + g);
        int32x4_t q = vshlq_s32(vaddq_s32(vmulq_s32(a, mf), vf), vsh);
        uint32x4_t neg = vcltq_s32(c, vdupq_n_s32(0));
        q = vbslq_s32(neg, vnegq_s32(q), q);
        vst1_s16(lev + g, vqmovn_s32(q));
    }
}

void n264_quant_4x4_neon(const int16_t coef[16], int16_t lev[16], int qp, int intra,
                         const int32_t mfrow[16])
{
    int qbits = 15 + qp / 6;
    n264_quant_4x4_fneon(coef, lev, qp, (1 << qbits) / (intra ? 3 : 6), mfrow);
}

/* 8x8 forward quant with explicit bias. */
void n264_quant_8x8_fneon(const int16_t coef[64], int16_t lev[64], int qp, int f,
                          const int32_t mfrow[64])
{
    int qbits = 16 + qp / 6;
    int32x4_t vf = vdupq_n_s32(f);
    int32x4_t vsh = vdupq_n_s32(-qbits);
    for (int g = 0; g < 64; g += 8) {
        int16x8_t c16 = vld1q_s16(coef + g);
        int32x4_t clo = vmovl_s16(vget_low_s16(c16));
        int32x4_t chi = vmovl_s16(vget_high_s16(c16));
        int32x4_t qlo = vshlq_s32(vaddq_s32(vmulq_s32(vabsq_s32(clo), vld1q_s32(mfrow + g)), vf), vsh);
        int32x4_t qhi = vshlq_s32(vaddq_s32(vmulq_s32(vabsq_s32(chi), vld1q_s32(mfrow + g + 4)), vf), vsh);
        qlo = vbslq_s32(vcltq_s32(clo, vdupq_n_s32(0)), vnegq_s32(qlo), qlo);
        qhi = vbslq_s32(vcltq_s32(chi, vdupq_n_s32(0)), vnegq_s32(qhi), qhi);
        vst1q_s16(lev + g, vcombine_s16(vqmovn_s32(qlo), vqmovn_s32(qhi)));
    }
}

void n264_dequant_4x4_neon(const int16_t lev[16], int16_t coef[16], int qp,
                           const int32_t lsrow[16])
{
    int shift = qp / 6;

    if (shift >= 4) {
        int32x4_t vs = vdupq_n_s32(shift - 4);    /* positive = left shift */
        for (int g = 0; g < 16; g += 4) {
            int32x4_t l = vmovl_s16(vld1_s16(lev + g));
            int32x4_t d = vshlq_s32(vmulq_s32(l, vld1q_s32(lsrow + g)), vs);
            vst1_s16(coef + g, vqmovn_s32(d));
        }
    } else {
        int32x4_t vr = vdupq_n_s32(1 << (3 - shift));
        int32x4_t vs = vdupq_n_s32(-(4 - shift));  /* arithmetic right shift */
        for (int g = 0; g < 16; g += 4) {
            int32x4_t l = vmovl_s16(vld1_s16(lev + g));
            int32x4_t p = vaddq_s32(vmulq_s32(l, vld1q_s32(lsrow + g)), vr);
            vst1_s16(coef + g, vqmovn_s32(vshlq_s32(p, vs)));
        }
    }
}

/* 8x8 dequant (High profile). Same structure as the 4x4. */
void n264_dequant_8x8_neon(const int16_t lev[64], int16_t coef[64], int qp,
                           const int32_t lsrow[64])
{
    int shift = qp / 6;
    const int32_t *ls64 = lsrow;

    if (shift >= 6) {
        int32x4_t vs = vdupq_n_s32(shift - 6);
        for (int g = 0; g < 64; g += 4) {
            int32x4_t l = vmovl_s16(vld1_s16(lev + g));
            int32x4_t d = vshlq_s32(vmulq_s32(l, vld1q_s32(ls64 + g)), vs);
            vst1_s16(coef + g, vqmovn_s32(d));
        }
    } else {
        int32x4_t vr = vdupq_n_s32(1 << (5 - shift));
        int32x4_t vs = vdupq_n_s32(-(6 - shift));
        for (int g = 0; g < 64; g += 4) {
            int32x4_t l = vmovl_s16(vld1_s16(lev + g));
            int32x4_t p = vaddq_s32(vmulq_s32(l, vld1q_s32(ls64 + g)), vr);
            vst1_s16(coef + g, vqmovn_s32(vshlq_s32(p, vs)));
        }
    }
}

/* ---- 4x4 / 8x8 integer transforms --------------------------------------- */

/* 4x4 int16 transpose in place. */
#define TRN4_S16(r0, r1, r2, r3) do {                                        \
    int16x4x2_t p01_ = vtrn_s16(r0, r1);                                     \
    int16x4x2_t p23_ = vtrn_s16(r2, r3);                                     \
    int32x2x2_t q02_ = vtrn_s32(vreinterpret_s32_s16(p01_.val[0]),           \
                                vreinterpret_s32_s16(p23_.val[0]));          \
    int32x2x2_t q13_ = vtrn_s32(vreinterpret_s32_s16(p01_.val[1]),           \
                                vreinterpret_s32_s16(p23_.val[1]));          \
    r0 = vreinterpret_s16_s32(q02_.val[0]);                                  \
    r1 = vreinterpret_s16_s32(q13_.val[0]);                                  \
    r2 = vreinterpret_s16_s32(q02_.val[1]);                                  \
    r3 = vreinterpret_s16_s32(q13_.val[1]);                                  \
} while (0)

/* 4x4 int32 transpose in place. */
#define TRN4_S32(r0, r1, r2, r3) do {                                        \
    int32x4x2_t p01_ = vtrnq_s32(r0, r1);                                    \
    int32x4x2_t p23_ = vtrnq_s32(r2, r3);                                    \
    r0 = vreinterpretq_s32_s64(vtrn1q_s64(vreinterpretq_s64_s32(p01_.val[0]),\
                                          vreinterpretq_s64_s32(p23_.val[0])));\
    r2 = vreinterpretq_s32_s64(vtrn2q_s64(vreinterpretq_s64_s32(p01_.val[0]),\
                                          vreinterpretq_s64_s32(p23_.val[0])));\
    r1 = vreinterpretq_s32_s64(vtrn1q_s64(vreinterpretq_s64_s32(p01_.val[1]),\
                                          vreinterpretq_s64_s32(p23_.val[1])));\
    r3 = vreinterpretq_s32_s64(vtrn2q_s64(vreinterpretq_s64_s32(p01_.val[1]),\
                                          vreinterpretq_s64_s32(p23_.val[1])));\
} while (0)

/* 4x4 forward core transform. 16-bit lanes are exact for |input| <= 255
 * (pixel diffs or reconstructed pixels; per-pass gains 6x then 6x stay under
 * int16). Transpose-butterfly-transpose-butterfly implements exactly the
 * scalar row/column passes. */
static inline void fdct4x4_rows_neon(int16x4_t r0, int16x4_t r1,
                                     int16x4_t r2, int16x4_t r3,
                                     int16_t coef[16])
{
    TRN4_S16(r0, r1, r2, r3);
    for (int pass = 0; pass < 2; pass++) {
        int16x4_t z0 = vadd_s16(r0, r3), z3 = vsub_s16(r0, r3);
        int16x4_t z1 = vadd_s16(r1, r2), z2 = vsub_s16(r1, r2);
        r0 = vadd_s16(z0, z1);
        r1 = vadd_s16(vadd_s16(z3, z3), z2);
        r2 = vsub_s16(z0, z1);
        r3 = vsub_s16(z3, vadd_s16(z2, z2));
        if (pass == 0)
            TRN4_S16(r0, r1, r2, r3);
    }
    vst1_s16(coef + 0, r0); vst1_s16(coef + 4, r1);
    vst1_s16(coef + 8, r2); vst1_s16(coef + 12, r3);
}

void n264_fdct4x4_neon(const int16_t diff[16], int16_t coef[16])
{
    fdct4x4_rows_neon(vld1_s16(diff + 0), vld1_s16(diff + 4),
                      vld1_s16(diff + 8), vld1_s16(diff + 12), coef);
}

/* Two 4-pixel rows loaded into one d-register via exact 4-byte reads (never
 * touches bytes past the 4x4 block -- src can sit at the frame edge). */
static inline uint8x8_t ld_4x2_u8(const uint8_t *p, int stride)
{
    uint32_t w0, w1;
    __builtin_memcpy(&w0, p, 4);
    __builtin_memcpy(&w1, p + stride, 4);
    uint32x2_t v = vdup_n_u32(w0);
    return vreinterpret_u8_u32(vset_lane_u32(w1, v, 1));
}

/* Fused src - pred subtract + 4x4 forward DCT. The usubl residual equals the
 * scalar (dctcoef)(src - pred) diff exactly, so this is bit-exact with the
 * unfused diff-build + fdct sequence. */
void n264_sub4x4_dct_neon(int16_t coef[16], const uint8_t *src, int ss,
                          const uint8_t *pred, int ps)
{
    int16x8_t d01 = vreinterpretq_s16_u16(
        vsubl_u8(ld_4x2_u8(src, ss), ld_4x2_u8(pred, ps)));
    int16x8_t d23 = vreinterpretq_s16_u16(
        vsubl_u8(ld_4x2_u8(src + 2 * ss, ss), ld_4x2_u8(pred + 2 * ps, ps)));
    fdct4x4_rows_neon(vget_low_s16(d01), vget_high_s16(d01),
                      vget_low_s16(d23), vget_high_s16(d23), coef);
}

/* Two independent 4x4 int16 transposes in one q-register: 16-bit lanes 0-3 are
 * block A, 4-7 block B. Both TRN stages pair lanes within a half (trn16 takes
 * (0,0,2,2) then (4,4,6,6); trn32 takes 32-bit (0,0,2,2), which is 16-bit
 * (0..3, 0..3, 4..7, 4..7)), so the halves never mix. */
#define TRN4_S16Q(r0, r1, r2, r3) do {                                       \
    int16x8x2_t p01_ = vtrnq_s16(r0, r1);                                    \
    int16x8x2_t p23_ = vtrnq_s16(r2, r3);                                    \
    int32x4x2_t q02_ = vtrnq_s32(vreinterpretq_s32_s16(p01_.val[0]),         \
                                 vreinterpretq_s32_s16(p23_.val[0]));        \
    int32x4x2_t q13_ = vtrnq_s32(vreinterpretq_s32_s16(p01_.val[1]),         \
                                 vreinterpretq_s32_s16(p23_.val[1]));        \
    r0 = vreinterpretq_s16_s32(q02_.val[0]);                                 \
    r1 = vreinterpretq_s16_s32(q13_.val[0]);                                 \
    r2 = vreinterpretq_s16_s32(q02_.val[1]);                                 \
    r3 = vreinterpretq_s16_s32(q13_.val[1]);                                 \
} while (0)

/* Two side-by-side 4x4 forward transforms, same butterflies as
 * fdct4x4_rows_neon on the full 8 lanes -- the single-block kernel leaves half
 * the datapath idle, and the 4x4 residual blocks of a macroblock always come
 * in horizontally adjacent pairs. */
static inline void fdct4x4_dual_neon(int16x8_t r0, int16x8_t r1,
                                     int16x8_t r2, int16x8_t r3,
                                     int16_t ca[16], int16_t cb[16])
{
    TRN4_S16Q(r0, r1, r2, r3);
    for (int pass = 0; pass < 2; pass++) {
        int16x8_t z0 = vaddq_s16(r0, r3), z3 = vsubq_s16(r0, r3);
        int16x8_t z1 = vaddq_s16(r1, r2), z2 = vsubq_s16(r1, r2);
        r0 = vaddq_s16(z0, z1);
        r1 = vaddq_s16(vaddq_s16(z3, z3), z2);
        r2 = vsubq_s16(z0, z1);
        r3 = vsubq_s16(z3, vaddq_s16(z2, z2));
        if (pass == 0)
            TRN4_S16Q(r0, r1, r2, r3);
    }
    vst1_s16(ca + 0, vget_low_s16(r0));  vst1_s16(cb + 0, vget_high_s16(r0));
    vst1_s16(ca + 4, vget_low_s16(r1));  vst1_s16(cb + 4, vget_high_s16(r1));
    vst1_s16(ca + 8, vget_low_s16(r2));  vst1_s16(cb + 8, vget_high_s16(r2));
    vst1_s16(ca + 12, vget_low_s16(r3)); vst1_s16(cb + 12, vget_high_s16(r3));
}

/* Batched forward transform of an nbw x nbh block grid (x264's <reference-internal> /
 * sub8x8_dct), raster block order, two horizontally adjacent blocks at a time.
 * The 8-byte loads stay inside the grid, so an edge macroblock is safe. */
void n264_sub_dct4_blocks_neon(int16_t (*coef)[16], int nbw, int nbh,
                               const uint8_t *src, int ss,
                               const uint8_t *pred, int ps)
{
    for (int by = 0; by < nbh; by++) {
        const uint8_t *s = src + (by * 4) * ss;
        const uint8_t *p = pred + (by * 4) * ps;
        for (int bx = 0; bx < nbw; bx += 2) {
#define DIFF8P(y) vreinterpretq_s16_u16(vsubl_u8(vld1_u8(s + (y) * ss + bx * 4), \
                                                 vld1_u8(p + (y) * ps + bx * 4)))
            fdct4x4_dual_neon(DIFF8P(0), DIFF8P(1), DIFF8P(2), DIFF8P(3),
                              coef[by * nbw + bx], coef[by * nbw + bx + 1]);
#undef DIFF8P
        }
    }
}

/* 4x4 inverse transform core. 32-bit lanes, so it is exact for the full int16
 * coefficient domain (matching the scalar int arithmetic bit for bit); the
 * final narrowing truncates like the scalar (dctcoef) cast. Outputs the four
 * residual rows as int16x4. */
static inline void idct4x4_core_neon(const int16_t coef[16], int16x4_t out[4])
{
    int32x4_t r0 = vmovl_s16(vld1_s16(coef + 0));
    int32x4_t r1 = vmovl_s16(vld1_s16(coef + 4));
    int32x4_t r2 = vmovl_s16(vld1_s16(coef + 8));
    int32x4_t r3 = vmovl_s16(vld1_s16(coef + 12));
    TRN4_S32(r0, r1, r2, r3);
    for (int pass = 0; pass < 2; pass++) {
        int32x4_t i0 = vaddq_s32(r0, r2);
        int32x4_t i1 = vsubq_s32(r0, r2);
        int32x4_t i2 = vsubq_s32(vshrq_n_s32(r1, 1), r3);
        int32x4_t i3 = vaddq_s32(r1, vshrq_n_s32(r3, 1));
        r0 = vaddq_s32(i0, i3);
        r1 = vaddq_s32(i1, i2);
        r2 = vsubq_s32(i1, i2);
        r3 = vsubq_s32(i0, i3);
        if (pass == 0)
            TRN4_S32(r0, r1, r2, r3);
    }
    const int32x4_t r32 = vdupq_n_s32(32);
    out[0] = vmovn_s32(vshrq_n_s32(vaddq_s32(r0, r32), 6));
    out[1] = vmovn_s32(vshrq_n_s32(vaddq_s32(r1, r32), 6));
    out[2] = vmovn_s32(vshrq_n_s32(vaddq_s32(r2, r32), 6));
    out[3] = vmovn_s32(vshrq_n_s32(vaddq_s32(r3, r32), 6));
}

void n264_idct4x4_neon(const int16_t coef[16], int16_t res[16])
{
    int16x4_t r[4];
    idct4x4_core_neon(coef, r);
    vst1_s16(res + 0, r[0]); vst1_s16(res + 4,  r[1]);
    vst1_s16(res + 8, r[2]); vst1_s16(res + 12, r[3]);
}

/* Store two 4-pixel rows from one d-register via exact 4-byte writes. */
static inline void st_4x2_u8(uint8_t *p, int stride, uint8x8_t v)
{
    uint32x2_t w = vreinterpret_u32_u8(v);
    uint32_t w0 = vget_lane_u32(w, 0), w1 = vget_lane_u32(w, 1);
    __builtin_memcpy(p, &w0, 4);
    __builtin_memcpy(p + stride, &w1, 4);
}

/* Fused 4x4 inverse + residual add + clip. The scalar sequence is
 * clip8(pred + res) with res already truncated to int16; vqadd saturates
 * where the scalar int sum exceeds int16, but every such value is out of
 * [0,255] on the same side, so the final unsigned-saturating narrow (== clip8)
 * produces the identical pixel. Bit-exact. */
void n264_add4x4_idct_neon(uint8_t *dst, int ds, const uint8_t *pred, int ps,
                           const int16_t coef[16])
{
    int16x4_t r[4];
    idct4x4_core_neon(coef, r);
    int16x8_t p01 = vreinterpretq_s16_u16(vmovl_u8(ld_4x2_u8(pred, ps)));
    int16x8_t p23 = vreinterpretq_s16_u16(vmovl_u8(ld_4x2_u8(pred + 2 * ps, ps)));
    uint8x8_t o01 = vqmovun_s16(vqaddq_s16(vcombine_s16(r[0], r[1]), p01));
    uint8x8_t o23 = vqmovun_s16(vqaddq_s16(vcombine_s16(r[2], r[3]), p23));
    st_4x2_u8(dst, ds, o01);
    st_4x2_u8(dst + 2 * ds, ds, o23);
}

/* 8x8 int16 transpose in place (trn 16/32/64). */
#define TRN8_S16(v0, v1, v2, v3, v4, v5, v6, v7) do {                        \
    int16x8_t t0_ = vtrn1q_s16(v0, v1), t1_ = vtrn2q_s16(v0, v1);            \
    int16x8_t t2_ = vtrn1q_s16(v2, v3), t3_ = vtrn2q_s16(v2, v3);            \
    int16x8_t t4_ = vtrn1q_s16(v4, v5), t5_ = vtrn2q_s16(v4, v5);            \
    int16x8_t t6_ = vtrn1q_s16(v6, v7), t7_ = vtrn2q_s16(v6, v7);            \
    int32x4_t u0_ = vtrn1q_s32(vreinterpretq_s32_s16(t0_), vreinterpretq_s32_s16(t2_)); \
    int32x4_t u2_ = vtrn2q_s32(vreinterpretq_s32_s16(t0_), vreinterpretq_s32_s16(t2_)); \
    int32x4_t u1_ = vtrn1q_s32(vreinterpretq_s32_s16(t1_), vreinterpretq_s32_s16(t3_)); \
    int32x4_t u3_ = vtrn2q_s32(vreinterpretq_s32_s16(t1_), vreinterpretq_s32_s16(t3_)); \
    int32x4_t u4_ = vtrn1q_s32(vreinterpretq_s32_s16(t4_), vreinterpretq_s32_s16(t6_)); \
    int32x4_t u6_ = vtrn2q_s32(vreinterpretq_s32_s16(t4_), vreinterpretq_s32_s16(t6_)); \
    int32x4_t u5_ = vtrn1q_s32(vreinterpretq_s32_s16(t5_), vreinterpretq_s32_s16(t7_)); \
    int32x4_t u7_ = vtrn2q_s32(vreinterpretq_s32_s16(t5_), vreinterpretq_s32_s16(t7_)); \
    v0 = vreinterpretq_s16_s64(vtrn1q_s64(vreinterpretq_s64_s32(u0_), vreinterpretq_s64_s32(u4_))); \
    v4 = vreinterpretq_s16_s64(vtrn2q_s64(vreinterpretq_s64_s32(u0_), vreinterpretq_s64_s32(u4_))); \
    v1 = vreinterpretq_s16_s64(vtrn1q_s64(vreinterpretq_s64_s32(u1_), vreinterpretq_s64_s32(u5_))); \
    v5 = vreinterpretq_s16_s64(vtrn2q_s64(vreinterpretq_s64_s32(u1_), vreinterpretq_s64_s32(u5_))); \
    v2 = vreinterpretq_s16_s64(vtrn1q_s64(vreinterpretq_s64_s32(u2_), vreinterpretq_s64_s32(u6_))); \
    v6 = vreinterpretq_s16_s64(vtrn2q_s64(vreinterpretq_s64_s32(u2_), vreinterpretq_s64_s32(u6_))); \
    v3 = vreinterpretq_s16_s64(vtrn1q_s64(vreinterpretq_s64_s32(u3_), vreinterpretq_s64_s32(u7_))); \
    v7 = vreinterpretq_s16_s64(vtrn2q_s64(vreinterpretq_s64_s32(u3_), vreinterpretq_s64_s32(u7_))); \
} while (0)

/* One 8-point forward butterfly stage across eight row vectors (the scalar
 * fdct8_1d with s[]/d[] as vectors). 16-bit lanes; max magnitude after the
 * second pass is ~77*255 < 32767 for |input| <= 255, exact. */
#define FDCT8_1D_S16(v0, v1, v2, v3, v4, v5, v6, v7) do {                    \
    int16x8_t a0_ = vaddq_s16(v0, v7), a4_ = vsubq_s16(v0, v7);              \
    int16x8_t a1_ = vaddq_s16(v1, v6), a5_ = vsubq_s16(v1, v6);              \
    int16x8_t a2_ = vaddq_s16(v2, v5), a6_ = vsubq_s16(v2, v5);              \
    int16x8_t a3_ = vaddq_s16(v3, v4), a7_ = vsubq_s16(v3, v4);              \
    int16x8_t b0_ = vaddq_s16(a0_, a3_), b2_ = vsubq_s16(a0_, a3_);          \
    int16x8_t b1_ = vaddq_s16(a1_, a2_), b3_ = vsubq_s16(a1_, a2_);          \
    int16x8_t b4_ = vaddq_s16(vaddq_s16(a5_, a6_), vaddq_s16(vshrq_n_s16(a4_, 1), a4_)); \
    int16x8_t b5_ = vsubq_s16(vsubq_s16(a4_, a7_), vaddq_s16(vshrq_n_s16(a6_, 1), a6_)); \
    int16x8_t b6_ = vsubq_s16(vaddq_s16(a4_, a7_), vaddq_s16(vshrq_n_s16(a5_, 1), a5_)); \
    int16x8_t b7_ = vaddq_s16(vsubq_s16(a5_, a6_), vaddq_s16(vshrq_n_s16(a7_, 1), a7_)); \
    v0 = vaddq_s16(b0_, b1_);                                                \
    v2 = vaddq_s16(b2_, vshrq_n_s16(b3_, 1));                                \
    v4 = vsubq_s16(b0_, b1_);                                                \
    v6 = vsubq_s16(vshrq_n_s16(b2_, 1), b3_);                                \
    v1 = vaddq_s16(b4_, vshrq_n_s16(b7_, 2));                                \
    v3 = vaddq_s16(b5_, vshrq_n_s16(b6_, 2));                                \
    v5 = vsubq_s16(b6_, vshrq_n_s16(b5_, 2));                                \
    v7 = vsubq_s16(vshrq_n_s16(b4_, 2), b7_);                                \
} while (0)

#define FDCT8_FINISH(v0, v1, v2, v3, v4, v5, v6, v7, coef) do {              \
    TRN8_S16(v0, v1, v2, v3, v4, v5, v6, v7);       /* rows -> lanes */      \
    FDCT8_1D_S16(v0, v1, v2, v3, v4, v5, v6, v7);   /* horizontal pass */    \
    TRN8_S16(v0, v1, v2, v3, v4, v5, v6, v7);                                \
    FDCT8_1D_S16(v0, v1, v2, v3, v4, v5, v6, v7);   /* vertical pass */      \
    vst1q_s16((coef) + 0,  v0); vst1q_s16((coef) + 8,  v1);                  \
    vst1q_s16((coef) + 16, v2); vst1q_s16((coef) + 24, v3);                  \
    vst1q_s16((coef) + 32, v4); vst1q_s16((coef) + 40, v5);                  \
    vst1q_s16((coef) + 48, v6); vst1q_s16((coef) + 56, v7);                  \
} while (0)

void n264_fdct8x8_neon(const int16_t diff[64], int16_t coef[64])
{
    int16x8_t v0 = vld1q_s16(diff + 0),  v1 = vld1q_s16(diff + 8);
    int16x8_t v2 = vld1q_s16(diff + 16), v3 = vld1q_s16(diff + 24);
    int16x8_t v4 = vld1q_s16(diff + 32), v5 = vld1q_s16(diff + 40);
    int16x8_t v6 = vld1q_s16(diff + 48), v7 = vld1q_s16(diff + 56);
    FDCT8_FINISH(v0, v1, v2, v3, v4, v5, v6, v7, coef);
}

/* Fused src - pred subtract + 8x8 forward DCT (see n264_sub4x4_dct_neon). */
void n264_sub8x8_dct8_neon(int16_t coef[64], const uint8_t *src, int ss,
                           const uint8_t *pred, int ps)
{
#define DIFF8(y) vreinterpretq_s16_u16(vsubl_u8(vld1_u8(src + (y) * ss), \
                                                vld1_u8(pred + (y) * ps)))
    int16x8_t v0 = DIFF8(0), v1 = DIFF8(1), v2 = DIFF8(2), v3 = DIFF8(3);
    int16x8_t v4 = DIFF8(4), v5 = DIFF8(5), v6 = DIFF8(6), v7 = DIFF8(7);
#undef DIFF8
    FDCT8_FINISH(v0, v1, v2, v3, v4, v5, v6, v7, coef);
}

/* One 8-point inverse butterfly stage on an int32x4 half-row set. */
static inline void idct8_1d_s32(int32x4_t m[8])
{
    int32x4_t a0 = vaddq_s32(m[0], m[4]);
    int32x4_t a4 = vsubq_s32(m[0], m[4]);
    int32x4_t a2 = vsubq_s32(vshrq_n_s32(m[2], 1), m[6]);
    int32x4_t a6 = vaddq_s32(m[2], vshrq_n_s32(m[6], 1));
    int32x4_t a1 = vsubq_s32(vsubq_s32(m[5], m[3]), vaddq_s32(m[7], vshrq_n_s32(m[7], 1)));
    int32x4_t a3 = vsubq_s32(vaddq_s32(m[1], m[7]), vaddq_s32(m[3], vshrq_n_s32(m[3], 1)));
    int32x4_t a5 = vaddq_s32(vsubq_s32(m[7], m[1]), vaddq_s32(m[5], vshrq_n_s32(m[5], 1)));
    int32x4_t a7 = vaddq_s32(vaddq_s32(m[3], m[5]), vaddq_s32(m[1], vshrq_n_s32(m[1], 1)));
    int32x4_t b0 = vaddq_s32(a0, a6), b6 = vsubq_s32(a0, a6);
    int32x4_t b2 = vaddq_s32(a4, a2), b4 = vsubq_s32(a4, a2);
    int32x4_t b1 = vaddq_s32(a1, vshrq_n_s32(a7, 2));
    int32x4_t b7 = vsubq_s32(a7, vshrq_n_s32(a1, 2));
    int32x4_t b3 = vaddq_s32(a3, vshrq_n_s32(a5, 2));
    int32x4_t b5 = vsubq_s32(vshrq_n_s32(a3, 2), a5);
    m[0] = vaddq_s32(b0, b7);
    m[7] = vsubq_s32(b0, b7);
    m[1] = vaddq_s32(b2, b5);
    m[6] = vsubq_s32(b2, b5);
    m[2] = vaddq_s32(b4, b3);
    m[5] = vsubq_s32(b4, b3);
    m[3] = vaddq_s32(b6, b1);
    m[4] = vsubq_s32(b6, b1);
}

/* 8x8 inverse transform core. 32-bit lanes end to end: exact for the full
 * int16 coefficient domain like the scalar reference (16-bit lanes could wrap
 * on inputs the scalar computes fine, which would break recon-match).
 * Outputs the eight int16 residual rows (post (x+32)>>6 truncating narrow).
 *
 * A 16-bit-lane version is PROVEN UNSOUND for this encoder.
 * Encoder coefficients are fdct(d) + e with
 * |d| <= 255 (so |fdct| <= 64*255 = 16320, fine) but quantization error e up
 * to ~one dequant step per position, and e gets no orthogonality cancellation
 * through the passes: a symbolic per-node bound (255*L1 over the composed
 * linearized fdct->idct map + per-position step-weighted L1 for e, every
 * internal a/b/r node of both passes checked) exceeds int16 from QP >= ~11
 * flat-CQM / QP >= ~5 JVT-CQM. Constructively, plain +-1 levels (legal plain-
 * quant output) overflow int16 intermediates at QP >= 45 flat within a few
 * thousand random sign patterns. A sound gate would be QP <= ~10, which is
 * worthless at real operating points, so the int32 core stays. (x264's 16-bit
 * idct8 asm accepts this corner; our recon gate is bit-exact-mandatory.) */
static inline void idct8x8_core_neon(const int16_t coef[64], int16x8_t out[8])
{
    int32x4_t lo[8], hi[8];
    for (int i = 0; i < 8; i++) {
        int16x8_t r = vld1q_s16(coef + i * 8);
        lo[i] = vmovl_s16(vget_low_s16(r));
        hi[i] = vmovl_s16(vget_high_s16(r));
    }
    /* transpose 8x8 int32 held as four 4x4 blocks */
    TRN4_S32(lo[0], lo[1], lo[2], lo[3]);           /* TL^T */
    TRN4_S32(hi[0], hi[1], hi[2], hi[3]);           /* TR^T */
    TRN4_S32(lo[4], lo[5], lo[6], lo[7]);           /* BL^T */
    TRN4_S32(hi[4], hi[5], hi[6], hi[7]);           /* BR^T */
    int32x4_t m0[8] = { lo[0], lo[1], lo[2], lo[3], hi[0], hi[1], hi[2], hi[3] };
    int32x4_t m1[8] = { lo[4], lo[5], lo[6], lo[7], hi[4], hi[5], hi[6], hi[7] };
    /* horizontal pass: m0 = rows 0-3 of the transposed matrix, m1 = rows 4-7 */
    idct8_1d_s32(m0);
    idct8_1d_s32(m1);
    /* transpose back */
    TRN4_S32(m0[0], m0[1], m0[2], m0[3]);
    TRN4_S32(m1[0], m1[1], m1[2], m1[3]);
    TRN4_S32(m0[4], m0[5], m0[6], m0[7]);
    TRN4_S32(m1[4], m1[5], m1[6], m1[7]);
    int32x4_t n0[8] = { m0[0], m0[1], m0[2], m0[3], m1[0], m1[1], m1[2], m1[3] };
    int32x4_t n1[8] = { m0[4], m0[5], m0[6], m0[7], m1[4], m1[5], m1[6], m1[7] };
    /* vertical pass */
    idct8_1d_s32(n0);
    idct8_1d_s32(n1);
    const int32x4_t r32 = vdupq_n_s32(32);
    for (int i = 0; i < 8; i++) {
        int16x4_t a = vmovn_s32(vshrq_n_s32(vaddq_s32(n0[i], r32), 6));
        int16x4_t b = vmovn_s32(vshrq_n_s32(vaddq_s32(n1[i], r32), 6));
        out[i] = vcombine_s16(a, b);
    }
}

void n264_idct8x8_neon(const int16_t coef[64], int16_t res[64])
{
    int16x8_t r[8];
    idct8x8_core_neon(coef, r);
    for (int i = 0; i < 8; i++)
        vst1q_s16(res + i * 8, r[i]);
}

/* Fused 8x8 inverse + residual add + clip (saturation argument: see
 * n264_add4x4_idct_neon). Bit-exact with idct + clip8(pred + res). */
void n264_add8x8_idct8_neon(uint8_t *dst, int ds, const uint8_t *pred, int ps,
                            const int16_t coef[64])
{
    int16x8_t r[8];
    idct8x8_core_neon(coef, r);
    for (int i = 0; i < 8; i++) {
        int16x8_t p = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(pred + i * ps)));
        vst1_u8(dst + i * ds, vqmovun_s16(vqaddq_s16(r[i], p)));
    }
}


/* ---- Zig-zag scan / RDOQ coefficient marshalling -------------------------
 *
 * The RDOQ trellis and the decimator both read a block's coefficients in
 * SCAN order while the transform leaves them in raster order, and the C form
 * is a per-coefficient gather through the scan table. The permutation is a
 * compile-time constant, so it is a byte TBL: the index tables below are the
 * scan tables doubled out to int16 byte pairs.
 *
 * A 4x4 block is 32 bytes, which is one two-register TBL per output half. A
 * 8x8 block is 128 bytes, past TBL's 64-byte reach, so each output group is
 * looked up in both halves and OR'd: an index into the wrong half is either
 * above 63 (miss) or wrapped below zero by the unsigned subtract (also above
 * 63, also a miss), and TBL returns zero for a miss, so exactly one of the
 * two lookups contributes per lane. */
static const uint8_t zz4_bytes[32] = {
      0,   1,   2,   3,   8,   9,  16,  17,  10,  11,   4,   5,   6,   7,  12,  13,
     18,  19,  24,  25,  26,  27,  20,  21,  14,  15,  22,  23,  28,  29,  30,  31,
};
static const uint8_t zz8_bytes[128] = {
      0,   1,   2,   3,  16,  17,  32,  33,  18,  19,   4,   5,   6,   7,  20,  21,
     34,  35,  48,  49,  64,  65,  50,  51,  36,  37,  22,  23,   8,   9,  10,  11,
     24,  25,  38,  39,  52,  53,  66,  67,  80,  81,  96,  97,  82,  83,  68,  69,
     54,  55,  40,  41,  26,  27,  12,  13,  14,  15,  28,  29,  42,  43,  56,  57,
     70,  71,  84,  85,  98,  99, 112, 113, 114, 115, 100, 101,  86,  87,  72,  73,
     58,  59,  44,  45,  30,  31,  46,  47,  60,  61,  74,  75,  88,  89, 102, 103,
    116, 117, 118, 119, 104, 105,  90,  91,  76,  77,  62,  63,  78,  79,  92,  93,
    106, 107, 120, 121, 122, 123, 108, 109,  94,  95, 110, 111, 124, 125, 126, 127,
};

/* Scan-order group g of a 64-coefficient block. */
static inline int16x8_t zz8_group(const uint8x16x4_t *lo, const uint8x16x4_t *hi, int g)
{
    uint8x16_t idx = vld1q_u8(zz8_bytes + 16 * g);
    return vreinterpretq_s16_u8(vorrq_u8(vqtbl4q_u8(*lo, idx),
                                         vqtbl4q_u8(*hi, vsubq_u8(idx, vdupq_n_u8(64)))));
}

/* Widen BEFORE the abs: the scalar reference reads the coefficient into an int
 * and negates there, so |-32768| is 32768, a value a 16-bit ABS returns
 * unchanged. checkasm's all--32768 corner is exactly this. */
static inline void st_abs_s32(int *out, int16x8_t v)
{
    vst1q_s32(out + 0, vabsq_s32(vmovl_s16(vget_low_s16(v))));
    vst1q_s32(out + 4, vabsq_s32(vmovl_high_s16(v)));
}

void n264_zigzag_abs_8x8_neon(int out[64], const int16_t in[64])
{
    uint8x16x4_t lo, hi;
    for (int i = 0; i < 4; i++) {
        lo.val[i] = vld1q_u8((const uint8_t *)in + 16 * i);
        hi.val[i] = vld1q_u8((const uint8_t *)in + 64 + 16 * i);
    }
    for (int g = 0; g < 8; g++)
        st_abs_s32(out + 8 * g, zz8_group(&lo, &hi, g));
}

/* Per-group nonzero bitmask (bit i = lane i nonzero) plus the decimator's
 * |level| >= 2 test, written as (v >= 2) | (v <= -2) rather than through an
 * abs so that the int16 corner -32768 -- which ABS returns unchanged -- reads
 * the same as the scalar (unsigned)(v + 1) > 2u. */
static inline unsigned nz_mask8(int16x8_t v, uint16x8_t *bigacc)
{
    static const uint16_t wt[8] = { 1, 2, 4, 8, 16, 32, 64, 128 };
    uint16x8_t nz = vmvnq_u16(vceqzq_s16(v));
    *bigacc = vorrq_u16(*bigacc, vorrq_u16(vcgeq_s16(v, vdupq_n_s16(2)),
                                           vcleq_s16(v, vdupq_n_s16(-2))));
    return vaddvq_u16(vandq_u16(nz, vld1q_u16(wt)));
}

void n264_scan_mask_8x8_neon(const int16_t lev[64], uint64_t *omsk, int *obig)
{
    uint8x16x4_t lo, hi;
    for (int i = 0; i < 4; i++) {
        lo.val[i] = vld1q_u8((const uint8_t *)lev + 16 * i);
        hi.val[i] = vld1q_u8((const uint8_t *)lev + 64 + 16 * i);
    }
    uint16x8_t big = vdupq_n_u16(0);
    uint64_t msk = 0;
    for (int g = 0; g < 8; g++)
        msk |= (uint64_t)nz_mask8(zz8_group(&lo, &hi, g), &big) << (8 * g);
    *omsk = msk;
    *obig = vmaxvq_u16(big) != 0;
}

void n264_zigzag_scan_4x4_neon(int16_t out[16], const int16_t in[16],
                               uint32_t *omsk, int *obig)
{
    uint8x16x2_t t;
    t.val[0] = vld1q_u8((const uint8_t *)in);
    t.val[1] = vld1q_u8((const uint8_t *)in + 16);
    int16x8_t p0 = vreinterpretq_s16_u8(vqtbl2q_u8(t, vld1q_u8(zz4_bytes)));
    int16x8_t p1 = vreinterpretq_s16_u8(vqtbl2q_u8(t, vld1q_u8(zz4_bytes + 16)));
    vst1q_s16(out + 0, p0);
    vst1q_s16(out + 8, p1);
    uint16x8_t big = vdupq_n_u16(0);
    unsigned m0 = nz_mask8(p0, &big);
    unsigned m1 = nz_mask8(p1, &big);
    *omsk = m0 | (m1 << 8);
    *obig = vmaxvq_u16(big) != 0;
}

#endif /* __aarch64__ */
