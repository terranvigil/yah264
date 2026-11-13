/*
 * intrapred_x86_10.h - the 10-bit intra prediction kernels' shared 128-bit bodies
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The 16-bit-sample twins of intrapred_x86.h. Every body here is `static
 * inline` and 128-bit: intrapred_sse4_10.c wraps each one under its SSE4.2
 * name, and intrapred_avx2_10.c wraps the 8x8 ones the same way (VEX-encoded
 * there) and writes its own 256-bit 16x16 and chroma builders, where a row of
 * sixteen samples, or two rows of eight, is one register.
 *
 * The routing is the 8-bit tiers': 16x16 and cw == 8 chroma take every mode,
 * 8x8 the five branchy diagonals, 4x4 nothing. What moves at 10 bits:
 *
 *   - the 8x8 filters run on u16 lanes. (a + 2b + c + 2) >> 2 is still
 *     ((a + c) >> 1 rounded-averaged with b), and the inner halving add must
 *     TRUNCATE: a + c <= 2046 fits a u16 lane, so it is an add and a shift,
 *     never pavgw, which rounds and gives the wrong filter.
 *   - a sample is two bytes, so a pshufb reaches only eight of them. The
 *     filtered windows are split into a low-byte and a high-byte plane of
 *     sixteen entries each, both planes go through the same 8-bit shuffle
 *     control, and the two results interleave back into samples. So the
 *     controls are the 8-bit tiers' numbers, and sixteen of them make two
 *     output rows.
 *   - the plane modes leave int16. At 16x16 |a| <= 16 * 2046 = 32736 and
 *     |b|, |c| <= (5 * 36828 + 32) >> 6 = 2877, so a sum reaches 78784; 8-wide
 *     chroma reaches 32736 + 4 * 5435 + 8 * 2877 + 16 = 77508. Both run on
 *     int32 lanes, and the clip is packus_epi32 (negatives to 0) then a
 *     min against 1023.
 *
 * Loads are sized to the block, never to the register: checkasm maps each
 * kernel's window against a PROT_NONE page at both ends.
 */
#ifndef YAH264_DSP_X86_INTRAPRED_X86_10_H
#define YAH264_DSP_X86_INTRAPRED_X86_10_H

#include <immintrin.h>
#include <stdlib.h>
#include <string.h>

#include "dsp/arch.h"
#include "dsp/intrapred.h"

#if Y264_BIT_DEPTH != 10
#error "intrapred_x86_10.h is the 10-bit library's x86 tier"
#endif

/* ---- loads, stores, the clip --------------------------------------------- */

static inline __m128i ip10_ld8(const pixel *p)
{
    return _mm_loadu_si128((const __m128i *)(const void *)p);
}

static inline void ip10_st8(pixel *p, __m128i v)
{
    _mm_storeu_si128((__m128i *)(void *)p, v);
}

/* eight int32 sums, shifted down and clipped to [0, PIXEL_MAX] as samples */
static inline __m128i ip10_clip(__m128i lo, __m128i hi)
{
    return _mm_min_epu16(_mm_packus_epi32(_mm_srai_epi32(lo, 5),
                                          _mm_srai_epi32(hi, 5)),
                         _mm_set1_epi16(PIXEL_MAX));
}

/* ---- the 8x8 filters ------------------------------------------------------
 *
 * Over eight positions from in[0 .. 9]: F[i] = (e[i] + 2e[i+1] + e[i+2] + 2) >> 2
 * and H[i] = (e[i] + e[i+1] + 1) >> 1. The halving add truncates (see top).
 */
static inline __m128i ip10_f121(__m128i a, __m128i b, __m128i c)
{
    return _mm_avg_epu16(_mm_srli_epi16(_mm_add_epi16(a, c), 1), b);
}

static inline void ip10_fh8(const pixel *in, __m128i *f, __m128i *h)
{
    __m128i a = ip10_ld8(in), b = ip10_ld8(in + 1), c = ip10_ld8(in + 2);
    *f = ip10_f121(a, b, c);
    *h = _mm_avg_epu16(a, b);
}

/* sixteen samples as two byte planes: entry i's low byte at lo[i], high at hi[i] */
typedef struct { __m128i lo, hi; } ip10_win;

static inline ip10_win ip10_planes(__m128i a, __m128i b)
{
    const __m128i m = _mm_set1_epi16(0xff);
    ip10_win w;
    w.lo = _mm_packus_epi16(_mm_and_si128(a, m), _mm_and_si128(b, m));
    w.hi = _mm_packus_epi16(_mm_srli_epi16(a, 8), _mm_srli_epi16(b, 8));
    return w;
}

/* ---- the shuffle controls -------------------------------------------------
 *
 * The 8-bit tiers' six-mode controls, unchanged: one row of 64 per (mode,
 * window), the window entry each output sample reads, or -1 where the other
 * window owns it. The windows are
 *
 *   FLO = F[0..15]   FHI = F[9..24]   HLO = H[0..15]   HHI = H[9..24]
 *
 * and HU's own S = { F[0..5], H[0..6], e[0], (e[1] + 3 e[0] + 2) >> 2, 0 }.
 * They restate y264_intra8x8_dir6_idx, and checkasm holds every lane to the
 * C builder.
 */
static const int8_t ip10_dir6_shuf[9][64] = {
    {   /* DDL from FHI */
         0,  1,  2,  3,  4,  5,  6,  7,  1,  2,  3,  4,  5,  6,  7,  8,
         2,  3,  4,  5,  6,  7,  8,  9,  3,  4,  5,  6,  7,  8,  9, 10,
         4,  5,  6,  7,  8,  9, 10, 11,  5,  6,  7,  8,  9, 10, 11, 12,
         6,  7,  8,  9, 10, 11, 12, 13,  7,  8,  9, 10, 11, 12, 13, 14,
    },
    {   /* DDR from FLO */
         7,  8,  9, 10, 11, 12, 13, 14,  6,  7,  8,  9, 10, 11, 12, 13,
         5,  6,  7,  8,  9, 10, 11, 12,  4,  5,  6,  7,  8,  9, 10, 11,
         3,  4,  5,  6,  7,  8,  9, 10,  2,  3,  4,  5,  6,  7,  8,  9,
         1,  2,  3,  4,  5,  6,  7,  8,  0,  1,  2,  3,  4,  5,  6,  7,
    },
    {   /* VR from FLO */
        -1, -1, -1, -1, -1, -1, -1, -1,  7,  8,  9, 10, 11, 12, 13, 14,
         6, -1, -1, -1, -1, -1, -1, -1,  5,  7,  8,  9, 10, 11, 12, 13,
         4,  6, -1, -1, -1, -1, -1, -1,  3,  5,  7,  8,  9, 10, 11, 12,
         2,  4,  6, -1, -1, -1, -1, -1,  1,  3,  5,  7,  8,  9, 10, 11,
    },
    {   /* VR from HLO */
         8,  9, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1,
        -1,  8,  9, 10, 11, 12, 13, 14, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1,  8,  9, 10, 11, 12, 13, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1,  8,  9, 10, 11, 12, -1, -1, -1, -1, -1, -1, -1, -1,
    },
    {   /* HD from FLO */
        -1,  7,  8,  9, 10, 11, 12, 13, -1,  6, -1,  7,  8,  9, 10, 11,
        -1,  5, -1,  6, -1,  7,  8,  9, -1,  4, -1,  5, -1,  6, -1,  7,
        -1,  3, -1,  4, -1,  5, -1,  6, -1,  2, -1,  3, -1,  4, -1,  5,
        -1,  1, -1,  2, -1,  3, -1,  4, -1,  0, -1,  1, -1,  2, -1,  3,
    },
    {   /* HD from HLO */
         7, -1, -1, -1, -1, -1, -1, -1,  6, -1,  7, -1, -1, -1, -1, -1,
         5, -1,  6, -1,  7, -1, -1, -1,  4, -1,  5, -1,  6, -1,  7, -1,
         3, -1,  4, -1,  5, -1,  6, -1,  2, -1,  3, -1,  4, -1,  5, -1,
         1, -1,  2, -1,  3, -1,  4, -1,  0, -1,  1, -1,  2, -1,  3, -1,
    },
    {   /* VL from FHI */
        -1, -1, -1, -1, -1, -1, -1, -1,  0,  1,  2,  3,  4,  5,  6,  7,
        -1, -1, -1, -1, -1, -1, -1, -1,  1,  2,  3,  4,  5,  6,  7,  8,
        -1, -1, -1, -1, -1, -1, -1, -1,  2,  3,  4,  5,  6,  7,  8,  9,
        -1, -1, -1, -1, -1, -1, -1, -1,  3,  4,  5,  6,  7,  8,  9, 10,
    },
    {   /* VL from HHI */
         0,  1,  2,  3,  4,  5,  6,  7, -1, -1, -1, -1, -1, -1, -1, -1,
         1,  2,  3,  4,  5,  6,  7,  8, -1, -1, -1, -1, -1, -1, -1, -1,
         2,  3,  4,  5,  6,  7,  8,  9, -1, -1, -1, -1, -1, -1, -1, -1,
         3,  4,  5,  6,  7,  8,  9, 10, -1, -1, -1, -1, -1, -1, -1, -1,
    },
    {   /* HU from S */
        12,  5, 11,  4, 10,  3,  9,  2, 11,  4, 10,  3,  9,  2,  8,  1,
        10,  3,  9,  2,  8,  1,  7,  0,  9,  2,  8,  1,  7,  0,  6, 14,
         8,  1,  7,  0,  6, 14, 13, 13,  7,  0,  6, 14, 13, 13, 13, 13,
         6, 14, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13,
    },
};

enum {
    IP10_DDL = 0, IP10_DDR, IP10_VR_F, IP10_VR_H, IP10_HD_F, IP10_HD_H,
    IP10_VL_F, IP10_VL_H, IP10_HU
};

static inline __m128i ip10_ctl(int row, int k)
{
    return _mm_loadu_si128((const __m128i *)(const void *)(ip10_dir6_shuf[row] + k));
}

/* Outputs k .. k + 15 of one mode: each plane through the control (two
 * windows OR-ed where the mode has two, since -1 lanes come out zero), then
 * the planes zipped back into samples. `b` may be NULL. */
static inline void ip10_emit16(pixel *out, int k, const ip10_win *a, int ra,
                               const ip10_win *b, int rb)
{
    __m128i ca = ip10_ctl(ra, k);
    __m128i lo = _mm_shuffle_epi8(a->lo, ca), hi = _mm_shuffle_epi8(a->hi, ca);
    if (b) {
        __m128i cb = ip10_ctl(rb, k);
        lo = _mm_or_si128(lo, _mm_shuffle_epi8(b->lo, cb));
        hi = _mm_or_si128(hi, _mm_shuffle_epi8(b->hi, cb));
    }
    ip10_st8(out + k, _mm_unpacklo_epi8(lo, hi));
    ip10_st8(out + k + 8, _mm_unpackhi_epi8(lo, hi));
}

static inline void ip10_emit64(pixel out[64], const ip10_win *a, int ra,
                               const ip10_win *b, int rb)
{
    for (int k = 0; k < 64; k += 16)
        ip10_emit16(out, k, a, ra, b, rb);
}

/* The four windows over the flat edge array; the reads end at e[26]. */
typedef struct { ip10_win flo, fhi, hlo, hhi; __m128i f0, h0; } ip10_dirwin;

static inline void ip10_dirwin_build(ip10_dirwin *w, const pixel e[32])
{
    __m128i f0, f1, f2, f3, h0, h1, h2, h3;
    ip10_fh8(e, &f0, &h0);
    ip10_fh8(e + 8, &f1, &h1);
    ip10_fh8(e + 9, &f2, &h2);
    ip10_fh8(e + 17, &f3, &h3);
    w->flo = ip10_planes(f0, f1);
    w->hlo = ip10_planes(h0, h1);
    w->fhi = ip10_planes(f2, f3);
    w->hhi = ip10_planes(h2, h3);
    w->f0 = f0;
    w->h0 = h0;
}

/* HU's window S from F[0..7], H[0..7] and the bottom-left samples */
static inline ip10_win ip10_hu_window(__m128i f0, __m128i h0, const pixel e[32])
{
    __m128i s0 = _mm_blend_epi16(f0, _mm_slli_si128(h0, 12), 0xc0);
    __m128i s1 = _mm_srli_si128(h0, 4);
    s1 = _mm_insert_epi16(s1, e[0], 5);
    s1 = _mm_insert_epi16(s1, (e[1] + 3 * e[0] + 2) >> 2, 6);
    s1 = _mm_insert_epi16(s1, 0, 7);
    return ip10_planes(s0, s1);
}

/* ---- Intra8x8 from a precomputed edge (VERT/DDR/VR/HD/VL) ----------------- */
static inline void ip10_i8_from_edge(pixel pred[64], const pixel e[32], int mode)
{
    if (mode == Y264_I4_VERT) {
        __m128i t = ip10_ld8(e + 9);
        for (int y = 0; y < 8; y++)
            ip10_st8(pred + 8 * y, t);
        return;
    }
    ip10_dirwin w;
    ip10_dirwin_build(&w, e);
    switch (mode) {
    case Y264_I4_DDR: ip10_emit64(pred, &w.flo, IP10_DDR, NULL, 0); break;
    case Y264_I4_VR:  ip10_emit64(pred, &w.flo, IP10_VR_F, &w.hlo, IP10_VR_H); break;
    case Y264_I4_HD:  ip10_emit64(pred, &w.flo, IP10_HD_F, &w.hlo, IP10_HD_H); break;
    case Y264_I4_VL:  ip10_emit64(pred, &w.fhi, IP10_VL_F, &w.hhi, IP10_VL_H); break;
    default: break;
    }
}

/* ---- the six directional modes from one filter pass ----------------------- */
static inline void ip10_i8_dir6(pixel out[6][64], const pixel e[32])
{
    ip10_dirwin w;
    ip10_dirwin_build(&w, e);
    ip10_win s = ip10_hu_window(w.f0, w.h0, e);
    for (int k = 0; k < 64; k += 16) {
        ip10_emit16(out[0], k, &w.fhi, IP10_DDL, NULL, 0);
        ip10_emit16(out[1], k, &w.flo, IP10_DDR, NULL, 0);
        ip10_emit16(out[2], k, &w.flo, IP10_VR_F, &w.hlo, IP10_VR_H);
        ip10_emit16(out[3], k, &w.flo, IP10_HD_F, &w.hlo, IP10_HD_H);
        ip10_emit16(out[4], k, &w.fhi, IP10_VL_F, &w.hhi, IP10_VL_H);
        ip10_emit16(out[5], k, &s, IP10_HU, NULL, 0);
    }
}

/* ---- the filtered edge of one Intra8x8 block, as registers ----------------
 *
 * 8.3.2.2.1's endpoint rules are the ordinary 121 filter with the source
 * replicated: t[0]'s left neighbour is the corner (or t[0] itself without
 * one), t[15]'s right neighbour is itself, and likewise down the left column.
 * Returns the filtered corner and leaves t[0..7], t[8..15] and l[0..7] in
 * *t0, *t1, *lf, zero for a side that is not available. Nothing outside the
 * available neighbours is read: eight top samples without a top right, the
 * corner only with a top left.
 */
static inline int ip10_edge_regs(const pixel *rec, int stride, int have_top,
                                 int have_left, int have_topleft,
                                 int have_topright, __m128i *t0, __m128i *t1,
                                 __m128i *lf)
{
    const __m128i z = _mm_setzero_si128();
    const __m128i dup0 = _mm_setr_epi8(0, 1, 0, 1, 2, 3, 4, 5,
                                       6, 7, 8, 9, 10, 11, 12, 13);
    const __m128i dup7 = _mm_setr_epi8(2, 3, 4, 5, 6, 7, 8, 9,
                                       10, 11, 12, 13, 14, 15, 14, 15);
    int rtl = have_topleft ? rec[-stride - 1] : 0;
    int rt0 = 0, rl0 = 0;
    *t0 = *t1 = *lf = z;
    if (have_top) {
        const pixel *top = rec - stride;
        __m128i a = ip10_ld8(top);
        __m128i b = have_topright ? ip10_ld8(top + 8)
                                  : _mm_shuffle_epi8(a, _mm_set1_epi16(0x0f0e));
        __m128i pa = have_topleft
            ? _mm_insert_epi16(_mm_slli_si128(a, 2), rtl, 0)
            : _mm_shuffle_epi8(a, dup0);
        __m128i na = _mm_alignr_epi8(b, a, 2);
        __m128i pb = _mm_alignr_epi8(b, a, 14);
        __m128i nb = _mm_shuffle_epi8(b, dup7);
        *t0 = ip10_f121(pa, a, na);
        *t1 = ip10_f121(pb, b, nb);
        rt0 = top[0];
    }
    if (have_left) {
        const pixel *lp = rec - 1;
        __m128i l = _mm_cvtsi32_si128(lp[0]);
        l = _mm_insert_epi16(l, lp[stride], 1);
        l = _mm_insert_epi16(l, lp[2 * stride], 2);
        l = _mm_insert_epi16(l, lp[3 * stride], 3);
        l = _mm_insert_epi16(l, lp[4 * stride], 4);
        l = _mm_insert_epi16(l, lp[5 * stride], 5);
        l = _mm_insert_epi16(l, lp[6 * stride], 6);
        l = _mm_insert_epi16(l, lp[7 * stride], 7);
        __m128i pl = have_topleft
            ? _mm_insert_epi16(_mm_slli_si128(l, 2), rtl, 0)
            : _mm_shuffle_epi8(l, dup0);
        *lf = ip10_f121(pl, l, _mm_shuffle_epi8(l, dup7));
        rl0 = lp[0];
    }
    if (!have_topleft)
        return 0;
    if (have_top && have_left) return (rt0 + 2 * rtl + rl0 + 2) >> 2;
    if (have_top)              return (3 * rtl + rt0 + 2) >> 2;
    return (3 * rtl + rl0 + 2) >> 2;
}

/* The flat view { l7..l0, tl, t0..t15, t15, t15, 0... } from those registers;
 * the tail replications fold DDL's endpoint into the ordinary filter. */
static inline void ip10_edge_flat(pixel f[32], __m128i t0, __m128i t1,
                                  __m128i lf, int tl)
{
    ip10_st8(f, _mm_shuffle_epi8(lf, _mm_setr_epi8(14, 15, 12, 13, 10, 11, 8, 9,
                                                   6, 7, 4, 5, 2, 3, 0, 1)));
    f[8] = (pixel)tl;
    ip10_st8(f + 9, t0);
    ip10_st8(f + 17, t1);
    ip10_st8(f + 24, _mm_shuffle_epi8(t1, _mm_setr_epi8(14, 15, 14, 15, 14, 15,
                                                        -1, -1, -1, -1, -1, -1,
                                                        -1, -1, -1, -1)));
}

/* ---- Intra16x16 ------------------------------------------------------------
 * The plane's lane bound is at the top of this file. */
static inline int ip10_hsum8(__m128i v)
{
    v = _mm_madd_epi16(v, _mm_set1_epi16(1));
    v = _mm_add_epi32(v, _mm_shuffle_epi32(v, 0x4e));
    v = _mm_add_epi32(v, _mm_shuffle_epi32(v, 0xb1));
    return _mm_cvtsi128_si32(v);
}

/* the plane parameters a, b, c of 8.3.3.4; both neighbours present */
static inline void ip10_i16_plane_abc(const pixel *rec, int stride, int *a,
                                      int *b, int *c)
{
    const pixel *top = rec - stride;
    int corner = rec[-stride - 1];
    int H = 0, V = 0;
    for (int x = 0; x < 8; x++) {
        int tprev = (6 - x >= 0) ? top[6 - x] : corner;
        H += (x + 1) * (top[8 + x] - tprev);
    }
    for (int y = 0; y < 8; y++) {
        int lprev = (6 - y >= 0) ? rec[(6 - y) * stride - 1] : corner;
        V += (y + 1) * (rec[(8 + y) * stride - 1] - lprev);
    }
    *b = (5 * H + 32) >> 6;
    *c = (5 * V + 32) >> 6;
    *a = 16 * (rec[15 * stride - 1] + top[15]);
}

static inline int ip10_i16_dc(const pixel *rec, int stride, int have_top,
                              int have_left)
{
    int st = 0, sl = 0;
    if (have_top)
        st = ip10_hsum8(_mm_add_epi16(ip10_ld8(rec - stride),
                                      ip10_ld8(rec - stride + 8)));
    if (have_left)
        for (int i = 0; i < 16; i++) sl += rec[i * stride - 1];
    if (have_top && have_left) return (st + sl + 16) >> 5;
    if (have_top)              return (st + 8) >> 4;
    if (have_left)             return (sl + 8) >> 4;
    return 1 << (Y264_BIT_DEPTH - 1);
}

static inline void ip10_i16(pixel pred[256], const pixel *rec, int stride,
                            int mode, int have_top, int have_left)
{
    switch (mode) {
    case Y264_I16_VERT: {
        __m128i t0 = _mm_setzero_si128(), t1 = t0;
        if (have_top) {
            t0 = ip10_ld8(rec - stride);
            t1 = ip10_ld8(rec - stride + 8);
        }
        for (int y = 0; y < 16; y++) {
            ip10_st8(pred + 16 * y, t0);
            ip10_st8(pred + 16 * y + 8, t1);
        }
        return;
    }
    case Y264_I16_HORIZ:
        for (int y = 0; y < 16; y++) {
            __m128i v = _mm_set1_epi16((short)(have_left ? rec[y * stride - 1] : 0));
            ip10_st8(pred + 16 * y, v);
            ip10_st8(pred + 16 * y + 8, v);
        }
        return;
    case Y264_I16_DC: {
        __m128i v = _mm_set1_epi16((short)ip10_i16_dc(rec, stride, have_top,
                                                      have_left));
        for (int y = 0; y < 16; y++) {
            ip10_st8(pred + 16 * y, v);
            ip10_st8(pred + 16 * y + 8, v);
        }
        return;
    }
    case Y264_I16_PLANE: {
        int a, b, c;
        ip10_i16_plane_abc(rec, stride, &a, &b, &c);
        __m128i vb = _mm_set1_epi32(b), vc = _mm_set1_epi32(c);
        __m128i bx0 = _mm_mullo_epi32(_mm_setr_epi32(-7, -6, -5, -4), vb);
        __m128i bx1 = _mm_mullo_epi32(_mm_setr_epi32(-3, -2, -1, 0), vb);
        __m128i bx2 = _mm_mullo_epi32(_mm_setr_epi32(1, 2, 3, 4), vb);
        __m128i bx3 = _mm_mullo_epi32(_mm_setr_epi32(5, 6, 7, 8), vb);
        __m128i base = _mm_set1_epi32(a - 7 * c + 16);
        for (int y = 0; y < 16; y++) {
            ip10_st8(pred + 16 * y, ip10_clip(_mm_add_epi32(base, bx0),
                                              _mm_add_epi32(base, bx1)));
            ip10_st8(pred + 16 * y + 8, ip10_clip(_mm_add_epi32(base, bx2),
                                                  _mm_add_epi32(base, bx3)));
            base = _mm_add_epi32(base, vc);
        }
        return;
    }
    default: break;
    }
}

/* ---- Intra chroma (cw == 8; ch == 8 or 16) ---------------------------------- */

/* the per-4x4 DC of 8.3.4.1 for block row `by`, both columns */
static inline __m128i ip10_ichroma_dc_row(const pixel *top, const pixel *left,
                                          int have_top, int have_left, int by)
{
    int d[2];
    for (int bx = 0; bx < 2; bx++) {
        int st = 0, sl = 0;
        for (int i = 0; i < 4; i++) {
            st += top[bx * 4 + i];
            sl += left[by * 4 + i];
        }
        int dc;
        int both = (bx == 0 && by == 0) || (bx > 0 && by > 0);
        int prefer_top = both ? 1 : (by == 0);
        if (have_top && have_left && both)
            dc = (st + sl + 4) >> 3;
        else if (prefer_top ? have_top : have_left)
            dc = (prefer_top ? (st + 2) : (sl + 2)) >> 2;
        else if (prefer_top ? have_left : have_top)
            dc = (prefer_top ? (sl + 2) : (st + 2)) >> 2;
        else
            dc = 1 << (Y264_BIT_DEPTH - 1);
        d[bx] = dc;
    }
    return _mm_unpacklo_epi64(_mm_set1_epi16((short)d[0]), _mm_set1_epi16((short)d[1]));
}

/* the chroma plane's a, b, c (8.3.4.4, xCF = 0); both neighbours present */
static inline void ip10_ichroma_abc(const pixel *top, const pixel *left,
                                    int corner, int ch, int *a, int *b, int *c)
{
    int yCF = (ch == 16) ? 4 : 0;
    int H = 0, V = 0;
    for (int x = 0; x <= 3; x++) {
        int i = 2 - x;
        int tprev = (i >= 0) ? top[i] : corner;
        H += (x + 1) * (top[4 + x] - tprev);
    }
    for (int y = 0; y <= 3 + yCF; y++) {
        int i = 2 + yCF - y;
        int lprev = (i >= 0) ? left[i] : corner;
        V += (y + 1) * (left[4 + yCF + y] - lprev);
    }
    *b = (34 * H + 32) >> 6;
    *c = ((34 - 29 * (ch == 16)) * V + 32) >> 6;
    *a = 16 * (left[ch - 1] + top[7]);
}

static inline void ip10_ichroma(pixel *pred, const pixel *rec, int stride,
                                int mode, int have_top, int have_left, int ch)
{
    pixel top[8], left[16];
    if (have_top) memcpy(top, rec - stride, sizeof(top));
    else          memset(top, 0, sizeof(top));
    for (int i = 0; i < ch; i++) left[i] = have_left ? rec[i * stride - 1] : 0;
    int corner = (have_top && have_left) ? rec[-stride - 1] : 0;

    switch (mode) {
    case Y264_IC_VERT: {
        __m128i t = ip10_ld8(top);
        for (int y = 0; y < ch; y++) ip10_st8(pred + 8 * y, t);
        return;
    }
    case Y264_IC_HORIZ:
        for (int y = 0; y < ch; y++)
            ip10_st8(pred + 8 * y, _mm_set1_epi16((short)left[y]));
        return;
    case Y264_IC_DC:
        for (int by = 0; by < ch / 4; by++) {
            __m128i v = ip10_ichroma_dc_row(top, left, have_top, have_left, by);
            for (int y = 0; y < 4; y++) ip10_st8(pred + (by * 4 + y) * 8, v);
        }
        return;
    case Y264_IC_PLANE: {
        int a, b, c, yCF = (ch == 16) ? 4 : 0;
        ip10_ichroma_abc(top, left, corner, ch, &a, &b, &c);
        __m128i vb = _mm_set1_epi32(b), vc = _mm_set1_epi32(c);
        __m128i bx0 = _mm_mullo_epi32(_mm_setr_epi32(-3, -2, -1, 0), vb);
        __m128i bx1 = _mm_mullo_epi32(_mm_setr_epi32(1, 2, 3, 4), vb);
        __m128i base = _mm_set1_epi32(a + c * (-3 - yCF) + 16);
        for (int y = 0; y < ch; y++) {
            ip10_st8(pred + 8 * y, ip10_clip(_mm_add_epi32(base, bx0),
                                             _mm_add_epi32(base, bx1)));
            base = _mm_add_epi32(base, vc);
        }
        return;
    }
    default: break;
    }
}

/* ---- the lowres estimate's four base 8x8 predictions -----------------------
 *
 * DC, vertical, horizontal and the plane with a, b, c already derived. The
 * estimate's samples are on the 8-bit scale at every depth, so |a| <= 8160
 * and |b|, |c| <= 1355 and a + 16 + b(x - 3) + c(y - 3) stays inside int16;
 * arguments outside that bound take the scalar form, as on the NEON tier.
 */
static inline void ip10_i8_dvhp(pixel out[4][64], const pixel top[8],
                                const pixel left[8], int dc, int a, int b, int c)
{
    if (abs(a) + 4 * abs(b) + 4 * abs(c) + 16 > 32767) {
        y264_intra8_dvhp_c(out, top, left, dc, a, b, c);
        return;
    }
    __m128i vd = _mm_set1_epi16((short)dc), vt = ip10_ld8(top);
    __m128i vc = _mm_set1_epi16((short)c);
    __m128i r = _mm_add_epi16(_mm_set1_epi16((short)(a + 16 - 3 * c)),
        _mm_mullo_epi16(_mm_setr_epi16(-3, -2, -1, 0, 1, 2, 3, 4),
                        _mm_set1_epi16((short)b)));
    const __m128i zero = _mm_setzero_si128(), vmax = _mm_set1_epi16(PIXEL_MAX);
    for (int y = 0; y < 8; y++) {
        ip10_st8(out[0] + 8 * y, vd);
        ip10_st8(out[1] + 8 * y, vt);
        ip10_st8(out[2] + 8 * y, _mm_set1_epi16((short)left[y]));
        ip10_st8(out[3] + 8 * y,
                 _mm_min_epi16(_mm_max_epi16(_mm_srai_epi16(r, 5), zero), vmax));
        r = _mm_add_epi16(r, vc);
    }
}

#endif /* YAH264_DSP_X86_INTRAPRED_X86_10_H */
