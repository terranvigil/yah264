/*
 * deblock_neon.c - aarch64 NEON in-loop deblocking kernels (ITU-T H.264 8.7)
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Spec-exact 4-line luma edge filters: one call covers the four lines of a
 * 4x4-block edge segment (uniform bS/alpha/beta/tc0 -- exactly the unit the
 * scalar loop feeds filter_line). Recon path, so every branch of the scalar
 * filter is reproduced per lane with masks: conditions become compare masks,
 * conditional writes become bsl merges with the original samples, and the
 * stores cover only the sample span the scalar filter can write ([-3, +2]
 * around the edge), so unfiltered lanes/positions are rewritten with their
 * own values inside a span the filter owns anyway.
 *
 * All arithmetic fits int16 lanes: samples are 8-bit, the widest sum is the
 * strong filter's 8*255 + 4, and the deltas are clipped to +-tc (<= 25).
 * Shifts on negative intermediates use vshr (arithmetic), matching the
 * scalar's signed >>.
 */
#if defined(__aarch64__)
#include <arm_neon.h>
#include <stdint.h>
#include <string.h>
#include "deblock.h"

/* Filter core for four lines held in int16x4 lanes. in/out order:
 * p3 p2 p1 p0 q0 q1 q2 q3 -> out[0..5] = p2 p1 p0 q0 q1 q2. */
static inline void db_luma4_core(const int16x4_t s[8], int bs, int alpha,
                                 int beta, int tc0, int16x4_t out[6])
{
    const int16x4_t p3 = s[0], p2 = s[1], p1 = s[2], p0 = s[3];
    const int16x4_t q0 = s[4], q1 = s[5], q2 = s[6], q3 = s[7];
    const int16x4_t vb = vdup_n_s16((int16_t)beta);

    uint16x4_t filt = vclt_s16(vabd_s16(p0, q0), vdup_n_s16((int16_t)alpha));
    filt = vand_u16(filt, vclt_s16(vabd_s16(p1, p0), vb));
    filt = vand_u16(filt, vclt_s16(vabd_s16(q1, q0), vb));

    uint16x4_t apb = vclt_s16(vabd_s16(p2, p0), vb);   /* ap < beta */
    uint16x4_t aqb = vclt_s16(vabd_s16(q2, q0), vb);   /* aq < beta */

    out[0] = p2; out[5] = q2;

    if (bs < 4) {
        /* tc = tc0 + (ap<beta) + (aq<beta): masks are -1, so subtract. */
        int16x4_t tc = vdup_n_s16((int16_t)tc0);
        tc = vsub_s16(tc, vreinterpret_s16_u16(apb));
        tc = vsub_s16(tc, vreinterpret_s16_u16(aqb));
        int16x4_t d = vadd_s16(vshl_n_s16(vsub_s16(q0, p0), 2), vsub_s16(p1, q1));
        d = vshr_n_s16(vadd_s16(d, vdup_n_s16(4)), 3);
        d = vmax_s16(vneg_s16(tc), vmin_s16(tc, d));
        int16x4_t p0n = vadd_s16(p0, d), q0n = vsub_s16(q0, d);
        const int16x4_t z = vdup_n_s16(0), m255 = vdup_n_s16(255);
        p0n = vmin_s16(vmax_s16(p0n, z), m255);
        q0n = vmin_s16(vmax_s16(q0n, z), m255);

        int16x4_t tcl = vdup_n_s16((int16_t)tc0);
        int16x4_t av = vrhadd_s16(p0, q0);             /* (p0 + q0 + 1) >> 1 */
        int16x4_t dp1 = vshr_n_s16(vsub_s16(vadd_s16(p2, av), vshl_n_s16(p1, 1)), 1);
        dp1 = vmax_s16(vneg_s16(tcl), vmin_s16(tcl, dp1));
        int16x4_t dq1 = vshr_n_s16(vsub_s16(vadd_s16(q2, av), vshl_n_s16(q1, 1)), 1);
        dq1 = vmax_s16(vneg_s16(tcl), vmin_s16(tcl, dq1));

        out[1] = vbsl_s16(vand_u16(filt, apb), vadd_s16(p1, dp1), p1);
        out[2] = vbsl_s16(filt, p0n, p0);
        out[3] = vbsl_s16(filt, q0n, q0);
        out[4] = vbsl_s16(vand_u16(filt, aqb), vadd_s16(q1, dq1), q1);
        return;
    }

    /* bs == 4 */
    uint16x4_t strong = vclt_s16(vabd_s16(p0, q0),
                                 vdup_n_s16((int16_t)((alpha >> 2) + 2)));
    uint16x4_t sp = vand_u16(vand_u16(filt, strong), apb);
    uint16x4_t sq = vand_u16(vand_u16(filt, strong), aqb);
    const int16x4_t c2 = vdup_n_s16(2), c4 = vdup_n_s16(4);

    int16x4_t pq = vadd_s16(p0, q0);
    /* strong P: (p2 + 2p1 + 2p0 + 2q0 + q1 + 4)>>3, (p2+p1+p0+q0+2)>>2,
 * (2p3 + 3p2 + p1 + p0 + q0 + 4)>>3 */
    int16x4_t p0s = vshr_n_s16(vadd_s16(vadd_s16(vadd_s16(p2, q1),
                    vshl_n_s16(vadd_s16(p1, pq), 1)), c4), 3);
    int16x4_t p1s = vshr_n_s16(vadd_s16(vadd_s16(vadd_s16(p2, p1), pq), c2), 2);
    int16x4_t p2s = vshr_n_s16(vadd_s16(vadd_s16(vadd_s16(vshl_n_s16(p3, 1),
                    vadd_s16(vshl_n_s16(p2, 1), p2)), vadd_s16(p1, pq)), c4), 3);
    int16x4_t p0w = vshr_n_s16(vadd_s16(vadd_s16(vshl_n_s16(p1, 1),
                    vadd_s16(p0, q1)), c2), 2);
    /* strong Q mirror */
    int16x4_t q0s = vshr_n_s16(vadd_s16(vadd_s16(vadd_s16(q2, p1),
                    vshl_n_s16(vadd_s16(q1, pq), 1)), c4), 3);
    int16x4_t q1s = vshr_n_s16(vadd_s16(vadd_s16(vadd_s16(q2, q1), pq), c2), 2);
    int16x4_t q2s = vshr_n_s16(vadd_s16(vadd_s16(vadd_s16(vshl_n_s16(q3, 1),
                    vadd_s16(vshl_n_s16(q2, 1), q2)), vadd_s16(q1, pq)), c4), 3);
    int16x4_t q0w = vshr_n_s16(vadd_s16(vadd_s16(vshl_n_s16(q1, 1),
                    vadd_s16(q0, p1)), c2), 2);

    out[0] = vbsl_s16(sp, p2s, p2);
    out[1] = vbsl_s16(sp, p1s, p1);
    out[2] = vbsl_s16(filt, vbsl_s16(sp, p0s, p0w), p0);
    out[3] = vbsl_s16(filt, vbsl_s16(sq, q0s, q0w), q0);
    out[4] = vbsl_s16(sq, q1s, q1);
    out[5] = vbsl_s16(sq, q2s, q2);
}

/* trn network mapping four int16x8 "rows" to the packed-column form
 * ({c0,c4}, {c1,c5}, {c2,c6}, {c3,c7}); it is an involution, so the same
 * network converts packed columns back to rows. */
#define DB_TRN4x8(v0, v1, v2, v3) do {                                       \
    int16x8x2_t t01_ = vtrnq_s16(v0, v1);                                    \
    int16x8x2_t t23_ = vtrnq_s16(v2, v3);                                    \
    int32x4x2_t u02_ = vtrnq_s32(vreinterpretq_s32_s16(t01_.val[0]),         \
                                 vreinterpretq_s32_s16(t23_.val[0]));        \
    int32x4x2_t u13_ = vtrnq_s32(vreinterpretq_s32_s16(t01_.val[1]),         \
                                 vreinterpretq_s32_s16(t23_.val[1]));        \
    v0 = vreinterpretq_s16_s32(u02_.val[0]);   /* {c0, c4} */                \
    v1 = vreinterpretq_s16_s32(u13_.val[0]);   /* {c1, c5} */                \
    v2 = vreinterpretq_s16_s32(u02_.val[1]);   /* {c2, c6} */                \
    v3 = vreinterpretq_s16_s32(u13_.val[1]);   /* {c3, c7} */                \
} while (0)

/* Vertical edge (scalar step == 1): four lines at q0 + ln*stride, samples
 * across the row. Load 8 bytes per line, transpose to sample vectors,
 * filter, transpose back, store the [-3, +2] span per line. */
void y264_deblock_luma_v4_neon(uint8_t *q0p, int stride, int bs, int alpha,
                               int beta, int tc0)
{
    int16x8_t v0 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(q0p - 4)));
    int16x8_t v1 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(q0p - 4 + stride)));
    int16x8_t v2 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(q0p - 4 + 2 * stride)));
    int16x8_t v3 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(q0p - 4 + 3 * stride)));

    DB_TRN4x8(v0, v1, v2, v3);
    int16x4_t s[8], o[6];
    s[0] = vget_low_s16(v0);  s[4] = vget_high_s16(v0);   /* p3, q0 */
    s[1] = vget_low_s16(v1);  s[5] = vget_high_s16(v1);   /* p2, q1 */
    s[2] = vget_low_s16(v2);  s[6] = vget_high_s16(v2);   /* p1, q2 */
    s[3] = vget_low_s16(v3);  s[7] = vget_high_s16(v3);   /* p0, q3 */

    db_luma4_core(s, bs, alpha, beta, tc0, o);

    /* repack columns (p3/q3 unmodified) and run the network back to rows */
    v0 = vcombine_s16(s[0], o[3]);
    v1 = vcombine_s16(o[0], o[4]);
    v2 = vcombine_s16(o[1], o[5]);
    v3 = vcombine_s16(o[2], s[7]);
    DB_TRN4x8(v0, v1, v2, v3);

    const int16x8_t rows[4] = { v0, v1, v2, v3 };
    for (int ln = 0; ln < 4; ln++) {
        uint8_t tmp[8];
        vst1_u8(tmp, vqmovun_s16(rows[ln]));
        memcpy(q0p - 3 + ln * stride, tmp + 1, 6);   /* [-3, +2] span only */
    }
}

/* Horizontal edge (scalar step == stride): four columns at q0 + ln, samples
 * down the rows -- lanes are naturally the four columns, no transpose. */
void y264_deblock_luma_h4_neon(uint8_t *q0p, int stride, int bs, int alpha,
                               int beta, int tc0)
{
    int16x4_t s[8], o[6];
    for (int k = 0; k < 8; k++) {
        uint32_t w;
        memcpy(&w, q0p + (k - 4) * stride, 4);
        s[k] = vget_low_s16(vreinterpretq_s16_u16(
                   vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(w)))));
    }
    db_luma4_core(s, bs, alpha, beta, tc0, o);
    for (int k = 0; k < 6; k++) {
        uint8x8_t v = vqmovun_s16(vcombine_s16(o[k], o[k]));
        uint32_t w = vget_lane_u32(vreinterpret_u32_u8(v), 0);
        memcpy(q0p + (k - 3) * stride, &w, 4);
    }
}


/* ---- Chroma edge filter (chromaStyleFilteringFlag == 1) -----------------
 *
 * The chroma filter touches only p0 and q0, and its bS < 4 clip is tc0 + 1
 * with no ap/aq term -- so it needs four sample vectors, not eight, and the
 * whole edge fits one pass. That is the restructure this kernel exists for:
 * the scalar loop feeds two (4:2:0) or four (4:2:2) lines per bS unit, which
 * is quarter-lane work, while a chroma edge is eight lines that differ only
 * in their bS. tc and the bS == 4 select therefore become per-LANE values
 * derived from the caller's four bS entries, and one pass filters the edge.
 *
 * A bS == 0 line is passed as tc 0 with the strong select clear: the weak
 * delta then clips to zero and the line is rewritten with its own samples,
 * so no lane needs to be branched around.
 *
 * All arithmetic fits int16 lanes (samples 8-bit, widest sum 2*255*2 + 2). */
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

    /* bS < 4: delta = Clip3(-tc, tc, ((q0 - p0)*4 + (p1 - q1) + 4) >> 3) */
    int16x8_t d = vaddq_s16(vshlq_n_s16(vsubq_s16(q0, p0), 2), vsubq_s16(p1, q1));
    d = vshrq_n_s16(vaddq_s16(d, vdupq_n_s16(4)), 3);
    d = vmaxq_s16(vnegq_s16(tc), vminq_s16(tc, d));
    const int16x8_t z = vdupq_n_s16(0), m255 = vdupq_n_s16(255);
    int16x8_t p0w = vminq_s16(vmaxq_s16(vaddq_s16(p0, d), z), m255);
    int16x8_t q0w = vminq_s16(vmaxq_s16(vsubq_s16(q0, d), z), m255);

    /* bS == 4: (2*p1 + p0 + q1 + 2) >> 2 and its mirror; always in range. */
    const int16x8_t c2 = vdupq_n_s16(2);
    int16x8_t p0s = vshrq_n_s16(vaddq_s16(vaddq_s16(vshlq_n_s16(p1, 1),
                                vaddq_s16(p0, q1)), c2), 2);
    int16x8_t q0s = vshrq_n_s16(vaddq_s16(vaddq_s16(vshlq_n_s16(q1, 1),
                                vaddq_s16(q0, p1)), c2), 2);

    *p0o = vbslq_s16(filt, vbslq_s16(bs4, p0s, p0w), p0);
    *q0o = vbslq_s16(filt, vbslq_s16(bs4, q0s, q0w), q0);
}

/* Per-lane tc and bS == 4 mask from the caller's four bS entries, each
 * covering `span` consecutive lines (2 for 4:2:0, 4 for 4:2:2). */
static inline void db_chroma8_params(const uint8_t bs[4], const uint8_t tc0tab[3],
                                     int span, int g,
                                     int16x8_t *tc, uint16x8_t *bs4)
{
    int16_t t[8]; uint16_t m[8];
    for (int i = 0; i < 8; i++) {
        int b = bs[(g * 8 + i) / span];
        t[i] = (int16_t)(b == 0 || b >= 4 ? 0 : tc0tab[b - 1] + 1);
        m[i] = (uint16_t)(b >= 4 ? 0xffff : 0);
    }
    *tc = vld1q_s16(t);
    *bs4 = vld1q_u16(m);
}

/* Horizontal edge (samples across the edge run down the plane): the eight
 * lines are eight consecutive columns, so each sample vector is one load and
 * each written sample one store -- no transpose anywhere.
 *
 * There is no vertical counterpart on purpose. A vertical chroma edge is
 * eight rows of four bytes, so the kernel has to gather and scatter across
 * the stride, and both forms of that measured slower than the scalar filter:
 * 12.10 ns against 10.38 staging through memory and 11.98 with eight
 * ld1-lane / st1-lane pairs straight into registers, i.e. 0.86x and 0.87x.
 * The chroma filter only touches p0 and q0, so there is too little
 * arithmetic to amortize a transpose. Vertical chroma edges stay scalar. */
void y264_deblock_chroma8_h_neon(uint8_t *q0p, int stride, int alpha, int beta,
                                 const uint8_t bs[4], const uint8_t tc0tab[3],
                                 int span, int g)
{
    int16x8_t tc; uint16x8_t bs4;
    db_chroma8_params(bs, tc0tab, span, g, &tc, &bs4);
#define LD(off) vreinterpretq_s16_u16(vmovl_u8(vld1_u8(q0p + (off) * stride)))
    int16x8_t p1 = LD(-2), p0 = LD(-1), q0 = LD(0), q1 = LD(1);
#undef LD
    int16x8_t p0o, q0o;
    db_chroma8_core(p1, p0, q0, q1, alpha, beta, tc, bs4, &p0o, &q0o);
    vst1_u8(q0p - stride, vqmovun_s16(p0o));
    vst1_u8(q0p, vqmovun_s16(q0o));
}

/* ------------------------------------------------------------------ *
 * Boundary strengths, one macroblock per call (8.7.2.1).
 *
 * The scalar shape derived one edge at a time behind a lazy memo: a call, six
 * strided grid reads and a chain of early-outs, up to thirty-two times per
 * macroblock. Every test in 8.7.2.1 is a lane-wise compare, so all sixteen
 * vertical strengths fall out of two eight-lane passes and the sixteen
 * horizontal ones out of two more. The eight lanes of a pass are two
 * macroblock rows of four edges, which is why the results narrow straight into
 * a single eight-byte store.
 *
 * The only part that is not a plain grid read is the coefficient flag: under
 * the 8x8 transform a 4x4 block's coded status is its whole quadrant's, so the
 * mask is folded with vrev32 (which swaps the lanes inside each 32-bit pair)
 * before it is used, and the left column and top row fold with their OWN
 * macroblock's flag rather than this one's.
 * ------------------------------------------------------------------ */

/* Four bytes of an int8 grid widened to int16 lanes, without reading a fifth:
 * the grids are exactly wide enough for the macroblock and an 8-byte load off
 * the last one would run past the row. */
static inline int16x4_t db_ld4_s8(const int8_t *p)
{
    int32_t w;
    memcpy(&w, p, 4);
    return vget_low_s16(vmovl_s8(vreinterpret_s8_s32(vdup_n_s32(w))));
}

/* "Has coefficients" for four consecutive 4x4 blocks of row `dy`, under the
 * transform flag of the macroblock those blocks belong to. */
static inline uint16x4_t db_co_row(const int8_t *nnz, int s, int dy, int tr8)
{
    const int16x4_t z = vdup_n_s16(0);
    if (!tr8)
        return vcgt_s16(db_ld4_s8(nnz + dy * s), z);
    int qy = dy & ~1;                       /* -1 -> -2: the quadrant's first row */
    uint16x4_t t = vorr_u16(vcgt_s16(db_ld4_s8(nnz + qy * s), z),
                            vcgt_s16(db_ld4_s8(nnz + (qy + 1) * s), z));
    return vorr_u16(t, vrev32_u16(t));      /* pair the columns */
}

/* The same for one block, used for the left neighbour column. */
static inline int db_co_at(const int8_t *nnz, int s, int dx, int dy, int tr8)
{
    if (!tr8)
        return nnz[dy * s + dx] > 0;
    int qx = dx & ~1, qy = dy & ~1;
    return nnz[qy * s + qx] > 0 || nnz[qy * s + qx + 1] > 0
        || nnz[(qy + 1) * s + qx] > 0 || nnz[(qy + 1) * s + qx + 1] > 0;
}

struct db_side4 {
    int16x4_t r0, r1, x0, y0, x1, y1;
    uint16x4_t co;
};
struct db_side8 {
    int16x8_t r0, r1, x0, y0, x1, y1;
    uint16x8_t co;
};

static inline struct db_side4 db_load4(const struct y264_bs_ctx *c, int dy,
                                       int dx, uint16x4_t co)
{
    int i = dy * c->mv_stride + dx;
    struct db_side4 s;
    s.r0 = db_ld4_s8(c->ref0 + i);
    s.r1 = db_ld4_s8(c->ref1 + i);
    s.x0 = vld1_s16(c->mvx0 + i);
    s.y0 = vld1_s16(c->mvy0 + i);
    s.x1 = vld1_s16(c->mvx1 + i);
    s.y1 = vld1_s16(c->mvy1 + i);
    s.co = co;
    return s;
}

/* Rotate a row's lanes right by one: (a0,a1,a2,a3) -> (a3,a0,a1,a2). Lane 0 is
 * then the wrong block, and only feeds edges the caller discards. */
static inline struct db_side4 db_rot4(struct db_side4 a)
{
    struct db_side4 s;
    s.r0 = vext_s16(a.r0, a.r0, 3);
    s.r1 = vext_s16(a.r1, a.r1, 3);
    s.x0 = vext_s16(a.x0, a.x0, 3);
    s.y0 = vext_s16(a.y0, a.y0, 3);
    s.x1 = vext_s16(a.x1, a.x1, 3);
    s.y1 = vext_s16(a.y1, a.y1, 3);
    s.co = vext_u16(a.co, a.co, 3);
    return s;
}

static inline struct db_side8 db_pair(struct db_side4 a, struct db_side4 b)
{
    struct db_side8 s;
    s.r0 = vcombine_s16(a.r0, b.r0);
    s.r1 = vcombine_s16(a.r1, b.r1);
    s.x0 = vcombine_s16(a.x0, b.x0);
    s.y0 = vcombine_s16(a.y0, b.y0);
    s.x1 = vcombine_s16(a.x1, b.x1);
    s.y1 = vcombine_s16(a.y1, b.y1);
    s.co = vcombine_u16(a.co, b.co);
    return s;
}

/* Eight edges at once. `edge` selects the macroblock-edge lanes (bS 4 rather
 * than 3 on the intra branch). */
static inline uint8x8_t db_bs8(const struct db_side8 *p, const struct db_side8 *q,
                               uint16x8_t edge)
{
    const int16x8_t z = vdupq_n_s16(0), four = vdupq_n_s16(4);
    uint16x8_t ip = vandq_u16(vcltq_s16(p->r0, z), vcltq_s16(p->r1, z));
    uint16x8_t iq = vandq_u16(vcltq_s16(q->r0, z), vcltq_s16(q->r1, z));
    uint16x8_t intra = vorrq_u16(ip, iq);
    uint16x8_t coeff = vorrq_u16(p->co, q->co);

    uint16x8_t p0 = vcgeq_s16(p->r0, z), q0 = vcgeq_s16(q->r0, z);
    uint16x8_t p1 = vcgeq_s16(p->r1, z), q1 = vcgeq_s16(q->r1, z);
    uint16x8_t one = veorq_u16(p0, q0);                 /* different list membership */
    one = vorrq_u16(one, veorq_u16(p1, q1));
    /* multi-ref: same list, different list-0 picture */
    one = vorrq_u16(one, vandq_u16(p0, vmvnq_u16(vceqq_s16(p->r0, q->r0))));
    uint16x8_t d0 = vorrq_u16(vcgeq_s16(vabdq_s16(p->x0, q->x0), four),
                              vcgeq_s16(vabdq_s16(p->y0, q->y0), four));
    one = vorrq_u16(one, vandq_u16(p0, d0));
    uint16x8_t d1 = vorrq_u16(vcgeq_s16(vabdq_s16(p->x1, q->x1), four),
                              vcgeq_s16(vabdq_s16(p->y1, q->y1), four));
    one = vorrq_u16(one, vandq_u16(p1, d1));

    int16x8_t bs = vreinterpretq_s16_u16(vandq_u16(one, vdupq_n_u16(1)));
    bs = vbslq_s16(coeff, vdupq_n_s16(2), bs);
    bs = vbslq_s16(intra, vbslq_s16(edge, vdupq_n_s16(4), vdupq_n_s16(3)), bs);
    return vmovn_u16(vreinterpretq_u16_s16(bs));
}

void y264_deblock_strength_neon(const struct y264_bs_ctx *c,
                                uint8_t bsv[4][4], uint8_t bsh[4][4])
{
    const int8_t *nnz = c->nnz;
    const int ns = c->nnz_stride;

    /* Columns 0..3 of rows -1..3; hi[0] is the top neighbour's row. */
    uint16x4_t co[5];
    for (int y = 0; y < 4; y++)
        co[y + 1] = db_co_row(nnz, ns, y, c->tr8_cur);
    co[0] = c->have_top ? db_co_row(nnz, ns, -1, c->tr8_top) : co[1];
    struct db_side4 hi[5];
    hi[0] = db_load4(c, c->have_top ? -1 : 0, 0, co[0]);
    for (int y = 0; y < 4; y++)
        hi[y + 1] = db_load4(c, y, 0, co[y + 1]);

    /* Vertical edges: the p side is the same row shifted one column left, and
 * its coefficient lane comes from the LEFT macroblock's fold. With no left
 * macroblock that column is outside the frame and must not be read, so the
 * p side is rotated out of the row itself -- lane 0 then holds column 3's
 * value, and the xb == 0 edge it feeds is one the caller does not filter
 * and is zeroed below. */
    struct db_side4 lo[4];
    for (int y = 0; y < 4; y++) {
        if (c->have_left) {
            int l = db_co_at(nnz, ns, -1, y, c->tr8_left);
            uint16x4_t colo = vext_u16(vdup_n_u16(l ? 0xffff : 0), co[y + 1], 3);
            lo[y] = db_load4(c, y, -1, colo);
        } else {
            lo[y] = db_rot4(hi[y + 1]);
        }
    }
    {
        static const uint16_t EDGEV[8] = { 0xffff, 0, 0, 0, 0xffff, 0, 0, 0 };
        /* the kernel produces a row of four edges at a time and bsv is stored
 * edge-major, so the 4x4 result matrix is transposed on the way out --
 * one table lookup, against a 32-bit skip test per edge in the caller */
        static const uint8_t TR[16] = { 0, 4,  8, 12, 1, 5,  9, 13,
                                        2, 6, 10, 14, 3, 7, 11, 15 };
        uint16x8_t ev = vld1q_u16(EDGEV);
        struct db_side8 p = db_pair(lo[0], lo[1]), q = db_pair(hi[1], hi[2]);
        uint8x8_t r01 = db_bs8(&p, &q, ev);
        p = db_pair(lo[2], lo[3]); q = db_pair(hi[3], hi[4]);
        uint8x8_t r23 = db_bs8(&p, &q, ev);
        uint8x16_t rows = vcombine_u8(r01, r23);
        if (!c->have_left)
            rows = vandq_u8(rows, vreinterpretq_u8_u32(vdupq_n_u32(0xffffff00)));
        vst1q_u8(&bsv[0][0], vqtbl1q_u8(rows, vld1q_u8(TR)));
    }

    /* Horizontal edges: the p side is the row above, so no shift is needed. */
    {
        struct db_side8 p = db_pair(hi[0], hi[1]), q = db_pair(hi[1], hi[2]);
        uint8x8_t r = db_bs8(&p, &q, vcombine_u16(vdup_n_u16(0xffff), vdup_n_u16(0)));
        if (!c->have_top)
            r = vreinterpret_u8_u32(vset_lane_u32(0, vreinterpret_u32_u8(r), 0));
        vst1_u8(&bsh[0][0], r);
        p = db_pair(hi[2], hi[3]); q = db_pair(hi[3], hi[4]);
        vst1_u8(&bsh[2][0], db_bs8(&p, &q, vdupq_n_u16(0)));
    }
}

#endif /* __aarch64__ */
