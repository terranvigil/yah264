/*
 * pixel.c - portable SAD kernels and dispatch
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "pixel.h"
#include "../common/cpu.h"
#include "predict.h"
#include "transform.h"
#include "../common/ledger.h"
#include <stdlib.h>

const uint8_t y264_pu_width[Y264_PU_COUNT]  = { 16, 16, 8, 8, 8, 4, 4 };
const uint8_t y264_pu_height[Y264_PU_COUNT] = { 16,  8, 16, 8, 4, 8, 4 };
const char *const y264_pu_name[Y264_PU_COUNT] = {
    "16x16", "16x8", "8x16", "8x8", "8x4", "4x8", "4x4"
};

#define DEF_SAD_C(w, h)                                                     \
static int sad_c_##w##x##h(const pixel *a, int as,                        \
                           const pixel *b, int bs)                        \
{                                                                          \
    int sum = 0;                                                           \
    NLED(sad_call, 1); NLED(sad_pix, (w)*(h));                             \
    for (int y = 0; y < (h); y++) {                                        \
        for (int x = 0; x < (w); x++)                                      \
            sum += abs((int)a[x] - (int)b[x]);                             \
        a += as;                                                          \
        b += bs;                                                          \
    }                                                                     \
    return sum;                                                            \
}

DEF_SAD_C(16, 16)
DEF_SAD_C(16, 8)
DEF_SAD_C(8, 16)
DEF_SAD_C(8, 8)
DEF_SAD_C(8, 4)
DEF_SAD_C(4, 8)
DEF_SAD_C(4, 4)

/* Batched-x4 references: four single SADs, so scores are exact by
 * construction (SAD is a pure per-pair sum; batching only shares loads). */
#define DEF_SAD_X4_C(w, h)                                                  \
static void sad_x4_c_##w##x##h(const pixel *src, int ss,                    \
                               const pixel *r0, const pixel *r1,            \
                               const pixel *r2, const pixel *r3,            \
                               int rs, int scores[4])                       \
{                                                                           \
    scores[0] = sad_c_##w##x##h(src, ss, r0, rs);                           \
    scores[1] = sad_c_##w##x##h(src, ss, r1, rs);                           \
    scores[2] = sad_c_##w##x##h(src, ss, r2, rs);                           \
    scores[3] = sad_c_##w##x##h(src, ss, r3, rs);                           \
}

DEF_SAD_X4_C(16, 16)
DEF_SAD_X4_C(16, 8)
DEF_SAD_X4_C(8, 16)
DEF_SAD_X4_C(8, 8)
DEF_SAD_X4_C(8, 4)
DEF_SAD_X4_C(4, 8)
DEF_SAD_X4_C(4, 4)

/* SWAR SATD (x264's scalar Hadamard trick): pack two transform lanes into one wide
 * integer so each butterfly does two values at once -- ~half the arithmetic ops of a
 * naive per-element SATD. sum_t holds one lane, sum2_t two. BYTE-IDENTICAL to the old
 * naive kernel: yah264's SATD is exactly 2x x264's, so we drop x264's final >>1 and
 * return the un-halved sum (verified across 300k random blocks). */
#if Y264_BIT_DEPTH > 8
typedef uint32_t y264_sum_t;
typedef int32_t  y264_sum_signed_t;
typedef uint64_t y264_sum2_t;
#define Y264_BPS 32
#else
typedef uint16_t y264_sum_t;
typedef int16_t  y264_sum_signed_t;
typedef uint32_t y264_sum2_t;
#define Y264_BPS 16
#endif
static inline y264_sum2_t satd_abs2(y264_sum2_t a)
{
    y264_sum2_t s = ((a >> (Y264_BPS - 1)) & (((y264_sum2_t)1 << Y264_BPS) + 1)) * (y264_sum_t)-1;
    return (a + s) ^ s;
}
#define Y264_HADAMARD4(d0,d1,d2,d3,s0,s1,s2,s3) do {              \
    y264_sum2_t t0 = (s0)+(s1), t1 = (s0)-(s1);                  \
    y264_sum2_t t2 = (s2)+(s3), t3 = (s2)-(s3);                  \
    (d0)=t0+t2; (d2)=t0-t2; (d1)=t1+t3; (d3)=t1-t3; } while (0)

/* Recover one packed word's two lanes and return |lo| + |hi|.
 *
 * A lane holds a signed value, and a negative low lane borrows from the high
 * lane during the butterfly -- so the high half reads one short exactly when
 * the low half's sign bit is set. Add that borrow back before taking either
 * absolute value and the pair is exact. Keeping the two lanes as ordinary
 * signed scalars here (rather than folding them with a packed abs) is what
 * lets the 4x4 accumulate in a plain int. */
static inline int satd_lane_absum(y264_sum2_t v)
{
    int lo = (int)(y264_sum_signed_t)(y264_sum_t)v;
    int hi = (int)(y264_sum_signed_t)(y264_sum_t)((v >> Y264_BPS)
                                                  + ((v >> (Y264_BPS - 1)) & 1));
    return (lo < 0 ? -lo : lo) + (hi < 0 ? -hi : hi);
}

/* One 4x4 SATD, SWAR: two independent transforms ride in the two halves of one
 * word. Bit-identical to the naive y264_hadamard4x4 + abs sum.
 *
 * The column transform runs first, unpacked, and its last butterfly stage is
 * fused with the packing step -- so a word leaves the first loop already
 * holding a PAIR OF ROWS, and the row transform in the second loop then runs
 * on both of them at once. That ordering is the whole trick: pack along the
 * axis you are not currently transforming, and the two lanes stay independent.
 *
 * Which axis goes first is free. A 2-D Hadamard is separable, so transforming
 * columns-then-rows yields the transpose of rows-then-columns, and the sum of
 * absolute coefficients does not care about the transpose. */
static inline int satd4x4_core(const pixel *a, int as, const pixel *b, int bs)
{
    NLED(satd_call, 1); NLED(satd_pix, 16); NLED_SATD(16);
    y264_sum2_t w[4][2];
    for (int x = 0; x < 4; x++) {
        y264_sum2_t c0 = (y264_sum2_t)(a[0*as + x] - b[0*bs + x]);
        y264_sum2_t c1 = (y264_sum2_t)(a[1*as + x] - b[1*bs + x]);
        y264_sum2_t c2 = (y264_sum2_t)(a[2*as + x] - b[2*bs + x]);
        y264_sum2_t c3 = (y264_sum2_t)(a[3*as + x] - b[3*bs + x]);
        /* stage 1 + pack: u0 carries rows {0,1}, u1 carries rows {2,3} */
        y264_sum2_t u0 = (c0 + c1) + ((c0 - c1) << Y264_BPS);
        y264_sum2_t u1 = (c2 + c3) + ((c2 - c3) << Y264_BPS);
        w[x][0] = u0 + u1; w[x][1] = u0 - u1;
    }
    int sum = 0;
    for (int j = 0; j < 2; j++) {
        y264_sum2_t p0 = w[0][j], p1 = w[1][j], p2 = w[2][j], p3 = w[3][j];
        y264_sum2_t e0 = p0 + p1, e1 = p0 - p1, e2 = p2 + p3, e3 = p2 - p3;
        sum += satd_lane_absum(e0 + e2) + satd_lane_absum(e1 + e3)
             + satd_lane_absum(e0 - e2) + satd_lane_absum(e1 - e3);
    }
    return sum;
}

/* One 8x4 SATD (SWAR, two 4-wide column groups packed) = the two constituent 4x4
 * SATDs summed, at ~half the ops of two satd4x4 calls. */
static inline int satd8x4_core(const pixel *a, int as, const pixel *b, int bs)
{
    NLED(satd_call, 1); NLED(satd_pix, 32); NLED_SATD(32);
    y264_sum2_t tmp[4][4], a0, a1, a2, a3, sum = 0;
    for (int i = 0; i < 4; i++, a += as, b += bs) {
        a0 = (y264_sum2_t)(a[0] - b[0]) + ((y264_sum2_t)(a[4] - b[4]) << Y264_BPS);
        a1 = (y264_sum2_t)(a[1] - b[1]) + ((y264_sum2_t)(a[5] - b[5]) << Y264_BPS);
        a2 = (y264_sum2_t)(a[2] - b[2]) + ((y264_sum2_t)(a[6] - b[6]) << Y264_BPS);
        a3 = (y264_sum2_t)(a[3] - b[3]) + ((y264_sum2_t)(a[7] - b[7]) << Y264_BPS);
        Y264_HADAMARD4(tmp[i][0], tmp[i][1], tmp[i][2], tmp[i][3], a0, a1, a2, a3);
    }
    for (int i = 0; i < 4; i++) {
        Y264_HADAMARD4(a0, a1, a2, a3, tmp[0][i], tmp[1][i], tmp[2][i], tmp[3][i]);
        sum += satd_abs2(a0) + satd_abs2(a1) + satd_abs2(a2) + satd_abs2(a3);
    }
    return (int)(((y264_sum_t)sum) + (sum >> Y264_BPS));
}

/* SATD: sum of absolute values of the 4x4 Hadamard transform of the residual. */
static int satd_c_4x4(const pixel *a, int as, const pixel *b, int bs)
{
    return satd4x4_core(a, as, b, bs);
}

/* Fused 8x8 / 16x16 SATD from the 8-wide 8x4 kernel (two/eight tiles). */
static int satd_c_8x8(const pixel *a, int as, const pixel *b, int bs)
{
    return satd8x4_core(a, as, b, bs)
         + satd8x4_core(a + 4*as, as, b + 4*bs, bs);
}

/* The x4 reference: four single SATDs, so the scores are exact by construction
 * (a SATD depends only on its own (src, ref) pair). The C tier keeps this form
 * -- there is nothing to share without a vector register file. */
static void satd_x4_c_8x8(const pixel *src, int ss,
                          const pixel *r0, const pixel *r1,
                          const pixel *r2, const pixel *r3,
                          int rs, int scores[4])
{
    scores[0] = satd_c_8x8(src, ss, r0, rs);
    scores[1] = satd_c_8x8(src, ss, r1, rs);
    scores[2] = satd_c_8x8(src, ss, r2, rs);
    scores[3] = satd_c_8x8(src, ss, r3, rs);
}

static int satd_c_16x16(const pixel *a, int as, const pixel *b, int bs)
{
    int s = 0;
    for (int by = 0; by < 16; by += 4)
        for (int bx = 0; bx < 16; bx += 8)
            s += satd8x4_core(a + by*as + bx, as, b + by*bs + bx, bs);
    return s;
}

/* 8-point Walsh-Hadamard (unnormalized). Sum-abs is order-agnostic, so any valid
 * butterfly ordering gives the same SA8D. */
static inline void had8(const int in[8], int out[8])
{
    int a[8], b[8];
    for (int i = 0; i < 4; i++) { a[i] = in[i] + in[i+4]; a[i+4] = in[i] - in[i+4]; }
    b[0]=a[0]+a[2]; b[1]=a[1]+a[3]; b[2]=a[0]-a[2]; b[3]=a[1]-a[3];
    b[4]=a[4]+a[6]; b[5]=a[5]+a[7]; b[6]=a[4]-a[6]; b[7]=a[5]-a[7];
    out[0]=b[0]+b[1]; out[1]=b[0]-b[1]; out[2]=b[2]+b[3]; out[3]=b[2]-b[3];
    out[4]=b[4]+b[5]; out[5]=b[4]-b[5]; out[6]=b[6]+b[7]; out[7]=b[6]-b[7];
}

/* SA8D of an 8x8: sum |2D 8x8 Walsh-Hadamard of the residual|, normalised like
 * x264 ((sum+2)>>2) so it compares directly against the 4x4-support SATD for the
 * transform-size pre-decision. */
static int sa8d_c_8x8(const pixel *a, int as, const pixel *b, int bs)
{
    NLED(sa8d_call, 1); NLED(sa8d_pix, 64); NLED_SATD(64);
    int t[8][8], r[8], c[8];
    for (int y = 0; y < 8; y++) {
        int d[8];
        for (int x = 0; x < 8; x++) d[x] = (int)a[y*as+x] - (int)b[y*bs+x];
        had8(d, r);
        for (int x = 0; x < 8; x++) t[y][x] = r[x];
    }
    long sum = 0;
    for (int x = 0; x < 8; x++) {
        int col[8];
        for (int y = 0; y < 8; y++) col[y] = t[y][x];
        had8(col, c);
        for (int y = 0; y < 8; y++) sum += c[y] < 0 ? -c[y] : c[y];
    }
    return (int)((sum + 2) >> 2);
}

/* SA8D of a 16x16 = sum of the four 8x8 SA8Ds (as x264's 16x16 SA8D does). */
static int sa8d_c_16x16(const pixel *a, int as, const pixel *b, int bs)
{
    return sa8d_c_8x8(a, as, b, bs)
         + sa8d_c_8x8(a + 8, as, b + 8, bs)
         + sa8d_c_8x8(a + 8*as, as, b + 8*bs, bs)
         + sa8d_c_8x8(a + 8*as + 8, as, b + 8*bs + 8, bs);
}

/* AC magnitude of one 8x8 via the 8x8 Hadamard (SA8D support): sum |coeff|
 * minus |DC|. The psy-RD texture term (moved here from macroblock.c so it can
 * dispatch). */
static long hadamard_ac_c_8x8(const pixel *p, int stride)
{
    int m[8][8];
    for (int i = 0; i < 8; i++) {
        int r[8];
        for (int j = 0; j < 8; j++) r[j] = p[i * stride + j];
        had8(r, m[i]);
    }
    long s = 0;
    int dc = 0;
    for (int j = 0; j < 8; j++) {
        int col[8], c[8];
        for (int i = 0; i < 8; i++) col[i] = m[i][j];
        had8(col, c);
        for (int i = 0; i < 8; i++) s += c[i] < 0 ? -c[i] : c[i];
        if (j == 0) dc = c[0] < 0 ? -c[0] : c[0];
    }
    return s - dc;
}

/* SATD of one 4x4 tile against a flat block of its own rounded mean -- the
 * psy-RD texture term. Subtracting a constant only moves the Hadamard's DC, so
 * this is (sum-abs of the tile's own 2D Hadamard, minus its DC) plus the DC
 * against the rounded mean; the tile's pixel sum IS that DC coefficient. */
static inline long ac4_tile(const pixel *b, int s)
{
    int tmp[4][4];
    for (int i = 0; i < 4; i++, b += s) {
        int s0 = b[0] + b[1], d0 = b[0] - b[1];
        int s1 = b[2] + b[3], d1 = b[2] - b[3];
        tmp[i][0] = s0 + s1; tmp[i][1] = d0 + d1;
        tmp[i][2] = s0 - s1; tmp[i][3] = d0 - d1;
    }
    long sum = 0;
    int dc = 0;
    for (int j = 0; j < 4; j++) {
        int s0 = tmp[0][j] + tmp[1][j], d0 = tmp[0][j] - tmp[1][j];
        int s1 = tmp[2][j] + tmp[3][j], d1 = tmp[2][j] - tmp[3][j];
        int c0 = s0 + s1, c1 = d0 + d1, c2 = s0 - s1, c3 = d0 - d1;
        if (j == 0) dc = c0;                  /* == the tile's pixel sum */
        sum += (c0 < 0 ? -c0 : c0) + (c1 < 0 ? -c1 : c1)
             + (c2 < 0 ? -c2 : c2) + (c3 < 0 ? -c3 : c3);
    }
    int flat = 16 * ((dc + 8) >> 4);
    return sum - dc + (dc < flat ? flat - dc : dc - flat);
}

static long texture_ac4_c_16x16(const pixel *p, int stride)
{
    long e = 0;
    for (int by = 0; by < 16; by += 4)
        for (int bx = 0; bx < 16; bx += 4)
            e += ac4_tile(p + by * stride + bx, stride);
    return e;
}

/* Both psy-RD texture terms of one 16x16 in one pass over the pixels.
 *
 * A 2D 8x8 Walsh-Hadamard is the four quadrant 4x4 transforms combined by one
 * 2x2 butterfly per coefficient position, and the 8x8's DC is the sum of the
 * four tile DCs -- so the SA8D-support term is a cheap epilogue on the tile
 * coefficients the SATD-support term already computes, instead of a second
 * transform of every pixel. The tiles use the SWAR packing of the SATD kernels
 * above (lane 0 carries the even horizontal frequencies, lane 1 the odd), which
 * halves the arithmetic on top.
 *
 * Lane bound, 8-bit: a lane accumulates 32 of the 8x8's coefficient magnitudes.
 * |DC| <= 64*255 and, by Cauchy-Schwarz on the AC part (which is orthogonal to
 * the mean), sum|AC| <= sqrt(31 * 64 * 64 * 127.5^2) < 45500 -- so a lane holds
 * < 61800 < 2^16 and never carries into its neighbour. Deeper bit depths use
 * 32-bit lanes with the same margin. */
static void texture_ac48_c_16x16(const pixel *p, int stride, long out[2])
{
    long e4 = 0, e8 = 0;
    for (int qy = 0; qy < 16; qy += 8) {
        for (int qx = 0; qx < 16; qx += 8) {
            y264_sum2_t co[4][8];       /* the quadrant's four 4x4 tiles, packed */
            for (int t = 0; t < 4; t++) {
                const pixel *b = p + (qy + (t >> 1) * 4) * stride + qx + (t & 1) * 4;
                y264_sum2_t tmp[4][2], a0, a1, sum = 0;
                for (int i = 0; i < 4; i++, b += stride) {
                    a0 = (y264_sum2_t)(b[0] + b[1]) + ((y264_sum2_t)(b[0] - b[1]) << Y264_BPS);
                    a1 = (y264_sum2_t)(b[2] + b[3]) + ((y264_sum2_t)(b[2] - b[3]) << Y264_BPS);
                    tmp[i][0] = a0 + a1; tmp[i][1] = a0 - a1;
                }
                for (int i = 0; i < 2; i++) {
                    y264_sum2_t c0, c1, c2, c3;
                    Y264_HADAMARD4(c0, c1, c2, c3, tmp[0][i], tmp[1][i], tmp[2][i], tmp[3][i]);
                    co[t][i*4+0] = c0; co[t][i*4+1] = c1;
                    co[t][i*4+2] = c2; co[t][i*4+3] = c3;
                    sum += satd_abs2(c0) + satd_abs2(c1) + satd_abs2(c2) + satd_abs2(c3);
                }
                /* co[t][0] lane 0 is the tile's DC == its pixel sum. */
                int dc = (int)(y264_sum_t)co[t][0];
                int flat = 16 * ((dc + 8) >> 4);
                e4 += (long)((y264_sum_t)sum) + (long)(sum >> Y264_BPS)
                    - dc + (dc < flat ? flat - dc : dc - flat);
            }
            y264_sum2_t s8 = 0;
            for (int k = 0; k < 8; k++) {
                y264_sum2_t a = co[0][k], b = co[1][k], c = co[2][k], d = co[3][k];
                y264_sum2_t t0 = a + b, t1 = a - b, t2 = c + d, t3 = c - d;
                s8 += satd_abs2(t0 + t2) + satd_abs2(t0 - t2)
                    + satd_abs2(t1 + t3) + satd_abs2(t1 - t3);
            }
            long dc8 = (long)(y264_sum_t)(co[0][0] + co[1][0] + co[2][0] + co[3][0]);
            e8 += (long)((y264_sum_t)s8) + (long)(s8 >> Y264_BPS) - dc8;
        }
    }
    out[0] = e4; out[1] = e8;
}

/* Pixel sum and sum of squares of a 16x16. */
static void var_c_16x16(const pixel *p, int stride, uint32_t out[2])
{
    uint32_t s1 = 0, s2 = 0;
    for (int y = 0; y < 16; y++, p += stride) {
        uint32_t r1 = 0, r2 = 0;
        for (int x = 0; x < 16; x++) {
            uint32_t v = p[x];
            r1 += v; r2 += v * v;
        }
        s1 += r1; s2 += r2;
    }
    out[0] = s1; out[1] = s2;
}

/* All nine Intra4x4 mode costs: the reference is literally the loop it
 * replaces (build the mode's prediction, SATD it against the source), so
 * "bit-exact with the C reference" and "the mode decision is unchanged" are
 * the same statement. */
static void intra4x4_x9_c(const pixel *src, int ss, const pixel *rec, int rs,
                          int ht, int hl, int htl, int htr, int costs[9])
{
    for (int mode = 0; mode < 9; mode++) {
        pixel pred[16];
        y264_intra4x4_c(pred, rec, rs, mode, ht, hl, htl, htr);
        costs[mode] = satd4x4_core(src, ss, pred, 4);
    }
}

/* I16x16 V/H/DC costs; the reference is the loop it replaces. */
static void intra_satd_x3_16_c(const pixel *src, int ss, const pixel *top,
                               const pixel *left, int dc, int costs[3])
{
    pixel pred[256];
    for (int i = 0; i < 256; i++) pred[i] = top[i & 15];
    costs[0] = satd_c_16x16(src, ss, pred, 16);
    for (int i = 0; i < 256; i++) pred[i] = left[i >> 4];
    costs[1] = satd_c_16x16(src, ss, pred, 16);
    for (int i = 0; i < 256; i++) pred[i] = (pixel)dc;
    costs[2] = satd_c_16x16(src, ss, pred, 16);
}

void y264_pixel_init_c(y264_pixel_fn_t *pf)
{
    pf->sad[Y264_PU_16x16] = sad_c_16x16;
    pf->sad[Y264_PU_16x8]  = sad_c_16x8;
    pf->sad[Y264_PU_8x16]  = sad_c_8x16;
    pf->sad[Y264_PU_8x8]   = sad_c_8x8;
    pf->sad[Y264_PU_8x4]   = sad_c_8x4;
    pf->sad[Y264_PU_4x8]   = sad_c_4x8;
    pf->sad[Y264_PU_4x4]   = sad_c_4x4;
    pf->sad_x4[Y264_PU_16x16] = sad_x4_c_16x16;
    pf->sad_x4[Y264_PU_16x8]  = sad_x4_c_16x8;
    pf->sad_x4[Y264_PU_8x16]  = sad_x4_c_8x16;
    pf->sad_x4[Y264_PU_8x8]   = sad_x4_c_8x8;
    pf->sad_x4[Y264_PU_8x4]   = sad_x4_c_8x4;
    pf->sad_x4[Y264_PU_4x8]   = sad_x4_c_4x8;
    pf->sad_x4[Y264_PU_4x4]   = sad_x4_c_4x4;
    pf->satd4x4 = satd_c_4x4;
    pf->satd8x8 = satd_c_8x8;
    pf->satd_x4_8x8 = satd_x4_c_8x8;
    pf->satd16x16 = satd_c_16x16;
    pf->sa8d8x8 = sa8d_c_8x8;
    pf->sa8d16x16 = sa8d_c_16x16;
    pf->hadamard_ac8x8 = hadamard_ac_c_8x8;
    pf->texture_ac4_16x16 = texture_ac4_c_16x16;
    pf->texture_ac48_16x16 = texture_ac48_c_16x16;
    pf->var16x16 = var_c_16x16;
    pf->intra4x4_x9 = intra4x4_x9_c;
    pf->intra_satd_x3_16 = intra_satd_x3_16_c;
}

/* NEON kernels live in pixel_neon.c and are declared here for the dispatcher. */
#if defined(__aarch64__) && Y264_BIT_DEPTH == 8
int y264_sad_16x16_neon(const pixel *, int, const pixel *, int);
int y264_sad_16x8_neon(const pixel *, int, const pixel *, int);
int y264_sad_8x16_neon(const pixel *, int, const pixel *, int);
int y264_sad_8x8_neon(const pixel *, int, const pixel *, int);
int y264_sad_16x16_neon_dotprod(const pixel *, int, const pixel *, int);
int y264_sad_8x16_neon_dotprod(const pixel *, int, const pixel *, int);
void y264_sad_x4_16x16_neon(const pixel *, int, const pixel *, const pixel *,
                            const pixel *, const pixel *, int, int[4]);
void y264_sad_x4_16x8_neon(const pixel *, int, const pixel *, const pixel *,
                           const pixel *, const pixel *, int, int[4]);
void y264_sad_x4_8x16_neon(const pixel *, int, const pixel *, const pixel *,
                           const pixel *, const pixel *, int, int[4]);
void y264_sad_x4_8x8_neon(const pixel *, int, const pixel *, const pixel *,
                          const pixel *, const pixel *, int, int[4]);
void y264_sad_x4_8x4_neon(const pixel *, int, const pixel *, const pixel *,
                          const pixel *, const pixel *, int, int[4]);
int y264_satd_4x4_neon(const pixel *, int, const pixel *, int);
int y264_satd_8x8_neon(const pixel *, int, const pixel *, int);
void y264_satd_x4_8x8_neon(const pixel *, int, const pixel *, const pixel *,
                           const pixel *, const pixel *, int, int[4]);
int y264_satd_16x16_neon_ded(const pixel *, int, const pixel *, int);
int y264_sa8d_8x8_neon(const pixel *, int, const pixel *, int);
int y264_sa8d_16x16_neon(const pixel *, int, const pixel *, int);
long y264_hadamard_ac_8x8_neon(const pixel *, int);
long y264_texture_ac4_16x16_neon(const pixel *, int);
void y264_texture_ac48_16x16_neon(const pixel *, int, long[2]);
void y264_var_16x16_neon(const pixel *, int, uint32_t[2]);
void y264_var_16x16_neon_dotprod(const pixel *, int, uint32_t[2]);
void y264_intra4x4_x9_neon(const pixel *, int, const pixel *, int,
                           int, int, int, int, int[9]);
void y264_intra_satd_x3_16x16_neon(const pixel *, int, const pixel *,
                                   const pixel *, int, int[3]);
#endif

void y264_pixel_init(uint32_t cpu, y264_pixel_fn_t *pf)
{
    y264_pixel_init_c(pf);

#if defined(__aarch64__) && Y264_BIT_DEPTH == 8
    if (cpu & Y264_CPU_NEON) {
        pf->sad[Y264_PU_16x16] = y264_sad_16x16_neon;
        pf->sad[Y264_PU_16x8]  = y264_sad_16x8_neon;
        pf->sad[Y264_PU_8x16]  = y264_sad_8x16_neon;
        pf->sad[Y264_PU_8x8]   = y264_sad_8x8_neon;
        pf->sad_x4[Y264_PU_16x16] = y264_sad_x4_16x16_neon;
        pf->sad_x4[Y264_PU_16x8]  = y264_sad_x4_16x8_neon;
        pf->sad_x4[Y264_PU_8x16]  = y264_sad_x4_8x16_neon;
        pf->sad_x4[Y264_PU_8x8]   = y264_sad_x4_8x8_neon;
        pf->sad_x4[Y264_PU_8x4]   = y264_sad_x4_8x4_neon;
        pf->satd4x4            = y264_satd_4x4_neon;
        pf->satd8x8            = y264_satd_8x8_neon;
        pf->satd_x4_8x8        = y264_satd_x4_8x8_neon;
        pf->satd16x16          = y264_satd_16x16_neon_ded;
        pf->sa8d8x8            = y264_sa8d_8x8_neon;
        pf->sa8d16x16          = y264_sa8d_16x16_neon;
        pf->hadamard_ac8x8     = y264_hadamard_ac_8x8_neon;
        pf->texture_ac4_16x16  = y264_texture_ac4_16x16_neon;
        pf->texture_ac48_16x16 = y264_texture_ac48_16x16_neon;
        pf->var16x16           = y264_var_16x16_neon;
        pf->intra4x4_x9        = y264_intra4x4_x9_neon;
        pf->intra_satd_x3_16   = y264_intra_satd_x3_16x16_neon;
        if (cpu & Y264_CPU_DOTPROD) {
            /* Only the tall blocks win from UDOT (longer accumulate chain the
 * plain-NEON uabal replaces); 16x8/8x8 measure neutral in checkasm
 * so they stay on plain NEON. */
            pf->sad[Y264_PU_16x16] = y264_sad_16x16_neon_dotprod;
            pf->sad[Y264_PU_8x16]  = y264_sad_8x16_neon_dotprod;
            pf->var16x16           = y264_var_16x16_neon_dotprod;
        }
    }
#else
    (void)cpu;
#endif
}

y264_pixel_fn_t y264_dsp;

void y264_dsp_init(void)
{
    static int done = 0;
    if (done)
        return;
    uint32_t cpu = y264_cpu_detect();
    if (y264_asm_off_ & Y264_ASM_PIXEL)   /* ablation hook, cpu.h */
        cpu = 0;
    y264_pixel_init(cpu, &y264_dsp);
    done = 1;
}
