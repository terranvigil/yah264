/*
 * predict.c - H.264 intra prediction (8.3)
 * Copyright (c) 2026, the next264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "predict.h"

static inline int clip8(int x)
{
    return x < 0 ? 0 : (x > PIXEL_MAX ? PIXEL_MAX : x);
}

void n264_intra16x16_c(pixel pred[256], const pixel *rec, int stride,
                     int mode, int have_top, int have_left)
{
    pixel top[16], left[16];
    int corner = 0;
    for (int i = 0; i < 16; i++) {
        top[i]  = have_top  ? rec[-stride + i] : 0;
        left[i] = have_left ? rec[i * stride - 1] : 0;
    }
    if (have_top && have_left)
        corner = rec[-stride - 1];

    switch (mode) {
    case N264_I16_VERT:
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++)
                pred[y * 16 + x] = top[x];
        break;
    case N264_I16_HORIZ:
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++)
                pred[y * 16 + x] = left[y];
        break;
    case N264_I16_DC: {
        int dc, st = 0, sl = 0;
        for (int i = 0; i < 16; i++) { st += top[i]; sl += left[i]; }
        if (have_top && have_left) dc = (st + sl + 16) >> 5;
        else if (have_top)         dc = (st + 8) >> 4;
        else if (have_left)        dc = (sl + 8) >> 4;
        else                       dc = 1 << (N264_BIT_DEPTH - 1);
        for (int i = 0; i < 256; i++) pred[i] = (pixel)dc;
        break;
    }
    case N264_I16_PLANE: {
        int H = 0, Vv = 0;
        for (int x = 0; x < 8; x++) {
            int tprev = (6 - x >= 0) ? top[6 - x] : corner;   /* x=7 -> corner */
            H += (x + 1) * (top[8 + x] - tprev);
        }
        for (int y = 0; y < 8; y++) {
            int lprev = (6 - y >= 0) ? left[6 - y] : corner;
            Vv += (y + 1) * (left[8 + y] - lprev);
        }
        int b = (5 * H + 32) >> 6;
        int c = (5 * Vv + 32) >> 6;
        int a = 16 * (left[15] + top[15]);
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++)
                pred[y * 16 + x] =
                    (pixel)clip8((a + b * (x - 7) + c * (y - 7) + 16) >> 5);
        break;
    }
    default: break;
    }
}

/* Intra chroma prediction for a cw x ch block (8.3.4.1-4). cw/ch are MbWidthC /
 * MbHeightC: 8x8 (4:2:0), 8x16 (4:2:2), 16x16 (4:4:4). The DC block-selection,
 * plane xCF/yCF offsets and plane b/c coefficients all follow from cw/ch, so the
 * 8x8 path is byte-identical to the old fixed-size code. pred is row-major with
 * stride cw. */
void n264_intra_chroma_c(pixel *pred, const pixel *rec, int stride,
                       int mode, int have_top, int have_left, int cw, int ch)
{
    pixel top[16], left[16];
    int corner = 0;
    for (int i = 0; i < cw; i++) top[i]  = have_top  ? rec[-stride + i] : 0;
    for (int i = 0; i < ch; i++) left[i] = have_left ? rec[i * stride - 1] : 0;
    if (have_top && have_left)
        corner = rec[-stride - 1];

    switch (mode) {
    case N264_IC_VERT:
        for (int y = 0; y < ch; y++)
            for (int x = 0; x < cw; x++)
                pred[y * cw + x] = top[x];
        break;
    case N264_IC_HORIZ:
        for (int y = 0; y < ch; y++)
            for (int x = 0; x < cw; x++)
                pred[y * cw + x] = left[y];
        break;
    case N264_IC_DC:
        /* Each 4x4 chroma block gets its own DC (8.3.4.1). Selection by block
 * position: both top+left when (bx==0&&by==0) or (bx>0&&by>0); top-only
 * on the first row past column 0; left-only on the first column past
 * row 0 -- each with the spec's fallback to the other edge, then 128. */
        for (int by = 0; by < ch / 4; by++) {
            for (int bx = 0; bx < cw / 4; bx++) {
                int st = 0, sl = 0;
                for (int i = 0; i < 4; i++) {
                    st += top[bx * 4 + i];
                    sl += left[by * 4 + i];
                }
                int dc;
                int both = (bx == 0 && by == 0) || (bx > 0 && by > 0);
                int prefer_top = both ? 1 : (by == 0);   /* by==0,bx>0 -> top */
                if (have_top && have_left && both)
                    dc = (st + sl + 4) >> 3;
                else if (prefer_top ? have_top : have_left)
                    dc = (prefer_top ? (st + 2) : (sl + 2)) >> 2;
                else if (prefer_top ? have_left : have_top)
                    dc = (prefer_top ? (sl + 2) : (st + 2)) >> 2;
                else
                    dc = 1 << (N264_BIT_DEPTH - 1);
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++)
                        pred[(by * 4 + y) * cw + bx * 4 + x] = (pixel)dc;
            }
        }
        break;
    case N264_IC_PLANE: {
        int xCF = (cw == 16) ? 4 : 0, yCF = (ch == 16) ? 4 : 0;
        int H = 0, Vv = 0;
        for (int x = 0; x <= 3 + xCF; x++) {
            int i = 2 + xCF - x;
            int tprev = (i >= 0) ? top[i] : corner;
            H += (x + 1) * (top[4 + xCF + x] - tprev);
        }
        for (int y = 0; y <= 3 + yCF; y++) {
            int i = 2 + yCF - y;
            int lprev = (i >= 0) ? left[i] : corner;
            Vv += (y + 1) * (left[4 + yCF + y] - lprev);
        }
        int b = ((34 - 29 * (cw == 16)) * H + 32) >> 6;
        int c = ((34 - 29 * (ch == 16)) * Vv + 32) >> 6;
        int a = 16 * (left[ch - 1] + top[cw - 1]);
        for (int y = 0; y < ch; y++)
            for (int x = 0; x < cw; x++)
                pred[y * cw + x] =
                    (pixel)clip8((a + b * (x - 3 - xCF) + c * (y - 3 - yCF) + 16) >> 5);
        break;
    }
    default: break;
    }
}

/* Neighbour accessors for the 4x4 predictors: T(-1)=corner, L(-1)=corner. */
void n264_intra4x4_c(pixel pred[16], const pixel *rec, int stride,
                   int mode, int have_top, int have_left,
                   int have_topleft, int have_topright)
{
    int t[8], l[4], tl = 0;
    for (int i = 0; i < 4; i++) {
        t[i] = have_top ? rec[-stride + i] : 0;
        l[i] = have_left ? rec[i * stride - 1] : 0;
    }
    for (int i = 4; i < 8; i++)
        t[i] = have_topright ? rec[-stride + i] : t[3];
    if (have_topleft)
        tl = rec[-stride - 1];

    #define T(i) ((i) < 0 ? tl : t[i])
    #define L(i) ((i) < 0 ? tl : l[i])
    #define P(x, y) pred[(y) * 4 + (x)]

    switch (mode) {
    case N264_I4_VERT:
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) P(x, y) = (pixel)t[x];
        break;
    case N264_I4_HORIZ:
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) P(x, y) = (pixel)l[y];
        break;
    case N264_I4_DC: {
        int dc;
        if (have_top && have_left) dc = (t[0]+t[1]+t[2]+t[3]+l[0]+l[1]+l[2]+l[3]+4) >> 3;
        else if (have_top)         dc = (t[0]+t[1]+t[2]+t[3]+2) >> 2;
        else if (have_left)        dc = (l[0]+l[1]+l[2]+l[3]+2) >> 2;
        else                       dc = 1 << (N264_BIT_DEPTH - 1);
        for (int i = 0; i < 16; i++) pred[i] = (pixel)dc;
        break;
    }
    case N264_I4_DDL:
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
            if (x == 3 && y == 3) P(x, y) = (pixel)((t[6] + 3*t[7] + 2) >> 2);
            else P(x, y) = (pixel)((t[x+y] + 2*t[x+y+1] + t[x+y+2] + 2) >> 2);
        }
        break;
    case N264_I4_DDR:
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
            if (x > y)      P(x, y) = (pixel)((T(x-y-2) + 2*T(x-y-1) + T(x-y) + 2) >> 2);
            else if (x < y) P(x, y) = (pixel)((L(y-x-2) + 2*L(y-x-1) + L(y-x) + 2) >> 2);
            else            P(x, y) = (pixel)((T(0) + 2*tl + L(0) + 2) >> 2);
        }
        break;
    case N264_I4_VR:
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
            int z = 2*x - y;
            int i = x - (y >> 1);
            if (z >= 0 && (z & 1) == 0)      P(x, y) = (pixel)((T(i-1) + T(i) + 1) >> 1);
            else if (z >= 0)                 P(x, y) = (pixel)((T(i-2) + 2*T(i-1) + T(i) + 2) >> 2);
            else if (z == -1)                P(x, y) = (pixel)((L(0) + 2*tl + T(0) + 2) >> 2);
            else                             P(x, y) = (pixel)((L(y-1) + 2*L(y-2) + L(y-3) + 2) >> 2);
        }
        break;
    case N264_I4_HD:
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
            int z = 2*y - x;
            int i = y - (x >> 1);
            if (z >= 0 && (z & 1) == 0)      P(x, y) = (pixel)((L(i-1) + L(i) + 1) >> 1);
            else if (z >= 0)                 P(x, y) = (pixel)((L(i-2) + 2*L(i-1) + L(i) + 2) >> 2);
            else if (z == -1)                P(x, y) = (pixel)((T(0) + 2*tl + L(0) + 2) >> 2);
            else                             P(x, y) = (pixel)((T(x-1) + 2*T(x-2) + T(x-3) + 2) >> 2);
        }
        break;
    case N264_I4_VL:
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
            int i = x + (y >> 1);
            if ((y & 1) == 0) P(x, y) = (pixel)((t[i] + t[i+1] + 1) >> 1);
            else              P(x, y) = (pixel)((t[i] + 2*t[i+1] + t[i+2] + 2) >> 2);
        }
        break;
    case N264_I4_HU:
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
            int z = x + 2*y;
            int i = y + (x >> 1);
            if (z < 5 && (z & 1) == 0)  P(x, y) = (pixel)((L(i) + L(i+1) + 1) >> 1);
            else if (z < 5)             P(x, y) = (pixel)((L(i) + 2*L(i+1) + L(i+2) + 2) >> 2);
            else if (z == 5)            P(x, y) = (pixel)((l[2] + 3*l[3] + 2) >> 2);
            else                        P(x, y) = (pixel)l[3];
        }
        break;
    default: break;
    }

    #undef T
    #undef L
    #undef P
}

void n264_intra8x8_edge_c(n264_i8_edge_t *e, const pixel *rec, int stride,
                          int have_top, int have_left,
                          int have_topleft, int have_topright)
{
    /* Raw reference samples: 16 top (8 + 8 top-right), 8 left, 1 corner. */
    int rt[16], rl[8], rtl = 0;
    for (int i = 0; i < 8; i++) {
        rt[i] = have_top  ? rec[-stride + i] : 0;
        rl[i] = have_left ? rec[i * stride - 1] : 0;
    }
    for (int i = 8; i < 16; i++)
        rt[i] = have_topright ? rec[-stride + i] : rt[7];
    if (have_topleft)
        rtl = rec[-stride - 1];

    /* Reference-sample low-pass filter (8.3.2.2.1) -> t[16], l[8], tl. Zeroed so
 * an unavailable side reads as 0 (the DC mode sums all of t[]/l[] before it
 * branches on availability; only the used branch's sum matters). */
    int *t = e->t, *l = e->l;
    for (int i = 0; i < 16; i++) t[i] = 0;
    for (int i = 0; i < 8; i++) l[i] = 0;
    e->tl = 0;
    if (have_topleft) {
        if (have_top && have_left) e->tl = (rt[0] + 2 * rtl + rl[0] + 2) >> 2;
        else if (have_top)         e->tl = (3 * rtl + rt[0] + 2) >> 2;
        else                       e->tl = (3 * rtl + rl[0] + 2) >> 2;
    }
    if (have_top) {
        t[0] = have_topleft ? (rtl + 2 * rt[0] + rt[1] + 2) >> 2
                            : (3 * rt[0] + rt[1] + 2) >> 2;
        for (int x = 1; x < 15; x++)
            t[x] = (rt[x - 1] + 2 * rt[x] + rt[x + 1] + 2) >> 2;
        t[15] = (rt[14] + 3 * rt[15] + 2) >> 2;
    }
    if (have_left) {
        l[0] = have_topleft ? (rtl + 2 * rl[0] + rl[1] + 2) >> 2
                            : (3 * rl[0] + rl[1] + 2) >> 2;
        for (int y = 1; y < 7; y++)
            l[y] = (rl[y - 1] + 2 * rl[y] + rl[y + 1] + 2) >> 2;
        l[7] = (rl[6] + 3 * rl[7] + 2) >> 2;
    }

    /* The flat view the NEON builders index. Zeroed sides stay zero, which is
 * what they built for themselves before. */
    for (int i = 0; i < 32; i++) e->f[i] = 0;
    for (int i = 0; i < 8; i++)  e->f[7 - i] = (pixel)l[i];
    e->f[8] = (pixel)e->tl;
    for (int i = 0; i < 16; i++) e->f[9 + i] = (pixel)t[i];
    e->f[25] = e->f[26] = e->f[24];
}

void n264_intra8x8_from_edge_c(pixel pred[64], const n264_i8_edge_t *e,
                               int mode, int have_top, int have_left)
{
    const int *t = e->t, *l = e->l;
    const int tl = e->tl;

    #define T(i) ((i) < 0 ? tl : t[i])
    #define L(i) ((i) < 0 ? tl : l[i])
    #define P(x, y) pred[(y) * 8 + (x)]

    switch (mode) {
    case N264_I4_VERT:
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) P(x, y) = (pixel)t[x];
        break;
    case N264_I4_HORIZ:
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) P(x, y) = (pixel)l[y];
        break;
    case N264_I4_DC: {
        int dc, st = 0, sl = 0;
        for (int i = 0; i < 8; i++) { st += t[i]; sl += l[i]; }
        if (have_top && have_left) dc = (st + sl + 8) >> 4;
        else if (have_top)         dc = (st + 4) >> 3;
        else if (have_left)        dc = (sl + 4) >> 3;
        else                       dc = 1 << (N264_BIT_DEPTH - 1);
        for (int i = 0; i < 64; i++) pred[i] = (pixel)dc;
        break;
    }
    case N264_I4_DDL:
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
            if (x == 7 && y == 7) P(x, y) = (pixel)((t[14] + 3*t[15] + 2) >> 2);
            else P(x, y) = (pixel)((t[x+y] + 2*t[x+y+1] + t[x+y+2] + 2) >> 2);
        }
        break;
    case N264_I4_DDR:
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
            if (x > y)      P(x, y) = (pixel)((T(x-y-2) + 2*T(x-y-1) + T(x-y) + 2) >> 2);
            else if (x < y) P(x, y) = (pixel)((L(y-x-2) + 2*L(y-x-1) + L(y-x) + 2) >> 2);
            else            P(x, y) = (pixel)((T(0) + 2*tl + L(0) + 2) >> 2);
        }
        break;
    case N264_I4_VR:
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
            int z = 2*x - y;
            int i = x - (y >> 1);
            if (z >= 0 && (z & 1) == 0)      P(x, y) = (pixel)((T(i-1) + T(i) + 1) >> 1);
            else if (z >= 0)                 P(x, y) = (pixel)((T(i-2) + 2*T(i-1) + T(i) + 2) >> 2);
            else if (z == -1)                P(x, y) = (pixel)((L(0) + 2*tl + T(0) + 2) >> 2);
            else { int m = y - 2*x;          P(x, y) = (pixel)((L(m-1) + 2*L(m-2) + L(m-3) + 2) >> 2); }
        }
        break;
    case N264_I4_HD:
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
            int z = 2*y - x;
            int i = y - (x >> 1);
            if (z >= 0 && (z & 1) == 0)      P(x, y) = (pixel)((L(i-1) + L(i) + 1) >> 1);
            else if (z >= 0)                 P(x, y) = (pixel)((L(i-2) + 2*L(i-1) + L(i) + 2) >> 2);
            else if (z == -1)                P(x, y) = (pixel)((T(0) + 2*tl + L(0) + 2) >> 2);
            else { int n = x - 2*y;          P(x, y) = (pixel)((T(n-1) + 2*T(n-2) + T(n-3) + 2) >> 2); }
        }
        break;
    case N264_I4_VL:
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
            int i = x + (y >> 1);
            if ((y & 1) == 0) P(x, y) = (pixel)((t[i] + t[i+1] + 1) >> 1);
            else              P(x, y) = (pixel)((t[i] + 2*t[i+1] + t[i+2] + 2) >> 2);
        }
        break;
    case N264_I4_HU:
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
            int z = x + 2*y;
            int i = y + (x >> 1);
            if (z < 13 && (z & 1) == 0) P(x, y) = (pixel)((L(i) + L(i+1) + 1) >> 1);
            else if (z < 13)            P(x, y) = (pixel)((L(i) + 2*L(i+1) + L(i+2) + 2) >> 2);
            else if (z == 13)           P(x, y) = (pixel)((l[6] + 3*l[7] + 2) >> 2);
            else                        P(x, y) = (pixel)l[7];
        }
        break;
    default: break;
    }

    #undef T
    #undef L
    #undef P
}

void n264_intra8x8_c(pixel pred[64], const pixel *rec, int stride,
                     int mode, int have_top, int have_left,
                     int have_topleft, int have_topright)
{
    n264_i8_edge_t e;
    n264_intra8x8_edge_c(&e, rec, stride, have_top, have_left,
                         have_topleft, have_topright);
    n264_intra8x8_from_edge_c(pred, &e, mode, have_top, have_left);
}

/* ---- dispatch ------------------------------------------------------------
 * The _c bodies above are the portable references (checkasm baselines); the
 * public entry points route to the NEON builders on aarch64 PER MODE, from a
 * measured per-mode NEON-vs-C table (the simple fills auto-vectorize well in
 * C, so only the modes where NEON measured faster are routed): 16x16 and
 * cw==8 chroma route every mode; 8x8 routes VERT/DDR/VR/HD/VL (the branchy
 * diagonals, up to 3.7x) and keeps HORIZ/DC/DDL/HU in C; 4x4 stays entirely
 * C (blocks too small to amortize the edge-filter precompute -- measured a
 * net loss). All routed builders are bit-exact (checkasm, every mode x
 * availability combination). */
#if defined(__aarch64__) && N264_BIT_DEPTH == 8
#include "../common/cpu.h"
void n264_intra16x16_neon(pixel pred[256], const pixel *rec, int stride,
                          int mode, int have_top, int have_left);
void n264_intra_chroma_neon(pixel *pred, const pixel *rec, int stride,
                            int mode, int have_top, int have_left, int cw, int ch);
void n264_intra8x8_neon(pixel pred[64], const pixel *rec, int stride,
                        int mode, int have_top, int have_left,
                        int have_topleft, int have_topright);
void n264_intra8x8_from_edge_neon(pixel pred[64], const pixel e[32], int mode);
static int pr_have_neon(void) { return n264_asm_on(N264_ASM_PRED); }
#endif

/* Same per-mode routing table as n264_intra8x8, one derivation earlier. */
void n264_intra8x8_from_edge(pixel pred[64], const n264_i8_edge_t *e,
                             int mode, int have_top, int have_left)
{
#if defined(__aarch64__) && N264_BIT_DEPTH == 8
    switch (mode) {
    case N264_I4_VERT: case N264_I4_DDR: case N264_I4_VR:
    case N264_I4_HD: case N264_I4_VL:
        if (pr_have_neon()) {
            n264_intra8x8_from_edge_neon(pred, e->f, mode);
            return;
        }
        break;
    default:
        break;
    }
#endif
    n264_intra8x8_from_edge_c(pred, e, mode, have_top, have_left);
}

void n264_intra16x16(pixel pred[256], const pixel *rec, int stride,
                     int mode, int have_top, int have_left)
{
#if defined(__aarch64__) && N264_BIT_DEPTH == 8
    if (pr_have_neon()) {
        n264_intra16x16_neon(pred, rec, stride, mode, have_top, have_left);
        return;
    }
#endif
    n264_intra16x16_c(pred, rec, stride, mode, have_top, have_left);
}

void n264_intra_chroma(pixel *pred, const pixel *rec, int stride,
                       int mode, int have_top, int have_left, int cw, int ch)
{
#if defined(__aarch64__) && N264_BIT_DEPTH == 8
    if (cw == 8 && pr_have_neon()) {
        n264_intra_chroma_neon(pred, rec, stride, mode, have_top, have_left, cw, ch);
        return;
    }
#endif
    n264_intra_chroma_c(pred, rec, stride, mode, have_top, have_left, cw, ch);
}

void n264_intra4x4(pixel pred[16], const pixel *rec, int stride,
                   int mode, int have_top, int have_left,
                   int have_topleft, int have_topright)
{
    /* 4x4 stays C on every mode: the NEON builder measured a net loss (the
 * edge-filter precompute doesn't amortize over 16 pixels). */
    n264_intra4x4_c(pred, rec, stride, mode, have_top, have_left,
                    have_topleft, have_topright);
}

void n264_intra8x8(pixel pred[64], const pixel *rec, int stride,
                   int mode, int have_top, int have_left,
                   int have_topleft, int have_topright)
{
#if defined(__aarch64__) && N264_BIT_DEPTH == 8
    switch (mode) {
    case N264_I4_VERT: case N264_I4_DDR: case N264_I4_VR:
    case N264_I4_HD: case N264_I4_VL:
        if (pr_have_neon()) {
            n264_intra8x8_neon(pred, rec, stride, mode, have_top, have_left,
                               have_topleft, have_topright);
            return;
        }
        break;
    default:                    /* HORIZ/DC/DDL/HU: auto-vectorized C wins */
        break;
    }
#endif
    n264_intra8x8_c(pred, rec, stride, mode, have_top, have_left,
                    have_topleft, have_topright);
}
