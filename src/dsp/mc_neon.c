/*
 * mc_neon.c - aarch64 NEON luma interpolation for the in-bounds 16x16 case
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
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
void y264_mc_luma_neon16(uint8_t *dst, int dstride,
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
void y264_mc_luma_neon8(uint8_t *dst, int dstride,
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
 * Vectorized interiors of y264_mc_build_hpel_rows (mc.c keeps the border
 * columns scalar). All are pure functions of their inputs, so overlapping
 * tail vectors recompute identical values -- bit-exact and band-safe.
 * Load discipline: the reference plane is only guaranteed readable on
 * [0, pw); the horizontal helper therefore uses two 8-byte loads covering
 * exactly offsets [x-2, x+11) instead of one 16-byte load. */

/* srow[x] = unclipped tap6(row[x-2..x+3]) for x in [x0, x1); caller
 * guarantees x0 >= 2 and x1 <= pw - 3 and x1 - x0 >= 8. */
void y264_hpel_hrow_neon(int32_t *srow, const uint8_t *row, int x0, int x1)
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
void y264_hpel_outrow_neon(uint8_t *Hr, uint8_t *Vr, uint8_t *Cr,
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

/* The whole half-pel build for a band of output rows, borders included, in
 * one NEON pass (the row helpers above leave the border columns to scalar
 * code and stage the horizontal taps as int32). Each source row is clamped
 * into a padded copy once, its horizontal 6-tap kept as int16 (the tap of
 * bytes spans [-2550, 10710]); a ring of six of each feeds the output rows:
 *   H = clip1((htap + 16) >> 5)          the rounding narrow does it
 *   V = clip1((vtap of bytes + 16) >> 5) int16 throughout
 *   C = clip1((vtap of htaps + 512) >> 10), the pair sums in int16 (at most
 *       21420) and the weighted sum in int32
 * Bit-exact with y264_mc_build_hpel_rows. `mem` must hold
 * 6 * (W + 32) + 12 * W bytes, W = pw + 2 * border, and W >= 16. */
void y264_hpel_build_band_neon(uint8_t *Hp, uint8_t *Vp, uint8_t *Cp, int stride,
                               const uint8_t *ref, int rstride, int pw, int ph,
                               int border, uint8_t *mem, int y0, int y1)
{
    const int W = pw + 2 * border, PW = W + 32;
    uint8_t *P[6];
    int16_t *T[6];
    for (int k = 0; k < 6; k++) {
        P[k] = mem + (size_t)k * PW;
        T[k] = (int16_t *)(void *)(mem + (size_t)6 * PW + (size_t)k * W * 2);
    }
    const int sy0 = y0 - 2;
    for (int y = sy0; y < y1 + 3; y++) {
        int k = (y - sy0) % 6;
        int cy = y < 0 ? 0 : (y >= ph ? ph - 1 : y);
        const uint8_t *row = ref + (size_t)cy * rstride;
        uint8_t *pad = P[k];                 /* pad[j] = row[clamp(j - border - 2)] */
        memset(pad, row[0], (size_t)border + 2);
        memcpy(pad + border + 2, row, (size_t)pw);
        memset(pad + border + 2 + pw, row[pw - 1], (size_t)border + 3);
        int16_t *t = T[k];
        for (int j = 0; j < W; j += 16) {
            if (j > W - 16) j = W - 16;      /* overlap tail */
            /* c is the next sixteen pad bytes; only its first five reach a
             * live lane (pad index j + 20 <= W + 4 at the last block), the
             * rest lie inside the pad's 32-byte slack */
            uint8x16_t a = vld1q_u8(pad + j), c = vld1q_u8(pad + j + 16);
            uint8x16_t a1 = vextq_u8(a, c, 1), a2 = vextq_u8(a, c, 2);
            uint8x16_t a3 = vextq_u8(a, c, 3), a4 = vextq_u8(a, c, 4);
            uint8x16_t b = vextq_u8(a, c, 5);
            int16x8_t lo = vreinterpretq_s16_u16(vaddl_u8(vget_low_u8(a), vget_low_u8(b)));
            int16x8_t hi = vreinterpretq_s16_u16(vaddl_high_u8(a, b));
            int16x8_t m5l = vreinterpretq_s16_u16(vaddl_u8(vget_low_u8(a1), vget_low_u8(a4)));
            int16x8_t m5h = vreinterpretq_s16_u16(vaddl_high_u8(a1, a4));
            int16x8_t m20l = vreinterpretq_s16_u16(vaddl_u8(vget_low_u8(a2), vget_low_u8(a3)));
            int16x8_t m20h = vreinterpretq_s16_u16(vaddl_high_u8(a2, a3));
            lo = vmlsq_n_s16(vmlaq_n_s16(lo, m20l, 20), m5l, 5);
            hi = vmlsq_n_s16(vmlaq_n_s16(hi, m20h, 20), m5h, 5);
            vst1q_s16(t + j, lo);
            vst1q_s16(t + j + 8, hi);
        }
        if (y < y0 + 3) continue;
        int oy = y - 3;                      /* rows oy-2 .. oy+3 are in the rings */
        int k0 = (oy - 2 - sy0) % 6;
        const uint8_t *p0 = P[k0], *p1 = P[(k0 + 1) % 6], *p2 = P[(k0 + 2) % 6];
        const uint8_t *p3 = P[(k0 + 3) % 6], *p4 = P[(k0 + 4) % 6], *p5 = P[(k0 + 5) % 6];
        const int16_t *t0 = T[k0], *t1 = T[(k0 + 1) % 6], *t2 = T[(k0 + 2) % 6];
        const int16_t *t3 = T[(k0 + 3) % 6], *t4 = T[(k0 + 4) % 6], *t5 = T[(k0 + 5) % 6];
        uint8_t *Hr = Hp + (ptrdiff_t)oy * stride - border;
        uint8_t *Vr = Vp + (ptrdiff_t)oy * stride - border;
        uint8_t *Cr = Cp + (ptrdiff_t)oy * stride - border;
        for (int j = 0; j < W; j += 16) {
            if (j > W - 16) j = W - 16;
            /* H from the centre row's taps */
            vst1q_u8(Hr + j, vcombine_u8(vqrshrun_n_s16(vld1q_s16(t2 + j), 5),
                                         vqrshrun_n_s16(vld1q_s16(t2 + j + 8), 5)));
            /* V: the vertical tap over the padded rows (pad column j + 2 is
             * output column j) */
            int o = j + 2;
            uint8x16_t b0 = vld1q_u8(p0 + o), b1 = vld1q_u8(p1 + o), b2 = vld1q_u8(p2 + o);
            uint8x16_t b3 = vld1q_u8(p3 + o), b4 = vld1q_u8(p4 + o), b5 = vld1q_u8(p5 + o);
            int16x8_t vl = vreinterpretq_s16_u16(vaddl_u8(vget_low_u8(b0), vget_low_u8(b5)));
            int16x8_t vh = vreinterpretq_s16_u16(vaddl_high_u8(b0, b5));
            vl = vmlaq_n_s16(vl, vreinterpretq_s16_u16(vaddl_u8(vget_low_u8(b2), vget_low_u8(b3))), 20);
            vh = vmlaq_n_s16(vh, vreinterpretq_s16_u16(vaddl_high_u8(b2, b3)), 20);
            vl = vmlsq_n_s16(vl, vreinterpretq_s16_u16(vaddl_u8(vget_low_u8(b1), vget_low_u8(b4))), 5);
            vh = vmlsq_n_s16(vh, vreinterpretq_s16_u16(vaddl_high_u8(b1, b4)), 5);
            vst1q_u8(Vr + j, vcombine_u8(vqrshrun_n_s16(vl, 5), vqrshrun_n_s16(vh, 5)));
            /* C: the vertical tap over the horizontal taps */
            uint8x8_t cc[2];
            for (int h = 0; h < 2; h++) {
                int q = j + 8 * h;
                int16x8_t s05 = vaddq_s16(vld1q_s16(t0 + q), vld1q_s16(t5 + q));
                int16x8_t s14 = vaddq_s16(vld1q_s16(t1 + q), vld1q_s16(t4 + q));
                int16x8_t s23 = vaddq_s16(vld1q_s16(t2 + q), vld1q_s16(t3 + q));
                int32x4_t cl = vmull_n_s16(vget_low_s16(s23), 20);
                int32x4_t ch = vmull_high_n_s16(s23, 20);
                cl = vmlsl_n_s16(cl, vget_low_s16(s14), 5);
                ch = vmlsl_high_n_s16(ch, s14, 5);
                cl = vaddw_s16(cl, vget_low_s16(s05));
                ch = vaddw_high_s16(ch, s05);
                int16x8_t n = vcombine_s16(vqmovn_s32(vrshrq_n_s32(cl, 10)),
                                           vqmovn_s32(vrshrq_n_s32(ch, 10)));
                cc[h] = vqmovun_s16(n);
            }
            vst1q_u8(Cr + j, vcombine_u8(cc[0], cc[1]));
        }
    }
}

/* Width-8 chroma bilinear rows. Each source row is loaded once and serves
 * as the bottom of one output row and the top of the next; the rounding
 * shift folds the +32. Bit-exact with y264_mc_chroma_c's in-bounds path. */
static inline void mcc_w8_rows(uint8_t *dst, int dstride, const uint8_t *r, int rstride,
                               int fx, int fy, int h)
{
    /* the terms whose weight is zero contribute nothing to the sum, so a
     * phase with fx or fy zero drops them, and the zero phase is a copy
     * ((64 * a + 32) >> 6 == a) */
    if (!(fx | fy)) {
        for (int y = 0; y < h; y++, dst += dstride, r += rstride)
            vst1_u8(dst, vld1_u8(r));
        return;
    }
    if (!fx) {
        uint8x8_t k0 = vdup_n_u8((uint8_t)(8 * (8 - fy))), k2 = vdup_n_u8((uint8_t)(8 * fy));
        uint8x8_t t0 = vld1_u8(r);
        for (int y = 0; y < h; y++, dst += dstride) {
            r += rstride;
            uint8x8_t b0 = vld1_u8(r);
            vst1_u8(dst, vrshrn_n_u16(vmlal_u8(vmull_u8(t0, k0), b0, k2), 6));
            t0 = b0;
        }
        return;
    }
    if (!fy) {
        uint8x8_t k0 = vdup_n_u8((uint8_t)((8 - fx) * 8)), k1 = vdup_n_u8((uint8_t)(fx * 8));
        for (int y = 0; y < h; y++, dst += dstride, r += rstride)
            vst1_u8(dst, vrshrn_n_u16(vmlal_u8(vmull_u8(vld1_u8(r), k0), vld1_u8(r + 1), k1), 6));
        return;
    }
    uint8x8_t vw0 = vdup_n_u8((uint8_t)((8 - fx) * (8 - fy)));
    uint8x8_t vw1 = vdup_n_u8((uint8_t)(fx * (8 - fy)));
    uint8x8_t vw2 = vdup_n_u8((uint8_t)((8 - fx) * fy));
    uint8x8_t vw3 = vdup_n_u8((uint8_t)(fx * fy));
    uint8x8_t t0 = vld1_u8(r), t1 = vld1_u8(r + 1);
    for (int y = 0; y < h; y++) {
        r += rstride;
        uint8x8_t b0 = vld1_u8(r), b1 = vld1_u8(r + 1);
        uint16x8_t acc = vmull_u8(t0, vw0);
        acc = vmlal_u8(acc, t1, vw1);
        acc = vmlal_u8(acc, b0, vw2);
        acc = vmlal_u8(acc, b1, vw3);
        vst1_u8(dst, vrshrn_n_u16(acc, 6));
        dst += dstride;
        t0 = b0; t1 = b1;
    }
}

/* Two in-bounds 8x8 chroma bilinear predictions, one per list, averaged
 * into dst in the same pass: the rows of y264_mc_chroma_neon8 for each list,
 * then the weighted average of y264_pixel_avg_wt_neon ((a + b + 1) >> 1 at
 * the flat pair, else (a*w0 + b*w1 + 32) >> 6 saturated). Bit-exact with
 * the two compensations into buffers followed by the average. */
void y264_mc_chroma_bi8_neon(uint8_t *dst, int dstride,
                             const uint8_t *r0, int s0, int fx0, int fy0,
                             const uint8_t *r1, int s1, int fx1, int fy1,
                             int w0, int w1)
{
    uint8x8_t a0 = vdup_n_u8((uint8_t)((8 - fx0) * (8 - fy0))), a1 = vdup_n_u8((uint8_t)(fx0 * (8 - fy0)));
    uint8x8_t a2 = vdup_n_u8((uint8_t)((8 - fx0) * fy0)), a3 = vdup_n_u8((uint8_t)(fx0 * fy0));
    uint8x8_t b0 = vdup_n_u8((uint8_t)((8 - fx1) * (8 - fy1))), b1 = vdup_n_u8((uint8_t)(fx1 * (8 - fy1)));
    uint8x8_t b2 = vdup_n_u8((uint8_t)((8 - fx1) * fy1)), b3 = vdup_n_u8((uint8_t)(fx1 * fy1));
    int flat = w0 == 32 && w1 == 32;
    int16x8_t k0 = vdupq_n_s16((int16_t)w0), k1 = vdupq_n_s16((int16_t)w1);
    int16x8_t rnd = vdupq_n_s16(32);
    uint8x8_t t0 = vld1_u8(r0), t1 = vld1_u8(r0 + 1);
    uint8x8_t u0 = vld1_u8(r1), u1 = vld1_u8(r1 + 1);
    for (int y = 0; y < 8; y++) {
        r0 += s0; r1 += s1;
        uint8x8_t n0 = vld1_u8(r0), n1 = vld1_u8(r0 + 1);
        uint8x8_t m0 = vld1_u8(r1), m1 = vld1_u8(r1 + 1);
        uint16x8_t p = vmull_u8(t0, a0);
        p = vmlal_u8(p, t1, a1); p = vmlal_u8(p, n0, a2); p = vmlal_u8(p, n1, a3);
        uint16x8_t q = vmull_u8(u0, b0);
        q = vmlal_u8(q, u1, b1); q = vmlal_u8(q, m0, b2); q = vmlal_u8(q, m1, b3);
        uint8x8_t pa = vrshrn_n_u16(p, 6), pb = vrshrn_n_u16(q, 6);
        uint8x8_t out;
        if (flat) {
            out = vrhadd_u8(pa, pb);
        } else {
            int16x8_t v = vaddq_s16(vaddq_s16(vmulq_s16(vreinterpretq_s16_u16(vmovl_u8(pa)), k0),
                                              vmulq_s16(vreinterpretq_s16_u16(vmovl_u8(pb)), k1)), rnd);
            out = vqshrun_n_s16(v, 6);
        }
        vst1_u8(dst, out);
        dst += dstride;
        t0 = n0; t1 = n1; u0 = m0; u1 = m1;
    }
}

/* Chroma bilinear (eighth-pel) for an in-bounds 8x8 block. */
void y264_mc_chroma_neon8(uint8_t *dst, int dstride, const uint8_t *ref,
                          int rstride, int ix, int iy, int fx, int fy)
{
    mcc_w8_rows(dst, dstride, ref + iy * rstride + ix, rstride, fx, fy, 8);
}

/* Width-8, any height (16x8 partitions, 4:2:2 blocks). */
void y264_mc_chroma_neon_w8h(uint8_t *dst, int dstride, const uint8_t *ref,
                             int rstride, int ix, int iy, int fx, int fy, int h)
{
    mcc_w8_rows(dst, dstride, ref + iy * rstride + ix, rstride, fx, fy, h);
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
 * running the C. Bit-exact with y264_mc_chroma_c's in-bounds path. */
static inline uint8x8_t mcc_ld2x4(const uint8_t *a, const uint8_t *b)
{
    uint32_t x, y;
    memcpy(&x, a, 4);
    memcpy(&y, b, 4);
    return vreinterpret_u8_u32(vset_lane_u32(y, vdup_n_u32(x), 1));
}

void y264_mc_chroma_neon_w4h(uint8_t *dst, int dstride, const uint8_t *ref,
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
        uint32x2_t o = vreinterpret_u32_u8(vrshrn_n_u16(acc, 6));
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
void y264_pixel_avg_wt_neon(uint8_t *dst, const uint8_t *a, const uint8_t *b,
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


/* Strided form of y264_pixel_avg_wt_neon: eight samples per step (a
 * macroblock's 8x8 quadrants and 16-wide rows are the common shapes), a
 * scalar tail for the 4- and 2-wide chroma blocks. Same lane bound, same
 * saturating narrow, so it is exact against the C form. */
void y264_pixel_avg_wt_2d_neon(uint8_t *dst, int ds, const uint8_t *a, int as,
                               const uint8_t *b, int bs, int w, int h, int w0, int w1)
{
    int16x8_t k0 = vdupq_n_s16((int16_t)w0), k1 = vdupq_n_s16((int16_t)w1);
    int16x8_t rnd = vdupq_n_s16(32);
    int flat = w0 == 32 && w1 == 32;
    /* the flat pair at the two macroblock widths: one rounding average per
     * row, without the general row's width loops and scalar tail */
    if (flat && w == 16) {
        for (int y = 0; y < h; y++, dst += ds, a += as, b += bs)
            vst1q_u8(dst, vrhaddq_u8(vld1q_u8(a), vld1q_u8(b)));
        return;
    }
    if (flat && w == 8) {
        for (int y = 0; y < h; y++, dst += ds, a += as, b += bs)
            vst1_u8(dst, vrhadd_u8(vld1_u8(a), vld1_u8(b)));
        return;
    }
    for (int y = 0; y < h; y++, dst += ds, a += as, b += bs) {
        int x = 0;
        if (flat) {
            for (; x + 16 <= w; x += 16)
                vst1q_u8(dst + x, vrhaddq_u8(vld1q_u8(a + x), vld1q_u8(b + x)));
            for (; x + 8 <= w; x += 8)
                vst1_u8(dst + x, vrhadd_u8(vld1_u8(a + x), vld1_u8(b + x)));
        } else {
            for (; x + 16 <= w; x += 16) {
                uint8x16_t ua = vld1q_u8(a + x), ub = vld1q_u8(b + x);
                int16x8_t al = vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(ua)));
                int16x8_t ah = vreinterpretq_s16_u16(vmovl_high_u8(ua));
                int16x8_t bl = vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(ub)));
                int16x8_t bh = vreinterpretq_s16_u16(vmovl_high_u8(ub));
                int16x8_t lo = vaddq_s16(vaddq_s16(vmulq_s16(al, k0), vmulq_s16(bl, k1)), rnd);
                int16x8_t hi = vaddq_s16(vaddq_s16(vmulq_s16(ah, k0), vmulq_s16(bh, k1)), rnd);
                vst1q_u8(dst + x, vcombine_u8(vqshrun_n_s16(lo, 6), vqshrun_n_s16(hi, 6)));
            }
            for (; x + 8 <= w; x += 8) {
                int16x8_t va = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(a + x)));
                int16x8_t vb = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(b + x)));
                int16x8_t v = vaddq_s16(vaddq_s16(vmulq_s16(va, k0), vmulq_s16(vb, k1)), rnd);
                vst1_u8(dst + x, vqshrun_n_s16(v, 6));
            }
        }
        for (; x < w; x++) {
            int v = (a[x] * w0 + b[x] * w1 + 32) >> 6;
            dst[x] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
}

/* --- half-pel plane fetch: strided copy and 2-tap rounding average ---
 * The subpel prediction fetch (build_pred_hpel in me.c) reads one or two
 * registered half-pel planes at a source stride and writes a stride-16 pred
 * block. Widths are 4, 8 or 16, so each row is one, one half, or one quarter of a
 * vector; (a+b+1)>>1 is vrhadd. */
void y264_pred_copy_neon(uint8_t *dst, int dstride,
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

void y264_pred_avg2_neon(uint8_t *dst, int dstride, const uint8_t *s1,
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
