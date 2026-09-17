/*
 * checkasm: boundary strengths and the in-loop deblocking edge filters.
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The edge filters have no C twin to diff against -- the scalar path is a
 * per-line loop inside the encoder's deblock pass, not an exported kernel -- so
 * the reference here is the SPECIFICATION written out: 8.7.2.3 for the
 * bS < 4 filter and 8.7.2.4 for bS == 4, transcribed independently. A checker
 * that shared the encoder's own filter would agree with it by construction and
 * would gate nothing.
 *
 * Content is a random walk rather than uniform noise, because on noise the
 * alpha/beta gate rejects nearly every line and the kernel returns without
 * having filtered anything: a green run over samples the filter never touched.
 * Half the trials add a wide-swing term on top so the gate's reject side is
 * exercised too.
 */

#include "checkasm.h"
#include "dsp/arch.h"
#include "dsp/deblock.h"

/* Table 8-16 (alpha, beta by index) and Table 8-17 (tc0 by index and bS). */
static const uint8_t ALPHA[52] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,4,5,6,7,8,9,10,12,13,
    15,17,20,22,25,28,32,36,40,45,50,56,63,71,80,90,101,113,
    127,144,162,182,203,226,255,255
};
static const uint8_t BETA[52] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,3,3,3,3,4,4,4,6,6,
    7,7,8,8,9,9,10,10,11,11,12,12,13,13,14,14,15,15,16,16,17,17,18,18
};
static const uint8_t TC0[52][3] = {
    {0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},
    {0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},
    {0,0,0},{0,0,1},{0,0,1},{0,0,1},{0,0,1},{0,1,1},{0,1,1},{1,1,1},
    {1,1,1},{1,1,1},{1,1,1},{1,1,2},{1,1,2},{1,1,2},{1,1,2},{1,2,3},
    {1,2,3},{2,2,3},{2,2,4},{2,3,4},{2,3,4},{3,3,5},{3,4,6},{3,4,6},
    {4,5,7},{4,5,8},{4,6,9},{5,7,10},{6,8,11},{6,8,13},{7,10,14},
    {8,11,16},{9,12,18},{10,13,20},{11,15,23},{13,17,25}
};

#if Y264_HAVE_NEON

static int clipv(int v) { return v < 0 ? 0 : (v > PIXEL_MAX ? PIXEL_MAX : v); }

/* 8.7.2.3 / 8.7.2.4 on one luma line. `st` steps ACROSS the edge. */
static void luma_line_c(pixel *q0, int st, int bs, int alpha, int beta, int tc0)
{
    int p0 = q0[-st], p1 = q0[-2*st], p2 = q0[-3*st], p3 = q0[-4*st];
    int Q0 = q0[0], Q1 = q0[st], Q2 = q0[2*st], Q3 = q0[3*st];
    if (abs(p0 - Q0) >= alpha || abs(p1 - p0) >= beta || abs(Q1 - Q0) >= beta)
        return;
    int ap = abs(p2 - p0), aq = abs(Q2 - Q0);
    if (bs < 4) {
        int tc = tc0 + (ap < beta) + (aq < beta);
        int d = ((Q0 - p0) * 4 + (p1 - Q1) + 4) >> 3;
        d = d < -tc ? -tc : d > tc ? tc : d;
        q0[-st] = (pixel)clipv(p0 + d);
        q0[0]   = (pixel)clipv(Q0 - d);
        if (ap < beta) {
            int dl = (p2 + ((p0 + Q0 + 1) >> 1) - 2 * p1) >> 1;
            dl = dl < -tc0 ? -tc0 : dl > tc0 ? tc0 : dl;
            q0[-2*st] = (pixel)(p1 + dl);
        }
        if (aq < beta) {
            int dl = (Q2 + ((p0 + Q0 + 1) >> 1) - 2 * Q1) >> 1;
            dl = dl < -tc0 ? -tc0 : dl > tc0 ? tc0 : dl;
            q0[st] = (pixel)(Q1 + dl);
        }
        return;
    }
    int strong = abs(p0 - Q0) < ((alpha >> 2) + 2);
    if (ap < beta && strong) {
        q0[-st]   = (pixel)((p2 + 2*p1 + 2*p0 + 2*Q0 + Q1 + 4) >> 3);
        q0[-2*st] = (pixel)((p2 + p1 + p0 + Q0 + 2) >> 2);
        q0[-3*st] = (pixel)((2*p3 + 3*p2 + p1 + p0 + Q0 + 4) >> 3);
    } else {
        q0[-st] = (pixel)((2*p1 + p0 + Q1 + 2) >> 2);
    }
    if (aq < beta && strong) {
        q0[0]    = (pixel)((Q2 + 2*Q1 + 2*Q0 + 2*p0 + p1 + 4) >> 3);
        q0[st]   = (pixel)((Q2 + Q1 + Q0 + p0 + 2) >> 2);
        q0[2*st] = (pixel)((2*Q3 + 3*Q2 + Q1 + Q0 + p0 + 4) >> 3);
    } else {
        q0[0] = (pixel)((2*Q1 + Q0 + p1 + 2) >> 2);
    }
}

/* chromaStyleFilteringFlag == 1: only p0/q0 move, and tc is tc0 + 1. */
static void chroma_line_c(pixel *q0, int st, int b, int alpha, int beta, int tc0)
{
    int p0 = q0[-st], p1 = q0[-2*st], Q0 = q0[0], Q1 = q0[st];
    if (abs(p0 - Q0) >= alpha || abs(p1 - p0) >= beta || abs(Q1 - Q0) >= beta)
        return;
    if (b < 4) {
        int tc = tc0 + 1;
        int d = ((Q0 - p0) * 4 + (p1 - Q1) + 4) >> 3;
        d = d < -tc ? -tc : d > tc ? tc : d;
        q0[-st] = (pixel)clipv(p0 + d);
        q0[0]   = (pixel)clipv(Q0 - d);
    } else {
        q0[-st] = (pixel)((2*p1 + p0 + Q1 + 2) >> 2);
        q0[0]   = (pixel)((2*Q1 + Q0 + p1 + 2) >> 2);
    }
}

/* The four lines of one edge, the shape the kernel replaces: `st` steps across
 * the edge and `ls` along it. */
static void luma_edge4_c(pixel *q0, int st, int ls, int bs, int alpha, int beta,
                         int tc0)
{
    for (int ln = 0; ln < 4; ln++)
        luma_line_c(q0 + ln * ls, st, bs, alpha, beta, tc0);
}

static pixel *ea, *eb;

/* A smooth random walk about mid-grey, with a wide-swing term on alternate
 * trials so both sides of the alpha/beta gate are reached. */
static void edge_plane(int wide)
{
    if (!ea) {
        ea = ca_guard_alloc(STRIDE * PLANE_H * sizeof(pixel));
        eb = ca_guard_alloc(STRIDE * PLANE_H * sizeof(pixel));
    }
    for (int i = 0; i < STRIDE * PLANE_H; i++)
        ea[i] = (pixel)clipv(128 + (int)ca_below(32) - 16 +
                             (wide ? (int)ca_below(200) - 100 : 0));
    memcpy(eb, ea, STRIDE * PLANE_H * sizeof(pixel));
}

static int t_deblock_luma(void)
{
    int bad = 0;
    for (int t = 0; t < TRIALS * 4; t++) {
        int qi = 16 + (int)ca_below(36);
        int bs = 1 + (int)ca_below(4);
        int alpha = ALPHA[qi], beta = BETA[qi];
        int tc0 = TC0[qi][bs < 4 ? bs - 1 : 0];
        edge_plane(t & 1);
        ca_guard_poison(eb, STRIDE * PLANE_H * sizeof(pixel));
        pixel *ra = ea + 8 * STRIDE + 8, *rb = eb + 8 * STRIDE + 8;

        luma_edge4_c(ra, 1, STRIDE, bs, alpha, beta, tc0);   /* vertical edge */
        y264_deblock_luma_v4_neon(rb, STRIDE, bs, alpha, beta, tc0);
        if (memcmp(ea, eb, STRIDE * PLANE_H * sizeof(pixel))) {
            if (!bad) ca_fail("deblock_luma_v4: bs=%d qi=%d", bs, qi);
            bad++;
        }
        if (!bad && ca_guard_check(eb, STRIDE * PLANE_H * sizeof(pixel),
                                   "deblock_luma_v4"))
            bad++;

        memcpy(eb, ea, STRIDE * PLANE_H * sizeof(pixel));
        luma_edge4_c(ra, STRIDE, 1, bs, alpha, beta, tc0);   /* horizontal edge */
        y264_deblock_luma_h4_neon(rb, STRIDE, bs, alpha, beta, tc0);
        if (memcmp(ea, eb, STRIDE * PLANE_H * sizeof(pixel))) {
            if (!bad) ca_fail("deblock_luma_h4: bs=%d qi=%d", bs, qi);
            bad++;
        }
        if (!bad && ca_guard_check(eb, STRIDE * PLANE_H * sizeof(pixel),
                                   "deblock_luma_h4"))
            bad++;
    }
    if (ca_bench) {
        pixel *r = eb + 8 * STRIDE + 8;
        CA_BENCH2("deblock_v4 bs3",
                  y264_deblock_luma_v4_neon(r, STRIDE, 3, 40, 10, 4),
                  luma_edge4_c(r, 1, STRIDE, 3, 40, 10, 4));
        CA_BENCH2("deblock_h4 bs4",
                  y264_deblock_luma_h4_neon(r, STRIDE, 4, 40, 10, 4),
                  luma_edge4_c(r, STRIDE, 1, 4, 40, 10, 4));
    }
    return bad;
}

/* The whole-edge chroma filter: eight lines in one pass with per-lane tc and a
 * bS == 4 select. Every bS combination over the four groups is drawn, including
 * all-zero and mixed bS == 4, because sharing one pass across four groups with
 * different parameters is the kernel's whole point. `span` is the lines per bS
 * group -- 2 for 4:2:0, 4 for a 4:2:2 vertical edge, which is sixteen lines in
 * two groups, so both groups are run. */
static int t_deblock_chroma(void)
{
    int bad = 0;
    for (int t = 0; t < TRIALS * 8; t++) {
        int qi = 16 + (int)ca_below(36);
        int alpha = ALPHA[qi], beta = BETA[qi];
        const uint8_t *tc0tab = TC0[qi];
        uint8_t bs[4];
        for (int k = 0; k < 4; k++) bs[k] = (uint8_t)ca_below(5);
        if (t == 0) bs[0] = bs[1] = bs[2] = bs[3] = 4;
        if (t == 1) bs[0] = bs[1] = bs[2] = bs[3] = 0;
        int span = (t & 1) ? 2 : 4;
        int grp = (span == 4) ? (int)ca_below(2) : 0;
        edge_plane(t & 2);
        ca_guard_poison(eb, STRIDE * PLANE_H * sizeof(pixel));
        pixel *ra = ea + 8 * STRIDE + 8, *rb = eb + 8 * STRIDE + 8;
        for (int i = 0; i < 8; i++) {
            int b = bs[(grp * 8 + i) / span];
            if (!b)
                continue;
            chroma_line_c(ra + i, STRIDE, b, alpha, beta,
                          tc0tab[b < 4 ? b - 1 : 0]);
        }
        y264_deblock_chroma8_h_neon(rb, STRIDE, alpha, beta, bs, tc0tab, span, grp);
        if (memcmp(ea, eb, STRIDE * PLANE_H * sizeof(pixel))) {
            if (!bad)
                ca_fail("deblock_chroma8_h: qi=%d span=%d grp=%d bs %d%d%d%d",
                        qi, span, grp, bs[0], bs[1], bs[2], bs[3]);
            bad++;
        }
        if (!bad && ca_guard_check(eb, STRIDE * PLANE_H * sizeof(pixel),
                                   "deblock_chroma8_h"))
            bad++;
    }
    return bad;
}

/* Boundary strengths (8.7.2.1) over a whole macroblock. The motion and
 * coefficient field is deliberately degenerate -- few reference indices, small
 * MVs, mostly-zero nnz -- because a uniformly random one makes almost every
 * edge intra or coded and never reaches the reference/MV branches the kernel
 * has to get right. Every transform-flag and neighbour-availability
 * combination is drawn. */
static int t_deblock_strength(void)
{
    enum { GW = 8, GH = 8, GORG = 2 * GW + 2 };   /* two blocks of margin */
    int8_t ref0[GW * GH], ref1[GW * GH], nnz[GW * GH];
    int16_t mx0[GW * GH], my0[GW * GH], mx1[GW * GH], my1[GW * GH];
    int bad = 0;
    for (int t = 0; t < TRIALS * 4; t++) {
        int bslice = t & 1;
        for (int i = 0; i < GW * GH; i++) {
            int intra = ca_below(5) == 0;
            ref0[i] = intra ? -1 : (int8_t)ca_below(3);
            ref1[i] = (intra || !bslice) ? -1 : (int8_t)(ca_below(2) ? 0 : -1);
            nnz[i]  = ca_below(4) ? 0 : (int8_t)(1 + ca_below(15));
            mx0[i] = (int16_t)((int)ca_below(17) - 8);
            my0[i] = (int16_t)((int)ca_below(17) - 8);
            mx1[i] = (int16_t)((int)ca_below(17) - 8);
            my1[i] = (int16_t)((int)ca_below(17) - 8);
        }
        struct y264_bs_ctx c = {
            .ref0 = ref0 + GORG, .ref1 = ref1 + GORG,
            .mvx0 = mx0 + GORG,  .mvy0 = my0 + GORG,
            .mvx1 = mx1 + GORG,  .mvy1 = my1 + GORG,
            .mv_stride = GW,
            .nnz = nnz + GORG,   .nnz_stride = GW,
            .tr8_cur = (uint8_t)(t >> 1 & 1), .tr8_left = (uint8_t)(t >> 2 & 1),
            .tr8_top = (uint8_t)(t >> 3 & 1),
            .have_left = (uint8_t)(t >> 4 & 1), .have_top = (uint8_t)(t >> 5 & 1),
        };
        uint8_t av[4][4], ah[4][4], bv[4][4], bh[4][4];
        memset(av, 0xA5, 16); memset(ah, 0xA5, 16);
        memset(bv, 0x5A, 16); memset(bh, 0x5A, 16);
        y264_deblock_strength_c(&c, av, ah);
        y264_deblock_strength_neon(&c, bv, bh);
        if (memcmp(av, bv, 16) || memcmp(ah, bh, 16)) {
            if (!bad) ca_fail("deblock_strength: trial %d (b-slice %d)", t, bslice);
            bad++;
        }
    }
    return bad;
}

#endif /* Y264_HAVE_NEON */

const ca_test ca_deblock_tests[] = {
#if Y264_HAVE_NEON
    { "deblock_luma",     "deblock", Y264_CPU_NEON, t_deblock_luma },
    { "deblock_chroma8_h","deblock", Y264_CPU_NEON, t_deblock_chroma },
    { "deblock_strength", "deblock", Y264_CPU_NEON, t_deblock_strength },
#endif
    { NULL, "deblock", 0, NULL },
};
