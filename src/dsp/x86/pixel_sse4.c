/*
 * pixel_sse4.c - SSE4.2 kernels for the pixel family
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Wave 1 of docs/x86-plan.md: the SSE4.2 twin of every pixel-family kernel the
 * NEON twin covers. Each one is bit-exact with the portable reference in
 * src/dsp/pixel.c -- not close, exact -- which is what lets the tier be a
 * runtime axis (Y264_SIMD_FORCE) that cannot move a byte of output.
 *
 * The shared 128-bit building blocks are in pixel_x86.h, including the two
 * identities the SATD family rests on. What is decided HERE is the shape each
 * metric is decomposed into:
 *
 *   - every SATD in this encoder is a sum of 4x4-tile SATDs (the portable 8x8
 *     is two 8x4 halves and the 16x16 is eight of them), so one primitive over
 *     four rows of eight columns -- two tiles at a time -- serves the 4x4, the
 *     8x8, the 16x16, the batched x4 form and both fused intra costs.
 *   - SA8D, the 8x8 AC magnitude and the psy 8x8 term are the same 8x8
 *     Walsh-Hadamard under three different epilogues, so they share one.
 *   - the psy 4x4 term needs each tile's DC as a scalar for its rounded-mean
 *     correction, and that DC is already a lane of the tile's own transform:
 *     the first hadd pair leaves it in lane 0 for the left tile and lane 1 for
 *     the right, so the correction costs two extracts and no second pass.
 *
 * C11 intrinsics only. There is no assembly in this tree, and hygiene_check
 * refuses any inline form under this directory.
 */
#include "dsp/x86/pixel_x86.h"

#if Y264_HAVE_SSE4

/* ---- SAD ----------------------------------------------------------------- */

/* PSADBW is the whole kernel: eight absolute differences summed per half
 * register, per instruction. The 16-wide forms take one row at a time and the
 * 8-wide ones load eight bytes twice, because the declared window is the block
 * and checkasm maps a PROT_NONE page hard against it. */
#define Y264_SAD16(h)                                                          \
static inline int sad_16x##h(const uint8_t *a, int as, const uint8_t *b, int bs)\
{                                                                              \
    __m128i acc = _mm_setzero_si128();                                         \
    for (int y = 0; y < (h); y++)                                              \
        acc = _mm_add_epi32(acc, _mm_sad_epu8(y264_ld128(a + y * as),          \
                                              y264_ld128(b + y * bs)));        \
    return y264_hsum_sad(acc);                                                 \
}
Y264_SAD16(16)
Y264_SAD16(8)

#define Y264_SAD8(h)                                                           \
static inline int sad_8x##h(const uint8_t *a, int as, const uint8_t *b, int bs) \
{                                                                              \
    __m128i acc = _mm_setzero_si128();                                         \
    for (int y = 0; y < (h); y += 2) {                                         \
        __m128i va = _mm_unpacklo_epi64(y264_ld64(a + y * as),                 \
                                        y264_ld64(a + (y + 1) * as));          \
        __m128i vb = _mm_unpacklo_epi64(y264_ld64(b + y * bs),                 \
                                        y264_ld64(b + (y + 1) * bs));          \
        acc = _mm_add_epi32(acc, _mm_sad_epu8(va, vb));                        \
    }                                                                          \
    return y264_hsum_sad(acc);                                                 \
}
Y264_SAD8(16)
Y264_SAD8(8)

int y264_sad_16x16_sse4(const pixel *a, int as, const pixel *b, int bs)
{ return sad_16x16(a, as, b, bs); }
int y264_sad_16x8_sse4(const pixel *a, int as, const pixel *b, int bs)
{ return sad_16x8(a, as, b, bs); }
int y264_sad_8x16_sse4(const pixel *a, int as, const pixel *b, int bs)
{ return sad_8x16(a, as, b, bs); }
int y264_sad_8x8_sse4(const pixel *a, int as, const pixel *b, int bs)
{ return sad_8x8(a, as, b, bs); }

/* The batched form scores four candidates against one source. What it saves is
 * the source loads and the loop, not the PSADBWs: the candidates overlap by a
 * sample or a row and share nothing a register can reuse. */
#define Y264_SADX4_16(h)                                                       \
void y264_sad_x4_16x##h##_sse4(const pixel *src, int ss, const pixel *r0,      \
                               const pixel *r1, const pixel *r2,               \
                               const pixel *r3, int rs, int scores[4])         \
{                                                                              \
    __m128i a0 = _mm_setzero_si128(), a1 = a0, a2 = a0, a3 = a0;               \
    for (int y = 0; y < (h); y++) {                                            \
        __m128i s = y264_ld128(src + y * ss);                                  \
        a0 = _mm_add_epi32(a0, _mm_sad_epu8(s, y264_ld128(r0 + y * rs)));      \
        a1 = _mm_add_epi32(a1, _mm_sad_epu8(s, y264_ld128(r1 + y * rs)));      \
        a2 = _mm_add_epi32(a2, _mm_sad_epu8(s, y264_ld128(r2 + y * rs)));      \
        a3 = _mm_add_epi32(a3, _mm_sad_epu8(s, y264_ld128(r3 + y * rs)));      \
    }                                                                          \
    scores[0] = y264_hsum_sad(a0); scores[1] = y264_hsum_sad(a1);              \
    scores[2] = y264_hsum_sad(a2); scores[3] = y264_hsum_sad(a3);              \
}
Y264_SADX4_16(16)
Y264_SADX4_16(8)

/* Two rows of an 8-wide candidate fill one register, so the 8-wide batch runs
 * the same four PSADBWs over half as many iterations. */
#define Y264_SADX4_8(h)                                                        \
void y264_sad_x4_8x##h##_sse4(const pixel *src, int ss, const pixel *r0,       \
                              const pixel *r1, const pixel *r2,                \
                              const pixel *r3, int rs, int scores[4])          \
{                                                                              \
    __m128i a0 = _mm_setzero_si128(), a1 = a0, a2 = a0, a3 = a0;               \
    for (int y = 0; y < (h); y += 2) {                                         \
        __m128i s  = _mm_unpacklo_epi64(y264_ld64(src + y * ss),               \
                                        y264_ld64(src + (y + 1) * ss));        \
        __m128i v0 = _mm_unpacklo_epi64(y264_ld64(r0 + y * rs),                \
                                        y264_ld64(r0 + (y + 1) * rs));         \
        __m128i v1 = _mm_unpacklo_epi64(y264_ld64(r1 + y * rs),                \
                                        y264_ld64(r1 + (y + 1) * rs));         \
        __m128i v2 = _mm_unpacklo_epi64(y264_ld64(r2 + y * rs),                \
                                        y264_ld64(r2 + (y + 1) * rs));         \
        __m128i v3 = _mm_unpacklo_epi64(y264_ld64(r3 + y * rs),                \
                                        y264_ld64(r3 + (y + 1) * rs));         \
        a0 = _mm_add_epi32(a0, _mm_sad_epu8(s, v0));                           \
        a1 = _mm_add_epi32(a1, _mm_sad_epu8(s, v1));                           \
        a2 = _mm_add_epi32(a2, _mm_sad_epu8(s, v2));                           \
        a3 = _mm_add_epi32(a3, _mm_sad_epu8(s, v3));                           \
    }                                                                          \
    scores[0] = y264_hsum_sad(a0); scores[1] = y264_hsum_sad(a1);              \
    scores[2] = y264_hsum_sad(a2); scores[3] = y264_hsum_sad(a3);              \
}
Y264_SADX4_8(16)
Y264_SADX4_8(8)
Y264_SADX4_8(4)

/* ---- SATD ---------------------------------------------------------------- */

/* Four rows of eight columns, as residual: the unit every SATD here is a sum
 * of. y264_satd_rows8 turns it into the 32 coefficient magnitudes of the two
 * 4x4 tiles it spans. */
static inline __m128i satd_group8(const uint8_t *a, int as,
                                  const uint8_t *b, int bs)
{
    __m128i d[4];
    for (int i = 0; i < 4; i++)
        d[i] = _mm_sub_epi16(_mm_cvtepu8_epi16(y264_ld64(a + i * as)),
                             _mm_cvtepu8_epi16(y264_ld64(b + i * bs)));
    return y264_satd_rows8(d[0], d[1], d[2], d[3]);
}

int y264_satd_4x4_sse4(const pixel *a, int as, const pixel *b, int bs)
{
    return y264_satd_4x4_x86(a, as, b, bs);
}

int y264_satd_8x8_sse4(const pixel *a, int as, const pixel *b, int bs)
{
    return y264_hsum_epi32(_mm_add_epi32(
        satd_group8(a, as, b, bs),
        satd_group8(a + 4 * as, as, b + 4 * bs, bs)));
}

int y264_satd_16x16_sse4(const pixel *a, int as, const pixel *b, int bs)
{
    __m128i acc = _mm_setzero_si128();
    for (int y = 0; y < 16; y += 4)
        for (int x = 0; x < 16; x += 8)
            acc = _mm_add_epi32(acc, satd_group8(a + y * as + x, as,
                                                 b + y * bs + x, bs));
    return y264_hsum_epi32(acc);
}

void y264_satd_x4_8x8_sse4(const pixel *src, int ss, const pixel *r0,
                           const pixel *r1, const pixel *r2, const pixel *r3,
                           int rs, int scores[4])
{
    const pixel *const r[4] = { r0, r1, r2, r3 };
    for (int k = 0; k < 4; k++)
        scores[k] = y264_satd_8x8_sse4(src, ss, r[k], rs);
}

/* ---- SA8D and the 8x8 AC magnitude --------------------------------------- */

int y264_sa8d_8x8_sse4(const pixel *a, int as, const pixel *b, int bs)
{
    __m128i d[8];
    y264_load8x8_diff(d, a, as, b, bs);
    /* Normalised as the portable kernel is, so the value compares directly
     * against a 4x4-support SATD in the transform-size decision. */
    return (y264_had8x8_absum(d) + 2) >> 2;
}

int y264_sa8d_16x16_sse4(const pixel *a, int as, const pixel *b, int bs)
{
    return y264_sa8d_8x8_sse4(a, as, b, bs)
         + y264_sa8d_8x8_sse4(a + 8, as, b + 8, bs)
         + y264_sa8d_8x8_sse4(a + 8 * as, as, b + 8 * bs, bs)
         + y264_sa8d_8x8_sse4(a + 8 * as + 8, as, b + 8 * bs + 8, bs);
}

/* Sum of the 64 samples of an 8x8, which IS the DC coefficient of its
 * Walsh-Hadamard -- so the AC magnitude never forms the DC twice. */
static inline int sum8x8(const uint8_t *p, int stride)
{
    const __m128i z = _mm_setzero_si128();
    __m128i acc = z;
    for (int y = 0; y < 8; y += 2)
        acc = _mm_add_epi32(acc, _mm_sad_epu8(
            _mm_unpacklo_epi64(y264_ld64(p + y * stride),
                               y264_ld64(p + (y + 1) * stride)), z));
    return y264_hsum_sad(acc);
}

long y264_hadamard_ac_8x8_sse4(const pixel *p, int stride)
{
    __m128i d[8];
    y264_load8x8(d, p, stride);
    return (long)y264_had8x8_absum(d) - (long)sum8x8(p, stride);
}

/* ---- the psy texture terms ----------------------------------------------- */

/* Four rows of eight, transformed as two 4x4 tiles, with each tile's DC handed
 * back: it is lane 0 (left tile) and lane 1 (right tile) of the first hadd
 * pair, before any magnitude is taken. */
static inline __m128i tex_group8(const uint8_t *p, int stride, int dc[2])
{
    __m128i d[4];
    for (int i = 0; i < 4; i++)
        d[i] = _mm_cvtepu8_epi16(y264_ld64(p + i * stride));
    {
        __m128i t0 = _mm_add_epi16(d[0], d[1]), t1 = _mm_sub_epi16(d[0], d[1]);
        __m128i t2 = _mm_add_epi16(d[2], d[3]), t3 = _mm_sub_epi16(d[2], d[3]);
        __m128i o0 = _mm_add_epi16(t0, t2), o2 = _mm_sub_epi16(t0, t2);
        __m128i o1 = _mm_add_epi16(t1, t3), o3 = _mm_sub_epi16(t1, t3);
        __m128i h01 = _mm_hadd_epi16(o0, o1), g01 = _mm_hsub_epi16(o0, o1);
        __m128i h23 = _mm_hadd_epi16(o2, o3), g23 = _mm_hsub_epi16(o2, o3);
        __m128i c0 = _mm_hadd_epi16(h01, g01);
        __m128i a = _mm_abs_epi16(c0);
        __m128i b = _mm_abs_epi16(_mm_hsub_epi16(h01, g01));
        __m128i c = _mm_abs_epi16(_mm_hadd_epi16(h23, g23));
        __m128i e = _mm_abs_epi16(_mm_hsub_epi16(h23, g23));
        __m128i s = _mm_add_epi16(_mm_add_epi16(a, b), _mm_add_epi16(c, e));
        dc[0] = (int16_t)_mm_extract_epi16(c0, 0);
        dc[1] = (int16_t)_mm_extract_epi16(c0, 1);
        return _mm_madd_epi16(s, _mm_set1_epi16(1));
    }
}

/* Each 4x4 tile scored against a flat block of its own rounded mean.
 * Subtracting a constant moves only the DC, so the tile's own magnitude sum
 * minus its DC plus the DC's distance from that flat value is the whole
 * answer -- and the tile's DC is its sample sum. */
static inline long tex_correction(int dc)
{
    int flat = 16 * ((dc + 8) >> 4);
    return (long)(dc < flat ? flat - dc : dc - flat) - (long)dc;
}

long y264_texture_ac4_16x16_sse4(const pixel *p, int stride)
{
    __m128i acc = _mm_setzero_si128();
    long corr = 0;
    for (int by = 0; by < 16; by += 4)
        for (int bx = 0; bx < 16; bx += 8) {
            int dc[2];
            acc = _mm_add_epi32(acc, tex_group8(p + by * stride + bx, stride, dc));
            corr += tex_correction(dc[0]) + tex_correction(dc[1]);
        }
    return (long)y264_hsum_epi32(acc) + corr;
}

void y264_texture_ac48_16x16_sse4(const pixel *p, int stride, long out[2])
{
    __m128i a4 = _mm_setzero_si128();
    long corr = 0, e8 = 0;
    /* Both terms walk the same quadrant: its eight rows feed the 8x8 transform
     * for the SA8D-support term, and the four 4x4 tiles inside it feed the
     * other, so the samples are loaded once for both. */
    for (int qy = 0; qy < 16; qy += 8)
        for (int qx = 0; qx < 16; qx += 8) {
            const uint8_t *q = p + qy * stride + qx;
            __m128i d[8];
            y264_load8x8(d, q, stride);
            e8 += (long)y264_had8x8_absum(d) - (long)sum8x8(q, stride);
            for (int i = 0; i < 8; i += 4) {
                int dc[2];
                a4 = _mm_add_epi32(a4, tex_group8(q + i * stride, stride, dc));
                corr += tex_correction(dc[0]) + tex_correction(dc[1]);
            }
        }
    out[0] = (long)y264_hsum_epi32(a4) + corr;
    out[1] = e8;
}

/* ---- variance ------------------------------------------------------------ */

void y264_var_16x16_sse4(const pixel *p, int stride, uint32_t out[2])
{
    const __m128i z = _mm_setzero_si128();
    __m128i s1 = z, s2 = z;
    for (int y = 0; y < 16; y++) {
        __m128i v = y264_ld128(p + y * stride);
        __m128i lo = _mm_unpacklo_epi8(v, z), hi = _mm_unpackhi_epi8(v, z);
        s1 = _mm_add_epi32(s1, _mm_sad_epu8(v, z));
        s2 = _mm_add_epi32(s2, _mm_add_epi32(_mm_madd_epi16(lo, lo),
                                             _mm_madd_epi16(hi, hi)));
    }
    out[0] = (uint32_t)y264_hsum_sad(s1);
    out[1] = (uint32_t)y264_hsum_epi32(s2);
}

/* ---- the fused intra costs ----------------------------------------------- */

void y264_intra4x4_x9_sse4(const pixel *src, int ss, const pixel *rec, int rs,
                           int ht, int hl, int htl, int htr, int costs[9])
{
    __m128i tab[4];
    __m128i s01 = y264_rows2x4(src, ss), s23 = y264_rows2x4(src + 2 * ss, ss);

    y264_i4_edge_tab(tab, rec, rs, ht, hl, htl, htr);
    for (int m = 0; m < 9; m++) {
        __m128i pr = y264_tbl64(tab, y264_ld128(y264_i4_x9_idx[m]));
        __m128i d01 = _mm_sub_epi16(_mm_cvtepu8_epi16(pr), s01);
        __m128i d23 = _mm_sub_epi16(_mm_cvtepu8_epi16(_mm_srli_si128(pr, 8)),
                                    s23);
        costs[m] = y264_hsum_epi32(y264_satd_tile4(d01, d23));
    }
}

/* All three I16x16 predictions are broadcasts -- V repeats the top row, H
 * repeats one left sample across a row, DC is a single value -- so none of them
 * reaches memory: one pass of source loads forms all three residuals in
 * registers and the three SATDs share the loop. */
void y264_intra_satd_x3_16x16_sse4(const pixel *src, int ss, const pixel *top,
                                   const pixel *left, int dc, int costs[3])
{
    const __m128i z = _mm_setzero_si128();
    __m128i vt = y264_ld128(top);
    __m128i tl = _mm_unpacklo_epi8(vt, z), th = _mm_unpackhi_epi8(vt, z);
    __m128i vd = _mm_set1_epi16((short)dc);
    __m128i av = z, ah = z, ad = z;

    for (int y = 0; y < 16; y += 4) {
        __m128i sl[4], sh[4], hb[4];
        for (int i = 0; i < 4; i++) {
            __m128i s = y264_ld128(src + (y + i) * ss);
            sl[i] = _mm_unpacklo_epi8(s, z);
            sh[i] = _mm_unpackhi_epi8(s, z);
            hb[i] = _mm_set1_epi16((short)left[y + i]);
        }
#define Y264_X3(acc, p0, p1, p2, p3, q0, q1, q2, q3)                           \
        acc = _mm_add_epi32(acc,                                               \
            _mm_add_epi32(y264_satd_rows8(_mm_sub_epi16(sl[0], p0),            \
                                          _mm_sub_epi16(sl[1], p1),            \
                                          _mm_sub_epi16(sl[2], p2),            \
                                          _mm_sub_epi16(sl[3], p3)),           \
                          y264_satd_rows8(_mm_sub_epi16(sh[0], q0),            \
                                          _mm_sub_epi16(sh[1], q1),            \
                                          _mm_sub_epi16(sh[2], q2),            \
                                          _mm_sub_epi16(sh[3], q3))))
        Y264_X3(av, tl, tl, tl, tl, th, th, th, th);
        Y264_X3(ah, hb[0], hb[1], hb[2], hb[3], hb[0], hb[1], hb[2], hb[3]);
        Y264_X3(ad, vd, vd, vd, vd, vd, vd, vd, vd);
#undef Y264_X3
    }
    costs[0] = y264_hsum_epi32(av);
    costs[1] = y264_hsum_epi32(ah);
    costs[2] = y264_hsum_epi32(ad);
}

/* ---- SSD ------------------------------------------------------------------
 *
 * PMADDWD squares and pair-sums in one instruction, so a row of sixteen costs
 * two of them. The widest total a 16x16 can reach is 16*16*255^2, inside an
 * int32 accumulator with two bits to spare. */
int y264_ssd_16xh_sse4(const uint8_t *a, int as, const uint8_t *b, int bs, int h)
{
    const __m128i z = _mm_setzero_si128();
    __m128i acc = z;
    for (int y = 0; y < h; y++) {
        __m128i va = y264_ld128(a + y * as), vb = y264_ld128(b + y * bs);
        __m128i dl = _mm_sub_epi16(_mm_unpacklo_epi8(va, z),
                                   _mm_unpacklo_epi8(vb, z));
        __m128i dh = _mm_sub_epi16(_mm_unpackhi_epi8(va, z),
                                   _mm_unpackhi_epi8(vb, z));
        acc = _mm_add_epi32(acc, _mm_add_epi32(_mm_madd_epi16(dl, dl),
                                               _mm_madd_epi16(dh, dh)));
    }
    return y264_hsum_epi32(acc);
}

int y264_ssd_8xh_sse4(const uint8_t *a, int as, const uint8_t *b, int bs, int h)
{
    __m128i acc = _mm_setzero_si128();
    for (int y = 0; y < h; y++) {
        __m128i d = _mm_sub_epi16(_mm_cvtepu8_epi16(y264_ld64(a + y * as)),
                                  _mm_cvtepu8_epi16(y264_ld64(b + y * bs)));
        acc = _mm_add_epi32(acc, _mm_madd_epi16(d, d));
    }
    return y264_hsum_epi32(acc);
}

#endif /* Y264_HAVE_SSE4 */
