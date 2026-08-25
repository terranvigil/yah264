/*
 * mc_neon.c - aarch64 NEON luma interpolation for the in-bounds 16x16 case
 * Copyright (c) 2026, the next264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Bit-exact with the scalar block-separable path in mc.c: the same half-pel
 * planes, and quarter-pel averaging via vrhadd (rounding halving add, exactly
 * (a+b+1)>>1). Only handles fully in-bounds 16x16 blocks; mc.c falls back to
 * scalar at frame edges and for other sizes. Validated by checkasm and the
 * recon-match conformance gate.
 */
#if defined(__aarch64__)
#include <arm_neon.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define ST 24   /* plane row stride */

/* Widen a uint8x16 to two int16x8 halves. */
#define WIDEN(v, lo, hi) do { \
    lo = vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(v))); \
    hi = vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(v))); \
} while (0)

/* Horizontal 6-tap over cols 0..15 of a row; `p` points at sample (ix-2, row).
 * If `clip` writes clipped u8 to `out8`, else writes raw int16 taps to `out16`. */
static inline void hfilt(const uint8_t *p, uint8_t *out8, int16_t *out16, int clip)
{
    int16x8_t l0a, l0b, l1a, l1b, l2a, l2b, l3a, l3b, l4a, l4b, l5a, l5b;
    WIDEN(vld1q_u8(p + 0), l0a, l0b);
    WIDEN(vld1q_u8(p + 1), l1a, l1b);
    WIDEN(vld1q_u8(p + 2), l2a, l2b);
    WIDEN(vld1q_u8(p + 3), l3a, l3b);
    WIDEN(vld1q_u8(p + 4), l4a, l4b);
    WIDEN(vld1q_u8(p + 5), l5a, l5b);
    int16x8_t ta = vaddq_s16(l0a, l5a);
    ta = vsubq_s16(ta, vmulq_n_s16(vaddq_s16(l1a, l4a), 5));
    ta = vaddq_s16(ta, vmulq_n_s16(vaddq_s16(l2a, l3a), 20));
    int16x8_t tb = vaddq_s16(l0b, l5b);
    tb = vsubq_s16(tb, vmulq_n_s16(vaddq_s16(l1b, l4b), 5));
    tb = vaddq_s16(tb, vmulq_n_s16(vaddq_s16(l2b, l3b), 20));
    if (clip) {
        uint8x8_t a = vqmovun_s16(vshrq_n_s16(vaddq_s16(ta, vdupq_n_s16(16)), 5));
        uint8x8_t b = vqmovun_s16(vshrq_n_s16(vaddq_s16(tb, vdupq_n_s16(16)), 5));
        vst1q_u8(out8, vcombine_u8(a, b));
    } else {
        vst1q_s16(out16, ta);
        vst1q_s16(out16 + 8, tb);
    }
}

/* Vertical 6-tap over 16 cols from six reference rows (each at col ix). */
static inline void vfilt(const uint8_t *r0, const uint8_t *r1, const uint8_t *r2,
                         const uint8_t *r3, const uint8_t *r4, const uint8_t *r5,
                         uint8_t *out)
{
    int16x8_t a0, b0, a1, b1, a2, b2, a3, b3, a4, b4, a5, b5;
    WIDEN(vld1q_u8(r0), a0, b0);
    WIDEN(vld1q_u8(r1), a1, b1);
    WIDEN(vld1q_u8(r2), a2, b2);
    WIDEN(vld1q_u8(r3), a3, b3);
    WIDEN(vld1q_u8(r4), a4, b4);
    WIDEN(vld1q_u8(r5), a5, b5);
    int16x8_t ta = vaddq_s16(a0, a5);
    ta = vsubq_s16(ta, vmulq_n_s16(vaddq_s16(a1, a4), 5));
    ta = vaddq_s16(ta, vmulq_n_s16(vaddq_s16(a2, a3), 20));
    int16x8_t tb = vaddq_s16(b0, b5);
    tb = vsubq_s16(tb, vmulq_n_s16(vaddq_s16(b1, b4), 5));
    tb = vaddq_s16(tb, vmulq_n_s16(vaddq_s16(b2, b3), 20));
    uint8x8_t la = vqmovun_s16(vshrq_n_s16(vaddq_s16(ta, vdupq_n_s16(16)), 5));
    uint8x8_t lb = vqmovun_s16(vshrq_n_s16(vaddq_s16(tb, vdupq_n_s16(16)), 5));
    vst1q_u8(out, vcombine_u8(la, lb));
}

/* Vertical 6-tap of one 4-lane group of int16 intermediates, into int32. */
static inline int32x4_t jgroup(int16x4_t g0, int16x4_t g1, int16x4_t g2,
                               int16x4_t g3, int16x4_t g4, int16x4_t g5)
{
    int32x4_t s05 = vaddl_s16(g0, g5);
    int32x4_t s14 = vaddl_s16(g1, g4);
    int32x4_t s23 = vaddl_s16(g2, g3);
    int32x4_t t = vsubq_s32(s05, vmulq_n_s32(s14, 5));
    t = vaddq_s32(t, vmulq_n_s32(s23, 20));
    return vshrq_n_s32(vaddq_s32(t, vdupq_n_s32(512)), 10);
}

/* Centre 6-tap: vertical filter over six int16 horizontal-intermediate rows. */
static inline void jvfilt(const int16_t *c0, const int16_t *c1, const int16_t *c2,
                          const int16_t *c3, const int16_t *c4, const int16_t *c5,
                          uint8_t *out)
{
    int16x8_t r0a = vld1q_s16(c0), r0b = vld1q_s16(c0 + 8);
    int16x8_t r1a = vld1q_s16(c1), r1b = vld1q_s16(c1 + 8);
    int16x8_t r2a = vld1q_s16(c2), r2b = vld1q_s16(c2 + 8);
    int16x8_t r3a = vld1q_s16(c3), r3b = vld1q_s16(c3 + 8);
    int16x8_t r4a = vld1q_s16(c4), r4b = vld1q_s16(c4 + 8);
    int16x8_t r5a = vld1q_s16(c5), r5b = vld1q_s16(c5 + 8);

    int32x4_t j0 = jgroup(vget_low_s16(r0a), vget_low_s16(r1a), vget_low_s16(r2a),
                          vget_low_s16(r3a), vget_low_s16(r4a), vget_low_s16(r5a));
    int32x4_t j1 = jgroup(vget_high_s16(r0a), vget_high_s16(r1a), vget_high_s16(r2a),
                          vget_high_s16(r3a), vget_high_s16(r4a), vget_high_s16(r5a));
    int32x4_t j2 = jgroup(vget_low_s16(r0b), vget_low_s16(r1b), vget_low_s16(r2b),
                          vget_low_s16(r3b), vget_low_s16(r4b), vget_low_s16(r5b));
    int32x4_t j3 = jgroup(vget_high_s16(r0b), vget_high_s16(r1b), vget_high_s16(r2b),
                          vget_high_s16(r3b), vget_high_s16(r4b), vget_high_s16(r5b));
    uint8x8_t lo = vqmovun_s16(vcombine_s16(vqmovn_s32(j0), vqmovn_s32(j1)));
    uint8x8_t hi = vqmovun_s16(vcombine_s16(vqmovn_s32(j2), vqmovn_s32(j3)));
    vst1q_u8(out, vcombine_u8(lo, hi));
}

/* Fully in-bounds 16-wide luma interpolation, 1 <= h <= 16 rows. Caller
 * guarantees the reference window [ix-2..ix+18] x [iy-2..iy+h+3] is inside
 * the padded plane. Narrower blocks go through the dispatcher's copy-out. */
void n264_mc_luma_neon16(uint8_t *dst, int dstride,
                         const uint8_t *ref, int rstride,
                         int ix, int iy, int fx, int fy, int h)
{
    if (fx == 0 && fy == 0) {
        for (int y = 0; y < h; y++)
            vst1q_u8(dst + y * dstride, vld1q_u8(ref + (iy + y) * rstride + ix));
        return;
    }

    uint8_t G[17 * ST], H[17 * ST], V[17 * ST], J[16 * ST];
    int16_t cc[21 * ST];

    int needH = (fx != 0);
    int needV = (fy != 0);
    int needJ = (fx == 2 && fy != 0) || (fy == 2 && fx != 0);
    int needG = ((fx & 1) && fy == 0) || (fx == 0 && (fy & 1));

    if (needG)
        for (int y = 0; y <= h; y++) {
            const uint8_t *p = ref + (iy + y) * rstride + ix;
            vst1q_u8(G + y * ST, vld1q_u8(p));
            G[y * ST + 16] = p[16];
        }
    if (needH)
        for (int y = 0; y <= h; y++)
            hfilt(ref + (iy + y) * rstride + ix - 2, H + y * ST, NULL, 1);
    if (needV)
        for (int y = 0; y <= h; y++) {
            const uint8_t *p = ref + (iy + y) * rstride + ix;
            vfilt(p - 2 * rstride, p - rstride, p, p + rstride,
                  p + 2 * rstride, p + 3 * rstride, V + y * ST);
            /* col 16 for the shifted (x+1) accesses */
            int s = 0; const uint8_t *q = p + 16;
            s = q[-2*rstride] - 5*q[-rstride] + 20*q[0] + 20*q[rstride]
                - 5*q[2*rstride] + q[3*rstride];
            s = (s + 16) >> 5; V[y * ST + 16] = (uint8_t)(s < 0 ? 0 : s > 255 ? 255 : s);
        }
    if (needJ) {
        for (int y = -2; y <= h + 2; y++)
            hfilt(ref + (iy + y) * rstride + ix - 2, NULL, cc + (y + 2) * ST, 0);
        for (int y = 0; y < h; y++)
            jvfilt(cc + (y + 0) * ST, cc + (y + 1) * ST, cc + (y + 2) * ST,
                   cc + (y + 3) * ST, cc + (y + 4) * ST, cc + (y + 5) * ST,
                   J + y * ST);
    }

    for (int y = 0; y < h; y++) {
        uint8_t *d = dst + y * dstride;
        const uint8_t *Hy = H + y * ST, *Vy = V + y * ST, *Jy = J + y * ST;
        const uint8_t *Gy = G + y * ST;
        uint8x16_t out;
        switch (fy * 4 + fx) {
        case 0*4+1: out = vrhaddq_u8(vld1q_u8(Gy), vld1q_u8(Hy)); break;       /* a */
        case 0*4+2: out = vld1q_u8(Hy); break;                                /* b */
        case 0*4+3: out = vrhaddq_u8(vld1q_u8(Gy + 1), vld1q_u8(Hy)); break;   /* c */
        case 1*4+0: out = vrhaddq_u8(vld1q_u8(Gy), vld1q_u8(Vy)); break;       /* d */
        case 2*4+0: out = vld1q_u8(Vy); break;                                /* h */
        case 3*4+0: out = vrhaddq_u8(vld1q_u8(Gy + ST), vld1q_u8(Vy)); break;  /* n */
        case 1*4+1: out = vrhaddq_u8(vld1q_u8(Hy), vld1q_u8(Vy)); break;       /* e */
        case 1*4+2: out = vrhaddq_u8(vld1q_u8(Hy), vld1q_u8(Jy)); break;       /* f */
        case 1*4+3: out = vrhaddq_u8(vld1q_u8(Hy), vld1q_u8(Vy + 1)); break;   /* g */
        case 2*4+1: out = vrhaddq_u8(vld1q_u8(Vy), vld1q_u8(Jy)); break;       /* i */
        case 2*4+2: out = vld1q_u8(Jy); break;                                /* j */
        case 2*4+3: out = vrhaddq_u8(vld1q_u8(Jy), vld1q_u8(Vy + 1)); break;   /* k */
        case 3*4+1: out = vrhaddq_u8(vld1q_u8(Vy), vld1q_u8(Hy + ST)); break;  /* p */
        case 3*4+2: out = vrhaddq_u8(vld1q_u8(Jy), vld1q_u8(Hy + ST)); break;  /* q */
        case 3*4+3: out = vrhaddq_u8(vld1q_u8(Vy + 1), vld1q_u8(Hy + ST)); break; /* r */
        default:    out = vld1q_u8(Gy); break;
        }
        vst1q_u8(d, out);
    }
}

/* 8-wide variants: one 16-byte load provides all six horizontal taps via
 * vext, so the reference window is only [ix-2..ix+13]. Same plane scheme and
 * rounding as the 16-wide kernel; bit-exact with the scalar path. */
static inline void hfilt8(const uint8_t *p, uint8_t *out8, int16_t *out16, int clip)
{
    uint8x16_t v = vld1q_u8(p);
    int16x8_t lo = vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(v)));
    int16x8_t hi = vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(v)));
    int16x8_t l0 = lo;
    int16x8_t l1 = vextq_s16(lo, hi, 1);
    int16x8_t l2 = vextq_s16(lo, hi, 2);
    int16x8_t l3 = vextq_s16(lo, hi, 3);
    int16x8_t l4 = vextq_s16(lo, hi, 4);
    int16x8_t l5 = vextq_s16(lo, hi, 5);
    int16x8_t t = vaddq_s16(l0, l5);
    t = vsubq_s16(t, vmulq_n_s16(vaddq_s16(l1, l4), 5));
    t = vaddq_s16(t, vmulq_n_s16(vaddq_s16(l2, l3), 20));
    if (clip)
        vst1_u8(out8, vqmovun_s16(vshrq_n_s16(vaddq_s16(t, vdupq_n_s16(16)), 5)));
    else
        vst1q_s16(out16, t);
}

static inline void vfilt8(const uint8_t *r0, const uint8_t *r1, const uint8_t *r2,
                          const uint8_t *r3, const uint8_t *r4, const uint8_t *r5,
                          uint8_t *out)
{
    int16x8_t a0 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(r0)));
    int16x8_t a1 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(r1)));
    int16x8_t a2 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(r2)));
    int16x8_t a3 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(r3)));
    int16x8_t a4 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(r4)));
    int16x8_t a5 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(r5)));
    int16x8_t t = vaddq_s16(a0, a5);
    t = vsubq_s16(t, vmulq_n_s16(vaddq_s16(a1, a4), 5));
    t = vaddq_s16(t, vmulq_n_s16(vaddq_s16(a2, a3), 20));
    vst1_u8(out, vqmovun_s16(vshrq_n_s16(vaddq_s16(t, vdupq_n_s16(16)), 5)));
}

static inline void jvfilt8(const int16_t *c0, const int16_t *c1, const int16_t *c2,
                           const int16_t *c3, const int16_t *c4, const int16_t *c5,
                           uint8_t *out)
{
    int16x8_t r0 = vld1q_s16(c0), r1 = vld1q_s16(c1), r2 = vld1q_s16(c2);
    int16x8_t r3 = vld1q_s16(c3), r4 = vld1q_s16(c4), r5 = vld1q_s16(c5);
    int32x4_t j0 = jgroup(vget_low_s16(r0), vget_low_s16(r1), vget_low_s16(r2),
                          vget_low_s16(r3), vget_low_s16(r4), vget_low_s16(r5));
    int32x4_t j1 = jgroup(vget_high_s16(r0), vget_high_s16(r1), vget_high_s16(r2),
                          vget_high_s16(r3), vget_high_s16(r4), vget_high_s16(r5));
    vst1_u8(out, vqmovun_s16(vcombine_s16(vqmovn_s32(j0), vqmovn_s32(j1))));
}

/* Fully in-bounds 8-wide luma interpolation, 1 <= h <= 16 rows. Caller
 * guarantees [ix-2..ix+13] x [iy-2..iy+h+3] is inside the padded plane. */
void n264_mc_luma_neon8(uint8_t *dst, int dstride,
                        const uint8_t *ref, int rstride,
                        int ix, int iy, int fx, int fy, int h)
{
    if (fx == 0 && fy == 0) {
        for (int y = 0; y < h; y++)
            vst1_u8(dst + y * dstride, vld1_u8(ref + (iy + y) * rstride + ix));
        return;
    }

    uint8_t G[17 * ST], H[17 * ST], V[17 * ST], J[16 * ST];
    int16_t cc[21 * ST];

    int needH = (fx != 0);
    int needV = (fy != 0);
    int needJ = (fx == 2 && fy != 0) || (fy == 2 && fx != 0);
    int needG = ((fx & 1) && fy == 0) || (fx == 0 && (fy & 1));

    if (needG)
        for (int y = 0; y <= h; y++) {
            const uint8_t *p = ref + (iy + y) * rstride + ix;
            vst1_u8(G + y * ST, vld1_u8(p));
            G[y * ST + 8] = p[8];
        }
    if (needH)
        for (int y = 0; y <= h; y++)
            hfilt8(ref + (iy + y) * rstride + ix - 2, H + y * ST, NULL, 1);
    if (needV)
        for (int y = 0; y <= h; y++) {
            const uint8_t *p = ref + (iy + y) * rstride + ix;
            vfilt8(p - 2 * rstride, p - rstride, p, p + rstride,
                   p + 2 * rstride, p + 3 * rstride, V + y * ST);
            int s = 0; const uint8_t *q = p + 8;
            s = q[-2*rstride] - 5*q[-rstride] + 20*q[0] + 20*q[rstride]
                - 5*q[2*rstride] + q[3*rstride];
            s = (s + 16) >> 5; V[y * ST + 8] = (uint8_t)(s < 0 ? 0 : s > 255 ? 255 : s);
        }
    if (needJ) {
        for (int y = -2; y <= h + 2; y++)
            hfilt8(ref + (iy + y) * rstride + ix - 2, NULL, cc + (y + 2) * ST, 0);
        for (int y = 0; y < h; y++)
            jvfilt8(cc + (y + 0) * ST, cc + (y + 1) * ST, cc + (y + 2) * ST,
                    cc + (y + 3) * ST, cc + (y + 4) * ST, cc + (y + 5) * ST,
                    J + y * ST);
    }

    for (int y = 0; y < h; y++) {
        uint8_t *d = dst + y * dstride;
        const uint8_t *Hy = H + y * ST, *Vy = V + y * ST, *Jy = J + y * ST;
        const uint8_t *Gy = G + y * ST;
        uint8x8_t out;
        switch (fy * 4 + fx) {
        case 0*4+1: out = vrhadd_u8(vld1_u8(Gy), vld1_u8(Hy)); break;         /* a */
        case 0*4+2: out = vld1_u8(Hy); break;                                 /* b */
        case 0*4+3: out = vrhadd_u8(vld1_u8(Gy + 1), vld1_u8(Hy)); break;      /* c */
        case 1*4+0: out = vrhadd_u8(vld1_u8(Gy), vld1_u8(Vy)); break;          /* d */
        case 2*4+0: out = vld1_u8(Vy); break;                                 /* h */
        case 3*4+0: out = vrhadd_u8(vld1_u8(Gy + ST), vld1_u8(Vy)); break;     /* n */
        case 1*4+1: out = vrhadd_u8(vld1_u8(Hy), vld1_u8(Vy)); break;          /* e */
        case 1*4+2: out = vrhadd_u8(vld1_u8(Hy), vld1_u8(Jy)); break;          /* f */
        case 1*4+3: out = vrhadd_u8(vld1_u8(Hy), vld1_u8(Vy + 1)); break;      /* g */
        case 2*4+1: out = vrhadd_u8(vld1_u8(Vy), vld1_u8(Jy)); break;          /* i */
        case 2*4+2: out = vld1_u8(Jy); break;                                 /* j */
        case 2*4+3: out = vrhadd_u8(vld1_u8(Jy), vld1_u8(Vy + 1)); break;      /* k */
        case 3*4+1: out = vrhadd_u8(vld1_u8(Vy), vld1_u8(Hy + ST)); break;     /* p */
        case 3*4+2: out = vrhadd_u8(vld1_u8(Jy), vld1_u8(Hy + ST)); break;     /* q */
        case 3*4+3: out = vrhadd_u8(vld1_u8(Vy + 1), vld1_u8(Hy + ST)); break; /* r */
        default:    out = vld1_u8(Gy); break;
        }
        vst1_u8(d, out);
    }
}

/* ---- hpel plane build row helpers ----------------------------------------
 * Vectorized interiors of n264_mc_build_hpel_rows (mc.c keeps the border
 * columns scalar). All are pure functions of their inputs, so overlapping
 * tail vectors recompute identical values -- bit-exact and band-safe.
 * Load discipline: the reference plane is only guaranteed readable on
 * [0, pw); the horizontal helper therefore uses two 8-byte loads covering
 * exactly offsets [x-2, x+11) instead of one 16-byte load. */

/* srow[x] = unclipped tap6(row[x-2..x+3]) for x in [x0, x1); caller
 * guarantees x0 >= 2 and x1 <= pw - 3 and x1 - x0 >= 8. */
void n264_hpel_hrow_neon(int32_t *srow, const uint8_t *row, int x0, int x1)
{
    for (int x = x0; x < x1; x += 8) {
        if (x > x1 - 8) x = x1 - 8;                 /* overlap tail */
        const uint8_t *p = row + x - 2;
        int16x8_t lo = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(p)));      /* off 0..7 */
        int16x8_t hi = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(p + 5)));  /* off 5..12 */
        int16x8_t xh = vextq_s16(hi, hi, 3);        /* lanes 0..4 = off 8..12 */
        int16x8_t l0 = lo;
        int16x8_t l1 = vextq_s16(lo, xh, 1);
        int16x8_t l2 = vextq_s16(lo, xh, 2);
        int16x8_t l3 = vextq_s16(lo, xh, 3);
        int16x8_t l4 = vextq_s16(lo, xh, 4);
        int16x8_t l5 = vextq_s16(lo, xh, 5);
        int16x8_t t = vaddq_s16(l0, l5);
        t = vsubq_s16(t, vmulq_n_s16(vaddq_s16(l1, l4), 5));
        t = vaddq_s16(t, vmulq_n_s16(vaddq_s16(l2, l3), 20));
        vst1q_s32(srow + x, vmovl_s16(vget_low_s16(t)));
        vst1q_s32(srow + x + 4, vmovl_s16(vget_high_s16(t)));
    }
}

/* H/V/C output for x in [x0, x1): Hr = (s2+16)>>5 clip, Cr = vertical tap6 of
 * the six int32 scratch rows then (+512)>>10 clip, Vr = vertical tap6 of the
 * six integer rows then (+16)>>5 clip. Caller guarantees 0 <= x0, x1 <= pw
 * (so the r* reads need no clamp) and x1 - x0 >= 8. */
void n264_hpel_outrow_neon(uint8_t *Hr, uint8_t *Vr, uint8_t *Cr,
                           const int32_t *s0, const int32_t *s1, const int32_t *s2,
                           const int32_t *s3, const int32_t *s4, const int32_t *s5,
                           const uint8_t *r0, const uint8_t *r1, const uint8_t *r2,
                           const uint8_t *r3, const uint8_t *r4, const uint8_t *r5,
                           int x0, int x1)
{
    for (int x = x0; x < x1; x += 8) {
        if (x > x1 - 8) x = x1 - 8;                 /* overlap tail */
        /* H: clip1((s2[x] + 16) >> 5) */
        int32x4_t h0 = vshrq_n_s32(vaddq_s32(vld1q_s32(s2 + x), vdupq_n_s32(16)), 5);
        int32x4_t h1 = vshrq_n_s32(vaddq_s32(vld1q_s32(s2 + x + 4), vdupq_n_s32(16)), 5);
        vst1_u8(Hr + x, vqmovun_s16(vcombine_s16(vqmovn_s32(h0), vqmovn_s32(h1))));

        /* C: clip1((tap6(s0..s5[x]) + 512) >> 10), int32 throughout */
        for (int half = 0; half < 2; half++) {
            int off = x + 4 * half;
            int32x4_t c = vaddq_s32(vld1q_s32(s0 + off), vld1q_s32(s5 + off));
            c = vsubq_s32(c, vmulq_n_s32(vaddq_s32(vld1q_s32(s1 + off), vld1q_s32(s4 + off)), 5));
            c = vaddq_s32(c, vmulq_n_s32(vaddq_s32(vld1q_s32(s2 + off), vld1q_s32(s3 + off)), 20));
            c = vshrq_n_s32(vaddq_s32(c, vdupq_n_s32(512)), 10);
            if (half == 0)
                h0 = c;
            else
                vst1_u8(Cr + x, vqmovun_s16(vcombine_s16(vqmovn_s32(h0), vqmovn_s32(c))));
        }

        /* V: clip1((tap6(r0..r5[x]) + 16) >> 5), 16-bit taps */
        int16x8_t a0 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(r0 + x)));
        int16x8_t a1 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(r1 + x)));
        int16x8_t a2 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(r2 + x)));
        int16x8_t a3 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(r3 + x)));
        int16x8_t a4 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(r4 + x)));
        int16x8_t a5 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(r5 + x)));
        int16x8_t t = vaddq_s16(a0, a5);
        t = vsubq_s16(t, vmulq_n_s16(vaddq_s16(a1, a4), 5));
        t = vaddq_s16(t, vmulq_n_s16(vaddq_s16(a2, a3), 20));
        vst1_u8(Vr + x, vqmovun_s16(vshrq_n_s16(vaddq_s16(t, vdupq_n_s16(16)), 5)));
    }
}

/* Chroma bilinear (eighth-pel) for an in-bounds 8x8 block. */
void n264_mc_chroma_neon8(uint8_t *dst, int dstride, const uint8_t *ref,
                          int rstride, int ix, int iy, int fx, int fy)
{
    uint8_t w0 = (uint8_t)((8 - fx) * (8 - fy));
    uint8_t w1 = (uint8_t)(fx * (8 - fy));
    uint8_t w2 = (uint8_t)((8 - fx) * fy);
    uint8_t w3 = (uint8_t)(fx * fy);
    uint8x8_t vw0 = vdup_n_u8(w0), vw1 = vdup_n_u8(w1);
    uint8x8_t vw2 = vdup_n_u8(w2), vw3 = vdup_n_u8(w3);
    for (int y = 0; y < 8; y++) {
        const uint8_t *r = ref + (iy + y) * rstride + ix;
        uint16x8_t acc = vmull_u8(vld1_u8(r), vw0);
        acc = vmlal_u8(acc, vld1_u8(r + 1), vw1);
        acc = vmlal_u8(acc, vld1_u8(r + rstride), vw2);
        acc = vmlal_u8(acc, vld1_u8(r + rstride + 1), vw3);
        acc = vaddq_u16(acc, vdupq_n_u16(32));
        vst1_u8(dst + y * dstride, vshrn_n_u16(acc, 6));
    }
}

/* Width-8, any height (16x8 partitions, 4:2:2 blocks). Same bilinear as the
 * 8x8 kernel; bit-exact with n264_mc_chroma_c's in-bounds path. */
void n264_mc_chroma_neon_w8h(uint8_t *dst, int dstride, const uint8_t *ref,
                             int rstride, int ix, int iy, int fx, int fy, int h)
{
    uint8x8_t vw0 = vdup_n_u8((uint8_t)((8 - fx) * (8 - fy)));
    uint8x8_t vw1 = vdup_n_u8((uint8_t)(fx * (8 - fy)));
    uint8x8_t vw2 = vdup_n_u8((uint8_t)((8 - fx) * fy));
    uint8x8_t vw3 = vdup_n_u8((uint8_t)(fx * fy));
    for (int y = 0; y < h; y++) {
        const uint8_t *r = ref + (iy + y) * rstride + ix;
        uint16x8_t acc = vmull_u8(vld1_u8(r), vw0);
        acc = vmlal_u8(acc, vld1_u8(r + 1), vw1);
        acc = vmlal_u8(acc, vld1_u8(r + rstride), vw2);
        acc = vmlal_u8(acc, vld1_u8(r + rstride + 1), vw3);
        acc = vaddq_u16(acc, vdupq_n_u16(32));
        vst1_u8(dst + y * dstride, vshrn_n_u16(acc, 6));
    }
}


/* Width-4, any even height. THE SHAPE IS THE POINT: at half a vector per row
 * the width-8 kernel's per-row load/store assembly dominates the four multiply
 * accumulates, which is why round 2 measured a straight w4 port at or below the
 * auto-vectorised C and left narrow blocks scalar. This one packs TWO output
 * rows into each vector -- lanes 0-3 are row y, lanes 4-7 row y+1 -- so a full
 * uint8x8 of work happens per pass and the three source rows a pair needs are
 * six 32-bit loads instead of ten 8-byte ones.
 *
 * It matters because width 4 is not a corner: 77% of mc_chroma calls at the
 * board's samsung point are w4 (2.12M of 2.75M) and every one of them was
 * running the C. Bit-exact with n264_mc_chroma_c's in-bounds path. */
static inline uint8x8_t mcc_ld2x4(const uint8_t *a, const uint8_t *b)
{
    uint32_t x, y;
    memcpy(&x, a, 4);
    memcpy(&y, b, 4);
    return vreinterpret_u8_u32(vset_lane_u32(y, vdup_n_u32(x), 1));
}

void n264_mc_chroma_neon_w4h(uint8_t *dst, int dstride, const uint8_t *ref,
                             int rstride, int ix, int iy, int fx, int fy, int h)
{
    uint8x8_t vw0 = vdup_n_u8((uint8_t)((8 - fx) * (8 - fy)));
    uint8x8_t vw1 = vdup_n_u8((uint8_t)(fx * (8 - fy)));
    uint8x8_t vw2 = vdup_n_u8((uint8_t)((8 - fx) * fy));
    uint8x8_t vw3 = vdup_n_u8((uint8_t)(fx * fy));
    for (int y = 0; y < h; y += 2) {
        const uint8_t *r0 = ref + (iy + y) * rstride + ix;
        const uint8_t *r1 = r0 + rstride, *r2 = r1 + rstride;
        uint16x8_t acc = vmull_u8(mcc_ld2x4(r0, r1), vw0);
        acc = vmlal_u8(acc, mcc_ld2x4(r0 + 1, r1 + 1), vw1);
        acc = vmlal_u8(acc, mcc_ld2x4(r1, r2), vw2);
        acc = vmlal_u8(acc, mcc_ld2x4(r1 + 1, r2 + 1), vw3);
        acc = vaddq_u16(acc, vdupq_n_u16(32));
        uint32x2_t o = vreinterpret_u32_u8(vshrn_n_u16(acc, 6));
        uint32_t o0 = vget_lane_u32(o, 0), o1 = vget_lane_u32(o, 1);
        memcpy(dst + y * dstride, &o0, 4);
        memcpy(dst + (y + 1) * dstride, &o1, 4);
    }
}

/* Weighted average of two packed predictions .
 * (32, 32) -- ordinary unweighted bi-prediction and the overwhelmingly common
 * case -- is exactly the rounding average (32*(a+b) + 32) >> 6 == (a+b+1) >> 1,
 * so it is one URHADD per sixteen samples. The weighted form stays in int16
 * lanes: w0 + w1 == 64 with both in [-64, 128] bounds the sum at 255*128 + 32
 * = 32672, and SQSHRUN's saturating narrow IS the Clip1. */
void n264_pixel_avg_wt_neon(uint8_t *dst, const uint8_t *a, const uint8_t *b,
                            int n, int w0, int w1)
{
    int i = 0;
    if (w0 == 32 && w1 == 32) {
        for (; i + 16 <= n; i += 16)
            vst1q_u8(dst + i, vrhaddq_u8(vld1q_u8(a + i), vld1q_u8(b + i)));
    } else {
        int16x8_t k0 = vdupq_n_s16((int16_t)w0), k1 = vdupq_n_s16((int16_t)w1);
        int16x8_t rnd = vdupq_n_s16(32);
        for (; i + 16 <= n; i += 16) {
            uint8x16_t va = vld1q_u8(a + i), vb = vld1q_u8(b + i);
            int16x8_t alo = vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(va)));
            int16x8_t ahi = vreinterpretq_s16_u16(vmovl_high_u8(va));
            int16x8_t blo = vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(vb)));
            int16x8_t bhi = vreinterpretq_s16_u16(vmovl_high_u8(vb));
            int16x8_t lo = vaddq_s16(vaddq_s16(vmulq_s16(alo, k0), vmulq_s16(blo, k1)), rnd);
            int16x8_t hi = vaddq_s16(vaddq_s16(vmulq_s16(ahi, k0), vmulq_s16(bhi, k1)), rnd);
            vst1q_u8(dst + i, vcombine_u8(vqshrun_n_s16(lo, 6), vqshrun_n_s16(hi, 6)));
        }
    }
    for (; i < n; i++) {
        int v = (a[i] * w0 + b[i] * w1 + 32) >> 6;
        dst[i] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
    }
}


/* --- half-pel plane fetch: strided copy and 2-tap rounding average ---
 * The subpel prediction fetch (build_pred_hpel in me.c) reads one or two
 * registered half-pel planes at a source stride and writes a stride-16 pred
 * block. x264 has <reference-internal>/w8/w16 and <reference-internal>/w8/w16 for exactly this.
 * Widths are 4, 8 or 16, so each row is one, one half, or one quarter of a
 * vector; (a+b+1)>>1 is vrhadd. */
void n264_pred_copy_neon(uint8_t *dst, int dstride,
                         const uint8_t *s, int sstride, int w, int h)
{
    if (w == 16) {
        for (int y = 0; y < h; y++)
            vst1q_u8(dst + y * dstride, vld1q_u8(s + y * sstride));
    } else if (w == 8) {
        for (int y = 0; y < h; y++)
            vst1_u8(dst + y * dstride, vld1_u8(s + y * sstride));
    } else {
        for (int y = 0; y < h; y++)
            memcpy(dst + y * dstride, s + y * sstride, 4);
    }
}

void n264_pred_avg2_neon(uint8_t *dst, int dstride, const uint8_t *s1,
                         const uint8_t *s2, int sstride, int w, int h)
{
    if (w == 16) {
        for (int y = 0; y < h; y++)
            vst1q_u8(dst + y * dstride,
                     vrhaddq_u8(vld1q_u8(s1 + y * sstride), vld1q_u8(s2 + y * sstride)));
    } else if (w == 8) {
        for (int y = 0; y < h; y++)
            vst1_u8(dst + y * dstride,
                    vrhadd_u8(vld1_u8(s1 + y * sstride), vld1_u8(s2 + y * sstride)));
    } else {
        /* four bytes a row: pair two rows into one 8-lane operation */
        int y = 0;
        for (; y + 2 <= h; y += 2) {
            uint8x8_t a = vreinterpret_u8_u32(vld1_lane_u32((const uint32_t *)(const void *)(s1 + (size_t)y * sstride),
                              vreinterpret_u32_u8(vdup_n_u8(0)), 0));
            a = vreinterpret_u8_u32(vld1_lane_u32((const uint32_t *)(const void *)(s1 + (size_t)(y + 1) * sstride),
                              vreinterpret_u32_u8(a), 1));
            uint8x8_t b = vreinterpret_u8_u32(vld1_lane_u32((const uint32_t *)(const void *)(s2 + (size_t)y * sstride),
                              vreinterpret_u32_u8(vdup_n_u8(0)), 0));
            b = vreinterpret_u8_u32(vld1_lane_u32((const uint32_t *)(const void *)(s2 + (size_t)(y + 1) * sstride),
                              vreinterpret_u32_u8(b), 1));
            uint32x2_t r = vreinterpret_u32_u8(vrhadd_u8(a, b));
            vst1_lane_u32((uint32_t *)(void *)(dst + (size_t)y * dstride), r, 0);
            vst1_lane_u32((uint32_t *)(void *)(dst + (size_t)(y + 1) * dstride), r, 1);
        }
        for (; y < h; y++)
            for (int x = 0; x < 4; x++)
                dst[y * dstride + x] = (uint8_t)((s1[y * sstride + x] + s2[y * sstride + x] + 1) >> 1);
    }
}

#endif /* __aarch64__ */
