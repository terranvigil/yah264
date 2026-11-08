/*
 * intrapred_neon10.c - aarch64 NEON intra prediction for the 10-bit library
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The 10-bit twins of src/dsp/intrapred_neon.c, under the 8-bit kernels'
 * names (src/common/symbols10.h suffixes them): 16x16, 8-wide chroma, the
 * routed 8x8 modes, the six-mode 8x8 decision form and the lowres four-mode
 * builder. A sample is a u16 lane.
 *
 * The 8x8 edge filter keeps the 8-bit kernels' halving-add identity,
 * (a + 2b + c + 2) >> 2 == (((a + c) >> 1) + b + 1) >> 1, which holds on any
 * unsigned lane. The directional modes read a 64-entry table of filtered
 * samples through the index rows of y264_intra8x8_dir6_idx; a byte lookup
 * reaches 64 entries, so the u16 table is split into its low and high bytes,
 * looked up with the same indices and zipped back.
 *
 * The plane modes are where 10 bits change the arithmetic. At 16x16,
 * |a| <= 16 * 2046 = 32736 and |b|, |c| <= (5 * 36828 + 32) >> 6 = 2877, so a
 * sample's sum reaches 32736 + 2 * 8 * 2877 + 16 = 78784; 8-wide chroma
 * reaches 32736 + 4 * 5435 + 8 * 2877 + 16 = 77508. Both run on int32 lanes
 * where the 8-bit kernels keep int16.
 */
#if defined(__aarch64__)
#include "../intrapred.h"
#include "../arch.h"
#include <arm_neon.h>
#include <stdlib.h>
#include <string.h>

#if Y264_BIT_DEPTH != 10
#error "intrapred_neon10.c is the 10-bit library's NEON tier"
#endif

/* F and H over eight lanes from in[0 .. 9]. */
static inline void f121_8(const pixel *in, uint16x8_t *f, uint16x8_t *h)
{
    uint16x8_t a = vld1q_u16(in), b = vld1q_u16(in + 1), c = vld1q_u16(in + 2);
    *f = vrhaddq_u16(vhaddq_u16(a, c), b);
    *h = vrhaddq_u16(a, b);
}

/* (v >> 5) clipped to [0, PIXEL_MAX], eight int32 sums to eight samples */
static inline uint16x8_t plane_clip(int32x4_t lo, int32x4_t hi)
{
    return vminq_u16(vcombine_u16(vqshrun_n_s32(lo, 5), vqshrun_n_s32(hi, 5)),
                     vdupq_n_u16(PIXEL_MAX));
}

/* ---- the directional 8x8 table -------------------------------------------
 *
 * T = { F[0..23], H[0..23], e[0], the HU corner, zeros } over the flat edge
 * array, as the C six-mode builder lays it out. `t16` gets the first 48
 * entries for the modes whose rows are windows of it; `lo` and `hi` are the
 * whole table split by byte for the gathered ones. */
struct dir_tab {
    pixel t16[48];
    uint8x16x4_t lo, hi;
};

static inline void dir_tab_build(struct dir_tab *t, const pixel e[32])
{
    uint16x8_t f0, f1, f2, h0, h1, h2;
    f121_8(e, &f0, &h0);
    f121_8(e + 8, &f1, &h1);
    f121_8(e + 16, &f2, &h2);
    vst1q_u16(t->t16, f0);
    vst1q_u16(t->t16 + 8, f1);
    vst1q_u16(t->t16 + 16, f2);
    vst1q_u16(t->t16 + 24, h0);
    vst1q_u16(t->t16 + 32, h1);
    vst1q_u16(t->t16 + 40, h2);
    uint16x8_t t6 = vdupq_n_u16(0);
    t6 = vsetq_lane_u16(e[0], t6, 0);
    t6 = vsetq_lane_u16((uint16_t)((e[1] + 3 * e[0] + 2) >> 2), t6, 1);
    const uint16x8_t t7 = vdupq_n_u16(0);
#define B(v) vreinterpretq_u8_u16(v)
    t->lo.val[0] = vuzp1q_u8(B(f0), B(f1));
    t->hi.val[0] = vuzp2q_u8(B(f0), B(f1));
    t->lo.val[1] = vuzp1q_u8(B(f2), B(h0));
    t->hi.val[1] = vuzp2q_u8(B(f2), B(h0));
    t->lo.val[2] = vuzp1q_u8(B(h1), B(h2));
    t->hi.val[2] = vuzp2q_u8(B(h1), B(h2));
    t->lo.val[3] = vuzp1q_u8(B(t6), B(t7));
    t->hi.val[3] = vuzp2q_u8(B(t6), B(t7));
#undef B
}

/* one 8x8 block whose samples are table entries at the 64 indices `ix` */
static inline void dir_gather(pixel out[64], const struct dir_tab *t,
                              const uint8_t ix[64])
{
    for (int k = 0; k < 64; k += 16) {
        uint8x16_t i = vld1q_u8(ix + k);
        uint8x16_t l = vqtbl4q_u8(t->lo, i), h = vqtbl4q_u8(t->hi, i);
        vst1q_u8((uint8_t *)(out + k), vzip1q_u8(l, h));
        vst1q_u8((uint8_t *)(out + k + 8), vzip2q_u8(l, h));
    }
}

/* ---- Intra8x8 ---------------------------------------------------------------
 * Only the modes the dispatcher routes here (VERT/DDR/VR/HD/VL). */
void y264_intra8x8_from_edge_neon(pixel pred[64], const pixel e[32], int mode)
{
    if (mode == Y264_I4_VERT) {
        uint16x8_t t = vld1q_u16(e + 9);
        for (int y = 0; y < 8; y++) vst1q_u16(pred + 8 * y, t);
        return;
    }
    struct dir_tab t;
    dir_tab_build(&t, e);
    switch (mode) {
    case Y264_I4_DDR:
        for (int y = 0; y < 8; y++) vst1q_u16(pred + 8 * y, vld1q_u16(t.t16 + 7 - y));
        break;
    case Y264_I4_VL:                        /* even rows H, odd rows F */
        for (int y = 0; y < 8; y++)
            vst1q_u16(pred + 8 * y, vld1q_u16(t.t16 + ((y & 1) ? 9 : 33) + (y >> 1)));
        break;
    case Y264_I4_VR:
        dir_gather(pred, &t, y264_intra8x8_dir6_idx[2]);
        break;
    case Y264_I4_HD:
        dir_gather(pred, &t, y264_intra8x8_dir6_idx[3]);
        break;
    default: break;
    }
}

/* The six directional modes (DDL, DDR, VR, HD, VL, HU, in mode order) from
 * one filter pass, both edges present. */
void y264_intra8x8_dir6_neon(pixel out[6][64], const pixel e[32])
{
    struct dir_tab t;
    dir_tab_build(&t, e);
    for (int y = 0; y < 8; y++) {
        vst1q_u16(out[0] + 8 * y, vld1q_u16(t.t16 + 9 + y));
        vst1q_u16(out[1] + 8 * y, vld1q_u16(t.t16 + 7 - y));
        vst1q_u16(out[4] + 8 * y, vld1q_u16(t.t16 + ((y & 1) ? 9 : 33) + (y >> 1)));
    }
    dir_gather(out[2], &t, y264_intra8x8_dir6_idx[2]);
    dir_gather(out[3], &t, y264_intra8x8_dir6_idx[3]);
    dir_gather(out[5], &t, y264_intra8x8_dir6_idx[5]);
}

/* The flat edge array's filtered top run, e[9 .. 24], from a[0 .. 17]. */
static inline void edge_top(pixel e[32], const pixel a[24])
{
    uint16x8_t f0, f1, h;
    f121_8(a, &f0, &h);
    f121_8(a + 8, &f1, &h);
    vst1q_u16(e + 9, f0);
    vst1q_u16(e + 17, f1);
}

/* The filtered left column into e[7 .. 0], reversed, from al[0 .. 9]. */
static inline void edge_left(pixel e[32], const pixel al[16])
{
    uint16x8_t f, h;
    f121_8(al, &f, &h);
    f = vrev64q_u16(f);
    vst1q_u16(e, vextq_u16(f, f, 4));
}

/* y264_intra8x8_dir6_neon from the reconstructed neighbours directly, for a
 * block with the top, left and top-left samples and no top-right. */
void y264_intra8x8_dir6_rec_neon(pixel out[6][64], const pixel *rec, int stride)
{
    pixel a[24], al[16], e[32];
    const pixel *top = rec - stride;
    int rtl = top[-1];
    a[0] = (pixel)rtl;
    memcpy(a + 1, top, 8 * sizeof(pixel));
    for (int i = 9; i < 18; i++) a[i] = top[7];  /* rt[8..15] = rt[7], a[17] = rt[15] */
    al[0] = (pixel)rtl;
    for (int i = 0; i < 8; i++) al[1 + i] = rec[i * stride - 1];
    al[9] = al[8];
    memset(e, 0, sizeof(e));
    e[8] = (pixel)((top[0] + 2 * rtl + al[1] + 2) >> 2);
    edge_top(e, a);
    edge_left(e, al);
    e[25] = e[26] = e[24];
    y264_intra8x8_dir6_neon(out, e);
}

void y264_intra8x8_neon(pixel pred[64], const pixel *rec, int stride,
                        int mode, int have_top, int have_left,
                        int have_topleft, int have_topright)
{
    pixel rt[16], rl[8];
    int rtl = have_topleft ? rec[-stride - 1] : 0;
    for (int i = 0; i < 8; i++) {
        rt[i] = have_top ? rec[-stride + i] : 0;
        rl[i] = have_left ? rec[i * stride - 1] : 0;
    }
    for (int i = 8; i < 16; i++)
        rt[i] = have_topright ? rec[-stride + i] : rt[7];

    /* 8.3.2.2.1 low-pass into e = { l7..l0, tl, t0..t15, t15, t15 }; the
     * endpoint specials fold into the same filter by replicating a source */
    pixel e[32];
    memset(e, 0, sizeof(e));
    int tl = 0;
    if (have_topleft) {
        if (have_top && have_left) tl = (rt[0] + 2 * rtl + rl[0] + 2) >> 2;
        else if (have_top)         tl = (3 * rtl + rt[0] + 2) >> 2;
        else                       tl = (3 * rtl + rl[0] + 2) >> 2;
    }
    e[8] = (pixel)tl;
    if (have_top) {
        pixel a[24];
        a[0] = have_topleft ? (pixel)rtl : rt[0];
        memcpy(a + 1, rt, 16 * sizeof(pixel));
        a[17] = rt[15];
        edge_top(e, a);
    }
    if (have_left) {
        pixel al[16];
        al[0] = have_topleft ? (pixel)rtl : rl[0];
        memcpy(al + 1, rl, 8 * sizeof(pixel));
        al[9] = rl[7];
        edge_left(e, al);
    }
    e[25] = e[26] = e[24];
    y264_intra8x8_from_edge_neon(pred, e, mode);
}

/* The lowres intra estimate's four base 8x8 predictions: DC, vertical,
 * horizontal and the plane with a, b, c already derived. The estimate's
 * samples are on the 8-bit scale at every depth, so |a| <= 8160 and |b|,
 * |c| <= 1355, and a + 16 + b*(x-3) + c*(y-3) stays within int16; arguments
 * outside that bound take the scalar form. */
void y264_intra8_dvhp_neon(pixel out[4][64], const pixel top[8], const pixel left[8],
                           int dc, int a, int b, int c)
{
    if (abs(a) + 4 * abs(b) + 4 * abs(c) + 16 > 32767) {
        y264_intra8_dvhp_c(out, top, left, dc, a, b, c);
        return;
    }
    uint16x8_t vd = vdupq_n_u16((uint16_t)dc), vt = vld1q_u16(top);
    static const int16_t xm3[8] = { -3, -2, -1, 0, 1, 2, 3, 4 };
    int16x8_t r = vaddq_s16(vdupq_n_s16((int16_t)(a + 16 - 3 * c)),
                            vmulq_n_s16(vld1q_s16(xm3), (int16_t)b));
    int16x8_t vc = vdupq_n_s16((int16_t)c);
    const int16x8_t zero = vdupq_n_s16(0), vmax = vdupq_n_s16(PIXEL_MAX);
    for (int y = 0; y < 8; y++) {
        vst1q_u16(out[0] + 8 * y, vd);
        vst1q_u16(out[1] + 8 * y, vt);
        vst1q_u16(out[2] + 8 * y, vdupq_n_u16(left[y]));
        int16x8_t v = vminq_s16(vmaxq_s16(vshrq_n_s16(r, 5), zero), vmax);
        vst1q_u16(out[3] + 8 * y, vreinterpretq_u16_s16(v));
        r = vaddq_s16(r, vc);
    }
}

/* ---- Intra16x16 ------------------------------------------------------------ */
void y264_intra16x16_neon(pixel pred[256], const pixel *rec, int stride,
                          int mode, int have_top, int have_left)
{
    pixel left[16];
    for (int i = 0; i < 16; i++)
        left[i] = have_left ? rec[i * stride - 1] : 0;

    switch (mode) {
    case Y264_I16_VERT: {
        uint16x8x2_t t = { { vdupq_n_u16(0), vdupq_n_u16(0) } };
        if (have_top) t = vld1q_u16_x2(rec - stride);
        for (int y = 0; y < 16; y++) vst1q_u16_x2(pred + 16 * y, t);
        return;
    }
    case Y264_I16_HORIZ:
        for (int y = 0; y < 16; y++) {
            uint16x8_t v = vdupq_n_u16(left[y]);
            uint16x8x2_t r = { { v, v } };
            vst1q_u16_x2(pred + 16 * y, r);
        }
        return;
    case Y264_I16_DC: {
        int st = 0, sl = 0;
        if (have_top) {
            uint16x8x2_t t = vld1q_u16_x2(rec - stride);
            st = (int)vaddlvq_u16(vaddq_u16(t.val[0], t.val[1]));
        }
        for (int i = 0; i < 16; i++) sl += left[i];
        int dc;
        if (have_top && have_left) dc = (st + sl + 16) >> 5;
        else if (have_top)         dc = (st + 8) >> 4;
        else if (have_left)        dc = (sl + 8) >> 4;
        else                       dc = 1 << (Y264_BIT_DEPTH - 1);
        uint16x8_t v = vdupq_n_u16((uint16_t)dc);
        uint16x8x2_t r = { { v, v } };
        for (int y = 0; y < 16; y++) vst1q_u16_x2(pred + 16 * y, r);
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
        static const int32_t xm7[16] = { -7, -6, -5, -4, -3, -2, -1, 0,
                                         1, 2, 3, 4, 5, 6, 7, 8 };
        int32x4_t bx[4];
        for (int i = 0; i < 4; i++) bx[i] = vmulq_n_s32(vld1q_s32(xm7 + 4 * i), b);
        int32x4_t base = vdupq_n_s32(a - 7 * c + 16), vc = vdupq_n_s32(c);
        for (int y = 0; y < 16; y++) {
            uint16x8x2_t o = { { plane_clip(vaddq_s32(base, bx[0]), vaddq_s32(base, bx[1])),
                                 plane_clip(vaddq_s32(base, bx[2]), vaddq_s32(base, bx[3])) } };
            vst1q_u16_x2(pred + 16 * y, o);
            base = vaddq_s32(base, vc);
        }
        return;
    }
    default: break;
    }
}

/* ---- Intra chroma (cw == 8; ch == 8 or 16) --------------------------------- */
void y264_intra_chroma_neon(pixel *pred, const pixel *rec, int stride,
                            int mode, int have_top, int have_left,
                            int cw, int ch)
{
    pixel top[8], left[16];
    (void)cw;                                   /* dispatcher guarantees 8 */
    if (have_top) memcpy(top, rec - stride, 8 * sizeof(pixel));
    else          memset(top, 0, sizeof(top));
    for (int i = 0; i < ch; i++) left[i] = have_left ? rec[i * stride - 1] : 0;
    int corner = (have_top && have_left) ? rec[-stride - 1] : 0;

    switch (mode) {
    case Y264_IC_VERT: {
        uint16x8_t t = vld1q_u16(top);
        for (int y = 0; y < ch; y++) vst1q_u16(pred + 8 * y, t);
        return;
    }
    case Y264_IC_HORIZ:
        for (int y = 0; y < ch; y++) vst1q_u16(pred + 8 * y, vdupq_n_u16(left[y]));
        return;
    case Y264_IC_DC:
        for (int by = 0; by < ch / 4; by++) {
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
            uint16x8_t v = vcombine_u16(vdup_n_u16((uint16_t)d[0]), vdup_n_u16((uint16_t)d[1]));
            for (int y = 0; y < 4; y++) vst1q_u16(pred + (by * 4 + y) * 8, v);
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
        static const int32_t xm3[8] = { -3, -2, -1, 0, 1, 2, 3, 4 };
        int32x4_t b0 = vmulq_n_s32(vld1q_s32(xm3), b), b1 = vmulq_n_s32(vld1q_s32(xm3 + 4), b);
        int32x4_t base = vdupq_n_s32(a + c * (-3 - yCF) + 16), vc = vdupq_n_s32(c);
        for (int y = 0; y < ch; y++) {
            vst1q_u16(pred + 8 * y, plane_clip(vaddq_s32(base, b0), vaddq_s32(base, b1)));
            base = vaddq_s32(base, vc);
        }
        return;
    }
    default: break;
    }
}

#endif /* __aarch64__ */
