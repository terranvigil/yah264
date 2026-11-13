/*
 * transform_avx2_10.c - AVX2 transform, quant and scan kernels for the 10-bit
 * library
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * a row of eight int32 coefficients is one 256-bit register: an 8x8
 * transform holds its block in eight registers and transposes with in-lane
 * unpacks and four lane crossings, and quant, dequant and the 8x8 scan run
 * eight lanes at a time. the batched 4x4 forward puts two horizontally
 * adjacent blocks in the two 128-bit lanes, where the 4x4 transpose runs on
 * each independently.
 *
 * the lone 4x4 shapes and the byte-domain 8x8 scan mask have no second half
 * to fill; they use the shared 128-bit bodies, VEX-encoded.
 */
#include "dsp/x86/transform_x86_10.h"

#if Y264_HAVE_AVX2_10

/* ---- 256-bit plumbing ---------------------------------------------------- */

static inline __m256i t10_ldc8(const dctcoef *p)
{
    return _mm256_loadu_si256((const __m256i *)(const void *)p);
}

static inline void t10_stc8(dctcoef *p, __m256i v)
{
    _mm256_storeu_si256((__m256i *)(void *)p, v);
}

/* eight samples of src - pred as int32 */
static inline __m256i t10_diff8w(const pixel *s, const pixel *p)
{
    return _mm256_sub_epi32(_mm256_cvtepu16_epi32(t10_ldp8(s)),
                            _mm256_cvtepu16_epi32(t10_ldp8(p)));
}

#define T10_SRA1W(x) _mm256_srai_epi32(x, 1)
#define T10_SRA2W(x) _mm256_srai_epi32(x, 2)

/* 8x8 of int32: in-lane unpacks give the 4x4 tiles of each half, the lane
 * crossings join them */
static inline void t10_trn8w(__m256i v[8])
{
    __m256i a0 = _mm256_unpacklo_epi32(v[0], v[1]), a1 = _mm256_unpackhi_epi32(v[0], v[1]);
    __m256i a2 = _mm256_unpacklo_epi32(v[2], v[3]), a3 = _mm256_unpackhi_epi32(v[2], v[3]);
    __m256i a4 = _mm256_unpacklo_epi32(v[4], v[5]), a5 = _mm256_unpackhi_epi32(v[4], v[5]);
    __m256i a6 = _mm256_unpacklo_epi32(v[6], v[7]), a7 = _mm256_unpackhi_epi32(v[6], v[7]);
    __m256i b0 = _mm256_unpacklo_epi64(a0, a2), b1 = _mm256_unpackhi_epi64(a0, a2);
    __m256i b2 = _mm256_unpacklo_epi64(a1, a3), b3 = _mm256_unpackhi_epi64(a1, a3);
    __m256i b4 = _mm256_unpacklo_epi64(a4, a6), b5 = _mm256_unpackhi_epi64(a4, a6);
    __m256i b6 = _mm256_unpacklo_epi64(a5, a7), b7 = _mm256_unpackhi_epi64(a5, a7);
    v[0] = _mm256_permute2x128_si256(b0, b4, 0x20);
    v[1] = _mm256_permute2x128_si256(b1, b5, 0x20);
    v[2] = _mm256_permute2x128_si256(b2, b6, 0x20);
    v[3] = _mm256_permute2x128_si256(b3, b7, 0x20);
    v[4] = _mm256_permute2x128_si256(b0, b4, 0x31);
    v[5] = _mm256_permute2x128_si256(b1, b5, 0x31);
    v[6] = _mm256_permute2x128_si256(b2, b6, 0x31);
    v[7] = _mm256_permute2x128_si256(b3, b7, 0x31);
}

/* rows, then columns, as the reference runs them */
#define T10_XFORM8W(OP, v) do {                                               \
    t10_trn8w(v);                                                             \
    OP(__m256i, _mm256_add_epi32, _mm256_sub_epi32, T10_SRA1W, T10_SRA2W,     \
       v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);                       \
    t10_trn8w(v);                                                             \
    OP(__m256i, _mm256_add_epi32, _mm256_sub_epi32, T10_SRA1W, T10_SRA2W,     \
       v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);                       \
} while (0)

/* ---- 8x8 transforms ------------------------------------------------------ */

static inline void t10_fdct8_rows(__m256i v[8], dctcoef coef[64])
{
    T10_XFORM8W(T10_FDCT8_1D, v);
    for (int i = 0; i < 8; i++)
        t10_stc8(coef + 8 * i, v[i]);
}

void y264_fdct8x8_avx2(const dctcoef diff[64], dctcoef coef[64])
{
    __m256i v[8];
    for (int i = 0; i < 8; i++)
        v[i] = t10_ldc8(diff + 8 * i);
    t10_fdct8_rows(v, coef);
}

static inline void t10_sub8x8_dct8w(dctcoef coef[64], const pixel *src, int ss,
                                    const pixel *pred, int ps)
{
    __m256i v[8];
    for (int i = 0; i < 8; i++)
        v[i] = t10_diff8w(src + i * ss, pred + i * ps);
    t10_fdct8_rows(v, coef);
}

void y264_sub8x8_dct8_avx2(dctcoef coef[64], const pixel *src, int ss,
                           const pixel *pred, int ps)
{ t10_sub8x8_dct8w(coef, src, ss, pred, ps); }

void y264_sub_dct8_blocks_avx2(dctcoef (*coef)[64], int nbw, int nbh,
                               const pixel *src, int ss,
                               const pixel *pred, int ps)
{
    for (int by = 0; by < nbh; by++, src += 8 * ss, pred += 8 * ps)
        for (int bx = 0; bx < nbw; bx++, coef++)
            t10_sub8x8_dct8w(coef[0], src + 8 * bx, ss, pred + 8 * bx, ps);
}

static inline void t10_idct8w(const dctcoef coef[64], __m256i v[8])
{
    const __m256i k32 = _mm256_set1_epi32(32);
    for (int i = 0; i < 8; i++)
        v[i] = t10_ldc8(coef + 8 * i);
    T10_XFORM8W(T10_IDCT8_1D, v);
    for (int i = 0; i < 8; i++)
        v[i] = _mm256_srai_epi32(_mm256_add_epi32(v[i], k32), 6);
}

void y264_idct8x8_avx2(const dctcoef coef[64], dctcoef res[64])
{
    __m256i v[8];
    t10_idct8w(coef, v);
    for (int i = 0; i < 8; i++)
        t10_stc8(res + 8 * i, v[i]);
}

void y264_add8x8_idct8_avx2(pixel *dst, int ds, const pixel *pred, int ps,
                            const dctcoef coef[64])
{
    __m256i v[8];
    t10_idct8w(coef, v);
    for (int i = 0; i < 8; i++)
        _mm_storeu_si128((__m128i *)(void *)(dst + i * ds),
                         t10_recon8(pred + i * ps, _mm256_castsi256_si128(v[i]),
                                    _mm256_extracti128_si256(v[i], 1)));
}

/* ---- 4x4 transforms ------------------------------------------------------ */

void y264_fdct4x4_avx2(const dctcoef diff[16], dctcoef coef[16])
{ t10_fdct4x4(diff, coef); }

void y264_idct4x4_avx2(const dctcoef coef[16], dctcoef res[16])
{ t10_idct4x4(coef, res); }

void y264_sub4x4_dct_avx2(dctcoef coef[16], const pixel *src, int ss,
                          const pixel *pred, int ps)
{ t10_sub4x4_dct(coef, src, ss, pred, ps); }

void y264_add4x4_idct_avx2(pixel *dst, int ds, const pixel *pred, int ps,
                           const dctcoef coef[16])
{ t10_add4x4_idct(dst, ds, pred, ps, coef); }

/* two adjacent 4x4 blocks a pass: an 8-sample row widens to one register,
 * the left block in the low lane and the right in the high. an odd last block
 * takes the 128-bit body. */
void y264_sub_dct4_blocks_avx2(dctcoef (*coef)[16], int nbw, int nbh,
                               const pixel *src, int ss,
                               const pixel *pred, int ps)
{
    for (int by = 0; by < nbh; by++, src += 4 * ss, pred += 4 * ps) {
        int bx = 0;
        for (; bx + 2 <= nbw; bx += 2, coef += 2) {
            const pixel *s = src + 4 * bx, *p = pred + 4 * bx;
            __m256i r0 = t10_diff8w(s, p), r1 = t10_diff8w(s + ss, p + ps);
            __m256i r2 = t10_diff8w(s + 2 * ss, p + 2 * ps);
            __m256i r3 = t10_diff8w(s + 3 * ss, p + 3 * ps);
            T10_FDCT4_1D(__m256i, _mm256_add_epi32, _mm256_sub_epi32, r0, r1, r2, r3);
            T10_TRN4(__m256i, _mm256_unpacklo_epi32, _mm256_unpackhi_epi32,
                     _mm256_unpacklo_epi64, _mm256_unpackhi_epi64, r0, r1, r2, r3);
            T10_FDCT4_1D(__m256i, _mm256_add_epi32, _mm256_sub_epi32, r0, r1, r2, r3);
            T10_TRN4(__m256i, _mm256_unpacklo_epi32, _mm256_unpackhi_epi32,
                     _mm256_unpacklo_epi64, _mm256_unpackhi_epi64, r0, r1, r2, r3);
            t10_stc8(coef[0], _mm256_permute2x128_si256(r0, r1, 0x20));
            t10_stc8(coef[0] + 8, _mm256_permute2x128_si256(r2, r3, 0x20));
            t10_stc8(coef[1], _mm256_permute2x128_si256(r0, r1, 0x31));
            t10_stc8(coef[1] + 8, _mm256_permute2x128_si256(r2, r3, 0x31));
        }
        for (; bx < nbw; bx++, coef++)
            t10_sub4x4_dct(coef[0], src + 4 * bx, ss, pred + 4 * bx, ps);
    }
}

/* ---- forward quant ----------------------------------------------------------
 *
 * the 128-bit body eight lanes wide, same domain (flat multipliers only): a
 * coefficient of 2^17 or more, or a bias outside [0, 2^30), sends the block
 * to the scalar loop. */
static inline __m256i t10_quant8(__m256i c, __m256i mf, __m256i vf, __m128i sh)
{
    __m256i z = _mm256_setzero_si256();
    __m256i q = _mm256_srl_epi32(
        _mm256_add_epi32(_mm256_mullo_epi32(_mm256_abs_epi32(c), mf), vf), sh);
    return _mm256_blendv_epi8(q, _mm256_sub_epi32(z, q), _mm256_cmpgt_epi32(z, c));
}

static inline int t10_quant_wide8(const dctcoef *coef, int n)
{
    __m256i m = _mm256_setzero_si256();
    for (int i = 0; i < n; i += 8)
        m = _mm256_or_si256(m, _mm256_abs_epi32(t10_ldc8(coef + i)));
    return !_mm256_testz_si256(m, _mm256_set1_epi32(-T10_QUANT_SAFE));
}

static inline void t10_quant_nw(const dctcoef *coef, dctcoef *lev, int n, int f,
                                int qbits, const int32_t *mfrow)
{
    if ((unsigned)f >= T10_QUANT_FMAX || t10_quant_wide8(coef, n)) {
        t10_quant_scalar(coef, lev, n, f, qbits, mfrow);
        return;
    }
    __m256i vf = _mm256_set1_epi32(f);
    __m128i sh = _mm_cvtsi32_si128(qbits);
    for (int i = 0; i < n; i += 8)
        t10_stc8(lev + i, t10_quant8(t10_ldc8(coef + i), t10_ldc8(mfrow + i), vf, sh));
}

void y264_quant_4x4_f_avx2(const dctcoef coef[16], dctcoef lev[16], int qp, int f,
                           const int32_t mfrow[16])
{ t10_quant_nw(coef, lev, 16, f, 15 + qp / 6, mfrow); }

void y264_quant_4x4_avx2(const dctcoef coef[16], dctcoef lev[16], int qp, int intra,
                         const int32_t mfrow[16])
{
    int qbits = 15 + qp / 6;
    t10_quant_nw(coef, lev, 16, (1 << qbits) / (intra ? 3 : 6), qbits, mfrow);
}

void y264_quant_8x8_f_avx2(const dctcoef coef[64], dctcoef lev[64], int qp, int f,
                           const int32_t mfrow[64])
{ t10_quant_nw(coef, lev, 64, f, 16 + qp / 6, mfrow); }

void y264_quant_4x4_f_n_avx2(const dctcoef *coef, dctcoef *lev, int n, int qp,
                             int f, const int32_t mfrow[16])
{
    for (int k = 0; k < n; k++)
        t10_quant_nw(coef + 16 * k, lev + 16 * k, 16, f, 15 + qp / 6, mfrow);
}

void y264_quant_8x8_f_n_avx2(const dctcoef *coef, dctcoef *lev, int n, int qp,
                             int f, const int32_t mfrow[64])
{
    for (int k = 0; k < n; k++)
        t10_quant_nw(coef + 64 * k, lev + 64 * k, 64, f, 16 + qp / 6, mfrow);
}

int y264_quant_4x4_ac_f_avx2(const dctcoef *coef, dctcoef *lev, int n, int qp,
                             int f, const int32_t mfrow[16], dctcoef *dc, int *nz)
{
    int any = 0;
    for (int k = 0; k < n; k++, coef += 16, lev += 16) {
        t10_quant_nw(coef, lev, 16, f, 15 + qp / 6, mfrow);
        any |= t10_quant_ac_tail(coef, lev, &dc[k], &nz[k]);
    }
    return any;
}

/* ---- dequant ------------------------------------------------------------- */

static inline void t10_dequantw(const dctcoef *lev, dctcoef *coef, int n, int qp,
                                const int32_t *lsrow, int point)
{
    int shift = qp / 6;
    if (shift >= point) {
        __m128i vs = _mm_cvtsi32_si128(shift - point);
        for (int g = 0; g < n; g += 8)
            t10_stc8(coef + g, _mm256_sll_epi32(_mm256_mullo_epi32(t10_ldc8(lev + g),
                                                                   t10_ldc8(lsrow + g)), vs));
    } else {
        __m256i vr = _mm256_set1_epi32(1 << (point - 1 - shift));
        __m128i vs = _mm_cvtsi32_si128(point - shift);
        for (int g = 0; g < n; g += 8)
            t10_stc8(coef + g, _mm256_sra_epi32(_mm256_add_epi32(_mm256_mullo_epi32(
                t10_ldc8(lev + g), t10_ldc8(lsrow + g)), vr), vs));
    }
}

void y264_dequant_4x4_avx2(const dctcoef lev[16], dctcoef coef[16], int qp,
                           const int32_t lsrow[16])
{ t10_dequantw(lev, coef, 16, qp, lsrow, 4); }

void y264_dequant_8x8_avx2(const dctcoef lev[64], dctcoef coef[64], int qp,
                           const int32_t lsrow[64])
{ t10_dequantw(lev, coef, 64, qp, lsrow, 6); }

/* ---- scans ------------------------------------------------------------------
 *
 * the 8x8 absolute scan over eight row registers: each scan group of eight is
 * a lane permute of every row it draws on, blended in where the control is
 * not -1 (all ones, so the blend keeps what is there). controls come from
 * y264_zigzag8: for scan group k and row j, the column each position reads,
 * or -1 where another row owns it. */
static const uint8_t t10_zzw_src[36] = {
    0, 1, 2, 0, 1, 2, 3, 4, 1, 2, 3, 4, 5, 6, 0, 1, 2, 3,
    4, 5, 6, 7, 1, 2, 3, 4, 5, 6, 3, 4, 5, 6, 7, 5, 6, 7
};
static const uint8_t t10_zzw_off[9] = { 0, 3, 8, 14, 18, 22, 28, 33, 36 };
static const int32_t t10_zzw_idx[36][8] = {
    {  0,  1, -1, -1, -1,  2,  3, -1 }, { -1, -1,  0, -1,  1, -1, -1,  2 },
    { -1, -1, -1,  0, -1, -1, -1, -1 },
    { -1, -1, -1, -1, -1, -1,  4,  5 }, { -1, -1, -1, -1, -1,  3, -1, -1 },
    {  1, -1, -1, -1,  2, -1, -1, -1 }, { -1,  0, -1,  1, -1, -1, -1, -1 },
    { -1, -1,  0, -1, -1, -1, -1, -1 },
    {  4, -1, -1, -1, -1, -1, -1, -1 }, { -1,  3, -1, -1, -1, -1, -1, -1 },
    { -1, -1,  2, -1, -1, -1, -1, -1 }, { -1, -1, -1,  1, -1, -1, -1,  2 },
    { -1, -1, -1, -1,  0, -1,  1, -1 }, { -1, -1, -1, -1, -1,  0, -1, -1 },
    { -1, -1, -1,  6,  7, -1, -1, -1 }, { -1, -1,  5, -1, -1,  6, -1, -1 },
    { -1,  4, -1, -1, -1, -1,  5, -1 }, {  3, -1, -1, -1, -1, -1, -1,  4 },
    {  3, -1, -1, -1, -1, -1, -1,  4 }, { -1,  2, -1, -1, -1, -1,  3, -1 },
    { -1, -1,  1, -1, -1,  2, -1, -1 }, { -1, -1, -1,  0,  1, -1, -1, -1 },
    { -1, -1,  7, -1, -1, -1, -1, -1 }, { -1,  6, -1,  7, -1, -1, -1, -1 },
    {  5, -1, -1, -1,  6, -1, -1, -1 }, { -1, -1, -1, -1, -1,  5, -1, -1 },
    { -1, -1, -1, -1, -1, -1,  4, -1 }, { -1, -1, -1, -1, -1, -1, -1,  3 },
    { -1, -1, -1, -1, -1,  7, -1, -1 }, { -1, -1, -1, -1,  6, -1,  7, -1 },
    { -1, -1, -1,  5, -1, -1, -1,  6 }, { -1, -1,  4, -1, -1, -1, -1, -1 },
    {  2,  3, -1, -1, -1, -1, -1, -1 },
    { -1, -1, -1, -1,  7, -1, -1, -1 }, {  5, -1, -1,  6, -1,  7, -1, -1 },
    { -1,  4,  5, -1, -1, -1,  6,  7 },
};

void y264_zigzag_abs_8x8_avx2(int out[64], const dctcoef in[64])
{
    __m256i r[8];
    for (int j = 0; j < 8; j++)
        r[j] = t10_ldc8(in + 8 * j);
    for (int k = 0; k < 8; k++) {
        __m256i v = _mm256_setzero_si256();
        for (int i = t10_zzw_off[k]; i < t10_zzw_off[k + 1]; i++) {
            __m256i idx = _mm256_loadu_si256((const __m256i *)(const void *)t10_zzw_idx[i]);
            v = _mm256_blendv_epi8(_mm256_permutevar8x32_epi32(r[t10_zzw_src[i]], idx),
                                   v, idx);
        }
        t10_stc8(out + 8 * k, _mm256_abs_epi32(v));
    }
}

void y264_zigzag_abs_4x4_avx2(int out[16], const dctcoef in[16])
{ t10_zigzag_abs_4x4(out, in); }

void y264_scan_mask_8x8_avx2(const dctcoef lev[64], uint64_t *omsk, int *obig)
{ t10_scan_mask_8x8(lev, omsk, obig); }

void y264_zigzag_scan_4x4_avx2(dctcoef out[16], const dctcoef in[16],
                               uint32_t *omsk, int *obig)
{ t10_zigzag_scan_4x4(out, in, omsk, obig); }

#endif /* Y264_HAVE_AVX2_10 */
