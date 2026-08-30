/*
 * predict_neon.c - aarch64 NEON intra prediction builders (ITU-T H.264 8.3)
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Bit-exact with the _c references in predict.c. The directional 4x4/8x8
 * modes all reduce to two filters over one flattened edge array e[] (built
 * bottom-left -> top-right: l[n-1]..l[0], tl, t[0]..):
 *
 * F[i] = (e[i] + 2*e[i+1] + e[i+2] + 2) >> 2 (the 121 filter)
 * H[i] = (e[i] + e[i+1] + 1) >> 1 (the pairwise average)
 *
 * computed once, vector-wide, via the exact identity
 * (a + 2b + c + 2) >> 2 == ((a + c) >> 1 + b + 1) >> 1
 * (uhadd then urhadd; exact because (2k + eps) >> 2 == k >> 1 for eps in
 * {0,1} with 2k even). Every directional prediction sample is then a table
 * read F[i]/H[i]/e[i] at an index that is a pure function of (mode, x, y) --
 * derived from the reference formulas and verified exhaustively by checkasm
 * against the C builders over all mode x availability combinations.
 */
#if defined(__aarch64__)
#include "predict.h"

#if Y264_BIT_DEPTH == 8
#include <arm_neon.h>
#include <string.h>

/* F over 16 lanes: in points at e[i0]; reads e[i0 .. i0+17]. */
static inline void f121_16(const uint8_t *in, uint8_t *outF, uint8_t *outH)
{
    uint8x16_t a = vld1q_u8(in), b = vld1q_u8(in + 1), c = vld1q_u8(in + 2);
    vst1q_u8(outF, vrhaddq_u8(vhaddq_u8(a, c), b));
    vst1q_u8(outH, vrhaddq_u8(a, b));
}

static inline void f121_8(const uint8_t *in, uint8_t *outF, uint8_t *outH)
{
    uint8x8_t a = vld1_u8(in), b = vld1_u8(in + 1), c = vld1_u8(in + 2);
    vst1_u8(outF, vrhadd_u8(vhadd_u8(a, c), b));
    vst1_u8(outH, vrhadd_u8(a, b));
}

/* ---- Intra8x8 (reference-sample filter + routed modes) ------------------
 * Only the modes the dispatcher routes here (VERT/DDR/VR/HD/VL) are
 * implemented; a 4x4 variant and the remaining 8x8 modes measured slower
 * than the auto-vectorized C and were dropped. */
/* The routed modes, from the flat filtered edge array alone. Split out of
 * y264_intra8x8_neon so a nine-mode decision loop derives that array once
 * instead of once per mode; the loop's other four modes read the same numbers
 * through the C builder's t/l/tl view of the same struct. */
void y264_intra8x8_from_edge_neon(pixel pred[64], const pixel e[32], int mode)
{
    uint8_t Fb[24], Hb[24];

    if (mode == Y264_I4_VERT) {
        for (int y = 0; y < 8; y++) memcpy(pred + 8 * y, e + 9, 8);
        return;
    }

    f121_16(e, Fb, Hb);
    f121_8(e + 16, Fb + 16, Hb + 16);

    switch (mode) {
    case Y264_I4_DDR:                       /* P(x,y) = F[7 + x - y] */
        for (int y = 0; y < 8; y++) memcpy(pred + 8 * y, Fb + 7 - y, 8);
        break;
    case Y264_I4_VR:
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int z = 2 * x - y;
                uint8_t v;
                if (z >= 0) v = (z & 1) ? Fb[7 + x - (y >> 1)]
                                        : Hb[8 + x - (y >> 1)];
                else if (z == -1) v = Fb[7];
                else v = Fb[8 - (y - 2 * x)];
                pred[8 * y + x] = v;
            }
        break;
    case Y264_I4_HD:
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int z = 2 * y - x;
                uint8_t v;
                if (z >= 0) {
                    int i = y - (x >> 1);
                    v = (z & 1) ? Fb[7 - i] : Hb[7 - i];
                } else if (z == -1) v = Fb[7];
                else v = Fb[6 + x - 2 * y];
                pred[8 * y + x] = v;
            }
        break;
    case Y264_I4_VL:                        /* even rows H, odd rows F */
        for (int y = 0; y < 8; y++)
            memcpy(pred + 8 * y, ((y & 1) ? Fb : Hb) + 9 + (y >> 1), 8);
        break;
    default: break;
    }
}

void y264_intra8x8_neon(pixel pred[64], const pixel *rec, int stride,
                        int mode, int have_top, int have_left,
                        int have_topleft, int have_topright)
{
    /* raw samples */
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

    /* 8.3.2.2.1 low-pass -> filtered t[16], l[8], tl (all in one edge array
 * e = { l7..l0, tl, t0..t15, t15, t15 }). The endpoint specials fold into
 * the same 121 filter via source replication. */
    uint8_t e[32];
    memset(e, 0, sizeof(e));
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
        uint8x16_t v0 = vld1q_u8(a), v1 = vld1q_u8(a + 1), v2 = vld1q_u8(a + 2);
        vst1q_u8(e + 9, vrhaddq_u8(vhaddq_u8(v0, v2), v1));   /* t[0..15] */
    }
    if (have_left) {
        uint8_t al[12];
        al[0] = have_topleft ? (uint8_t)rtl : rl[0];
        memcpy(al + 1, rl, 8);
        al[9] = rl[7]; al[10] = al[11] = 0;
        uint8x8_t v0 = vld1_u8(al), v1 = vld1_u8(al + 1), v2 = vld1_u8(al + 2);
        uint8_t lf[8];
        vst1_u8(lf, vrhadd_u8(vhadd_u8(v0, v2), v1));         /* l[0..7] */
        for (int i = 0; i < 8; i++) e[7 - i] = lf[i];
    }
    e[25] = e[26] = e[24];               /* t15 replication for DDL's tail */

    y264_intra8x8_from_edge_neon(pred, e, mode);
}

/* ---- Intra16x16 --------------------------------------------------------- */
void y264_intra16x16_neon(pixel pred[256], const pixel *rec, int stride,
                          int mode, int have_top, int have_left)
{
    uint8_t left[16];
    for (int i = 0; i < 16; i++)
        left[i] = have_left ? rec[i * stride - 1] : 0;

    switch (mode) {
    case Y264_I16_VERT: {
        uint8x16_t top = have_top ? vld1q_u8(rec - stride) : vdupq_n_u8(0);
        for (int y = 0; y < 16; y++) vst1q_u8(pred + 16 * y, top);
        return;
    }
    case Y264_I16_HORIZ:
        for (int y = 0; y < 16; y++)
            vst1q_u8(pred + 16 * y, vdupq_n_u8(left[y]));
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
        for (int y = 0; y < 16; y++) vst1q_u8(pred + 16 * y, vdupq_n_u8((uint8_t)dc));
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
        /* row value = clip8((a + b*(x-7) + c*(y-7) + 16) >> 5); every sum
 * stays in int16 (|a| <= 8160, |b*8|,|c*8| <= 5736). */
        int16_t bx[16];
        for (int x = 0; x < 16; x++) bx[x] = (int16_t)(b * (x - 7));
        int16x8_t bx0 = vld1q_s16(bx), bx1 = vld1q_s16(bx + 8);
        for (int y = 0; y < 16; y++) {
            int16x8_t base = vdupq_n_s16((int16_t)(a + c * (y - 7) + 16));
            uint8x8_t lo = vqmovun_s16(vshrq_n_s16(vaddq_s16(base, bx0), 5));
            uint8x8_t hi = vqmovun_s16(vshrq_n_s16(vaddq_s16(base, bx1), 5));
            vst1q_u8(pred + 16 * y, vcombine_u8(lo, hi));
        }
        return;
    }
    default: break;
    }
}

/* ---- Intra chroma (cw == 8; ch == 8 or 16) ------------------------------ */
void y264_intra_chroma_neon(pixel *pred, const pixel *rec, int stride,
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
        uint8x8_t t = vld1_u8(top);
        for (int y = 0; y < ch; y++) vst1_u8(pred + 8 * y, t);
        return;
    }
    case Y264_IC_HORIZ:
        for (int y = 0; y < ch; y++)
            vst1_u8(pred + 8 * y, vdup_n_u8(left[y]));
        return;
    case Y264_IC_DC:
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
        int16x8_t bxv = vld1q_s16(bx);
        for (int y = 0; y < ch; y++) {
            int16x8_t base = vdupq_n_s16((int16_t)(a + c * (y - 3 - yCF) + 16));
            vst1_u8(pred + 8 * y,
                    vqmovun_s16(vshrq_n_s16(vaddq_s16(base, bxv), 5)));
        }
        return;
    }
    default: break;
    }
}

#endif /* Y264_BIT_DEPTH == 8 */
#endif /* __aarch64__ */
