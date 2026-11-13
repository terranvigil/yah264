/*
 * transform_neon10.c - aarch64 NEON transforms, quant and scans for the
 * 10-bit library
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The 10-bit twins of src/dsp/transform_neon.c, under the 8-bit kernels'
 * names (src/common/symbols10.h suffixes them). At 10 bits a coefficient is
 * an int32: a 4x4 residual is at most 1023 in magnitude and the forward 4x4
 * transform gains up to 36, past int16. So every transform here runs on
 * int32x4 lanes, the C reference's own width, and is exact wherever the
 * reference's int arithmetic is.
 */
#if defined(__aarch64__)
#include "../transform.h"
#include "../arch.h"
#include <arm_neon.h>

#if Y264_BIT_DEPTH != 10
#error "transform_neon10.c is the 10-bit library's NEON tier"
#endif

/* ---- forward quant ---------------------------------------------------------
 *
 * q = (|c| * mf + f) >> qbits, sign restored. The reference forms the product
 * in 64 bits; here it is u32, which is exact for the flat multipliers the
 * callers pass (mf <= 20972) when |c| < 2^17: the product is under 2.75e9,
 * and f is at most 2^26, leaving the sum under 2^32. Every coefficient the
 * 10-bit transforms produce is under 2^17 (the 8x8 DC of a full-swing
 * residual is 64 * 1023 = 65472). A block holding a larger value takes the
 * scalar loop. The multiplier is not checked: a scaling-matrix row (up to
 * 209712) would wrap the lane, and every such caller stays on C. */
#define QUANT_SAFE (1u << 17)

static inline int32x4_t quant4_u32(int32x4_t c, uint32x4_t mf, uint32x4_t vf,
                                   int32x4_t nsh)
{
    uint32x4_t a = vreinterpretq_u32_s32(vabsq_s32(c));
    uint32x4_t q = vshlq_u32(vmlaq_u32(vf, a, mf), nsh);
    int32x4_t qs = vreinterpretq_s32_u32(q);
    return vbslq_s32(vcltzq_s32(c), vnegq_s32(qs), qs);
}

static inline uint32_t absmax4(const dctcoef *c, int n)
{
    uint32x4_t m = vdupq_n_u32(0);
    for (int i = 0; i < n; i += 4)
        m = vmaxq_u32(m, vreinterpretq_u32_s32(vabsq_s32(vld1q_s32(c + i))));
    return vmaxvq_u32(m);
}

static void quant_scalar(const dctcoef *coef, dctcoef *lev, int n, int f,
                         int qbits, const int32_t *mfrow)
{
    for (int i = 0; i < n; i++) {
        int c = coef[i];
        int a = c < 0 ? -c : c;
        int q = (int)(((int64_t)a * mfrow[i] + f) >> qbits);
        lev[i] = (dctcoef)(c < 0 ? -q : q);
    }
}

static inline void quant_n(const dctcoef *coef, dctcoef *lev, int n, int f,
                           int qbits, const int32_t *mfrow)
{
    if (absmax4(coef, n) >= QUANT_SAFE) {
        quant_scalar(coef, lev, n, f, qbits, mfrow);
        return;
    }
    const uint32x4_t vf = vdupq_n_u32((uint32_t)f);
    const int32x4_t nsh = vdupq_n_s32(-qbits);
    for (int i = 0; i < n; i += 4)
        vst1q_s32(lev + i, quant4_u32(vld1q_s32(coef + i),
                                      vreinterpretq_u32_s32(vld1q_s32(mfrow + i)),
                                      vf, nsh));
}

void y264_quant_4x4_fneon(const dctcoef coef[16], dctcoef lev[16], int qp, int f,
                          const int32_t mfrow[16])
{
    quant_n(coef, lev, 16, f, 15 + qp / 6, mfrow);
}

void y264_quant_8x8_fneon(const dctcoef coef[64], dctcoef lev[64], int qp, int f,
                          const int32_t mfrow[64])
{
    quant_n(coef, lev, 64, f, 16 + qp / 6, mfrow);
}

/* n 4x4 AC blocks: each block quantised, its DC coefficient copied to dc[k]
 * and its level zeroed, and its nonzero AC count left in nz[k]. Returns
 * nonzero if any block has an AC level. */
int y264_quant_4x4_ac_fneon(const dctcoef *coef, dctcoef *lev, int n, int qp, int f,
                            const int32_t mfrow[16], dctcoef *dc, int *nz)
{
    int any = 0;
    for (int k = 0; k < n; k++, coef += 16, lev += 16) {
        quant_n(coef, lev, 16, f, 15 + qp / 6, mfrow);
        dc[k] = coef[0];
        lev[0] = 0;
        int32x4_t l0 = vld1q_s32(lev), l1 = vld1q_s32(lev + 4);
        int32x4_t l2 = vld1q_s32(lev + 8), l3 = vld1q_s32(lev + 12);
        uint32x4_t z = vaddq_u32(vaddq_u32(vtstq_s32(l0, l0), vtstq_s32(l1, l1)),
                                 vaddq_u32(vtstq_s32(l2, l2), vtstq_s32(l3, l3)));
        int c = -(int)vaddvq_s32(vreinterpretq_s32_u32(z));
        nz[k] = c;
        any |= c;
    }
    return any;
}

/* ---- dequant ---------------------------------------------------------------
 *
 * The reference's int multiply and shift, four lanes at a time: (lev * ls)
 * scaled up when qp/6 reaches the block's shift point, rounded down by the
 * remainder otherwise. */
static inline void dequant_n(const dctcoef *lev, dctcoef *coef, int n, int qp,
                             const int32_t *lsrow, int point)
{
    int shift = qp / 6;
    if (shift >= point) {
        int32x4_t vs = vdupq_n_s32(shift - point);
        for (int g = 0; g < n; g += 4)
            vst1q_s32(coef + g, vshlq_s32(vmulq_s32(vld1q_s32(lev + g),
                                                    vld1q_s32(lsrow + g)), vs));
    } else {
        int32x4_t vr = vdupq_n_s32(1 << (point - 1 - shift));
        int32x4_t vs = vdupq_n_s32(-(point - shift));
        for (int g = 0; g < n; g += 4)
            vst1q_s32(coef + g, vshlq_s32(vmlaq_s32(vr, vld1q_s32(lev + g),
                                                    vld1q_s32(lsrow + g)), vs));
    }
}

void y264_dequant_4x4_neon(const dctcoef lev[16], dctcoef coef[16], int qp,
                           const int32_t lsrow[16])
{
    dequant_n(lev, coef, 16, qp, lsrow, 4);
}

void y264_dequant_8x8_neon(const dctcoef lev[64], dctcoef coef[64], int qp,
                           const int32_t lsrow[64])
{
    dequant_n(lev, coef, 64, qp, lsrow, 6);
}

/* ---- 4x4 transforms --------------------------------------------------------*/

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

/* The forward core transform across four vectors, lane by lane. */
#define FDCT4_1D(r0, r1, r2, r3) do {                                        \
    int32x4_t z0_ = vaddq_s32(r0, r3), z3_ = vsubq_s32(r0, r3);              \
    int32x4_t z1_ = vaddq_s32(r1, r2), z2_ = vsubq_s32(r1, r2);              \
    r0 = vaddq_s32(z0_, z1_);                                                \
    r1 = vaddq_s32(vaddq_s32(z3_, z3_), z2_);                                \
    r2 = vsubq_s32(z0_, z1_);                                                \
    r3 = vsubq_s32(z3_, vaddq_s32(z2_, z2_));                                \
} while (0)

/* The forward 4x4 of four residual rows. The two passes have no rounding, so
 * taking the vertical one first, straight down the row vectors, gives the
 * reference's coefficients; one transpose then serves the horizontal pass
 * and a second puts the rows back. */
static inline void fdct4_rows(int32x4_t r0, int32x4_t r1, int32x4_t r2,
                              int32x4_t r3, dctcoef coef[16])
{
    FDCT4_1D(r0, r1, r2, r3);
    TRN4_S32(r0, r1, r2, r3);
    FDCT4_1D(r0, r1, r2, r3);
    TRN4_S32(r0, r1, r2, r3);
    vst1q_s32(coef, r0); vst1q_s32(coef + 4, r1);
    vst1q_s32(coef + 8, r2); vst1q_s32(coef + 12, r3);
}

void y264_fdct4x4_neon(const dctcoef diff[16], dctcoef coef[16])
{
    fdct4_rows(vld1q_s32(diff), vld1q_s32(diff + 4), vld1q_s32(diff + 8),
               vld1q_s32(diff + 12), coef);
}

static inline int32x4_t diff4(const pixel *s, const pixel *p)
{
    return vreinterpretq_s32_u32(vsubl_u16(vld1_u16(s), vld1_u16(p)));
}

void y264_sub4x4_dct_neon(dctcoef coef[16], const pixel *src, int ss,
                          const pixel *pred, int ps)
{
    fdct4_rows(diff4(src, pred), diff4(src + ss, pred + ps),
               diff4(src + 2 * ss, pred + 2 * ps), diff4(src + 3 * ss, pred + 3 * ps),
               coef);
}

/* A grid of 4x4 blocks, raster order. Each block's rows are four exact
 * 4-sample loads, so an edge macroblock reads nothing past its grid. */
void y264_sub_dct4_blocks_neon(dctcoef (*coef)[16], int nbw, int nbh,
                               const pixel *src, int ss,
                               const pixel *pred, int ps)
{
    for (int by = 0; by < nbh; by++)
        for (int bx = 0; bx < nbw; bx++)
            y264_sub4x4_dct_neon(coef[by * nbw + bx], src + (by * 4) * ss + bx * 4, ss,
                                 pred + (by * 4) * ps + bx * 4, ps);
}

/* The inverse 4x4: the normative order (rows, then columns) since each pass
 * truncates its half-weight terms, and the (x + 32) >> 6 at the end. */
static inline void idct4_core(const dctcoef coef[16], int32x4_t out[4])
{
    int32x4_t r0 = vld1q_s32(coef), r1 = vld1q_s32(coef + 4);
    int32x4_t r2 = vld1q_s32(coef + 8), r3 = vld1q_s32(coef + 12);
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
    out[0] = vrshrq_n_s32(r0, 6);
    out[1] = vrshrq_n_s32(r1, 6);
    out[2] = vrshrq_n_s32(r2, 6);
    out[3] = vrshrq_n_s32(r3, 6);
}

void y264_idct4x4_neon(const dctcoef coef[16], dctcoef res[16])
{
    int32x4_t r[4];
    idct4_core(coef, r);
    vst1q_s32(res, r[0]); vst1q_s32(res + 4, r[1]);
    vst1q_s32(res + 8, r[2]); vst1q_s32(res + 12, r[3]);
}

/* pred + res clipped to [0, PIXEL_MAX]: the signed narrow saturates below
 * zero exactly where the clip does, and the min takes the top. */
static inline uint16x4_t add_clip4(const pixel *p, int32x4_t r)
{
    int32x4_t s = vaddq_s32(vreinterpretq_s32_u32(vmovl_u16(vld1_u16(p))), r);
    return vmin_u16(vqmovun_s32(s), vdup_n_u16(PIXEL_MAX));
}

void y264_add4x4_idct_neon(pixel *dst, int ds, const pixel *pred, int ps,
                           const dctcoef coef[16])
{
    int32x4_t r[4];
    idct4_core(coef, r);
    for (int i = 0; i < 4; i++)
        vst1_u16(dst + i * ds, add_clip4(pred + i * ps, r[i]));
}

/* ---- 8x8 transforms --------------------------------------------------------
 *
 * An 8x8 block is sixteen int32x4: L0-L7 hold columns 0-3 of rows 0-7 and
 * H0-H7 columns 4-7. The 1D transforms here truncate inside each pass, so
 * both directions keep the reference's order: rows (across lanes, after a
 * transpose), then columns. Everything is named variables and macros, as the
 * 8-bit file does it: written over arrays, the compiler kept the block on the
 * stack and the kernel ran at half the C reference's speed. */
#define TRN8_S32() do {                                                      \
    TRN4_S32(L0, L1, L2, L3); TRN4_S32(H0, H1, H2, H3);                      \
    TRN4_S32(L4, L5, L6, L7); TRN4_S32(H4, H5, H6, H7);                      \
    int32x4_t t0_ = H0, t1_ = H1, t2_ = H2, t3_ = H3;                        \
    H0 = L4; H1 = L5; H2 = L6; H3 = L7;                                      \
    L4 = t0_; L5 = t1_; L6 = t2_; L7 = t3_;                                  \
} while (0)

/* fdct8_1d across eight vectors. */
#define FDCT8_1D(v0, v1, v2, v3, v4, v5, v6, v7) do {                        \
    int32x4_t a0_ = vaddq_s32(v0, v7), a4_ = vsubq_s32(v0, v7);              \
    int32x4_t a1_ = vaddq_s32(v1, v6), a5_ = vsubq_s32(v1, v6);              \
    int32x4_t a2_ = vaddq_s32(v2, v5), a6_ = vsubq_s32(v2, v5);              \
    int32x4_t a3_ = vaddq_s32(v3, v4), a7_ = vsubq_s32(v3, v4);              \
    int32x4_t b0_ = vaddq_s32(a0_, a3_), b2_ = vsubq_s32(a0_, a3_);          \
    int32x4_t b1_ = vaddq_s32(a1_, a2_), b3_ = vsubq_s32(a1_, a2_);          \
    int32x4_t b4_ = vaddq_s32(vaddq_s32(a5_, a6_), vaddq_s32(vshrq_n_s32(a4_, 1), a4_)); \
    int32x4_t b5_ = vsubq_s32(vsubq_s32(a4_, a7_), vaddq_s32(vshrq_n_s32(a6_, 1), a6_)); \
    int32x4_t b6_ = vsubq_s32(vaddq_s32(a4_, a7_), vaddq_s32(vshrq_n_s32(a5_, 1), a5_)); \
    int32x4_t b7_ = vaddq_s32(vsubq_s32(a5_, a6_), vaddq_s32(vshrq_n_s32(a7_, 1), a7_)); \
    v0 = vaddq_s32(b0_, b1_);                                                \
    v2 = vaddq_s32(b2_, vshrq_n_s32(b3_, 1));                                \
    v4 = vsubq_s32(b0_, b1_);                                                \
    v6 = vsubq_s32(vshrq_n_s32(b2_, 1), b3_);                                \
    v1 = vaddq_s32(b4_, vshrq_n_s32(b7_, 2));                                \
    v3 = vaddq_s32(b5_, vshrq_n_s32(b6_, 2));                                \
    v5 = vsubq_s32(b6_, vshrq_n_s32(b5_, 2));                                \
    v7 = vsubq_s32(vshrq_n_s32(b4_, 2), b7_);                                \
} while (0)

/* idct8_1d across eight vectors. */
#define IDCT8_1D(m0, m1, m2, m3, m4, m5, m6, m7) do {                        \
    int32x4_t a0_ = vaddq_s32(m0, m4);                                       \
    int32x4_t a4_ = vsubq_s32(m0, m4);                                       \
    int32x4_t a2_ = vsubq_s32(vshrq_n_s32(m2, 1), m6);                       \
    int32x4_t a6_ = vaddq_s32(m2, vshrq_n_s32(m6, 1));                       \
    int32x4_t a1_ = vsubq_s32(vsubq_s32(m5, m3), vaddq_s32(m7, vshrq_n_s32(m7, 1))); \
    int32x4_t a3_ = vsubq_s32(vaddq_s32(m1, m7), vaddq_s32(m3, vshrq_n_s32(m3, 1))); \
    int32x4_t a5_ = vaddq_s32(vsubq_s32(m7, m1), vaddq_s32(m5, vshrq_n_s32(m5, 1))); \
    int32x4_t a7_ = vaddq_s32(vaddq_s32(m3, m5), vaddq_s32(m1, vshrq_n_s32(m1, 1))); \
    int32x4_t b0_ = vaddq_s32(a0_, a6_), b6_ = vsubq_s32(a0_, a6_);          \
    int32x4_t b2_ = vaddq_s32(a4_, a2_), b4_ = vsubq_s32(a4_, a2_);          \
    int32x4_t b1_ = vaddq_s32(a1_, vshrq_n_s32(a7_, 2));                     \
    int32x4_t b7_ = vsubq_s32(a7_, vshrq_n_s32(a1_, 2));                     \
    int32x4_t b3_ = vaddq_s32(a3_, vshrq_n_s32(a5_, 2));                     \
    int32x4_t b5_ = vsubq_s32(vshrq_n_s32(a3_, 2), a5_);                     \
    m0 = vaddq_s32(b0_, b7_);                                                \
    m7 = vsubq_s32(b0_, b7_);                                                \
    m1 = vaddq_s32(b2_, b5_);                                                \
    m6 = vsubq_s32(b2_, b5_);                                                \
    m2 = vaddq_s32(b4_, b3_);                                                \
    m5 = vsubq_s32(b4_, b3_);                                                \
    m3 = vaddq_s32(b6_, b1_);                                                \
    m4 = vsubq_s32(b6_, b1_);                                                \
} while (0)

#define DECL8 int32x4_t L0, L1, L2, L3, L4, L5, L6, L7, H0, H1, H2, H3, H4, H5, H6, H7
#define LOAD8(p) do {                                                        \
    L0 = vld1q_s32((p) + 0);  H0 = vld1q_s32((p) + 4);                       \
    L1 = vld1q_s32((p) + 8);  H1 = vld1q_s32((p) + 12);                      \
    L2 = vld1q_s32((p) + 16); H2 = vld1q_s32((p) + 20);                      \
    L3 = vld1q_s32((p) + 24); H3 = vld1q_s32((p) + 28);                      \
    L4 = vld1q_s32((p) + 32); H4 = vld1q_s32((p) + 36);                      \
    L5 = vld1q_s32((p) + 40); H5 = vld1q_s32((p) + 44);                      \
    L6 = vld1q_s32((p) + 48); H6 = vld1q_s32((p) + 52);                      \
    L7 = vld1q_s32((p) + 56); H7 = vld1q_s32((p) + 60);                      \
} while (0)
#define STORE8(p) do {                                                       \
    vst1q_s32((p) + 0, L0);  vst1q_s32((p) + 4, H0);                         \
    vst1q_s32((p) + 8, L1);  vst1q_s32((p) + 12, H1);                        \
    vst1q_s32((p) + 16, L2); vst1q_s32((p) + 20, H2);                        \
    vst1q_s32((p) + 24, L3); vst1q_s32((p) + 28, H3);                        \
    vst1q_s32((p) + 32, L4); vst1q_s32((p) + 36, H4);                        \
    vst1q_s32((p) + 40, L5); vst1q_s32((p) + 44, H5);                        \
    vst1q_s32((p) + 48, L6); vst1q_s32((p) + 52, H6);                        \
    vst1q_s32((p) + 56, L7); vst1q_s32((p) + 60, H7);                        \
} while (0)
/* Both 2D transforms: transpose so a row runs down the vectors, the 1D pass,
 * transpose back, the 1D pass again for the columns. */
#define XFORM8(OP) do {                                                      \
    TRN8_S32();                                                              \
    OP(L0, L1, L2, L3, L4, L5, L6, L7); OP(H0, H1, H2, H3, H4, H5, H6, H7);  \
    TRN8_S32();                                                              \
    OP(L0, L1, L2, L3, L4, L5, L6, L7); OP(H0, H1, H2, H3, H4, H5, H6, H7);  \
} while (0)

void y264_fdct8x8_neon(const dctcoef diff[64], dctcoef coef[64])
{
    DECL8;
    LOAD8(diff);
    XFORM8(FDCT8_1D);
    STORE8(coef);
}

void y264_sub8x8_dct8_neon(dctcoef coef[64], const pixel *src, int ss,
                           const pixel *pred, int ps)
{
    DECL8;
#define D8(i) do {                                                           \
        uint16x8_t s_ = vld1q_u16(src + (i) * ss), p_ = vld1q_u16(pred + (i) * ps); \
        L##i = vreinterpretq_s32_u32(vsubl_u16(vget_low_u16(s_), vget_low_u16(p_))); \
        H##i = vreinterpretq_s32_u32(vsubl_high_u16(s_, p_));                \
    } while (0)
    D8(0); D8(1); D8(2); D8(3); D8(4); D8(5); D8(6); D8(7);
#undef D8
    XFORM8(FDCT8_1D);
    STORE8(coef);
}

#define RND8() do {                                                          \
    L0 = vrshrq_n_s32(L0, 6); L1 = vrshrq_n_s32(L1, 6);                      \
    L2 = vrshrq_n_s32(L2, 6); L3 = vrshrq_n_s32(L3, 6);                      \
    L4 = vrshrq_n_s32(L4, 6); L5 = vrshrq_n_s32(L5, 6);                      \
    L6 = vrshrq_n_s32(L6, 6); L7 = vrshrq_n_s32(L7, 6);                      \
    H0 = vrshrq_n_s32(H0, 6); H1 = vrshrq_n_s32(H1, 6);                      \
    H2 = vrshrq_n_s32(H2, 6); H3 = vrshrq_n_s32(H3, 6);                      \
    H4 = vrshrq_n_s32(H4, 6); H5 = vrshrq_n_s32(H5, 6);                      \
    H6 = vrshrq_n_s32(H6, 6); H7 = vrshrq_n_s32(H7, 6);                      \
} while (0)

void y264_idct8x8_neon(const dctcoef coef[64], dctcoef res[64])
{
    DECL8;
    LOAD8(coef);
    XFORM8(IDCT8_1D);
    RND8();
    STORE8(res);
}

void y264_add8x8_idct8_neon(pixel *dst, int ds, const pixel *pred, int ps,
                            const dctcoef coef[64])
{
    DECL8;
    LOAD8(coef);
    XFORM8(IDCT8_1D);
    RND8();
#define A8(i) vst1q_u16(dst + (i) * ds, vcombine_u16(add_clip4(pred + (i) * ps, L##i), \
                                                     add_clip4(pred + (i) * ps + 4, H##i)))
    A8(0); A8(1); A8(2); A8(3); A8(4); A8(5); A8(6); A8(7);
#undef A8
}

/* ---- zig-zag scans ---------------------------------------------------------
 *
 * The scan is a constant permutation, so it is a byte table lookup. A 4x4
 * block of int32 is 64 bytes, one four-register table. An 8x8 block is 256
 * bytes; it is split into a plane of low halfwords and a plane of high
 * halfwords, each 128 bytes, permuted the way the 8-bit kernel permutes its
 * int16 block (two lookups per group, a miss returns zero), and zipped back
 * together, which is exact for every int32. */
#define B4(z) 4 * (z), 4 * (z) + 1, 4 * (z) + 2, 4 * (z) + 3
static const uint8_t zz4_bytes32[64] = {
    B4(0), B4(1), B4(4), B4(8), B4(5), B4(2), B4(3), B4(6),
    B4(9), B4(12), B4(13), B4(10), B4(7), B4(11), B4(14), B4(15),
};
#undef B4

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

static const uint8_t zz8_pos[64] = {
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

/* Scan-order group g (eight halfwords) of a 64-halfword plane held as two
 * 64-byte tables. */
static inline uint8x16_t zz8_group(const uint8x16x4_t *lo, const uint8x16x4_t *hi,
                                   uint8x16_t idx)
{
    return vorrq_u8(vqtbl4q_u8(*lo, idx),
                    vqtbl4q_u8(*hi, vsubq_u8(idx, vdupq_n_u8(64))));
}

/* The low and high halfword planes of 64 int32, as table registers. */
static inline void split_planes(const dctcoef in[64], uint8x16x4_t *llo, uint8x16x4_t *lhi,
                                uint8x16x4_t *hlo, uint8x16x4_t *hhi)
{
    uint8x16_t l[8], h[8];
    for (int i = 0; i < 8; i++) {
        uint16x8x2_t u = vuzpq_u16(vreinterpretq_u16_s32(vld1q_s32(in + 8 * i)),
                                   vreinterpretq_u16_s32(vld1q_s32(in + 8 * i + 4)));
        l[i] = vreinterpretq_u8_u16(u.val[0]);
        h[i] = vreinterpretq_u8_u16(u.val[1]);
    }
    for (int i = 0; i < 4; i++) {
        llo->val[i] = l[i]; lhi->val[i] = l[4 + i];
        hlo->val[i] = h[i]; hhi->val[i] = h[4 + i];
    }
}

/* |in| in scan order. The abs is taken in int32 after the permutation, as
 * the reference takes it on the int it read. */
void y264_zigzag_abs_8x8_neon(int out[64], const dctcoef in[64])
{
    uint8x16x4_t llo, lhi, hlo, hhi;
    split_planes(in, &llo, &lhi, &hlo, &hhi);
    for (int g = 0; g < 8; g++) {
        uint8x16_t idx = vld1q_u8(zz8_bytes + 16 * g);
        uint16x8_t l = vreinterpretq_u16_u8(zz8_group(&llo, &lhi, idx));
        uint16x8_t h = vreinterpretq_u16_u8(zz8_group(&hlo, &hhi, idx));
        vst1q_s32(out + 8 * g, vabsq_s32(vreinterpretq_s32_u16(vzip1q_u16(l, h))));
        vst1q_s32(out + 8 * g + 4, vabsq_s32(vreinterpretq_s32_u16(vzip2q_u16(l, h))));
    }
}

/* The nonzero mask in scan order and the |level| >= 2 test, which is an OR
 * over the block and needs no permutation. Written as (v >= 2) | (v <= -2),
 * the reference's (unsigned)(v + 1) > 2u for every int32. */
static inline uint32x4_t big4(int32x4_t v)
{
    return vorrq_u32(vcgeq_s32(v, vdupq_n_s32(2)), vcleq_s32(v, vdupq_n_s32(-2)));
}

static inline uint8x8_t nz8(const dctcoef *p)
{
    int32x4_t a = vld1q_s32(p), b = vld1q_s32(p + 4);
    return vmovn_u16(vcombine_u16(vmovn_u32(vtstq_s32(a, a)), vmovn_u32(vtstq_s32(b, b))));
}

void y264_scan_mask_8x8_neon(const dctcoef lev[64], uint64_t *omsk, int *obig)
{
    uint8x16x4_t nz;
    uint32x4_t big = vdupq_n_u32(0);
    for (int i = 0; i < 16; i++)
        big = vorrq_u32(big, big4(vld1q_s32(lev + 4 * i)));
    for (int i = 0; i < 4; i++)
        nz.val[i] = vcombine_u8(nz8(lev + 16 * i), nz8(lev + 16 * i + 8));
    static const uint8_t wt[16] = { 1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128 };
    const uint8x16_t w = vld1q_u8(wt);
    uint8x16_t s0 = vandq_u8(vqtbl4q_u8(nz, vld1q_u8(zz8_pos + 0)), w);
    uint8x16_t s1 = vandq_u8(vqtbl4q_u8(nz, vld1q_u8(zz8_pos + 16)), w);
    uint8x16_t s2 = vandq_u8(vqtbl4q_u8(nz, vld1q_u8(zz8_pos + 32)), w);
    uint8x16_t s3 = vandq_u8(vqtbl4q_u8(nz, vld1q_u8(zz8_pos + 48)), w);
    uint8x16_t q = vpaddq_u8(vpaddq_u8(s0, s1), vpaddq_u8(s2, s3));
    q = vpaddq_u8(q, q);
    *omsk = vgetq_lane_u64(vreinterpretq_u64_u8(q), 0);
    *obig = vmaxvq_u32(big) != 0;
}

void y264_zigzag_scan_4x4_neon(dctcoef out[16], const dctcoef in[16],
                               uint32_t *omsk, int *obig)
{
    uint8x16x4_t t;
    for (int i = 0; i < 4; i++)
        t.val[i] = vld1q_u8((const uint8_t *)(in + 4 * i));
    int32x4_t p[4];
    uint32x4_t big = vdupq_n_u32(0);
    for (int i = 0; i < 4; i++) {
        p[i] = vreinterpretq_s32_u8(vqtbl4q_u8(t, vld1q_u8(zz4_bytes32 + 16 * i)));
        vst1q_s32(out + 4 * i, p[i]);
        big = vorrq_u32(big, big4(p[i]));
    }
    static const uint32_t wt[4] = { 1, 2, 4, 8 };
    const uint32x4_t w = vld1q_u32(wt);
    uint32_t m = 0;
    for (int i = 0; i < 4; i++)
        m |= vaddvq_u32(vandq_u32(vtstq_s32(p[i], p[i]), w)) << (4 * i);
    *omsk = m;
    *obig = vmaxvq_u32(big) != 0;
}

#endif /* __aarch64__ */
