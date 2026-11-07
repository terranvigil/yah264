/*
 * loopfilter_neon10.c - aarch64 NEON deblocking edge filters for the 10-bit
 * library (ITU-T H.264 8.7)
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The 10-bit twins of the whole-edge kernels in src/dsp/loopfilter_neon.c,
 * under the 8-bit kernels' names (src/common/symbols10.h suffixes them): the
 * sixteen-line luma edges and the chroma edges. The strength kernel reads no
 * samples and is shared (src/dsp/arm/loopfilter_bs_neon.c).
 *
 * The callers pass the 8-bit alpha, beta and tc0 table values; at 10 bits
 * the filter scales each by 4 (8.7.2.2), as the C line filter does, and the
 * literal rounding terms stay as they are. Every intermediate fits int16:
 * a sample is at most 1023, the widest sum is the strong filter's
 * 8 * 1023 + 4, and the deltas are clipped to tc, at most 25 * 4 + 2.
 * Samples load and store as u16 lanes with no widening.
 */
#if defined(__aarch64__)
#include <arm_neon.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "../../common/bitdepth.h"
#include "../loopfilter.h"
#include "../arch.h"

#if Y264_BIT_DEPTH != 10
#error "loopfilter_neon10.c is the 10-bit library's NEON tier"
#endif

#define DB_SH (Y264_BIT_DEPTH - 8)

static inline int16x8_t ld8(const pixel *p)
{
    return vreinterpretq_s16_u16(vld1q_u16(p));
}

static inline void st8(pixel *p, int16x8_t v)
{
    vst1q_u16(p, vreinterpretq_u16_s16(v));
}

/* ---- luma, sixteen lines per call ------------------------------------------
 *
 * Eight lines per pass, bS per lane: a bS 0 lane is masked out of the filter
 * decision, a bS 4 lane takes the strong filter, a bS 1-3 lane the normal
 * one with its own tc0. in/out: p3 p2 p1 p0 q0 q1 q2 q3 -> p2 p1 p0 q0 q1 q2.
 * `alpha` and `beta` arrive scaled. */
static inline __attribute__((always_inline))
void db_luma8_core(const int16x8_t s[8], int alpha, int beta,
                   int16x8_t tc0, uint16x8_t bs4, uint16x8_t bsnz,
                   int16x8_t out[6], int has_n, int has_4)
{
    const int16x8_t p3 = s[0], p2 = s[1], p1 = s[2], p0 = s[3];
    const int16x8_t q0 = s[4], q1 = s[5], q2 = s[6], q3 = s[7];
    const int16x8_t vb = vdupq_n_s16((int16_t)beta);
    const int16x8_t c2 = vdupq_n_s16(2), c4 = vdupq_n_s16(4);

    uint16x8_t filt = vcltq_s16(vabdq_s16(p0, q0), vdupq_n_s16((int16_t)alpha));
    filt = vandq_u16(filt, vcltq_s16(vabdq_s16(p1, p0), vb));
    filt = vandq_u16(filt, vcltq_s16(vabdq_s16(q1, q0), vb));
    filt = vandq_u16(filt, bsnz);
    uint16x8_t apb = vcltq_s16(vabdq_s16(p2, p0), vb);
    uint16x8_t aqb = vcltq_s16(vabdq_s16(q2, q0), vb);

    out[0] = p2; out[1] = p1; out[2] = p0; out[3] = q0; out[4] = q1; out[5] = q2;
    if (has_n) {
        int16x8_t tc = vsubq_s16(vsubq_s16(tc0, vreinterpretq_s16_u16(apb)),
                                 vreinterpretq_s16_u16(aqb));
        int16x8_t d = vaddq_s16(vshlq_n_s16(vsubq_s16(q0, p0), 2), vsubq_s16(p1, q1));
        d = vshrq_n_s16(vaddq_s16(d, c4), 3);
        d = vmaxq_s16(vnegq_s16(tc), vminq_s16(tc, d));
        const int16x8_t z = vdupq_n_s16(0), pmax = vdupq_n_s16(PIXEL_MAX);
        int16x8_t p0n = vminq_s16(vmaxq_s16(vaddq_s16(p0, d), z), pmax);
        int16x8_t q0n = vminq_s16(vmaxq_s16(vsubq_s16(q0, d), z), pmax);
        int16x8_t av = vrhaddq_s16(p0, q0);
        int16x8_t dp1 = vshrq_n_s16(vsubq_s16(vaddq_s16(p2, av), vshlq_n_s16(p1, 1)), 1);
        dp1 = vmaxq_s16(vnegq_s16(tc0), vminq_s16(tc0, dp1));
        int16x8_t dq1 = vshrq_n_s16(vsubq_s16(vaddq_s16(q2, av), vshlq_n_s16(q1, 1)), 1);
        dq1 = vmaxq_s16(vnegq_s16(tc0), vminq_s16(tc0, dq1));
        uint16x8_t fn = vbicq_u16(filt, bs4);
        out[1] = vbslq_s16(vandq_u16(fn, apb), vaddq_s16(p1, dp1), p1);
        out[2] = vbslq_s16(fn, p0n, p0);
        out[3] = vbslq_s16(fn, q0n, q0);
        out[4] = vbslq_s16(vandq_u16(fn, aqb), vaddq_s16(q1, dq1), q1);
    }
    if (!has_4)
        return;

    uint16x8_t strong = vcltq_s16(vabdq_s16(p0, q0),
                                  vdupq_n_s16((int16_t)((alpha >> 2) + 2)));
    uint16x8_t sp = vandq_u16(strong, apb), sq = vandq_u16(strong, aqb);
    int16x8_t pq = vaddq_s16(p0, q0);
    int16x8_t p0s = vshrq_n_s16(vaddq_s16(vaddq_s16(vaddq_s16(p2, q1),
                    vshlq_n_s16(vaddq_s16(p1, pq), 1)), c4), 3);
    int16x8_t p1s = vshrq_n_s16(vaddq_s16(vaddq_s16(vaddq_s16(p2, p1), pq), c2), 2);
    int16x8_t p2s = vshrq_n_s16(vaddq_s16(vaddq_s16(vaddq_s16(vshlq_n_s16(p3, 1),
                    vaddq_s16(vshlq_n_s16(p2, 1), p2)), vaddq_s16(p1, pq)), c4), 3);
    int16x8_t p0w = vshrq_n_s16(vaddq_s16(vaddq_s16(vshlq_n_s16(p1, 1),
                    vaddq_s16(p0, q1)), c2), 2);
    int16x8_t q0s = vshrq_n_s16(vaddq_s16(vaddq_s16(vaddq_s16(q2, p1),
                    vshlq_n_s16(vaddq_s16(q1, pq), 1)), c4), 3);
    int16x8_t q1s = vshrq_n_s16(vaddq_s16(vaddq_s16(vaddq_s16(q2, q1), pq), c2), 2);
    int16x8_t q2s = vshrq_n_s16(vaddq_s16(vaddq_s16(vaddq_s16(vshlq_n_s16(q3, 1),
                    vaddq_s16(vshlq_n_s16(q2, 1), q2)), vaddq_s16(q1, pq)), c4), 3);
    int16x8_t q0w = vshrq_n_s16(vaddq_s16(vaddq_s16(vshlq_n_s16(q1, 1),
                    vaddq_s16(q0, p1)), c2), 2);

    uint16x8_t f4 = vandq_u16(filt, bs4);
    uint16x8_t f4p = vandq_u16(f4, sp), f4q = vandq_u16(f4, sq);
    out[0] = vbslq_s16(f4p, p2s, out[0]);
    out[1] = vbslq_s16(f4p, p1s, out[1]);
    out[2] = vbslq_s16(f4, vbslq_s16(sp, p0s, p0w), out[2]);
    out[3] = vbslq_s16(f4, vbslq_s16(sq, q0s, q0w), out[3]);
    out[4] = vbslq_s16(f4q, q1s, out[4]);
    out[5] = vbslq_s16(f4q, q2s, out[5]);
}

/* Per-lane tc0 (scaled), bS 4 and bS != 0 for lanes [8g, 8g + 8) of a
 * sixteen-line edge, each bS covering four lines. */
static inline void db_luma16_params(const uint8_t bs[4], const uint8_t tc0tab[3],
                                    int g, int16x8_t *tc0, uint16x8_t *bs4,
                                    uint16x8_t *bsnz)
{
    int b0 = bs[2 * g], b1 = bs[2 * g + 1];
    int t0 = b0 >= 1 && b0 <= 3 ? tc0tab[b0 - 1] << DB_SH : 0;
    int t1 = b1 >= 1 && b1 <= 3 ? tc0tab[b1 - 1] << DB_SH : 0;
    *tc0 = vcombine_s16(vdup_n_s16((int16_t)t0), vdup_n_s16((int16_t)t1));
    *bs4 = vcombine_u16(vdup_n_u16(b0 >= 4 ? 0xffff : 0), vdup_n_u16(b1 >= 4 ? 0xffff : 0));
    *bsnz = vcombine_u16(vdup_n_u16(b0 ? 0xffff : 0), vdup_n_u16(b1 ? 0xffff : 0));
}

/* Horizontal edge: sixteen columns at q0p, the samples across the edge run
 * down the rows, eight columns a pass. */
void y264_deblock_luma_h16_neon(pixel *q0p, int stride, int alpha, int beta,
                                const uint8_t bs[4], const uint8_t tc0tab[3])
{
    alpha <<= DB_SH;
    beta <<= DB_SH;
    for (int g = 0; g < 2; g++) {
        int b0 = bs[2 * g], b1 = bs[2 * g + 1];
        if (!(b0 | b1))
            continue;
        pixel *c = q0p + 8 * g;
        int16x8_t s[8], o[6], tc0;
        uint16x8_t bs4, bsnz;
        for (int k = 0; k < 8; k++)
            s[k] = ld8(c + (k - 4) * stride);
        db_luma16_params(bs, tc0tab, g, &tc0, &bs4, &bsnz);
        db_luma8_core(s, alpha, beta, tc0, bs4, bsnz, o,
                      (b0 && b0 < 4) || (b1 && b1 < 4), b0 >= 4 || b1 >= 4);
        for (int k = 0; k < 6; k++)
            st8(c + (k - 3) * stride, o[k]);
    }
}

/* 8x8 transpose of int16 lanes. */
static inline void db_trn8(int16x8_t v[8])
{
    int16x8x2_t a0 = vtrnq_s16(v[0], v[1]), a1 = vtrnq_s16(v[2], v[3]);
    int16x8x2_t a2 = vtrnq_s16(v[4], v[5]), a3 = vtrnq_s16(v[6], v[7]);
    int32x4x2_t b0 = vtrnq_s32(vreinterpretq_s32_s16(a0.val[0]), vreinterpretq_s32_s16(a1.val[0]));
    int32x4x2_t b1 = vtrnq_s32(vreinterpretq_s32_s16(a0.val[1]), vreinterpretq_s32_s16(a1.val[1]));
    int32x4x2_t b2 = vtrnq_s32(vreinterpretq_s32_s16(a2.val[0]), vreinterpretq_s32_s16(a3.val[0]));
    int32x4x2_t b3 = vtrnq_s32(vreinterpretq_s32_s16(a2.val[1]), vreinterpretq_s32_s16(a3.val[1]));
    v[0] = vreinterpretq_s16_s64(vtrn1q_s64(vreinterpretq_s64_s32(b0.val[0]), vreinterpretq_s64_s32(b2.val[0])));
    v[4] = vreinterpretq_s16_s64(vtrn2q_s64(vreinterpretq_s64_s32(b0.val[0]), vreinterpretq_s64_s32(b2.val[0])));
    v[1] = vreinterpretq_s16_s64(vtrn1q_s64(vreinterpretq_s64_s32(b1.val[0]), vreinterpretq_s64_s32(b3.val[0])));
    v[5] = vreinterpretq_s16_s64(vtrn2q_s64(vreinterpretq_s64_s32(b1.val[0]), vreinterpretq_s64_s32(b3.val[0])));
    v[2] = vreinterpretq_s16_s64(vtrn1q_s64(vreinterpretq_s64_s32(b0.val[1]), vreinterpretq_s64_s32(b2.val[1])));
    v[6] = vreinterpretq_s16_s64(vtrn2q_s64(vreinterpretq_s64_s32(b0.val[1]), vreinterpretq_s64_s32(b2.val[1])));
    v[3] = vreinterpretq_s16_s64(vtrn1q_s64(vreinterpretq_s64_s32(b1.val[1]), vreinterpretq_s64_s32(b3.val[1])));
    v[7] = vreinterpretq_s16_s64(vtrn2q_s64(vreinterpretq_s64_s32(b1.val[1]), vreinterpretq_s64_s32(b3.val[1])));
}

/* Vertical edge: sixteen rows of eight samples [-4, +3], transposed so the
 * samples across the edge become vectors, eight rows a pass. p3 and q3 go
 * back unchanged, so each row is one sixteen-byte store. */
void y264_deblock_luma_v16_neon(pixel *q0p, int stride, int alpha, int beta,
                                const uint8_t bs[4], const uint8_t tc0tab[3])
{
    alpha <<= DB_SH;
    beta <<= DB_SH;
    for (int g = 0; g < 2; g++) {
        int b0 = bs[2 * g], b1 = bs[2 * g + 1];
        if (!(b0 | b1))
            continue;
        pixel *b = q0p - 4 + (ptrdiff_t)(8 * g) * stride;
        int16x8_t v[8], o[6], tc0;
        uint16x8_t bs4, bsnz;
        for (int k = 0; k < 8; k++)
            v[k] = ld8(b + (ptrdiff_t)k * stride);
        db_trn8(v);
        db_luma16_params(bs, tc0tab, g, &tc0, &bs4, &bsnz);
        db_luma8_core(v, alpha, beta, tc0, bs4, bsnz, o,
                      (b0 && b0 < 4) || (b1 && b1 < 4), b0 >= 4 || b1 >= 4);
        v[1] = o[0]; v[2] = o[1]; v[3] = o[2]; v[4] = o[3]; v[5] = o[4]; v[6] = o[5];
        db_trn8(v);
        for (int k = 0; k < 8; k++)
            st8(b + (ptrdiff_t)k * stride, v[k]);
    }
}

/* ---- chroma (chromaStyleFilteringFlag == 1) ---------------------------------
 *
 * Only p0 and q0 change, and the bS < 4 clip is tc0 + 1 with no ap/aq term,
 * so a whole edge is one pass with tc and the bS 4 select per lane. A bS 0
 * lane runs with tc 0 and the strong select clear, which rewrites its own
 * samples. `alpha` and `beta` arrive scaled; `tc` lanes are tc0 scaled, + 1. */
static inline void db_chroma8_core(int16x8_t p1, int16x8_t p0,
                                   int16x8_t q0, int16x8_t q1,
                                   int alpha, int beta,
                                   int16x8_t tc, uint16x8_t bs4,
                                   int16x8_t *p0o, int16x8_t *q0o)
{
    const int16x8_t vb = vdupq_n_s16((int16_t)beta);
    uint16x8_t filt = vcltq_s16(vabdq_s16(p0, q0), vdupq_n_s16((int16_t)alpha));
    filt = vandq_u16(filt, vcltq_s16(vabdq_s16(p1, p0), vb));
    filt = vandq_u16(filt, vcltq_s16(vabdq_s16(q1, q0), vb));

    int16x8_t d = vaddq_s16(vshlq_n_s16(vsubq_s16(q0, p0), 2), vsubq_s16(p1, q1));
    d = vshrq_n_s16(vaddq_s16(d, vdupq_n_s16(4)), 3);
    d = vmaxq_s16(vnegq_s16(tc), vminq_s16(tc, d));
    const int16x8_t z = vdupq_n_s16(0), pmax = vdupq_n_s16(PIXEL_MAX);
    int16x8_t p0w = vminq_s16(vmaxq_s16(vaddq_s16(p0, d), z), pmax);
    int16x8_t q0w = vminq_s16(vmaxq_s16(vsubq_s16(q0, d), z), pmax);

    const int16x8_t c2 = vdupq_n_s16(2);
    int16x8_t p0s = vshrq_n_s16(vaddq_s16(vaddq_s16(vshlq_n_s16(p1, 1),
                                vaddq_s16(p0, q1)), c2), 2);
    int16x8_t q0s = vshrq_n_s16(vaddq_s16(vaddq_s16(vshlq_n_s16(q1, 1),
                                vaddq_s16(q0, p1)), c2), 2);

    *p0o = vbslq_s16(filt, vbslq_s16(bs4, p0s, p0w), p0);
    *q0o = vbslq_s16(filt, vbslq_s16(bs4, q0s, q0w), q0);
}

/* Per-lane tc and bS 4 mask, each of the four bS entries covering `span`
 * consecutive lines (2 for 4:2:0, 4 for 4:2:2), lanes [8g, 8g + 8). */
static inline void db_chroma8_params(const uint8_t bs[4], const uint8_t tc0tab[3],
                                     int span, int g,
                                     int16x8_t *tc, uint16x8_t *bs4)
{
    int16_t t[8]; uint16_t m[8];
    for (int i = 0; i < 8; i++) {
        int b = bs[(g * 8 + i) / span];
        t[i] = (int16_t)(b == 0 || b >= 4 ? 0 : (tc0tab[b - 1] << DB_SH) + 1);
        m[i] = (uint16_t)(b >= 4 ? 0xffff : 0);
    }
    *tc = vld1q_s16(t);
    *bs4 = vld1q_u16(m);
}

/* Horizontal 4:2:0 chroma edge, both planes in one call: eight columns of
 * each share the edge's bS, alpha, beta and tc. */
void y264_deblock_chroma2_h_neon(pixel *cbq0, pixel *crq0, int cbs, int crs,
                                 int alpha, int beta, const uint8_t bs[4],
                                 const uint8_t tc0tab[3])
{
    int16x8_t tc; uint16x8_t bs4;
    db_chroma8_params(bs, tc0tab, 2, 0, &tc, &bs4);
    alpha <<= DB_SH;
    beta <<= DB_SH;
    int16x8_t p0a, q0a, p0b, q0b;
    db_chroma8_core(ld8(cbq0 - 2 * cbs), ld8(cbq0 - cbs), ld8(cbq0), ld8(cbq0 + cbs),
                    alpha, beta, tc, bs4, &p0a, &q0a);
    db_chroma8_core(ld8(crq0 - 2 * crs), ld8(crq0 - crs), ld8(crq0), ld8(crq0 + crs),
                    alpha, beta, tc, bs4, &p0b, &q0b);
    st8(cbq0 - cbs, p0a);
    st8(cbq0, q0a);
    st8(crq0 - crs, p0b);
    st8(crq0, q0b);
}

/* Horizontal chroma edge of one plane, eight columns: the 4:2:0 and 4:2:2
 * single-plane form. */
void y264_deblock_chroma8_h_neon(pixel *q0p, int stride, int alpha, int beta,
                                 const uint8_t bs[4], const uint8_t tc0tab[3],
                                 int span, int g)
{
    int16x8_t tc; uint16x8_t bs4;
    db_chroma8_params(bs, tc0tab, span, g, &tc, &bs4);
    int16x8_t p0o, q0o;
    db_chroma8_core(ld8(q0p - 2 * stride), ld8(q0p - stride), ld8(q0p), ld8(q0p + stride),
                    alpha << DB_SH, beta << DB_SH, tc, bs4, &p0o, &q0o);
    st8(q0p - stride, p0o);
    st8(q0p, q0o);
}

/* Vertical 4:2:0 chroma edge, both planes in one call. Each row's p1 p0 q0 q1
 * is one four-sample structure load into the row's lane of four vectors, and
 * p0 q0 go back as one two-sample structure store, so nothing outside the
 * filter's own samples is read or written. */
void y264_deblock_chroma2_v_neon(pixel *cbq0, pixel *crq0, int cbs, int crs,
                                 int alpha, int beta, const uint8_t bs[4],
                                 const uint8_t tc0tab[3])
{
    int16x8_t tc; uint16x8_t bs4;
    db_chroma8_params(bs, tc0tab, 2, 0, &tc, &bs4);
    alpha <<= DB_SH;
    beta <<= DB_SH;
    pixel *const base[2] = { cbq0, crq0 };
    const int st[2] = { cbs, crs };
    for (int c = 0; c < 2; c++) {
        uint16x8x4_t v = { { vdupq_n_u16(0), vdupq_n_u16(0), vdupq_n_u16(0), vdupq_n_u16(0) } };
        pixel *b = base[c] - 2;
        v = vld4q_lane_u16(b + 0 * st[c], v, 0);
        v = vld4q_lane_u16(b + 1 * st[c], v, 1);
        v = vld4q_lane_u16(b + 2 * st[c], v, 2);
        v = vld4q_lane_u16(b + 3 * st[c], v, 3);
        v = vld4q_lane_u16(b + 4 * st[c], v, 4);
        v = vld4q_lane_u16(b + 5 * st[c], v, 5);
        v = vld4q_lane_u16(b + 6 * st[c], v, 6);
        v = vld4q_lane_u16(b + 7 * st[c], v, 7);
        int16x8_t p0o, q0o;
        db_chroma8_core(vreinterpretq_s16_u16(v.val[0]), vreinterpretq_s16_u16(v.val[1]),
                        vreinterpretq_s16_u16(v.val[2]), vreinterpretq_s16_u16(v.val[3]),
                        alpha, beta, tc, bs4, &p0o, &q0o);
        uint16x8x2_t o = { { vreinterpretq_u16_s16(p0o), vreinterpretq_u16_s16(q0o) } };
        pixel *w = base[c] - 1;
        vst2q_lane_u16(w + 0 * st[c], o, 0);
        vst2q_lane_u16(w + 1 * st[c], o, 1);
        vst2q_lane_u16(w + 2 * st[c], o, 2);
        vst2q_lane_u16(w + 3 * st[c], o, 3);
        vst2q_lane_u16(w + 4 * st[c], o, 4);
        vst2q_lane_u16(w + 5 * st[c], o, 5);
        vst2q_lane_u16(w + 6 * st[c], o, 6);
        vst2q_lane_u16(w + 7 * st[c], o, 7);
    }
}

#endif /* __aarch64__ */
