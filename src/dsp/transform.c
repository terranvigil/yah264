/*
 * transform.c - H.264 integer transforms and quantization
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdlib.h>
#include <pthread.h>

#include "transform.h"
#include "../common/ledger.h"
#include "../common/cpu.h"

#if defined(__aarch64__) && Y264_BIT_DEPTH == 8
void y264_quant_4x4_neon(const dctcoef coef[16], dctcoef lev[16], int qp, int intra,
                         const int32_t mfrow[16]);
void y264_quant_4x4_fneon(const dctcoef coef[16], dctcoef lev[16], int qp, int f,
                          const int32_t mfrow[16]);
void y264_quant_8x8_fneon(const dctcoef coef[64], dctcoef lev[64], int qp, int f,
                          const int32_t mfrow[64]);
void y264_dequant_4x4_neon(const dctcoef lev[16], dctcoef coef[16], int qp,
                           const int32_t lsrow[16]);
void y264_fdct4x4_neon(const dctcoef diff[16], dctcoef coef[16]);
void y264_idct4x4_neon(const dctcoef coef[16], dctcoef res[16]);
void y264_fdct8x8_neon(const dctcoef diff[64], dctcoef coef[64]);
void y264_idct8x8_neon(const dctcoef coef[64], dctcoef res[64]);
void y264_sub4x4_dct_neon(dctcoef coef[16], const pixel *src, int ss,
                          const pixel *pred, int ps);
void y264_add4x4_idct_neon(pixel *dst, int ds, const pixel *pred, int ps,
                           const dctcoef coef[16]);
void y264_sub8x8_dct8_neon(dctcoef coef[64], const pixel *src, int ss,
                           const pixel *pred, int ps);
void y264_sub_dct4_blocks_neon(dctcoef (*coef)[16], int nbw, int nbh,
                               const pixel *src, int ss,
                               const pixel *pred, int ps);
void y264_add8x8_idct8_neon(pixel *dst, int ds, const pixel *pred, int ps,
                            const dctcoef coef[64]);
/* No local cache: y264_cpu_detect already caches under pthread_once, and a
 * second lazy static here just reintroduces the first-use race. */
void y264_zigzag_abs_8x8_neon(int out[64], const dctcoef in[64]);
void y264_scan_mask_8x8_neon(const dctcoef lev[64], uint64_t *omsk, int *obig);
void y264_zigzag_scan_4x4_neon(dctcoef out[16], const dctcoef in[16],
                               uint32_t *omsk, int *obig);
static int dct_have_neon(void)  { return y264_asm_on(Y264_ASM_DCT); }
static int scan_have_neon(void) { return y264_asm_on(Y264_ASM_SCAN); }
static int qnt_have_neon(void)  { return y264_asm_on(Y264_ASM_QUANT); }
#endif

/* H.264 default (JVT) scaling matrices, Table 7-3/7-4, in zig-zag scan order.
 * de-zig-zagged to raster in y264_cqm_jvt below. */
static const uint8_t JVT4_INTRA[16] = {
    6, 13, 13, 20, 20, 20, 28, 28, 28, 28, 32, 32, 32, 37, 37, 42
};
static const uint8_t JVT4_INTER[16] = {
    10, 14, 14, 20, 20, 20, 24, 24, 24, 24, 27, 27, 27, 30, 30, 34
};
static const uint8_t JVT8_INTRA[64] = {
     6, 10, 10, 13, 11, 13, 16, 16, 16, 16, 18, 18, 18, 18, 18, 23,
    23, 23, 23, 23, 23, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27,
    27, 27, 27, 27, 29, 29, 29, 29, 29, 29, 29, 31, 31, 31, 31, 33,
    33, 33, 36, 36, 36, 38, 38, 40, 40, 40, 42, 42, 42, 45, 45, 48
};
static const uint8_t JVT8_INTER[64] = {
     9, 13, 13, 15, 13, 15, 17, 17, 17, 17, 19, 19, 19, 19, 19, 21,
    21, 21, 21, 21, 21, 22, 22, 22, 22, 22, 22, 22, 24, 24, 24, 24,
    24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27, 28,
    28, 28, 30, 30, 30, 32, 32, 33, 33, 33, 35, 35, 35, 38, 38, 40
};
/* Coefficient zig-zag scan (frame), scan order -> raster index. One source of
 * truth: the scaling-list de-zig-zag below, the encoder's ZIGZAG/ZIGZAG8 and
 * the scan kernels all index these. */
const uint8_t y264_zigzag4[16] = { 0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15 };
const uint8_t y264_zigzag8[64] = {
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};
#define CQM_SCAN4 y264_zigzag4
#define CQM_SCAN8 y264_zigzag8

void y264_cqm_jvt(y264_cqm_t *m)
{
    for (int k = 0; k < 16; k++) {
        m->w4[0][CQM_SCAN4[k]] = JVT4_INTRA[k];
        m->w4[1][CQM_SCAN4[k]] = JVT4_INTER[k];
    }
    for (int k = 0; k < 64; k++) {
        m->w8[0][CQM_SCAN8[k]] = JVT8_INTRA[k];
        m->w8[1][CQM_SCAN8[k]] = JVT8_INTER[k];
    }
}

/* Forward quant multipliers MF[qp%6][category], categories:
 * 0: positions (0,0),(0,2),(2,0),(2,2)
 * 1: positions (1,1),(1,3),(3,1),(3,3)
 * 2: all other positions
 */
static const int MF[6][3] = {
    { 13107, 5243, 8066 },
    { 11916, 4660, 7490 },
    { 10082, 4194, 6554 },
    {  9362, 3647, 5825 },
    {  8192, 3355, 5243 },
    {  7282, 2893, 4559 },
};

/* Inverse-scale normAdjust V[qp%6][category], same categories as MF. */
static const int V[6][3] = {
    { 10, 16, 13 },
    { 11, 18, 14 },
    { 13, 20, 16 },
    { 14, 23, 18 },
    { 16, 25, 20 },
    { 18, 29, 23 },
};

/* Category of raster position idx for the tables above -- a pure function of idx,
 * precomputed (x264 does the same) so the hot per-coefficient quant/dequant loops
 * index a table instead of re-deriving the branch chain. Generated from the
 * even/odd-parity logic; verified byte-identical. */
static const uint8_t CAT4[16] = {0,2,0,2,2,1,2,1,0,2,0,2,2,1,2,1};
static inline int cat_of(int idx) { return CAT4[idx]; }

/* Forward-quant multiplier for raster position idx at qp (flat scaling), for the
 * encoder's stricter round-to-nearest skip probe. */
int y264_mf4_at(int idx, int qp) { return MF[qp % 6][cat_of(idx)]; }

/* Trellis-RDOQ inverse-quant multiplier : the direct inverse
 * of the forward quant, in fdct-coefficient units, so
 * unquant_abs = (mf * level + 128) >> 8
 * reconstructs the pre-quant coefficient magnitude for a per-coefficient
 * (transform-domain) distortion. w = per-position CQM weight (NULL = flat). */
long y264_unquant4_mf(int idx, int qp, const uint8_t *w)
{
    int mf = MF[qp % 6][cat_of(idx)];
    if (w) mf = (mf * 16 + (w[idx] >> 1)) / w[idx];
    return (long)((1LL << (qp / 6 + 23)) / mf);
}

/* Squared inverse basis norm per raster position (transform-domain distortion
 * weight): pixel SSD == sum_pos coef_err^2 / basis_norm2[pos]. As an integer
 * score in (pixel^2 * 256 * 25) units this is coef_err^2 * y264_dct4_w2(idx).
 * cat0 (even,even) norm2 16 -> 400; cat1 (odd,odd) 100 -> 64; cat2 40 -> 160. */
int y264_dct4_w2(int idx)
{
    static const int W2X[3] = { 400, 64, 160 };
    return W2X[cat_of(idx)];
}

/* Portable reference (checkasm baseline). The NEON kernel is exact for
 * |input| <= 255 (pixel diffs or reconstructed pixels -- every call site). */
void y264_fdct4x4_c(const dctcoef diff[16], dctcoef coef[16])
{
    int tmp[16];
    for (int i = 0; i < 4; i++) {
        const dctcoef *d = diff + i * 4;
        int z0 = d[0] + d[3];
        int z3 = d[0] - d[3];
        int z1 = d[1] + d[2];
        int z2 = d[1] - d[2];
        tmp[i * 4 + 0] = z0 + z1;
        tmp[i * 4 + 1] = 2 * z3 + z2;
        tmp[i * 4 + 2] = z0 - z1;
        tmp[i * 4 + 3] = z3 - 2 * z2;
    }
    for (int j = 0; j < 4; j++) {
        int z0 = tmp[0 * 4 + j] + tmp[3 * 4 + j];
        int z3 = tmp[0 * 4 + j] - tmp[3 * 4 + j];
        int z1 = tmp[1 * 4 + j] + tmp[2 * 4 + j];
        int z2 = tmp[1 * 4 + j] - tmp[2 * 4 + j];
        coef[0 * 4 + j] = (dctcoef)(z0 + z1);
        coef[1 * 4 + j] = (dctcoef)(2 * z3 + z2);
        coef[2 * 4 + j] = (dctcoef)(z0 - z1);
        coef[3 * 4 + j] = (dctcoef)(z3 - 2 * z2);
    }
}

void y264_idct4x4_c(const dctcoef coef[16], dctcoef res[16])
{
    int tmp[16];
    for (int i = 0; i < 4; i++) {
        const dctcoef *c = coef + i * 4;
        int i0 = c[0] + c[2];
        int i1 = c[0] - c[2];
        int i2 = (c[1] >> 1) - c[3];
        int i3 = c[1] + (c[3] >> 1);
        tmp[i * 4 + 0] = i0 + i3;
        tmp[i * 4 + 1] = i1 + i2;
        tmp[i * 4 + 2] = i1 - i2;
        tmp[i * 4 + 3] = i0 - i3;
    }
    for (int j = 0; j < 4; j++) {
        int i0 = tmp[0 * 4 + j] + tmp[2 * 4 + j];
        int i1 = tmp[0 * 4 + j] - tmp[2 * 4 + j];
        int i2 = (tmp[1 * 4 + j] >> 1) - tmp[3 * 4 + j];
        int i3 = tmp[1 * 4 + j] + (tmp[3 * 4 + j] >> 1);
        res[0 * 4 + j] = (dctcoef)((i0 + i3 + 32) >> 6);
        res[1 * 4 + j] = (dctcoef)((i1 + i2 + 32) >> 6);
        res[2 * 4 + j] = (dctcoef)((i1 - i2 + 32) >> 6);
        res[3 * 4 + j] = (dctcoef)((i0 - i3 + 32) >> 6);
    }
}

void y264_fdct4x4(const dctcoef diff[16], dctcoef coef[16])
{
    NLED(dct4_blk, 1);
#if defined(__aarch64__) && Y264_BIT_DEPTH == 8
    if (dct_have_neon()) { y264_fdct4x4_neon(diff, coef); return; }
#endif
    y264_fdct4x4_c(diff, coef);
}

void y264_idct4x4(const dctcoef coef[16], dctcoef res[16])
{
    NLED(idct4_blk, 1);
#if defined(__aarch64__) && Y264_BIT_DEPTH == 8
    if (dct_have_neon()) { y264_idct4x4_neon(coef, res); return; }
#endif
    y264_idct4x4_c(coef, res);
}

/* ---- Fused pixel-domain 4x4/8x8 transforms --------------------------------
 * References: exactly the unfused sequence the call sites used to run (same
 * int16 diff intermediate, same int16 truncation of the idct output, same
 * clip), so a fused call site is byte-identical to the old code on every
 * input. clip1p mirrors the encoder's clip8. */
static inline int clip1p(int v) { return v < 0 ? 0 : (v > PIXEL_MAX ? PIXEL_MAX : v); }

void y264_sub4x4_dct_c(dctcoef coef[16], const pixel *src, int ss,
                       const pixel *pred, int ps)
{
    dctcoef diff[16];
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            diff[y * 4 + x] = (dctcoef)(src[y * ss + x] - pred[y * ps + x]);
    y264_fdct4x4_c(diff, coef);
}

void y264_add4x4_idct_c(pixel *dst, int ds, const pixel *pred, int ps,
                        const dctcoef coef[16])
{
    dctcoef res[16];
    y264_idct4x4_c(coef, res);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            dst[y * ds + x] = (pixel)clip1p(pred[y * ps + x] + res[y * 4 + x]);
}

void y264_sub8x8_dct8_c(dctcoef coef[64], const pixel *src, int ss,
                        const pixel *pred, int ps)
{
    dctcoef diff[64];
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            diff[y * 8 + x] = (dctcoef)(src[y * ss + x] - pred[y * ps + x]);
    y264_fdct8x8_c(diff, coef);
}

void y264_add8x8_idct8_c(pixel *dst, int ds, const pixel *pred, int ps,
                         const dctcoef coef[64])
{
    dctcoef res[64];
    y264_idct8x8_c(coef, res);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            dst[y * ds + x] = (pixel)clip1p(pred[y * ps + x] + res[y * 8 + x]);
}

void y264_sub4x4_dct(dctcoef coef[16], const pixel *src, int ss,
                     const pixel *pred, int ps)
{
    NLED(dct4_blk, 1);
#if defined(__aarch64__) && Y264_BIT_DEPTH == 8
    if (dct_have_neon()) { y264_sub4x4_dct_neon(coef, src, ss, pred, ps); return; }
#endif
    y264_sub4x4_dct_c(coef, src, ss, pred, ps);
}

void y264_sub_dct4_blocks_c(dctcoef (*coef)[16], int nbw, int nbh,
                            const pixel *src, int ss, const pixel *pred, int ps)
{
    for (int by = 0; by < nbh; by++)
        for (int bx = 0; bx < nbw; bx++)
            y264_sub4x4_dct_c(coef[by * nbw + bx], src + (by * 4) * ss + bx * 4, ss,
                              pred + (by * 4) * ps + bx * 4, ps);
}

void y264_sub_dct4_blocks(dctcoef (*coef)[16], int nbw, int nbh,
                          const pixel *src, int ss, const pixel *pred, int ps)
{
    NLED(dct4_blk, nbw * nbh);
#if defined(__aarch64__) && Y264_BIT_DEPTH == 8
    if (dct_have_neon()) {
        y264_sub_dct4_blocks_neon(coef, nbw, nbh, src, ss, pred, ps);
        return;
    }
#endif
    y264_sub_dct4_blocks_c(coef, nbw, nbh, src, ss, pred, ps);
}

void y264_add4x4_idct(pixel *dst, int ds, const pixel *pred, int ps,
                      const dctcoef coef[16])
{
    NLED(idct4_blk, 1);
#if defined(__aarch64__) && Y264_BIT_DEPTH == 8
    if (dct_have_neon()) { y264_add4x4_idct_neon(dst, ds, pred, ps, coef); return; }
#endif
    y264_add4x4_idct_c(dst, ds, pred, ps, coef);
}

void y264_sub8x8_dct8(dctcoef coef[64], const pixel *src, int ss,
                      const pixel *pred, int ps)
{
    NLED(dct8_blk, 1);
#if defined(__aarch64__) && Y264_BIT_DEPTH == 8
    if (dct_have_neon()) { y264_sub8x8_dct8_neon(coef, src, ss, pred, ps); return; }
#endif
    y264_sub8x8_dct8_c(coef, src, ss, pred, ps);
}

void y264_add8x8_idct8(pixel *dst, int ds, const pixel *pred, int ps,
                       const dctcoef coef[64])
{
    NLED(idct8_blk, 1);
#if defined(__aarch64__) && Y264_BIT_DEPTH == 8
    if (dct_have_neon()) { y264_add8x8_idct8_neon(dst, ds, pred, ps, coef); return; }
#endif
    y264_add8x8_idct8_c(dst, ds, pred, ps, coef);
}

/* 8x8 forward transform, one dimension (ITU-T H.264, the integer 8x8 DCT that
 * pairs with the normative inverse below). Operates on 8 ints in place. */
static void fdct8_1d(const int *s, int *d)
{
    int a0 = s[0] + s[7], a1 = s[1] + s[6], a2 = s[2] + s[5], a3 = s[3] + s[4];
    int a4 = s[0] - s[7], a5 = s[1] - s[6], a6 = s[2] - s[5], a7 = s[3] - s[4];

    int b0 = a0 + a3, b1 = a1 + a2, b2 = a0 - a3, b3 = a1 - a2;
    int b4 = a5 + a6 + ((a4 >> 1) + a4);
    int b5 = a4 - a7 - ((a6 >> 1) + a6);
    int b6 = a4 + a7 - ((a5 >> 1) + a5);
    int b7 = a5 - a6 + ((a7 >> 1) + a7);

    d[0] = b0 + b1;
    d[2] = b2 + (b3 >> 1);
    d[4] = b0 - b1;
    d[6] = (b2 >> 1) - b3;
    d[1] = b4 + (b7 >> 2);
    d[3] = b5 + (b6 >> 2);
    d[5] = b6 - (b5 >> 2);
    d[7] = (b4 >> 2) - b7;
}

/* 8x8 inverse transform, one dimension (normative, 8.5.13.2). */
static void idct8_1d(const int *m, int *r)
{
    int a0 = m[0] + m[4];
    int a4 = m[0] - m[4];
    int a2 = (m[2] >> 1) - m[6];
    int a6 = m[2] + (m[6] >> 1);
    int a1 = -m[3] + m[5] - m[7] - (m[7] >> 1);
    int a3 =  m[1] + m[7] - m[3] - (m[3] >> 1);
    int a5 = -m[1] + m[7] + m[5] + (m[5] >> 1);
    int a7 =  m[3] + m[5] + m[1] + (m[1] >> 1);

    int b0 = a0 + a6, b6 = a0 - a6, b2 = a4 + a2, b4 = a4 - a2;
    int b1 = a1 + (a7 >> 2), b7 = a7 - (a1 >> 2);
    int b3 = a3 + (a5 >> 2), b5 = (a3 >> 2) - a5;

    r[0] = b0 + b7;
    r[7] = b0 - b7;
    r[1] = b2 + b5;
    r[6] = b2 - b5;
    r[2] = b4 + b3;
    r[5] = b4 - b3;
    r[3] = b6 + b1;
    r[4] = b6 - b1;
}

void y264_fdct8x8_c(const dctcoef diff[64], dctcoef coef[64])
{
    int tmp[64];
    for (int i = 0; i < 8; i++) {
        int in[8], out[8];
        for (int j = 0; j < 8; j++) in[j] = diff[i * 8 + j];
        fdct8_1d(in, out);
        for (int j = 0; j < 8; j++) tmp[i * 8 + j] = out[j];
    }
    for (int j = 0; j < 8; j++) {
        int in[8], out[8];
        for (int i = 0; i < 8; i++) in[i] = tmp[i * 8 + j];
        fdct8_1d(in, out);
        for (int i = 0; i < 8; i++) coef[i * 8 + j] = (dctcoef)out[i];
    }
}

void y264_idct8x8_c(const dctcoef coef[64], dctcoef res[64])
{
    int tmp[64];
    for (int i = 0; i < 8; i++) {
        int in[8], out[8];
        for (int j = 0; j < 8; j++) in[j] = coef[i * 8 + j];
        idct8_1d(in, out);
        for (int j = 0; j < 8; j++) tmp[i * 8 + j] = out[j];
    }
    for (int j = 0; j < 8; j++) {
        int in[8], out[8];
        for (int i = 0; i < 8; i++) in[i] = tmp[i * 8 + j];
        idct8_1d(in, out);
        for (int i = 0; i < 8; i++)
            res[i * 8 + j] = (dctcoef)((out[i] + 32) >> 6);
    }
}

void y264_fdct8x8(const dctcoef diff[64], dctcoef coef[64])
{
    NLED(dct8_blk, 1);
#if defined(__aarch64__) && Y264_BIT_DEPTH == 8
    if (dct_have_neon()) { y264_fdct8x8_neon(diff, coef); return; }
#endif
    y264_fdct8x8_c(diff, coef);
}

void y264_idct8x8(const dctcoef coef[64], dctcoef res[64])
{
    NLED(idct8_blk, 1);
#if defined(__aarch64__) && Y264_BIT_DEPTH == 8
    if (dct_have_neon()) { y264_idct8x8_neon(coef, res); return; }
#endif
    y264_idct8x8_c(coef, res);
}

/* 8x8 forward-quant multipliers and inverse normAdjust, indexed by qp%6 and the
 * six position categories A..F (8.5.9). Category of a raster position: */
static const int M8[6][6] = {
    { 13107, 12222, 16777, 11428, 20972, 15481 },
    { 11916, 11058, 14980, 10826, 19174, 14290 },
    { 10082,  9675, 12710,  8943, 15978, 11985 },
    {  9362,  8931, 11984,  8228, 14913, 11259 },
    {  8192,  7740, 10486,  7346, 13159,  9777 },
    {  7282,  6830,  9118,  6428, 11570,  8640 },
};
static const int V8[6][6] = {
    { 20, 19, 25, 18, 32, 24 },
    { 22, 21, 28, 19, 35, 26 },
    { 26, 24, 33, 23, 42, 31 },
    { 28, 26, 35, 25, 45, 33 },
    { 32, 30, 40, 28, 51, 38 },
    { 36, 34, 46, 32, 58, 43 },
};

/* 8x8 position->category, precomputed from the A..F region logic (see cat_of);
 * verified byte-identical. Table lookup replaces the branch chain in the hot
 * 8x8 quant/dequant loops. */
static const uint8_t CAT8[64] = {
    0,1,2,1,0,1,2,1,
    1,3,5,3,1,3,5,3,
    2,5,4,5,2,5,4,5,
    1,3,5,3,1,3,5,3,
    0,1,2,1,0,1,2,1,
    1,3,5,3,1,3,5,3,
    2,5,4,5,2,5,4,5,
    1,3,5,3,1,3,5,3
};
static inline int cat8_of(int idx) { return CAT8[idx]; }

/* 8x8 counterparts (see y264_unquant4_mf / y264_dct4_w2). The 8x8 forward quant
 * uses qbits = 16 + qp/6 (one more than 4x4), so the inverse gains a bit; the
 * distortion weights are the reference's 8x8 inverse-squared basis norms (8-bit
 * fixed point) rescaled to the same (pixel^2 * 256 * 25) score units, empirically ratio-1.0 against the
 * exact 8x8 dequant+idct SSD. */
long y264_unquant8_mf(int idx, int qp, const uint8_t *w)
{
    int mf = M8[qp % 6][cat8_of(idx)];
    if (w) mf = (mf * 16 + (w[idx] >> 1)) / w[idx];
    return (long)((1LL << (qp / 6 + 24)) / mf);
}

int y264_dct8_w2(int idx)
{
    /* The reference's squared 8x8 basis weights (8-bit fixed point) by category
     * A..F, scaled by 25/64 into score units. */
    static const int W8FIX[6] = { 256, 227, 410, 201, 656, 363 };
    return (W8FIX[cat8_of(idx)] * 25 + 32) / 64;
}

/* Per-QP rows of the trellis prep values, flat CQM. The rdoq prep loops called
 * y264_unquant*_mf / y264_dct*_w2 per coefficient -- an extern call plus a
 * 64-bit divide each -- for values that depend only on (qp, raster position).
 * Rows hold the identical values (built BY those functions), so a prep loop is
 * two indexed loads. Scaling-matrix streams keep the per-call path (w folds
 * into the divide). Warmed at open (y264_transform_warm_statics); the acquire
 * load + pthread_once fallback keeps any pre-warm caller race-free. */
#define UROW_QPS (52 + Y264_QP_BD_OFFSET)
static int  mf4_rows[UROW_QPS][16];
static int  mf8_rows[6][64];            /* flat-CQM 8x8 mf, by qp%6 */
static int  dq4_rows[6][16];            /* flat-CQM 4x4 dequant scale, by qp%6 */
static int  dq8_rows[6][64];            /* flat-CQM 8x8 dequant scale, by qp%6 */
static long u4_rows[UROW_QPS][16];
static long u8_rows[UROW_QPS][64];
static int  w24_row[16];
static int  w28_row[64];
/* The same four rows permuted into zig-zag scan order. The RDOQ trellis reads
 * every one of its per-position operands in scan order, and this permutation
 * is a compile-time constant, so the scan-order rows are built once at open
 * and the trellis prep reads them straight instead of gathering through
 * ZIGZAG per coefficient. */
static long u4z_rows[UROW_QPS][16];
static long u8z_rows[UROW_QPS][64];
static int  w24z_row[16];
static int  w28z_row[64];
static pthread_once_t urows_once = PTHREAD_ONCE_INIT;
static int urows_ready;
static void urows_build(void)
{
    for (int qp = 0; qp < UROW_QPS; qp++) {
        for (int i = 0; i < 16; i++) mf4_rows[qp][i] = y264_mf4_at(i, qp);
        for (int i = 0; i < 16; i++) u4_rows[qp][i] = y264_unquant4_mf(i, qp, NULL);
        for (int i = 0; i < 64; i++) u8_rows[qp][i] = y264_unquant8_mf(i, qp, NULL);
    }
    for (int m = 0; m < 6; m++) {
        for (int i = 0; i < 64; i++) mf8_rows[m][i] = M8[m][cat8_of(i)];
        for (int i = 0; i < 16; i++) dq4_rows[m][i] = 16 * V[m][cat_of(i)];
        for (int i = 0; i < 64; i++) dq8_rows[m][i] = 16 * V8[m][cat8_of(i)];
    }
    for (int i = 0; i < 16; i++) w24_row[i] = y264_dct4_w2(i);
    for (int i = 0; i < 64; i++) w28_row[i] = y264_dct8_w2(i);
    for (int qp = 0; qp < UROW_QPS; qp++) {
        for (int k = 0; k < 16; k++) u4z_rows[qp][k] = u4_rows[qp][y264_zigzag4[k]];
        for (int k = 0; k < 64; k++) u8z_rows[qp][k] = u8_rows[qp][y264_zigzag8[k]];
    }
    for (int k = 0; k < 16; k++) w24z_row[k] = w24_row[y264_zigzag4[k]];
    for (int k = 0; k < 64; k++) w28z_row[k] = w28_row[y264_zigzag8[k]];
    __atomic_store_n(&urows_ready, 1, __ATOMIC_RELEASE);
}
static inline void urows_ensure(void)
{
    if (!__atomic_load_n(&urows_ready, __ATOMIC_ACQUIRE))
        pthread_once(&urows_once, urows_build);
}
const int *y264_mf4_row(int qp)
{
    urows_ensure();
    return mf4_rows[qp < 0 ? 0 : (qp >= UROW_QPS ? UROW_QPS - 1 : qp)];
}
const long *y264_unquant4_row(int qp)
{
    urows_ensure();
    return u4_rows[qp < 0 ? 0 : (qp >= UROW_QPS ? UROW_QPS - 1 : qp)];
}
const long *y264_unquant8_row(int qp)
{
    urows_ensure();
    return u8_rows[qp < 0 ? 0 : (qp >= UROW_QPS ? UROW_QPS - 1 : qp)];
}
const int *y264_dct4_w2_row(void) { urows_ensure(); return w24_row; }
const int *y264_dct8_w2_row(void) { urows_ensure(); return w28_row; }
const long *y264_unquant4_row_zz(int qp)
{
    urows_ensure();
    return u4z_rows[qp < 0 ? 0 : (qp >= UROW_QPS ? UROW_QPS - 1 : qp)];
}
const long *y264_unquant8_row_zz(int qp)
{
    urows_ensure();
    return u8z_rows[qp < 0 ? 0 : (qp >= UROW_QPS ? UROW_QPS - 1 : qp)];
}
const int *y264_dct4_w2_row_zz(void) { urows_ensure(); return w24z_row; }
const int *y264_dct8_w2_row_zz(void) { urows_ensure(); return w28z_row; }
static inline const int *mf8_row(int m) { urows_ensure(); return mf8_rows[m]; }
static inline const int *dq4_row(int m) { urows_ensure(); return dq4_rows[m]; }
static inline const int *dq8_row(int m) { urows_ensure(); return dq8_rows[m]; }
static inline const int *mf4_row_i(int qp) { return y264_mf4_row(qp); }

/* Flat-CQM forward quant, the C path for both block sizes. The multiplier is a
 * function of (qp%6, raster position) only, so it comes from a per-QP row: the
 * old form's M[m][cat_of(idx)] double indirection was what stopped the loop
 * vectorizing (the numbers are identical -- the rows are built from the same
 * expression). The scaling-matrix streams keep their per-coefficient divide. */
static void quant4_flat(const dctcoef coef[16], dctcoef lev[16],
                        const int *mfr, int f, int qbits)
{
    for (int idx = 0; idx < 16; idx++) {
        int c = coef[idx];
        int a = (c < 0) ? -c : c;
        int q = (int)(((int64_t)a * mfr[idx] + f) >> qbits);
        lev[idx] = (dctcoef)((c < 0) ? -q : q);
    }
}

static void quant8_flat(const dctcoef coef[64], dctcoef lev[64],
                        const int *mfr, int f, int qbits)
{
    for (int idx = 0; idx < 64; idx++) {
        int c = coef[idx];
        int a = (c < 0) ? -c : c;
        int q = (int)(((int64_t)a * mfr[idx] + f) >> qbits);
        lev[idx] = (dctcoef)((c < 0) ? -q : q);
    }
}

/* Experimental deadzone override, in 1/64-of-step units (x264 --deadzone
 * semantics after the 32-x inversion: rounding bias = dz/64; the JM default
 * this encoder ships is intra 64/3 = 21.33, inter 64/6 = 10.67; x264's default
 * quant bias is intra 21, inter 11 -- numerically the same). -1 = unset: the
 * exact legacy expression (byte-identical, NEON fast path allowed). */
static int dz64_of(int intra)
{
    static int vi = -2, vp = -2;
    if (vi == -2) {
        const char *e = getenv("Y264_DZ_INTRA"); int a = e ? atoi(e) : -1;
        e = getenv("Y264_DZ_INTER");             vp = e ? atoi(e) : -1;
        vi = a;
    }
    return intra ? vi : vp;
}

void y264_transform_warm_statics(void)
{
    (void)dz64_of(0); (void)dz64_of(1);
    urows_ensure();             /* trellis prep rows: built before threads spawn */
}

/* Forward 8x8 quant with an explicit rounding bias f64 (1/64-of-step units).
 * f64 = 32 is round-to-nearest -- the reference's 8x8 trellis seed. */
void y264_quant_8x8_f64(const dctcoef coef[64], dctcoef lev[64], int qp, int f64,
                        const uint8_t *w)
{
    NLED(q8_blk, 1);
    int qbits = 16 + qp / 6;
    int m = qp % 6;
    int f = (int)(((int64_t)f64 << qbits) >> 6);
#if defined(__aarch64__) && Y264_BIT_DEPTH == 8
    if (!w && qnt_have_neon()) { y264_quant_8x8_fneon(coef, lev, qp, f, mf8_row(m)); return; }
#endif
    if (!w) { quant8_flat(coef, lev, mf8_row(m), f, qbits); return; }
    for (int idx = 0; idx < 64; idx++) {
        /* A scaling matrix divides the dequant step by 16/w, so the forward
 * multiplier is scaled by 16/w (rounded). w == 16 reproduces mf. */
        int mf = (M8[m][cat8_of(idx)] * 16 + (w[idx] >> 1)) / w[idx];
        int c = coef[idx];
        int a = (c < 0) ? -c : c;
        int q = (int)(((int64_t)a * mf + f) >> qbits);
        lev[idx] = (dctcoef)((c < 0) ? -q : q);
    }
}

void y264_quant_8x8(const dctcoef coef[64], dctcoef lev[64], int qp, int intra,
                    const uint8_t *w)
{
    NLED(q8_blk, 1);
    int dz = dz64_of(intra);
    if (dz >= 0) { y264_quant_8x8_f64(coef, lev, qp, dz, w); return; }
    int qbits = 16 + qp / 6;
    int m = qp % 6;
    int f = (1 << qbits) / (intra ? 3 : 6);
#if defined(__aarch64__) && Y264_BIT_DEPTH == 8
    if (!w && qnt_have_neon()) { y264_quant_8x8_fneon(coef, lev, qp, f, mf8_row(m)); return; }
#endif
    if (!w) { quant8_flat(coef, lev, mf8_row(m), f, qbits); return; }
    for (int idx = 0; idx < 64; idx++) {
        /* A scaling matrix divides the dequant step by 16/w, so the forward
 * multiplier is scaled by 16/w (rounded). w == 16 reproduces mf. */
        int mf = (M8[m][cat8_of(idx)] * 16 + (w[idx] >> 1)) / w[idx];
        int c = coef[idx];
        int a = (c < 0) ? -c : c;
        int q = (int)(((int64_t)a * mf + f) >> qbits);
        lev[idx] = (dctcoef)((c < 0) ? -q : q);
    }
}

#if defined(__aarch64__) && Y264_BIT_DEPTH == 8
void y264_dequant_8x8_neon(const dctcoef lev[64], dctcoef coef[64], int qp,
                           const int32_t lsrow[64]);
#endif

void y264_dequant_8x8(const dctcoef lev[64], dctcoef coef[64], int qp,
                      const uint8_t *w)
{
    NLED(dq8_blk, 1);
    int m = qp % 6;
    int shift = qp / 6;
#if defined(__aarch64__) && Y264_BIT_DEPTH == 8
    if (!w && qnt_have_neon()) { y264_dequant_8x8_neon(lev, coef, qp, dq8_row(m)); return; }
#endif
    const int *V8m = V8[m];
    if (qp >= 36) {                 /* qp>=36 <=> shift>=6, so shift-6 >= 0 */
        int mul = 1 << (shift - 6);
        for (int idx = 0; idx < 64; idx++) {
            int ls = (w ? w[idx] : 16) * V8m[cat8_of(idx)];
            coef[idx] = (dctcoef)((lev[idx] * ls) * mul);
        }
    } else {
        int rnd = 1 << (5 - shift), sh = 6 - shift;
        for (int idx = 0; idx < 64; idx++) {
            int ls = (w ? w[idx] : 16) * V8m[cat8_of(idx)];
            coef[idx] = (dctcoef)((lev[idx] * ls + rnd) >> sh);
        }
    }
}

void y264_hadamard4x4(const dctcoef in[16], dctcoef out[16])
{
    NLED(dcxf_blk, 1);
    int tmp[16];
    for (int i = 0; i < 4; i++) {
        const dctcoef *d = in + i * 4;
        int a0 = d[0] + d[2];
        int a1 = d[1] + d[3];
        int a2 = d[0] - d[2];
        int a3 = d[1] - d[3];
        tmp[i * 4 + 0] = a0 + a1;
        tmp[i * 4 + 1] = a2 + a3;
        tmp[i * 4 + 2] = a2 - a3;
        tmp[i * 4 + 3] = a0 - a1;
    }
    for (int j = 0; j < 4; j++) {
        int a0 = tmp[0 * 4 + j] + tmp[2 * 4 + j];
        int a1 = tmp[1 * 4 + j] + tmp[3 * 4 + j];
        int a2 = tmp[0 * 4 + j] - tmp[2 * 4 + j];
        int a3 = tmp[1 * 4 + j] - tmp[3 * 4 + j];
        out[0 * 4 + j] = (dctcoef)(a0 + a1);
        out[1 * 4 + j] = (dctcoef)(a2 + a3);
        out[2 * 4 + j] = (dctcoef)(a2 - a3);
        out[3 * 4 + j] = (dctcoef)(a0 - a1);
    }
}

/* ---- zig-zag scan kernels (see transform.h) ---------------------------- */

/* 4x4 stays C on purpose: a TBL form of this measured 0.87x -- clang already
 * vectorizes sixteen gathers as well as one permute plus the widening pair. */
void y264_zigzag_abs_4x4(int out[16], const dctcoef in[16])
{
    for (int k = 0; k < 16; k++) {
        int v = in[y264_zigzag4[k]];
        out[k] = v < 0 ? -v : v;
    }
}

void y264_zigzag_abs_8x8_c(int out[64], const dctcoef in[64])
{
    for (int k = 0; k < 64; k++) {
        int v = in[y264_zigzag8[k]];
        out[k] = v < 0 ? -v : v;
    }
}

void y264_scan_mask_8x8_c(const dctcoef lev[64], uint64_t *omsk, int *obig)
{
    uint64_t msk = 0;
    int big = 0;
    for (int k = 0; k < 64; k++) {
        int v = lev[y264_zigzag8[k]];
        msk |= (uint64_t)(v != 0) << k;
        big |= (unsigned)(v + 1) > 2u;
    }
    *omsk = msk;
    *obig = big;
}

void y264_zigzag_scan_4x4_c(dctcoef out[16], const dctcoef in[16],
                            uint32_t *omsk, int *obig)
{
    uint32_t msk = 0;
    int big = 0;
    for (int k = 0; k < 16; k++) {
        dctcoef v = in[y264_zigzag4[k]];
        out[k] = v;
        msk |= (uint32_t)(v != 0) << k;
        big |= (unsigned)(v + 1) > 2u;
    }
    *omsk = msk;
    *obig = big;
}

void y264_zigzag_abs_8x8(int out[64], const dctcoef in[64])
{
#if defined(__aarch64__) && Y264_BIT_DEPTH == 8
    if (scan_have_neon()) { y264_zigzag_abs_8x8_neon(out, in); return; }
#endif
    y264_zigzag_abs_8x8_c(out, in);
}

void y264_scan_mask_8x8(const dctcoef lev[64], uint64_t *omsk, int *obig)
{
#if defined(__aarch64__) && Y264_BIT_DEPTH == 8
    if (scan_have_neon()) { y264_scan_mask_8x8_neon(lev, omsk, obig); return; }
#endif
    y264_scan_mask_8x8_c(lev, omsk, obig);
}

void y264_zigzag_scan_4x4(dctcoef out[16], const dctcoef in[16],
                          uint32_t *omsk, int *obig)
{
#if defined(__aarch64__) && Y264_BIT_DEPTH == 8
    if (scan_have_neon()) { y264_zigzag_scan_4x4_neon(out, in, omsk, obig); return; }
#endif
    y264_zigzag_scan_4x4_c(out, in, omsk, obig);
}

void y264_hadamard2x2(const dctcoef in[4], dctcoef out[4])
{
    NLED(dcxf_blk, 1);
    int a = in[0] + in[1];
    int b = in[2] + in[3];
    int c = in[0] - in[1];
    int d = in[2] - in[3];
    out[0] = (dctcoef)(a + b);
    out[1] = (dctcoef)(c + d);
    out[2] = (dctcoef)(a - b);
    out[3] = (dctcoef)(c - d);
}

/* 4:2:2 chroma DC transform (8.5.11.1): 2-point horizontal across the 2 columns,
 * then 4-point vertical down the 4 rows. Input/output are raster [row*2+col],
 * row 0..3, col 0..1. Self-inverse up to x8. */
void y264_chroma422_dc(const dctcoef in[8], dctcoef out[8])
{
    int t[4][2];
    for (int r = 0; r < 4; r++) {
        int a = in[r * 2 + 0], b = in[r * 2 + 1];
        t[r][0] = a + b;
        t[r][1] = a - b;
    }
    for (int j = 0; j < 2; j++) {
        int z0 = t[0][j] + t[2][j];
        int z1 = t[0][j] - t[2][j];
        int z2 = t[1][j] - t[3][j];
        int z3 = t[1][j] + t[3][j];
        out[0 * 2 + j] = (dctcoef)(z0 + z3);
        out[1 * 2 + j] = (dctcoef)(z1 + z2);
        out[2 * 2 + j] = (dctcoef)(z1 - z2);
        out[3 * 2 + j] = (dctcoef)(z0 - z3);
    }
}

/* 4:2:2 chroma DC inverse scaling (8.5.11.2). qP is the chroma qP; qP_DC = qP+3.
 * Net normalization is >>6 (the 4-point vertical adds a bit of gain over the
 * 4:2:0 2x2 case), split into a rounded-right-shift / left-shift pair. */
void y264_dequant_dc_chroma422(const dctcoef f[8], dctcoef out[8], int qp, int w0)
{
    NLED(dq4_blk, 1);
    int qp_dc = qp + 3;
    int m = qp_dc % 6;
    int shift = qp_dc / 6;
    int ls = w0 * V[m][0];
    for (int k = 0; k < 8; k++) {
        int d;
        if (qp_dc >= 36)
            d = (f[k] * ls) * (1 << (shift - 6));
        else
            d = (f[k] * ls + (1 << (5 - shift))) >> (6 - shift);
        out[k] = (dctcoef)d;
    }
}

/* Forward quant for 4:2:2 chroma DC. Non-normative (only the inverse above is
 * gated by recon-match); mirrors the 4:2:0 chroma-DC quant with qP_DC = qP+3 and
 * one extra shift for the larger (x8) transform gain. */
void y264_quant_dc_chroma422(const dctcoef f[8], dctcoef lev[8], int qp, int intra,
                             int w0)
{
    NLED(qdc_blk, 1);
    int qp_dc = qp + 3;
    int qbits = 15 + qp_dc / 6;
    int m = qp_dc % 6;
    int mf = (MF[m][0] * 16 + (w0 >> 1)) / w0;
    int fdz = (1 << qbits) / (intra ? 3 : 6);
    for (int k = 0; k < 8; k++) {
        int c = f[k];
        int a = (c < 0) ? -c : c;
        int q = (int)(((int64_t)a * mf + 4 * fdz) >> (qbits + 2));
        lev[k] = (dctcoef)((c < 0) ? -q : q);
    }
}

int y264_chroma_qp(int qp_luma, int chroma_qp_index_offset)
{
    static const int map[22] = {
        29, 30, 31, 32, 32, 33, 34, 34, 35, 35, 36,
        36, 37, 37, 37, 38, 38, 38, 39, 39, 39, 39
    };
    int qpi = qp_luma + chroma_qp_index_offset;
    /* Clip3(-QpBdOffsetC, 51, qPi). QpBdOffsetC = 6*(BD-8) = 0 at 8-bit. */
    if (qpi < -Y264_QP_BD_OFFSET) qpi = -Y264_QP_BD_OFFSET;
    if (qpi > 51) qpi = 51;
    return (qpi < 30) ? qpi : map[qpi - 30];
}

/* Forward 4x4 quant with an explicit rounding bias f64 (1/64-of-step units).
 * f64 = 32 is round-to-nearest -- the reference's 4x4 trellis seed. */
void y264_quant_4x4_f64(const dctcoef coef[16], dctcoef lev[16], int qp, int f64,
                        const uint8_t *w)
{
    NLED(q4_blk, 1);
    int qbits = 15 + qp / 6;
    int m = qp % 6;
    int f = (int)(((int64_t)f64 << qbits) >> 6);
#if defined(__aarch64__) && Y264_BIT_DEPTH == 8
    if (!w && qnt_have_neon()) { y264_quant_4x4_fneon(coef, lev, qp, f, mf4_row_i(qp)); return; }
#endif
    if (!w) { quant4_flat(coef, lev, mf4_row_i(qp), f, qbits); return; }
    for (int idx = 0; idx < 16; idx++) {
        int mf = (MF[m][cat_of(idx)] * 16 + (w[idx] >> 1)) / w[idx];
        int c = coef[idx];
        int a = (c < 0) ? -c : c;
        int q = (int)(((int64_t)a * mf + f) >> qbits);
        lev[idx] = (dctcoef)((c < 0) ? -q : q);
    }
}

void y264_quant_4x4(const dctcoef coef[16], dctcoef lev[16], int qp, int intra,
                    const uint8_t *w)
{
    NLED(q4_blk, 1);
    int dz = dz64_of(intra);
    if (dz >= 0) { y264_quant_4x4_f64(coef, lev, qp, dz, w); return; }
#if defined(__aarch64__) && Y264_BIT_DEPTH == 8
    if (!w && qnt_have_neon()) { y264_quant_4x4_neon(coef, lev, qp, intra, mf4_row_i(qp)); return; }
#endif
    int qbits = 15 + qp / 6;
    int m = qp % 6;
    int f = (1 << qbits) / (intra ? 3 : 6);
    if (!w) { quant4_flat(coef, lev, mf4_row_i(qp), f, qbits); return; }
    for (int idx = 0; idx < 16; idx++) {
        int mf = (MF[m][cat_of(idx)] * 16 + (w[idx] >> 1)) / w[idx];
        int c = coef[idx];
        int a = (c < 0) ? -c : c;
        int q = (int)(((int64_t)a * mf + f) >> qbits);
        lev[idx] = (dctcoef)((c < 0) ? -q : q);
    }
}

void y264_dequant_4x4(const dctcoef lev[16], dctcoef coef[16], int qp,
                      const uint8_t *w)
{
    NLED(dq4_blk, 1);
#if defined(__aarch64__) && Y264_BIT_DEPTH == 8
    if (!w && qnt_have_neon()) { y264_dequant_4x4_neon(lev, coef, qp, dq4_row(qp % 6)); return; }
#endif
    int m = qp % 6;
    int shift = qp / 6;
    const int *Vm = V[m];
    /* Hoist the loop-invariant shift branch (x264 splits the two cases into
 * separate tight loops). Multiply by (1<<..) rather than left-shift: the
 * operand can be negative, and a signed left shift of a negative value is
 * undefined behaviour in C. Byte-identical to the per-element form. */
    if (shift >= 4) {
        int mul = 1 << (shift - 4);
        for (int idx = 0; idx < 16; idx++) {
            int ls = (w ? w[idx] : 16) * Vm[cat_of(idx)];
            coef[idx] = (dctcoef)((lev[idx] * ls) * mul);
        }
    } else {
        int rnd = 1 << (3 - shift), sh = 4 - shift;
        for (int idx = 0; idx < 16; idx++) {
            int ls = (w ? w[idx] : 16) * Vm[cat_of(idx)];
            coef[idx] = (dctcoef)((lev[idx] * ls + rnd) >> sh);
        }
    }
}

void y264_quant_dc_luma(const dctcoef had[16], dctcoef lev[16], int qp, int intra,
                        int w0)
{
    NLED(qdc_blk, 1);
    int qbits = 15 + qp / 6;
    int m = qp % 6;
    int mf = (MF[m][0] * 16 + (w0 >> 1)) / w0;
    int f = (1 << qbits) / (intra ? 3 : 6);
    for (int idx = 0; idx < 16; idx++) {
        int c = had[idx];
        int a = (c < 0) ? -c : c;
        int q = (int)(((int64_t)a * mf + 2 * f) >> (qbits + 1));
        lev[idx] = (dctcoef)((c < 0) ? -q : q);
    }
}

void y264_dequant_dc_luma(const dctcoef lev[16], dctcoef out[16], int qp, int w0)
{
    NLED(dq4_blk, 1);
    int m = qp % 6;
    int shift = qp / 6;
    int ls = w0 * V[m][0];
    for (int idx = 0; idx < 16; idx++) {
        int l = lev[idx];
        int d;
        if (qp >= 36)
            d = (l * ls) * (1 << (shift - 6));
        else
            d = (l * ls + (1 << (5 - shift))) >> (6 - shift);
        out[idx] = (dctcoef)d;
    }
}

void y264_quant_dc_chroma(const dctcoef had[4], dctcoef lev[4], int qp, int intra,
                          int w0)
{
    NLED(qdc_blk, 1);
    int qbits = 15 + qp / 6;
    int m = qp % 6;
    int mf = (MF[m][0] * 16 + (w0 >> 1)) / w0;
    int f = (1 << qbits) / (intra ? 3 : 6);
    for (int idx = 0; idx < 4; idx++) {
        int c = had[idx];
        int a = (c < 0) ? -c : c;
        int q = (int)(((int64_t)a * mf + 2 * f) >> (qbits + 1));
        lev[idx] = (dctcoef)((c < 0) ? -q : q);
    }
}

void y264_dequant_dc_chroma(const dctcoef lev[4], dctcoef out[4], int qp, int w0)
{
    NLED(dq4_blk, 1);
    int m = qp % 6;
    int shift = qp / 6;
    int ls = w0 * V[m][0];
    for (int idx = 0; idx < 4; idx++) {
        /* `<< shift` on a possibly-negative product is UB; multiply instead.
 * The final >>5 is a spec-defined arithmetic shift and stays. */
        int d = ((lev[idx] * ls) * (1 << shift)) >> 5;
        out[idx] = (dctcoef)d;
    }
}
