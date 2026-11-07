/*
 * metrics_neon10.c - aarch64 NEON pixel metrics for the 10-bit library
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The 16-bit-sample twins of src/dsp/metrics_neon.c. They carry the 8-bit
 * kernels' names, and src/common/symbols10.h gives every one of them its _10
 * suffix, so arch.h declares one prototype set for both depths and the 8-bit
 * archive never sees these symbols.
 *
 * The arithmetic is the 8-bit kernels' with the load stage swapped: a row is
 * eight u16 lanes, and a - b in u16 wraparound is the signed residual exactly,
 * because both samples are at most 1023. What changes are the lane bounds,
 * which every kernel below restates for 10-bit input. A residual is at most
 * 1023 in magnitude, so a 4x4 Hadamard coefficient is at most 16368 and an
 * 8x8 one 65472. The 4x4 work still fits int16; the 8x8 work fits only up to
 * its second-to-last stage, which is where the |a+b| + |a-b| = 2*max(|a|,|b|)
 * identity takes over, as it does at 8 bits.
 */
#if defined(__aarch64__)
#include "../metrics.h"
#include "../arch.h"
#include <arm_neon.h>

#if Y264_BIT_DEPTH != 10
#error "metrics_neon10.c is the 10-bit library's NEON tier"
#endif

/* The signed residual of eight samples. */
static inline int16x8_t diff8(const pixel *a, const pixel *b)
{
    return vreinterpretq_s16_u16(vsubq_u16(vld1q_u16(a), vld1q_u16(b)));
}

/* ---- SAD -------------------------------------------------------------------
 *
 * The two halves of a row are differenced independently and added before
 * they reach an accumulator, so each chain takes one add per row rather than
 * an abs-diff-accumulate per half-row, and four chains of rows keep those
 * adds pipelined. A row sum is <= 2 * 1023 per lane, a chain takes four rows
 * at h = 16 (<= 8184), and the four chains fold to <= 32736 before the
 * widening reduction. */
static inline int sad_16xh(const pixel *a, int as, const pixel *b, int bs, int h)
{
    uint16x8_t s[4] = { vdupq_n_u16(0), vdupq_n_u16(0), vdupq_n_u16(0), vdupq_n_u16(0) };
    for (int y = 0; y < h; y += 4) {
        for (int i = 0; i < 4; i++) {
            uint16x8x2_t va = vld1q_u16_x2(a), vb = vld1q_u16_x2(b);
            uint16x8_t d = vaddq_u16(vabdq_u16(va.val[0], vb.val[0]),
                                     vabdq_u16(va.val[1], vb.val[1]));
            s[i] = vaddq_u16(s[i], d);
            a += as; b += bs;
        }
    }
    return (int)vaddlvq_u16(vaddq_u16(vaddq_u16(s[0], s[1]), vaddq_u16(s[2], s[3])));
}

/* An 8-wide row is one register: rows go in pairs through one add, as the
 * halves of a 16-wide row do above. */
static inline int sad_8xh(const pixel *a, int as, const pixel *b, int bs, int h)
{
    uint16x8_t s0 = vdupq_n_u16(0), s1 = vdupq_n_u16(0);
    for (int y = 0; y < h; y += 4) {
        s0 = vaddq_u16(s0, vaddq_u16(vabdq_u16(vld1q_u16(a), vld1q_u16(b)),
                                     vabdq_u16(vld1q_u16(a + as), vld1q_u16(b + bs))));
        s1 = vaddq_u16(s1, vaddq_u16(vabdq_u16(vld1q_u16(a + 2 * as), vld1q_u16(b + 2 * bs)),
                                     vabdq_u16(vld1q_u16(a + 3 * as), vld1q_u16(b + 3 * bs))));
        a += 4 * as; b += 4 * bs;
    }
    return (int)vaddlvq_u16(vaddq_u16(s0, s1));
}

int y264_sad_16x16_neon(const pixel *a, int as, const pixel *b, int bs)
{ return sad_16xh(a, as, b, bs, 16); }
int y264_sad_16x8_neon(const pixel *a, int as, const pixel *b, int bs)
{ return sad_16xh(a, as, b, bs, 8); }
int y264_sad_8x16_neon(const pixel *a, int as, const pixel *b, int bs)
{ return sad_8xh(a, as, b, bs, 16); }
int y264_sad_8x8_neon(const pixel *a, int as, const pixel *b, int bs)
{ return sad_8xh(a, as, b, bs, 8); }

/* Batched SAD: one source load feeds every candidate's chain. A 16-wide
 * candidate keeps a chain per half-row, so a lane is <= 16 * 1023 = 16368 and
 * the two halves fold to <= 32736. */
#define SAD_BATCH_16(n)                                                       \
    for (int y = 0; y < h; y++) {                                             \
        uint16x8x2_t sv = vld1q_u16_x2(src);                                 \
        for (int k = 0; k < (n); k++) {                                       \
            uint16x8x2_t rv = vld1q_u16_x2(r[k]);                             \
            lo[k] = vabaq_u16(lo[k], sv.val[0], rv.val[0]);                   \
            hi[k] = vabaq_u16(hi[k], sv.val[1], rv.val[1]);                   \
            r[k] += rs;                                                       \
        }                                                                     \
        src += ss;                                                            \
    }                                                                         \
    for (int k = 0; k < (n); k++)                                             \
        scores[k] = (int)vaddlvq_u16(vaddq_u16(lo[k], hi[k]));

/* An 8-wide candidate keeps a chain per row parity for the same bound. */
#define SAD_BATCH_8(n)                                                        \
    for (int y = 0; y < h; y += 2) {                                          \
        uint16x8_t s0 = vld1q_u16(src), s1 = vld1q_u16(src + ss);             \
        for (int k = 0; k < (n); k++) {                                       \
            lo[k] = vabaq_u16(lo[k], s0, vld1q_u16(r[k]));                    \
            hi[k] = vabaq_u16(hi[k], s1, vld1q_u16(r[k] + rs));               \
            r[k] += 2 * rs;                                                   \
        }                                                                     \
        src += 2 * ss;                                                        \
    }                                                                         \
    for (int k = 0; k < (n); k++)                                             \
        scores[k] = (int)vaddlvq_u16(vaddq_u16(lo[k], hi[k]));

#define SAD_BATCH_DECL(n) \
    uint16x8_t lo[n], hi[n]; \
    for (int k = 0; k < (n); k++) { lo[k] = vdupq_n_u16(0); hi[k] = vdupq_n_u16(0); }

static inline void sad_quad_16xh(const pixel *src, int ss, const pixel *r0,
                                 const pixel *r1, const pixel *r2,
                                 const pixel *r3, int rs, int h, int scores[4])
{
    const pixel *r[4] = { r0, r1, r2, r3 };
    SAD_BATCH_DECL(4)
    SAD_BATCH_16(4)
}

static inline void sad_quad_8xh(const pixel *src, int ss, const pixel *r0,
                                const pixel *r1, const pixel *r2,
                                const pixel *r3, int rs, int h, int scores[4])
{
    const pixel *r[4] = { r0, r1, r2, r3 };
    SAD_BATCH_DECL(4)
    SAD_BATCH_8(4)
}

static inline void sad_trio_16xh(const pixel *src, int ss, const pixel *r0,
                                 const pixel *r1, const pixel *r2, int rs,
                                 int h, int scores[3])
{
    const pixel *r[3] = { r0, r1, r2 };
    SAD_BATCH_DECL(3)
    SAD_BATCH_16(3)
}

static inline void sad_trio_8xh(const pixel *src, int ss, const pixel *r0,
                                const pixel *r1, const pixel *r2, int rs,
                                int h, int scores[3])
{
    const pixel *r[3] = { r0, r1, r2 };
    SAD_BATCH_DECL(3)
    SAD_BATCH_8(3)
}

static inline void sad_pair_16xh(const pixel *src, int ss, const pixel *r0,
                                 const pixel *r1, int rs, int h, int scores[2])
{
    const pixel *r[2] = { r0, r1 };
    SAD_BATCH_DECL(2)
    SAD_BATCH_16(2)
}

static inline void sad_pair_8xh(const pixel *src, int ss, const pixel *r0,
                                const pixel *r1, int rs, int h, int scores[2])
{
    const pixel *r[2] = { r0, r1 };
    SAD_BATCH_DECL(2)
    SAD_BATCH_8(2)
}

#define DEF_SAD_XN(w, h)                                                      \
void y264_sad_quad_##w##x##h##_neon(const pixel *src, int ss, const pixel *r0,\
                                    const pixel *r1, const pixel *r2,         \
                                    const pixel *r3, int rs, int scores[4])   \
{ sad_quad_##w##xh(src, ss, r0, r1, r2, r3, rs, h, scores); }                 \
void y264_sad_trio_##w##x##h##_neon(const pixel *src, int ss, const pixel *r0,\
                                    const pixel *r1, const pixel *r2, int rs, \
                                    int scores[3])                            \
{ sad_trio_##w##xh(src, ss, r0, r1, r2, rs, h, scores); }                     \
void y264_sad_pair_##w##x##h##_neon(const pixel *src, int ss, const pixel *r0,\
                                    const pixel *r1, int rs, int scores[2])   \
{ sad_pair_##w##xh(src, ss, r0, r1, rs, h, scores); }
DEF_SAD_XN(16, 16)
DEF_SAD_XN(16, 8)
DEF_SAD_XN(8, 16)
DEF_SAD_XN(8, 8)
DEF_SAD_XN(8, 4)
#undef DEF_SAD_XN

/* ---- SATD ------------------------------------------------------------------
 *
 * Two side-by-side 4x4 blocks per call (lanes 0-3 and 4-7): the vertical
 * 4-point Hadamard, one horizontal stage, and 2*max(|u|,|v|) for the last.
 * Returns per-lane max sums; the SATD is twice their total. For 10-bit input
 * the vertical pass is <= 4092, the horizontal stage <= 8184, so a lane of
 * the result is <= 16368 and four results fold in u16 (<= 65472). */
static inline uint16x8_t satd_4rows_half(int16x8_t r0, int16x8_t r1,
                                         int16x8_t r2, int16x8_t r3)
{
    int16x8_t t0 = vaddq_s16(r0, r1), t1 = vsubq_s16(r0, r1);
    int16x8_t t2 = vaddq_s16(r2, r3), t3 = vsubq_s16(r2, r3);
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
    uint16x8_t m0 = vmaxq_u16(
        vreinterpretq_u16_s16(vabsq_s16(vreinterpretq_s16_s32(q0.val[0]))),
        vreinterpretq_u16_s16(vabsq_s16(vreinterpretq_s16_s32(q0.val[1]))));
    uint16x8_t m1 = vmaxq_u16(
        vreinterpretq_u16_s16(vabsq_s16(vreinterpretq_s16_s32(q1.val[0]))),
        vreinterpretq_u16_s16(vabsq_s16(vreinterpretq_s16_s32(q1.val[1]))));
    return vaddq_u16(m0, m1);
}

int y264_satd_4x4_neon(const pixel *a, int as, const pixel *b, int bs)
{
    /* rows 0 and 1 side by side, then 2 and 3: two half-blocks of one 4x4
     * would mix rows into lanes, so this keeps one 4x4 in lanes 0-3 and
     * leaves lanes 4-7 zero */
    int16x4_t r0 = vreinterpret_s16_u16(vsub_u16(vld1_u16(a), vld1_u16(b)));
    int16x4_t r1 = vreinterpret_s16_u16(vsub_u16(vld1_u16(a + as), vld1_u16(b + bs)));
    int16x4_t r2 = vreinterpret_s16_u16(vsub_u16(vld1_u16(a + 2 * as), vld1_u16(b + 2 * bs)));
    int16x4_t r3 = vreinterpret_s16_u16(vsub_u16(vld1_u16(a + 3 * as), vld1_u16(b + 3 * bs)));
    const int16x4_t z = vdup_n_s16(0);
    uint16x8_t m = satd_4rows_half(vcombine_s16(r0, z), vcombine_s16(r1, z),
                                   vcombine_s16(r2, z), vcombine_s16(r3, z));
    return 2 * (int)vaddlvq_u16(m);
}

static inline uint16x8_t satd8x8_lanes(const pixel *a, int as, const pixel *b, int bs)
{
    return vaddq_u16(satd_4rows_half(diff8(a, b), diff8(a + as, b + bs),
                                     diff8(a + 2 * as, b + 2 * bs),
                                     diff8(a + 3 * as, b + 3 * bs)),
                     satd_4rows_half(diff8(a + 4 * as, b + 4 * bs),
                                     diff8(a + 5 * as, b + 5 * bs),
                                     diff8(a + 6 * as, b + 6 * bs),
                                     diff8(a + 7 * as, b + 7 * bs)));
}

int y264_satd_8x8_neon(const pixel *a, int as, const pixel *b, int bs)
{
    return 2 * (int)vaddlvq_u16(satd8x8_lanes(a, as, b, bs));
}

/* Four references against one source. Unlike the 8-bit kernel this does not
 * hold the eight source rows across the four references: at sixteen bits
 * they take eight registers the four Hadamard chains need, and the held form
 * measured slower than four singles. Reloading the source is a load per row
 * the core has slots for. */
void y264_satd_quad_8x8_neon(const pixel *src, int ss, const pixel *r0,
                             const pixel *r1, const pixel *r2, const pixel *r3,
                             int rs, int scores[4])
{
    scores[0] = 2 * (int)vaddlvq_u16(satd8x8_lanes(src, ss, r0, rs));
    scores[1] = 2 * (int)vaddlvq_u16(satd8x8_lanes(src, ss, r1, rs));
    scores[2] = 2 * (int)vaddlvq_u16(satd8x8_lanes(src, ss, r2, rs));
    scores[3] = 2 * (int)vaddlvq_u16(satd8x8_lanes(src, ss, r3, rs));
}

/* SATD against (p + q + 1) >> 1 without building the average. */
static inline int16x8_t diff8_avg(const pixel *a, const pixel *p, const pixel *q)
{
    return vreinterpretq_s16_u16(vsubq_u16(vld1q_u16(a),
                                           vrhaddq_u16(vld1q_u16(p), vld1q_u16(q))));
}

int y264_satd_avg_8x8_neon(const pixel *a, int as, const pixel *p,
                           const pixel *q, int ps)
{
    uint16x8_t m = vaddq_u16(
        satd_4rows_half(diff8_avg(a, p, q), diff8_avg(a + as, p + ps, q + ps),
                        diff8_avg(a + 2 * as, p + 2 * ps, q + 2 * ps),
                        diff8_avg(a + 3 * as, p + 3 * ps, q + 3 * ps)),
        satd_4rows_half(diff8_avg(a + 4 * as, p + 4 * ps, q + 4 * ps),
                        diff8_avg(a + 5 * as, p + 5 * ps, q + 5 * ps),
                        diff8_avg(a + 6 * as, p + 6 * ps, q + 6 * ps),
                        diff8_avg(a + 7 * as, p + 7 * ps, q + 7 * ps)));
    return 2 * (int)vaddlvq_u16(m);
}

/* Eight rows of a 16-wide block: the two 8x8 tiles' lanes fold together in
 * u16, four 4x4-pair results per lane, <= 65472. */
static inline int satd_16x8_rows(const pixel *a, int as, const pixel *b, int bs)
{
    uint16x8_t m = vaddq_u16(satd8x8_lanes(a, as, b, bs),
                             satd8x8_lanes(a + 8, as, b + 8, bs));
    return 2 * (int)vaddlvq_u16(m);
}

static inline int satd_avg_16x8_rows(const pixel *a, int as, const pixel *p,
                                     const pixel *q, int ps)
{
    return y264_satd_avg_8x8_neon(a, as, p, q, ps)
         + y264_satd_avg_8x8_neon(a + 8, as, p + 8, q + 8, ps);
}

int y264_satd_16x8_neon(const pixel *a, int as, const pixel *b, int bs)
{ return satd_16x8_rows(a, as, b, bs); }

int y264_satd_8x16_neon(const pixel *a, int as, const pixel *b, int bs)
{
    uint16x8_t m = vaddq_u16(satd8x8_lanes(a, as, b, bs),
                             satd8x8_lanes(a + 8 * as, as, b + 8 * bs, bs));
    return 2 * (int)vaddlvq_u16(m);
}

int y264_satd_16x16_neon_ded(const pixel *a, int as, const pixel *b, int bs)
{
    return satd_16x8_rows(a, as, b, bs)
         + satd_16x8_rows(a + 8 * as, as, b + 8 * bs, bs);
}

int y264_satd_avg_16x16_neon(const pixel *a, int as, const pixel *p,
                             const pixel *q, int ps)
{
    return satd_avg_16x8_rows(a, as, p, q, ps)
         + satd_avg_16x8_rows(a + 8 * as, as, p + 8 * ps, q + 8 * ps, ps);
}

int y264_satd_avg_16x8_neon(const pixel *a, int as, const pixel *p,
                            const pixel *q, int ps)
{ return satd_avg_16x8_rows(a, as, p, q, ps); }

int y264_satd_avg_8x16_neon(const pixel *a, int as, const pixel *p,
                            const pixel *q, int ps)
{
    return y264_satd_avg_8x8_neon(a, as, p, q, ps)
         + y264_satd_avg_8x8_neon(a + 8 * as, as, p + 8 * ps, q + 8 * ps, ps);
}

/* ---- SA8D and the 8x8 Hadamard AC ------------------------------------------
 *
 * The full vertical 8-point transform, an 8x8 transpose, two of the three
 * horizontal stages, then the max identity for the third. For 10-bit input
 * the vertical pass is <= 8184 and the two horizontal stages <= 32736, still
 * inside int16, but four maxes no longer fold in u16, so they widen first.
 * Returns half the 2D Hadamard abs-sum. */
#define SUMSUB8(p, m, x, y) do { p = vaddq_s16(x, y); m = vsubq_s16(x, y); } while (0)

static inline uint32_t sa8d_half_core(int16x8_t v0, int16x8_t v1, int16x8_t v2,
                                      int16x8_t v3, int16x8_t v4, int16x8_t v5,
                                      int16x8_t v6, int16x8_t v7)
{
    int16x8_t t0, t1, t2, t3, t4, t5, t6, t7;
    int16x8_t u0, u1, u2, u3, u4, u5, u6, u7;

    SUMSUB8(t0, t1, v0, v1); SUMSUB8(t2, t3, v2, v3);
    SUMSUB8(t4, t5, v4, v5); SUMSUB8(t6, t7, v6, v7);
    SUMSUB8(u0, u2, t0, t2); SUMSUB8(u1, u3, t1, t3);
    SUMSUB8(u4, u6, t4, t6); SUMSUB8(u5, u7, t5, t7);
    SUMSUB8(v0, v4, u0, u4); SUMSUB8(v1, v5, u1, u5);
    SUMSUB8(v2, v6, u2, u6); SUMSUB8(v3, v7, u3, u7);

    t0 = vtrn1q_s16(v0, v1); t1 = vtrn2q_s16(v0, v1);
    t2 = vtrn1q_s16(v2, v3); t3 = vtrn2q_s16(v2, v3);
    t4 = vtrn1q_s16(v4, v5); t5 = vtrn2q_s16(v4, v5);
    t6 = vtrn1q_s16(v6, v7); t7 = vtrn2q_s16(v6, v7);
#define T32(o1, o2, x, y) do {                                                \
        int32x4_t x_ = vreinterpretq_s32_s16(x), y_ = vreinterpretq_s32_s16(y); \
        o1 = vreinterpretq_s16_s32(vtrn1q_s32(x_, y_));                       \
        o2 = vreinterpretq_s16_s32(vtrn2q_s32(x_, y_)); } while (0)
#define T64(o1, o2, x, y) do {                                                \
        int64x2_t x_ = vreinterpretq_s64_s16(x), y_ = vreinterpretq_s64_s16(y); \
        o1 = vreinterpretq_s16_s64(vtrn1q_s64(x_, y_));                       \
        o2 = vreinterpretq_s16_s64(vtrn2q_s64(x_, y_)); } while (0)
    T32(u0, u2, t0, t2); T32(u1, u3, t1, t3);
    T32(u4, u6, t4, t6); T32(u5, u7, t5, t7);
    T64(v0, v4, u0, u4); T64(v1, v5, u1, u5);
    T64(v2, v6, u2, u6); T64(v3, v7, u3, u7);
#undef T32
#undef T64

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
    uint32x4_t acc = vpaddlq_u16(m0);
    acc = vpadalq_u16(acc, m1);
    acc = vpadalq_u16(acc, m2);
    acc = vpadalq_u16(acc, m3);
    return vaddvq_u32(acc);
}

int y264_sa8d_8x8_neon(const pixel *a, int as, const pixel *b, int bs)
{
    uint32_t half = sa8d_half_core(diff8(a, b), diff8(a + as, b + bs),
                                   diff8(a + 2 * as, b + 2 * bs),
                                   diff8(a + 3 * as, b + 3 * bs),
                                   diff8(a + 4 * as, b + 4 * bs),
                                   diff8(a + 5 * as, b + 5 * bs),
                                   diff8(a + 6 * as, b + 6 * bs),
                                   diff8(a + 7 * as, b + 7 * bs));
    return (int)((2 * half + 2) >> 2);
}

int y264_sa8d_16x16_neon(const pixel *a, int as, const pixel *b, int bs)
{
    return y264_sa8d_8x8_neon(a, as, b, bs)
         + y264_sa8d_8x8_neon(a + 8, as, b + 8, bs)
         + y264_sa8d_8x8_neon(a + 8 * as, as, b + 8 * bs, bs)
         + y264_sa8d_8x8_neon(a + 8 * as + 8, as, b + 8 * bs + 8, bs);
}

/* Full abs-sum minus |DC|, DC = the pixel sum (<= 65472). */
long y264_hadamard_ac_8x8_neon(const pixel *p, int stride)
{
    uint16x8_t r[8];
    for (int i = 0; i < 8; i++)
        r[i] = vld1q_u16(p + i * stride);
    uint16x8_t dcv = vaddq_u16(vaddq_u16(vaddq_u16(r[0], r[1]), vaddq_u16(r[2], r[3])),
                               vaddq_u16(vaddq_u16(r[4], r[5]), vaddq_u16(r[6], r[7])));
    uint32_t dc = vaddlvq_u16(dcv);
    uint32_t half = sa8d_half_core(
        vreinterpretq_s16_u16(r[0]), vreinterpretq_s16_u16(r[1]),
        vreinterpretq_s16_u16(r[2]), vreinterpretq_s16_u16(r[3]),
        vreinterpretq_s16_u16(r[4]), vreinterpretq_s16_u16(r[5]),
        vreinterpretq_s16_u16(r[6]), vreinterpretq_s16_u16(r[7]));
    return (long)(2 * half) - (long)dc;
}

/* ---- psy-RD texture --------------------------------------------------------
 *
 * The SATD-support term: each 4x4 tile's SATD against a flat block of its own
 * rounded mean, two tiles per satd_4rows_half pass. A tile's DC is its pixel
 * sum, <= 16368. */
long y264_texture_ac4_16x16_neon(const pixel *p, int stride)
{
    long e = 0;
    for (int by = 0; by < 16; by += 4)
        for (int bx = 0; bx < 16; bx += 8) {
            const pixel *q = p + by * stride + bx;
            uint16x8_t a0 = vld1q_u16(q), a1 = vld1q_u16(q + stride);
            uint16x8_t a2 = vld1q_u16(q + 2 * stride), a3 = vld1q_u16(q + 3 * stride);
            uint16x8_t dcv = vaddq_u16(vaddq_u16(a0, a1), vaddq_u16(a2, a3));
            int dcl = (int)vaddlv_u16(vget_low_u16(dcv));
            int dch = (int)vaddlv_u16(vget_high_u16(dcv));
            uint16x8_t m = satd_4rows_half(
                vreinterpretq_s16_u16(a0), vreinterpretq_s16_u16(a1),
                vreinterpretq_s16_u16(a2), vreinterpretq_s16_u16(a3));
            long sl = 2 * (long)vaddlv_u16(vget_low_u16(m));
            long sh = 2 * (long)vaddlv_u16(vget_high_u16(m));
            int fl = 16 * ((dcl + 8) >> 4), fh = 16 * ((dch + 8) >> 4);
            e += sl - dcl + (dcl < fl ? fl - dcl : dcl - fl);
            e += sh - dch + (dch < fh ? fh - dch : dch - fh);
        }
    return e;
}

/* The full 4x4 Hadamard of two side-by-side tiles, the final butterfly taken.
 * The coefficients come out in a fixed permutation, the same for every call,
 * with each tile's DC in lane 0 (and 4) of val[0]. A coefficient is <= 16368. */
static inline int16x8x4_t tile4_coefs(uint16x8_t r0, uint16x8_t r1,
                                      uint16x8_t r2, uint16x8_t r3)
{
    int16x8_t t0 = vreinterpretq_s16_u16(vaddq_u16(r0, r1));
    int16x8_t t1 = vreinterpretq_s16_u16(vsubq_u16(r0, r1));
    int16x8_t t2 = vreinterpretq_s16_u16(vaddq_u16(r2, r3));
    int16x8_t t3 = vreinterpretq_s16_u16(vsubq_u16(r2, r3));
    int16x8_t h0 = vaddq_s16(t0, t2), h2 = vsubq_s16(t0, t2);
    int16x8_t h1 = vaddq_s16(t1, t3), h3 = vsubq_s16(t1, t3);

    int16x8x2_t p01 = vtrnq_s16(h0, h1);
    int16x8x2_t p23 = vtrnq_s16(h2, h3);
    int16x8_t s01 = vaddq_s16(p01.val[0], p01.val[1]);
    int16x8_t d01 = vsubq_s16(p01.val[0], p01.val[1]);
    int16x8_t s23 = vaddq_s16(p23.val[0], p23.val[1]);
    int16x8_t d23 = vsubq_s16(p23.val[0], p23.val[1]);

    int32x4x2_t q0 = vtrnq_s32(vreinterpretq_s32_s16(s01), vreinterpretq_s32_s16(d01));
    int32x4x2_t q1 = vtrnq_s32(vreinterpretq_s32_s16(s23), vreinterpretq_s32_s16(d23));
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
#define TEX_LO2(a, b) vreinterpretq_u16_u64(vtrn1q_u64(vreinterpretq_u64_u16(a), \
                                                       vreinterpretq_u64_u16(b)))
#define TEX_HI2(a, b) vreinterpretq_u16_u64(vtrn2q_u64(vreinterpretq_u64_u16(a), \
                                                       vreinterpretq_u64_u16(b)))

/* Both texture terms in one pass, the 8-bit kernel's factoring (an 8x8 WHT is
 * the four 4x4 tile transforms and a 2x2 butterfly across them). The bounds
 * are what moved: four tile-coefficient magnitudes fold in u16 (<= 65472)
 * where the 8-bit kernel folds eight, and a tile-row butterfly result is
 * <= 32736, so each paired max widens straight into the u32 accumulator. */
void y264_texture_ac48_16x16_neon(const pixel *p, int stride, long out[2])
{
    uint32x4_t a4 = vdupq_n_u32(0), a8 = vdupq_n_u32(0);
    int16x4_t dcq[4];
    int nq = 0;

    for (int qy = 0; qy < 16; qy += 8) {
        for (int qx = 0; qx < 16; qx += 8) {
            const pixel *q = p + qy * stride + qx;
            uint16x8_t v[8];
            for (int i = 0; i < 8; i++)
                v[i] = vld1q_u16(q + i * stride);

            int16x8x4_t ct = tile4_coefs(v[0], v[1], v[2], v[3]);
            int16x8x4_t cb = tile4_coefs(v[4], v[5], v[6], v[7]);

            uint16x8_t tt = vaddq_u16(vaddq_u16(TEX_ABS(ct.val[0]), TEX_ABS(ct.val[1])),
                                      vaddq_u16(TEX_ABS(ct.val[2]), TEX_ABS(ct.val[3])));
            uint16x8_t tb = vaddq_u16(vaddq_u16(TEX_ABS(cb.val[0]), TEX_ABS(cb.val[1])),
                                      vaddq_u16(TEX_ABS(cb.val[2]), TEX_ABS(cb.val[3])));
            a4 = vpadalq_u16(a4, tt);
            a4 = vpadalq_u16(a4, tb);

            int16x8_t e = vuzp1q_s16(ct.val[0], cb.val[0]);
            dcq[nq++] = vget_low_s16(vuzp1q_s16(e, e));

            for (int k = 0; k < 4; k += 2) {
                uint16x8_t s0 = TEX_ABS(vaddq_s16(ct.val[k],     cb.val[k]));
                uint16x8_t s1 = TEX_ABS(vaddq_s16(ct.val[k + 1], cb.val[k + 1]));
                uint16x8_t d0 = TEX_ABS(vsubq_s16(ct.val[k],     cb.val[k]));
                uint16x8_t d1 = TEX_ABS(vsubq_s16(ct.val[k + 1], cb.val[k + 1]));
                a8 = vpadalq_u16(a8, vmaxq_u16(TEX_LO2(s0, s1), TEX_HI2(s0, s1)));
                a8 = vpadalq_u16(a8, vmaxq_u16(TEX_LO2(d0, d1), TEX_HI2(d0, d1)));
            }
        }
    }
    /* the sixteen tile DCs (each <= 16368) re-scored against their rounded
     * means, all inside int16 */
    int16x8_t d0 = vcombine_s16(dcq[0], dcq[1]), d1 = vcombine_s16(dcq[2], dcq[3]);
    int16x8_t f0 = vshlq_n_s16(vshrq_n_s16(vaddq_s16(d0, vdupq_n_s16(8)), 4), 4);
    int16x8_t f1 = vshlq_n_s16(vshrq_n_s16(vaddq_s16(d1, vdupq_n_s16(8)), 4), 4);
    long corr = (long)vaddlvq_u16(vaddq_u16(vreinterpretq_u16_s16(vabdq_s16(d0, f0)),
                                            vreinterpretq_u16_s16(vabdq_s16(d1, f1))));
    long dcsum = (long)vaddlvq_s16(d0) + (long)vaddlvq_s16(d1);
    out[0] = (long)vaddvq_u32(a4) - dcsum + corr;
    out[1] = 2 * (long)vaddvq_u32(a8) - dcsum;
}

/* Pixel sum and sum of squares of a 16x16: the sum lanes widen as they
 * accumulate, and a square lane takes 32 squares of <= 1023, <= 33.5M. */
void y264_var_16x16_neon(const pixel *p, int stride, uint32_t out[2])
{
    uint32x4_t s1 = vdupq_n_u32(0);
    uint32x4_t s2a = vdupq_n_u32(0), s2b = vdupq_n_u32(0);
    for (int y = 0; y < 16; y++) {
        uint16x8_t v0 = vld1q_u16(p + y * stride), v1 = vld1q_u16(p + y * stride + 8);
        s1 = vpadalq_u16(s1, vaddq_u16(v0, v1));
        s2a = vmlal_u16(s2a, vget_low_u16(v0), vget_low_u16(v0));
        s2b = vmlal_high_u16(s2b, v0, v0);
        s2a = vmlal_u16(s2a, vget_low_u16(v1), vget_low_u16(v1));
        s2b = vmlal_high_u16(s2b, v1, v1);
    }
    out[0] = vaddvq_u32(s1);
    out[1] = vaddvq_u32(vaddq_u32(s2a, s2b));
}

/* ---- the fused I16x16 V/H/DC cost ------------------------------------------
 *
 * Every prediction is a broadcast, so each mode's residual is formed in
 * registers off one pass of source loads. Per group of four rows a lane of
 * one mode takes two 4x4-pair results (left and right 8 columns), <= 32736,
 * before it widens. */
void y264_intra16_satd_trio_neon(const pixel *src, int ss, const pixel *top,
                                 const pixel *left, int dc, int costs[3])
{
    const uint16x8_t tl = vld1q_u16(top), th = vld1q_u16(top + 8);
    const uint16x8_t vdc = vdupq_n_u16((uint16_t)dc);
    uint32x4_t av = vdupq_n_u32(0), ah = vdupq_n_u32(0), ad = vdupq_n_u32(0);

#define RD(s_, p_) vreinterpretq_s16_u16(vsubq_u16(s_, p_))
    for (int y = 0; y < 16; y += 4) {
        uint16x8_t sl[4], sh[4], hv[4];
        for (int i = 0; i < 4; i++) {
            sl[i] = vld1q_u16(src + (y + i) * ss);
            sh[i] = vld1q_u16(src + (y + i) * ss + 8);
            hv[i] = vdupq_n_u16(left[y + i]);
        }
        av = vpadalq_u16(av, vaddq_u16(
            satd_4rows_half(RD(sl[0], tl), RD(sl[1], tl), RD(sl[2], tl), RD(sl[3], tl)),
            satd_4rows_half(RD(sh[0], th), RD(sh[1], th), RD(sh[2], th), RD(sh[3], th))));
        ah = vpadalq_u16(ah, vaddq_u16(
            satd_4rows_half(RD(sl[0], hv[0]), RD(sl[1], hv[1]), RD(sl[2], hv[2]), RD(sl[3], hv[3])),
            satd_4rows_half(RD(sh[0], hv[0]), RD(sh[1], hv[1]), RD(sh[2], hv[2]), RD(sh[3], hv[3]))));
        ad = vpadalq_u16(ad, vaddq_u16(
            satd_4rows_half(RD(sl[0], vdc), RD(sl[1], vdc), RD(sl[2], vdc), RD(sl[3], vdc)),
            satd_4rows_half(RD(sh[0], vdc), RD(sh[1], vdc), RD(sh[2], vdc), RD(sh[3], vdc))));
    }
#undef RD
    costs[0] = 2 * (int)vaddvq_u32(av);
    costs[1] = 2 * (int)vaddvq_u32(ah);
    costs[2] = 2 * (int)vaddvq_u32(ad);
}

#endif /* __aarch64__ */
