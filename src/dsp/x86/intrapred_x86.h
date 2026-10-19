/*
 * intrapred_x86.h - the building blocks the x86 intra prediction kernels share
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Wave 3b of docs/simd.md. Every helper here is `static inline` and
 * 128-bit, so the SSE4.2 file gets SSE encodings and the AVX2 file gets VEX
 * encodings of the same source; the AVX2 file uses several of them whole,
 * because an 8x8 prediction row and an eight-wide chroma row have no second
 * half for a 256-bit register to hold.
 *
 * THREE FACTS CARRY THIS FILE.
 *
 * THE 121 FILTER IS TWO AVERAGES. Every directional 8x8 mode reduces to two
 * filters over one flattened edge array e[] (built bottom-left to top-right:
 * l[7]..l[0], tl, t[0]..t[15]):
 *
 *     F[i] = (e[i] + 2*e[i+1] + e[i+2] + 2) >> 2
 *     H[i] = (e[i] + e[i+1] + 1) >> 1
 *
 * and the identity (a + 2b + c + 2) >> 2 == (((a + c) >> 1) + b + 1) >> 1 is
 * exact, because (2k + eps) >> 2 == k >> 1 for eps in {0, 1} and 2k even. H is
 * PAVGB outright. The inner one is the TRUNCATING halving add, which x86 does
 * not have as an instruction and which PAVGB is emphatically not: it is
 * (a & c) + ((a ^ c) >> 1), three operations over bytes, with the shift done
 * 16 bits at a time and masked because there is no byte shift either. Both
 * filters then run over the whole edge at once, sixteen positions per
 * register, and every prediction sample is a table read.
 *
 * THE TABLE READS ARE ONE PSHUFB. Each directional sample is F[i], H[i] or
 * e[i] at an index that is a pure function of (mode, x, y), and for VR and HD
 * every index a row needs falls inside the first SIXTEEN entries of F and of
 * H. So a row is two PSHUFBs of one F register and one H register, OR-ed
 * together: the shuffle control's high bit zeroes the lanes the other source
 * owns, so the merge needs no blend and no mask register. The eight controls
 * per mode are generated from the reference formulas and checked exhaustively
 * against the C builder by checkasm, over every mode and availability
 * combination. The other three routed modes need no shuffle at all -- VERT,
 * DDR and VL each read eight CONSECUTIVE entries per row.
 *
 * THE PLANE MODES ARE ONE SATURATING PACK PER ROW. The prediction is
 * clip1((a + b*(x - o) + c*(y - o) + 16) >> 5) and every term stays inside a
 * signed 16-bit lane (|a| <= 8160, |b*8| and |c*8| <= 5736), so b*(x - o) is a
 * constant vector computed once, the row's scalar part is a broadcast, and
 * Clip1 is PACKUSWB -- which saturates signed 16-bit to unsigned 8-bit at both
 * ends, exactly the clip.
 *
 * WHAT IS NOT HERE. The 4x4 builder: the edge-filter precompute does not
 * amortize over sixteen samples and the NEON twin measured a net loss, which
 * is an arithmetic fact about the shape rather than one about the instruction
 * set. The four 8x8 modes the dispatcher leaves in C (HORIZ, DC, DDL, HU) are
 * left in C here too, for the same reason and by the same routing table.
 *
 * C11 intrinsics only. There is no assembly in this tree, and hygiene_check
 * refuses any inline form under this directory.
 */
#ifndef YAH264_DSP_X86_PREDICT_X86_H
#define YAH264_DSP_X86_PREDICT_X86_H

#include <immintrin.h>
#include <string.h>

#include "dsp/x86/metrics_x86.h"   /* the block-sized loads, and dsp/arch.h */
#include "dsp/intrapred.h"

/* (a + c) >> 1 over unsigned bytes, TRUNCATING. PAVGB rounds, which is the
 * wrong filter here; this is the standard identity, with the halving done in
 * 16-bit lanes and the borrowed bit masked off because there is no PSRLB. */
static inline __m128i y264_hadd_u8(__m128i a, __m128i c)
{
    __m128i x = _mm_srli_epi16(_mm_xor_si128(a, c), 1);
    return _mm_add_epi8(_mm_and_si128(a, c),
                        _mm_and_si128(x, _mm_set1_epi8(0x7f)));
}

/* F and H over sixteen positions; `in` addresses e[i0] and the read is
 * e[i0 .. i0+17]. */
static inline void y264_f121_16(const pixel *in, uint8_t *oF, uint8_t *oH)
{
    __m128i a = y264_ld128(in), b = y264_ld128(in + 1), c = y264_ld128(in + 2);
    _mm_storeu_si128((__m128i *)(void *)oF,
                     _mm_avg_epu8(y264_hadd_u8(a, c), b));
    _mm_storeu_si128((__m128i *)(void *)oH, _mm_avg_epu8(a, b));
}

/* The same over eight, reading e[i0 .. i0+9]. */
static inline void y264_f121_8(const pixel *in, uint8_t *oF, uint8_t *oH)
{
    __m128i a = y264_ld64(in), b = y264_ld64(in + 1), c = y264_ld64(in + 2);
    _mm_storel_epi64((__m128i *)(void *)oF,
                     _mm_avg_epu8(y264_hadd_u8(a, c), b));
    _mm_storel_epi64((__m128i *)(void *)oH, _mm_avg_epu8(a, b));
}

/* ---- the VR and HD shuffle controls -------------------------------------- *
 *
 * One pair per row: the F control names the F entries that row reads and 0x80
 * (written -1) everywhere H owns the sample, and the H control is its
 * complement. PSHUFB writes zero wherever the control's high bit is set, so
 * OR-ing the two results is the merge. Derived from 8.3.2.2 exactly as the
 * scalar formulas in intrapred.c are, and checked against them by checkasm.
 */
static const int8_t y264_i8_vr_f[8][16] = {
    { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
    {  7,  8,  9, 10, 11, 12, 13, 14, -1, -1, -1, -1, -1, -1, -1, -1 },
    {  6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
    {  5,  7,  8,  9, 10, 11, 12, 13, -1, -1, -1, -1, -1, -1, -1, -1 },
    {  4,  6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
    {  3,  5,  7,  8,  9, 10, 11, 12, -1, -1, -1, -1, -1, -1, -1, -1 },
    {  2,  4,  6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
    {  1,  3,  5,  7,  8,  9, 10, 11, -1, -1, -1, -1, -1, -1, -1, -1 },
};
static const int8_t y264_i8_vr_h[8][16] = {
    {  8,  9, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1 },
    { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
    { -1,  8,  9, 10, 11, 12, 13, 14, -1, -1, -1, -1, -1, -1, -1, -1 },
    { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
    { -1, -1,  8,  9, 10, 11, 12, 13, -1, -1, -1, -1, -1, -1, -1, -1 },
    { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
    { -1, -1, -1,  8,  9, 10, 11, 12, -1, -1, -1, -1, -1, -1, -1, -1 },
    { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
};
static const int8_t y264_i8_hd_f[8][16] = {
    { -1,  7,  8,  9, 10, 11, 12, 13, -1, -1, -1, -1, -1, -1, -1, -1 },
    { -1,  6, -1,  7,  8,  9, 10, 11, -1, -1, -1, -1, -1, -1, -1, -1 },
    { -1,  5, -1,  6, -1,  7,  8,  9, -1, -1, -1, -1, -1, -1, -1, -1 },
    { -1,  4, -1,  5, -1,  6, -1,  7, -1, -1, -1, -1, -1, -1, -1, -1 },
    { -1,  3, -1,  4, -1,  5, -1,  6, -1, -1, -1, -1, -1, -1, -1, -1 },
    { -1,  2, -1,  3, -1,  4, -1,  5, -1, -1, -1, -1, -1, -1, -1, -1 },
    { -1,  1, -1,  2, -1,  3, -1,  4, -1, -1, -1, -1, -1, -1, -1, -1 },
    { -1,  0, -1,  1, -1,  2, -1,  3, -1, -1, -1, -1, -1, -1, -1, -1 },
};
static const int8_t y264_i8_hd_h[8][16] = {
    {  7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
    {  6, -1,  7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
    {  5, -1,  6, -1,  7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
    {  4, -1,  5, -1,  6, -1,  7, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
    {  3, -1,  4, -1,  5, -1,  6, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
    {  2, -1,  3, -1,  4, -1,  5, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
    {  1, -1,  2, -1,  3, -1,  4, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
    {  0, -1,  1, -1,  2, -1,  3, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
};

static inline void y264_i8_rows_shuf(pixel pred[64], __m128i F, __m128i H,
                                     const int8_t mf[8][16],
                                     const int8_t mh[8][16])
{
    for (int y = 0; y < 8; y++) {
        __m128i a = _mm_shuffle_epi8(F, y264_ld128((const uint8_t *)mf[y]));
        __m128i b = _mm_shuffle_epi8(H, y264_ld128((const uint8_t *)mh[y]));
        _mm_storel_epi64((__m128i *)(void *)(pred + 8 * y),
                         _mm_or_si128(a, b));
    }
}

/* ---- Intra8x8 from a precomputed edge ------------------------------------ */
static inline void y264_i8_from_edge_x86(pixel pred[64], const pixel e[32],
                                         int mode)
{
    uint8_t Fb[24], Hb[24];

    if (mode == Y264_I4_VERT) {
        __m128i t = y264_ld64(e + 9);
        for (int y = 0; y < 8; y++)
            _mm_storel_epi64((__m128i *)(void *)(pred + 8 * y), t);
        return;
    }

    y264_f121_16(e, Fb, Hb);
    y264_f121_8(e + 16, Fb + 16, Hb + 16);

    switch (mode) {
    case Y264_I4_DDR:                       /* P(x,y) = F[7 + x - y] */
        for (int y = 0; y < 8; y++)
            _mm_storel_epi64((__m128i *)(void *)(pred + 8 * y),
                             y264_ld64(Fb + 7 - y));
        break;
    case Y264_I4_VR:
        y264_i8_rows_shuf(pred, y264_ld128(Fb), y264_ld128(Hb),
                          y264_i8_vr_f, y264_i8_vr_h);
        break;
    case Y264_I4_HD:
        y264_i8_rows_shuf(pred, y264_ld128(Fb), y264_ld128(Hb),
                          y264_i8_hd_f, y264_i8_hd_h);
        break;
    case Y264_I4_VL:                        /* even rows H, odd rows F */
        for (int y = 0; y < 8; y++)
            _mm_storel_epi64((__m128i *)(void *)(pred + 8 * y),
                             y264_ld64(((y & 1) ? Fb : Hb) + 9 + (y >> 1)));
        break;
    default: break;
    }
}

/* ---- Intra8x8: the 8.3.2.2.1 reference filter, then the builder ---------- *
 *
 * The filtered edge is the SAME 121 filter, with the two endpoint specials
 * folded into it by replicating the source: t[0] reads the corner (or its own
 * first sample when there is no corner) and t[15] reads itself, so no lane is
 * a special case.
 */
static inline void y264_i8_edge_x86(uint8_t e[32], const pixel *rec, int stride,
                                    int have_top, int have_left,
                                    int have_topleft, int have_topright)
{
    uint8_t rt[16], rl[8];
    int rtl = 0;
    for (int i = 0; i < 8; i++) {
        rt[i] = have_top ? rec[-stride + i] : 0;
        rl[i] = have_left ? rec[i * stride - 1] : 0;
    }
    for (int i = 8; i < 16; i++)
        rt[i] = have_topright ? rec[-stride + i] : rt[7];
    if (have_topleft)
        rtl = rec[-stride - 1];

    memset(e, 0, 32);
    int tl = 0;
    if (have_topleft) {
        if (have_top && have_left) tl = (rt[0] + 2 * rtl + rl[0] + 2) >> 2;
        else if (have_top)         tl = (3 * rtl + rt[0] + 2) >> 2;
        else                       tl = (3 * rtl + rl[0] + 2) >> 2;
    }
    e[8] = (uint8_t)tl;
    if (have_top) {
        uint8_t a[20];
        a[0] = have_topleft ? (uint8_t)rtl : rt[0];
        memcpy(a + 1, rt, 16);
        a[17] = rt[15]; a[18] = a[19] = 0;
        __m128i v0 = y264_ld128(a), v1 = y264_ld128(a + 1), v2 = y264_ld128(a + 2);
        _mm_storeu_si128((__m128i *)(void *)(e + 9),
                         _mm_avg_epu8(y264_hadd_u8(v0, v2), v1));
    }
    if (have_left) {
        uint8_t al[12];
        al[0] = have_topleft ? (uint8_t)rtl : rl[0];
        memcpy(al + 1, rl, 8);
        al[9] = rl[7]; al[10] = al[11] = 0;
        __m128i v0 = y264_ld64(al), v1 = y264_ld64(al + 1), v2 = y264_ld64(al + 2);
        __m128i lf = _mm_avg_epu8(y264_hadd_u8(v0, v2), v1);
        /* e[7 - i] = l[i]: the eight filtered left samples, reversed. */
        _mm_storel_epi64((__m128i *)(void *)e,
            _mm_shuffle_epi8(lf, _mm_setr_epi8(7, 6, 5, 4, 3, 2, 1, 0,
                                               -1, -1, -1, -1, -1, -1, -1, -1)));
    }
    e[25] = e[26] = e[24];               /* t15 replication for DDL's tail */
}

/* ---- Intra16x16 ---------------------------------------------------------- */
static inline void y264_i16_x86(pixel pred[256], const pixel *rec, int stride,
                                int mode, int have_top, int have_left)
{
    uint8_t left[16];
    for (int i = 0; i < 16; i++)
        left[i] = have_left ? rec[i * stride - 1] : 0;

    switch (mode) {
    case Y264_I16_VERT: {
        __m128i top = have_top ? y264_ld128(rec - stride) : _mm_setzero_si128();
        for (int y = 0; y < 16; y++)
            _mm_storeu_si128((__m128i *)(void *)(pred + 16 * y), top);
        return;
    }
    case Y264_I16_HORIZ:
        for (int y = 0; y < 16; y++)
            _mm_storeu_si128((__m128i *)(void *)(pred + 16 * y),
                             _mm_set1_epi8((char)left[y]));
        return;
    case Y264_I16_DC: {
        int st = 0, sl = 0;
        for (int i = 0; i < 16; i++) {
            st += have_top ? rec[-stride + i] : 0;
            sl += left[i];
        }
        int dc;
        if (have_top && have_left) dc = (st + sl + 16) >> 5;
        else if (have_top)         dc = (st + 8) >> 4;
        else if (have_left)        dc = (sl + 8) >> 4;
        else                       dc = 1 << (Y264_BIT_DEPTH - 1);
        __m128i v = _mm_set1_epi8((char)dc);
        for (int y = 0; y < 16; y++)
            _mm_storeu_si128((__m128i *)(void *)(pred + 16 * y), v);
        return;
    }
    case Y264_I16_PLANE: {
        /* only reached with both neighbours available (mode gating) */
        const pixel *top = rec - stride;
        int corner = rec[-stride - 1];
        int H = 0, Vv = 0;
        for (int x = 0; x < 8; x++) {
            int tprev = (6 - x >= 0) ? top[6 - x] : corner;
            H += (x + 1) * (top[8 + x] - tprev);
        }
        for (int y = 0; y < 8; y++) {
            int lprev = (6 - y >= 0) ? left[6 - y] : corner;
            Vv += (y + 1) * (left[8 + y] - lprev);
        }
        int b = (5 * H + 32) >> 6;
        int c = (5 * Vv + 32) >> 6;
        int a = 16 * (left[15] + top[15]);
        int16_t bx[16];
        for (int x = 0; x < 16; x++) bx[x] = (int16_t)(b * (x - 7));
        __m128i bx0 = _mm_loadu_si128((const __m128i *)(const void *)bx);
        __m128i bx1 = _mm_loadu_si128((const __m128i *)(const void *)(bx + 8));
        for (int y = 0; y < 16; y++) {
            __m128i base = _mm_set1_epi16((short)(a + c * (y - 7) + 16));
            __m128i lo = _mm_srai_epi16(_mm_add_epi16(base, bx0), 5);
            __m128i hi = _mm_srai_epi16(_mm_add_epi16(base, bx1), 5);
            _mm_storeu_si128((__m128i *)(void *)(pred + 16 * y),
                             _mm_packus_epi16(lo, hi));
        }
        return;
    }
    default: break;
    }
}

/* ---- Intra chroma (cw == 8; ch == 8 or 16) ------------------------------- */
static inline void y264_ichroma_x86(pixel *pred, const pixel *rec, int stride,
                                    int mode, int have_top, int have_left,
                                    int cw, int ch)
{
    uint8_t top[8], left[16];
    (void)cw;                                   /* dispatcher guarantees 8 */
    for (int i = 0; i < 8; i++) top[i] = have_top ? rec[-stride + i] : 0;
    for (int i = 0; i < ch; i++) left[i] = have_left ? rec[i * stride - 1] : 0;
    int corner = (have_top && have_left) ? rec[-stride - 1] : 0;

    switch (mode) {
    case Y264_IC_VERT: {
        __m128i t = y264_ld64(top);
        for (int y = 0; y < ch; y++)
            _mm_storel_epi64((__m128i *)(void *)(pred + 8 * y), t);
        return;
    }
    case Y264_IC_HORIZ:
        for (int y = 0; y < ch; y++)
            _mm_storel_epi64((__m128i *)(void *)(pred + 8 * y),
                             _mm_set1_epi8((char)left[y]));
        return;
    case Y264_IC_DC:
        /* Each 4x4 chroma block gets its own DC (8.3.4.1), so the block
         * selection is the arithmetic and the fill is four stores of four. */
        for (int by = 0; by < ch / 4; by++)
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
                for (int y = 0; y < 4; y++)
                    memset(pred + (by * 4 + y) * 8 + bx * 4, dc, 4);
            }
        return;
    case Y264_IC_PLANE: {
        int yCF = (ch == 16) ? 4 : 0;
        int H = 0, Vv = 0;
        for (int x = 0; x <= 3; x++) {
            int i = 2 - x;
            int tprev = (i >= 0) ? top[i] : corner;
            H += (x + 1) * (top[4 + x] - tprev);
        }
        for (int y = 0; y <= 3 + yCF; y++) {
            int i = 2 + yCF - y;
            int lprev = (i >= 0) ? left[i] : corner;
            Vv += (y + 1) * (left[4 + yCF + y] - lprev);
        }
        int b = (34 * H + 32) >> 6;
        int c = ((34 - 29 * (ch == 16)) * Vv + 32) >> 6;
        int a = 16 * (left[ch - 1] + top[7]);
        int16_t bx[8];
        for (int x = 0; x < 8; x++) bx[x] = (int16_t)(b * (x - 3));
        __m128i bxv = _mm_loadu_si128((const __m128i *)(const void *)bx);
        for (int y = 0; y < ch; y++) {
            __m128i base = _mm_set1_epi16((short)(a + c * (y - 3 - yCF) + 16));
            __m128i r = _mm_srai_epi16(_mm_add_epi16(base, bxv), 5);
            _mm_storel_epi64((__m128i *)(void *)(pred + 8 * y),
                             _mm_packus_epi16(r, r));
        }
        return;
    }
    default: break;
    }
}

/* ---- the filtered edge of one Intra8x8 block, as registers ---------------- *
 *
 * y264_i8_edge_x86 above stages the raw samples through byte arrays, which is
 * fine once per prediction and too much once per BLOCK of a decision loop
 * that asks for nothing else. This form keeps them in registers: the top row
 * is one load (eight samples replicated out to sixteen when there is no top
 * right), the left column is eight byte inserts, and the two neighbours each
 * filter tap needs are a byte shift of the same register with the endpoint
 * replicated, so 8.3.2.2.1's endpoint rules are still the ordinary 121 filter
 * and no lane is special.
 *
 * Returns the filtered corner and leaves the filtered top row (t[0..15]) in
 * *tf and the filtered left column (l[0..7], low half) in *lf, zero for a side
 * that is not available -- exactly the numbers y264_intra8x8_edge_c stores.
 * Nothing outside the available neighbours is read: eight top samples without
 * a top right, the corner only with a top left.
 */
static inline int y264_i8_edge_regs(const pixel *rec, int stride, int have_top,
                                    int have_left, int have_topleft,
                                    int have_topright, __m128i *tf, __m128i *lf)
{
    const __m128i z = _mm_setzero_si128();
    const __m128i dup0 = _mm_setr_epi8(0, 0, 1, 2, 3, 4, 5, 6,
                                       7, 8, 9, 10, 11, 12, 13, 14);
    int rtl = have_topleft ? rec[-stride - 1] : 0;
    int rt0 = 0, rl0 = 0;
    *tf = z;
    *lf = z;
    if (have_top) {
        const pixel *top = rec - stride;
        __m128i rt = have_topright
            ? y264_ld128(top)
            : _mm_shuffle_epi8(y264_ld64(top),
                               _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7,
                                             7, 7, 7, 7, 7, 7, 7, 7));
        __m128i prev = have_topleft
            ? _mm_or_si128(_mm_slli_si128(rt, 1), _mm_cvtsi32_si128(rtl))
            : _mm_shuffle_epi8(rt, dup0);
        __m128i next = _mm_shuffle_epi8(rt,
            _mm_setr_epi8(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 15));
        *tf = _mm_avg_epu8(y264_hadd_u8(prev, next), rt);
        rt0 = top[0];
    }
    if (have_left) {
        const pixel *lp = rec - 1;
        __m128i rl = _mm_cvtsi32_si128(lp[0]);
        rl = _mm_insert_epi8(rl, lp[stride], 1);
        rl = _mm_insert_epi8(rl, lp[2 * stride], 2);
        rl = _mm_insert_epi8(rl, lp[3 * stride], 3);
        rl = _mm_insert_epi8(rl, lp[4 * stride], 4);
        rl = _mm_insert_epi8(rl, lp[5 * stride], 5);
        rl = _mm_insert_epi8(rl, lp[6 * stride], 6);
        rl = _mm_insert_epi8(rl, lp[7 * stride], 7);
        __m128i prev = have_topleft
            ? _mm_or_si128(_mm_slli_si128(rl, 1), _mm_cvtsi32_si128(rtl))
            : _mm_shuffle_epi8(rl, dup0);
        __m128i next = _mm_shuffle_epi8(rl,
            _mm_setr_epi8(1, 2, 3, 4, 5, 6, 7, 7, -1, -1, -1, -1, -1, -1, -1, -1));
        /* the upper half is zeroed so the widening stores of both tiers can
         * take it as it is */
        *lf = _mm_move_epi64(_mm_avg_epu8(y264_hadd_u8(prev, next), rl));
        rl0 = lp[0];
    }
    if (!have_topleft)
        return 0;
    if (have_top && have_left) return (rt0 + 2 * rtl + rl0 + 2) >> 2;
    if (have_top)              return (3 * rtl + rt0 + 2) >> 2;
    return (3 * rtl + rl0 + 2) >> 2;
}

/* The flat view { l7..l0, tl, t0..t15, t15, t15, 0... } from those registers.
 * The left column goes in reversed; the two tail replications fold DDL's
 * endpoint into the ordinary filter, as in the scalar builder. */
static inline void y264_i8_edge_flat(pixel f[32], __m128i tf, __m128i lf, int tl)
{
    const __m128i z = _mm_setzero_si128();
    _mm_storeu_si128((__m128i *)(void *)f, z);
    _mm_storeu_si128((__m128i *)(void *)(f + 16), z);
    _mm_storel_epi64((__m128i *)(void *)f,
        _mm_shuffle_epi8(lf, _mm_setr_epi8(7, 6, 5, 4, 3, 2, 1, 0,
                                           -1, -1, -1, -1, -1, -1, -1, -1)));
    f[8] = (pixel)tl;
    _mm_storeu_si128((__m128i *)(void *)(f + 9), tf);
    f[25] = f[26] = (pixel)_mm_extract_epi8(tf, 15);
}

/* ---- the six directional modes from one filter pass ----------------------- *
 *
 * DDL, DDR, VR, HD, VL and HU in mode order, both edges present. Every sample
 * of every one of them is F[i], H[i], the bottom-left sample or HU's corner
 * (y264_intra8x8_dir6_idx in intrapred.c is the map), and each mode draws on at
 * most two sixteen-entry windows of F and H:
 *
 *   FLO = F[0..15]   FHI = F[9..24]   HLO = H[0..15]   HHI = H[9..24]
 *
 * so two output rows are one PSHUFB per window, OR-ed where there are two.
 * HU is the one that also wants the two specials, and it reads only F[0..5]
 * and H[0..6] beside them, so those fifteen bytes are gathered into a window
 * of their own (S) and HU is a single shuffle like DDL and DDR.
 *
 * The controls below are generated from y264_intra8x8_dir6_idx: one row of 64
 * per (mode, window), the byte of the window each output sample reads, or -1
 * where the other window owns it. checkasm holds every lane of them to the C
 * builder.
 */
static const int8_t y264_i8_dir6_shuf[9][64] = {
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
    Y264_D6_DDL = 0, Y264_D6_DDR, Y264_D6_VR_F, Y264_D6_VR_H,
    Y264_D6_HD_F, Y264_D6_HD_H, Y264_D6_VL_F, Y264_D6_VL_H, Y264_D6_HU
};

/* HU's window: F[0..5], H[0..6], the bottom-left sample, the corner
 * (l6 + 3*l7 + 2) >> 2, and a spare byte nothing reads. */
static inline __m128i y264_i8_hu_window(__m128i flo, __m128i hlo, const pixel e[32])
{
    __m128i s = _mm_or_si128(
        _mm_and_si128(flo, _mm_setr_epi8(-1, -1, -1, -1, -1, -1, 0, 0,
                                         0, 0, 0, 0, 0, 0, 0, 0)),
        _mm_slli_si128(hlo, 6));
    s = _mm_insert_epi8(s, e[0], 13);
    return _mm_insert_epi8(s, (e[1] + 3 * e[0] + 2) >> 2, 14);
}

#define Y264_D6_CTL(row, off) \
    y264_ld128((const uint8_t *)(const void *)(y264_i8_dir6_shuf[row] + (off)))

static inline void y264_i8_dir6_x86(pixel out[6][64], const pixel e[32])
{
    __m128i a0 = y264_ld128(e), b0 = y264_ld128(e + 1), c0 = y264_ld128(e + 2);
    __m128i a1 = y264_ld128(e + 9), b1 = y264_ld128(e + 10), c1 = y264_ld128(e + 11);
    __m128i flo = _mm_avg_epu8(y264_hadd_u8(a0, c0), b0);
    __m128i fhi = _mm_avg_epu8(y264_hadd_u8(a1, c1), b1);
    __m128i hlo = _mm_avg_epu8(a0, b0);
    __m128i hhi = _mm_avg_epu8(a1, b1);
    __m128i hu = y264_i8_hu_window(flo, hlo, e);

    for (int k = 0; k < 64; k += 16) {
        _mm_storeu_si128((__m128i *)(void *)(out[0] + k),
            _mm_shuffle_epi8(fhi, Y264_D6_CTL(Y264_D6_DDL, k)));
        _mm_storeu_si128((__m128i *)(void *)(out[1] + k),
            _mm_shuffle_epi8(flo, Y264_D6_CTL(Y264_D6_DDR, k)));
        _mm_storeu_si128((__m128i *)(void *)(out[2] + k), _mm_or_si128(
            _mm_shuffle_epi8(flo, Y264_D6_CTL(Y264_D6_VR_F, k)),
            _mm_shuffle_epi8(hlo, Y264_D6_CTL(Y264_D6_VR_H, k))));
        _mm_storeu_si128((__m128i *)(void *)(out[3] + k), _mm_or_si128(
            _mm_shuffle_epi8(flo, Y264_D6_CTL(Y264_D6_HD_F, k)),
            _mm_shuffle_epi8(hlo, Y264_D6_CTL(Y264_D6_HD_H, k))));
        _mm_storeu_si128((__m128i *)(void *)(out[4] + k), _mm_or_si128(
            _mm_shuffle_epi8(fhi, Y264_D6_CTL(Y264_D6_VL_F, k)),
            _mm_shuffle_epi8(hhi, Y264_D6_CTL(Y264_D6_VL_H, k))));
        _mm_storeu_si128((__m128i *)(void *)(out[5] + k),
            _mm_shuffle_epi8(hu, Y264_D6_CTL(Y264_D6_HU, k)));
    }
}

/* ---- the lowres estimate's four base 8x8 predictions ---------------------- *
 *
 * DC (a fill), vertical (the top row), horizontal (each row its left sample)
 * and the chroma-style plane with a, b, c already derived:
 * Clip1((a + b*(x-3) + c*(y-3) + 16) >> 5). Two rows per store. The plane
 * terms stay inside a signed 16-bit lane for 8-bit samples (|a| <= 8160 and
 * |b|, |c| <= 1355, so the sum is within +-19100) and the saturating pack is
 * the clip.
 */
static inline void y264_i8_dvhp_x86(pixel out[4][64], const pixel top[8],
                                    const pixel left[8], int dc, int a, int b,
                                    int c)
{
    __m128i vd = _mm_set1_epi8((char)dc);
    __m128i vt = y264_ld64(top);
    __m128i vl = y264_ld64(left);
    __m128i vc = _mm_set1_epi16((short)c);
    __m128i r = _mm_add_epi16(_mm_set1_epi16((short)(a + 16 - 3 * c)),
        _mm_mullo_epi16(_mm_setr_epi16(-3, -2, -1, 0, 1, 2, 3, 4),
                        _mm_set1_epi16((short)b)));
    __m128i rep = _mm_setr_epi8(0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1);
    const __m128i two = _mm_set1_epi8(2);
    vt = _mm_unpacklo_epi64(vt, vt);
    for (int k = 0; k < 64; k += 16) {
        __m128i r1 = _mm_add_epi16(r, vc);
        _mm_storeu_si128((__m128i *)(void *)(out[0] + k), vd);
        _mm_storeu_si128((__m128i *)(void *)(out[1] + k), vt);
        _mm_storeu_si128((__m128i *)(void *)(out[2] + k), _mm_shuffle_epi8(vl, rep));
        _mm_storeu_si128((__m128i *)(void *)(out[3] + k),
            _mm_packus_epi16(_mm_srai_epi16(r, 5), _mm_srai_epi16(r1, 5)));
        r = _mm_add_epi16(r1, vc);
        rep = _mm_add_epi8(rep, two);
    }
}

#endif /* YAH264_DSP_X86_PREDICT_X86_H */
