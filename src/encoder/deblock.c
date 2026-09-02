/*
 * deblock.c - H.264 in-loop deblocking filter (ITU-T H.264 8.7)
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Applied after a frame is fully reconstructed (intra prediction uses the
 * pre-deblock samples), in macroblock raster order, in place: all vertical
 * edges of a macroblock then all horizontal edges. Must match the decoder
 * exactly, which the recon-match conformance gate verifies across every QP.
 */
#include "deblock.h"
#include "../common/stgprof.h"
#include "../dsp/transform.h"
#include "../common/threadpool.h"
#include "../common/cpu.h"
#include "../dsp/deblock.h"
#include <stdlib.h>
#include <string.h>

#if defined(__aarch64__) && Y264_BIT_DEPTH == 8
void y264_deblock_luma_v4_neon(pixel *q0, int stride, int bs, int alpha,
                               int beta, int tc0);
void y264_deblock_luma_h4_neon(pixel *q0, int stride, int bs, int alpha,
                               int beta, int tc0);
static int db_have_neon(void) { return y264_asm_on(Y264_ASM_DEBLOCK); }
#endif

/* Table 8-16: alpha and beta thresholds, indexed by indexA / indexB. */
static const uint8_t ALPHA[52] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,4,5,6,7,8,9,10,12,13,
    15,17,20,22,25,28,32,36,40,45,50,56,63,71,80,90,101,113,
    127,144,162,182,203,226,255,255
};
static const uint8_t BETA[52] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,3,3,3,3,4,4,4,6,6,
    7,7,8,8,9,9,10,10,11,11,12,12,13,13,14,14,15,15,16,16,17,17,18,18
};
/* Table 8-17: tc0, indexed by indexA and boundary strength (bS = 1, 2, 3).
 * Matches x264's tc0 table. */
static const uint8_t TC0[52][3] = {
    {0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},
    {0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},
    {0,0,0},{0,0,1},{0,0,1},{0,0,1},{0,0,1},{0,1,1},{0,1,1},{1,1,1},
    {1,1,1},{1,1,1},{1,1,1},{1,1,2},{1,1,2},{1,1,2},{1,1,2},{1,2,3},
    {1,2,3},{2,2,3},{2,2,4},{2,3,4},{2,3,4},{3,3,5},{3,4,6},{3,4,6},
    {4,5,7},{4,5,8},{4,6,9},{5,7,10},{6,8,11},{6,8,13},{7,10,14},
    {8,11,16},{9,12,18},{10,13,20},{11,15,23},{13,17,25}
};

/* One name for "the NEON chroma-edge kernel exists in this build", used by the
 * declaration, the predicate and the call site alike. They were guarded
 * separately before, and the call site was missed: at BD>8 the predicate
 * compiled to a constant 0 but the unreachable call still needed a declaration
 * that was not there, so a 10-bit build did not compile at all. */
#if defined(__aarch64__) && Y264_BIT_DEPTH == 8
#define Y264_DEBLOCK_CHROMA_NEON 1
#else
#define Y264_DEBLOCK_CHROMA_NEON 0
#endif

#if Y264_DEBLOCK_CHROMA_NEON
/* Whole-chroma-edge NEON filter (dsp/deblock_neon.c): eight lines in one pass
 * with per-lane tc and bS==4 select. Horizontal edges only -- the vertical
 * shape needs a gather/scatter across the stride and measured 0.87x, see the
 * kernel file. 4:4:4 chroma uses the LUMA-style filter
 * (chromaStyleFilteringFlag == 0), so it stays on the scalar path too. */
void y264_deblock_chroma8_h_neon(pixel *q0, int stride, int alpha, int beta,
                                 const uint8_t bs[4], const uint8_t tc0tab[3],
                                 int span, int g);
static inline int chroma_edge_neon(int cstyle)
{
    return cstyle && y264_asm_on(Y264_ASM_DEBLOCK);
}
#else
static inline int chroma_edge_neon(int cstyle) { (void)cstyle; return 0; }
#endif

static inline int clip1(int v) { return v < 0 ? 0 : (v > PIXEL_MAX ? PIXEL_MAX : v); }
static inline int clip3(int lo, int hi, int v)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Filter one line across an edge. `q0` points at the first q sample; `step` is
 * the sample stride across the edge (1 for a vertical edge, plane stride for a
 * horizontal edge). */
static void filter_line(pixel *q0, int step, int bs, int alpha, int beta,
                        int tc0, int chroma)
{
    /* The alpha/beta/tC0 tables are 8-bit thresholds; at higher bit depth the
 * spec scales them by 2^(BitDepth-8) (8.7.2.2). The literal +1/+2 terms in
 * the filter stay unscaled -- they operate in the already-scaled domain. */
    alpha <<= (Y264_BIT_DEPTH - 8);
    beta  <<= (Y264_BIT_DEPTH - 8);
    tc0   <<= (Y264_BIT_DEPTH - 8);
    int p0 = q0[-step], p1 = q0[-2 * step], p2 = q0[-3 * step], p3 = q0[-4 * step];
    int Q0 = q0[0], Q1 = q0[step], Q2 = q0[2 * step], Q3 = q0[3 * step];

    if (abs(p0 - Q0) >= alpha || abs(p1 - p0) >= beta || abs(Q1 - Q0) >= beta)
        return;

    int ap = abs(p2 - p0), aq = abs(Q2 - Q0);

    if (bs < 4) {
        int tc = chroma ? tc0 + 1
                        : tc0 + (ap < beta ? 1 : 0) + (aq < beta ? 1 : 0);
        int delta = clip3(-tc, tc, ((Q0 - p0) * 4 + (p1 - Q1) + 4) >> 3);
        q0[-step] = (pixel)clip1(p0 + delta);
        q0[0]     = (pixel)clip1(Q0 - delta);
        if (!chroma) {
            if (ap < beta)
                q0[-2 * step] = (pixel)(p1 + clip3(-tc0, tc0,
                    (p2 + ((p0 + Q0 + 1) >> 1) - 2 * p1) >> 1));
            if (aq < beta)
                q0[step] = (pixel)(Q1 + clip3(-tc0, tc0,
                    (Q2 + ((p0 + Q0 + 1) >> 1) - 2 * Q1) >> 1));
        }
    } else {
        int strong = abs(p0 - Q0) < ((alpha >> 2) + 2);
        if (chroma) {
            q0[-step] = (pixel)((2 * p1 + p0 + Q1 + 2) >> 2);
            q0[0]     = (pixel)((2 * Q1 + Q0 + p1 + 2) >> 2);
            return;
        }
        if (ap < beta && strong) {
            q0[-step]     = (pixel)((p2 + 2*p1 + 2*p0 + 2*Q0 + Q1 + 4) >> 3);
            q0[-2 * step] = (pixel)((p2 + p1 + p0 + Q0 + 2) >> 2);
            q0[-3 * step] = (pixel)((2*p3 + 3*p2 + p1 + p0 + Q0 + 4) >> 3);
        } else {
            q0[-step] = (pixel)((2 * p1 + p0 + Q1 + 2) >> 2);
        }
        if (aq < beta && strong) {
            q0[0]         = (pixel)((Q2 + 2*Q1 + 2*Q0 + 2*p0 + p1 + 4) >> 3);
            q0[step]      = (pixel)((Q2 + Q1 + Q0 + p0 + 2) >> 2);
            q0[2 * step]  = (pixel)((2*Q3 + 3*Q2 + Q1 + Q0 + p0 + 4) >> 3);
        } else {
            q0[0] = (pixel)((2 * Q1 + Q0 + p1 + 2) >> 2);
        }
    }
}

/* Per-macroblock bS grid, derived in one pass by dsp/deblock.c. It used to be
 * filled lazily -- the chroma edges re-derive strengths the luma loops already
 * computed, and with the 8x8 transform the luma loops skip odd internal edges
 * the 4:2:2 chroma loop still needs -- but the memo's per-edge overhead cost
 * more than deriving all thirty-two together, and the batched shape is the one
 * a kernel can take. */
struct bs_grid { uint8_t v[4][4], h[4][4]; };   /* v[xb][yb], h[yb][xb] */

/* Does this edge carry any strength at all? Only about 15% of them do
 * (measured on samsung at 1200 and bus at 400: 0.60-0.62 live segments per
 * edge out of four), so the four-segment loop under it was almost always four
 * loads and four not-taken branches. The grid is stored edge-major precisely
 * so this is one 32-bit test. */
static inline int bs_any(const uint8_t b[4])
{
    uint32_t w;
    memcpy(&w, b, 4);
    return w != 0;
}

static void bs_derive(y264_frame_t *f, int mbx, int mby, struct bs_grid *g)
{
    /* I slice: every block on both sides of every edge is intra, so 8.7.2.1
 * stops at its first test and the whole grid is a constant -- 4 on the
 * macroblock edge, 3 inside. Worth special-casing because the derivation
 * this replaced was ALSO one test per edge here, and without the shortcut
 * an all-intra cell paid 0.5-0.7% for the batched form it cannot use. */
    if (f->slice_type == 0) {
        memset(g->v, 3, sizeof g->v);
        memset(g->h, 3, sizeof g->h);
        memset(g->v[0], mbx ? 4 : 0, 4);
        memset(g->h[0], mby ? 4 : 0, 4);
        return;
    }
    int bx0 = mbx * 4, by0 = mby * 4;
    int i = by0 * f->mv_stride + bx0, n = by0 * f->nnz_stride[0] + bx0;
    struct y264_bs_ctx c = {
        .ref0 = f->refidx + i,  .ref1 = f->refidx1 + i,
        .mvx0 = f->mvx + i,     .mvy0 = f->mvy + i,
        .mvx1 = f->mvx1 + i,    .mvy1 = f->mvy1 + i,
        .mv_stride = f->mv_stride,
        .nnz = f->nnz[0] + n,   .nnz_stride = f->nnz_stride[0],
        .tr8_cur  = (uint8_t)(f->mb_tr8 ? f->mb_tr8[mby * f->wmb + mbx] : 0),
        .tr8_left = (uint8_t)(f->mb_tr8 && mbx ? f->mb_tr8[mby * f->wmb + mbx - 1] : 0),
        .tr8_top  = (uint8_t)(f->mb_tr8 && mby ? f->mb_tr8[(mby - 1) * f->wmb + mbx] : 0),
        .have_left = (uint8_t)(mbx > 0), .have_top = (uint8_t)(mby > 0),
    };
#if defined(__aarch64__) && Y264_BIT_DEPTH == 8
    if (db_have_neon()) { y264_deblock_strength_neon(&c, g->v, g->h); return; }
#endif
    y264_deblock_strength_c(&c, g->v, g->h);
}

/* One macroblock's edges. Self-contained given (mbx, mby) -- no state carries
 * between macroblocks -- which is what lets the frame loop run on the wavefront. */
static void deblock_mb(y264_frame_t *f, int mbx, int mby)
{
    int rs = f->rec_stride[0];
    pixel *Y = f->rec[0];
    int wmb = f->wmb;
    /* Edge QP is the average of the two macroblocks' QPs (8.7.2.2). Constant
 * QP -> mbqp all equal -> identical to a single-QP filter. */
#define MBQP(mx, my) (f->mbqp ? f->mbqp[(my) * wmb + (mx)] : f->qp)
    {
        {
            int by0 = mby * 4;
            int tr8 = f->mb_tr8 ? f->mb_tr8[mby * f->wmb + mbx] : 0;
            struct bs_grid bsg;
            bs_derive(f, mbx, mby, &bsg);
            /* indexA/B = Clip3(0,51, qPav + offset) (spec 8.7.2.2); the tables are
 * 52-entry. Clamp defends the lookup if filter offsets are ever wired
 * (they are 0 today, so this is a no-op / byte-identical). */
            int qpc = clip3(0, 51, MBQP(mbx, mby));
            int qpv = mbx > 0 ? clip3(0, 51, (MBQP(mbx - 1, mby) + qpc + 1) >> 1) : qpc;
            int qph = mby > 0 ? clip3(0, 51, (MBQP(mbx, mby - 1) + qpc + 1) >> 1) : qpc;
            int cqc = clip3(0, 51, y264_chroma_qp(qpc, 0));
            int cqv = mbx > 0 ? clip3(0, 51, (y264_chroma_qp(MBQP(mbx - 1, mby), 0) + cqc + 1) >> 1) : cqc;
            int cqh = mby > 0 ? clip3(0, 51, (y264_chroma_qp(MBQP(mbx, mby - 1), 0) + cqc + 1) >> 1) : cqc;

            /* luma vertical edges (xb = 0,1,2,3 -> x = 0,4,8,12) */
            for (int xb = 0; xb < 4; xb++) {
                int lx = mbx * 16 + xb * 4;
                if (lx == 0) continue;
                if (tr8 && (xb & 1)) continue;  /* 8x8 transform: no internal 4x4 edge */
                if (!bs_any(bsg.v[xb])) continue;
                int mb_edge = (xb == 0), q = mb_edge ? qpv : qpc;
                int qa = ALPHA[q], qb = BETA[q];
                const uint8_t *qtc = TC0[q];
                for (int yb = 0; yb < 4; yb++) {
                    int bs = bsg.v[xb][yb];
                    if (!bs) continue;
#if defined(__aarch64__) && Y264_BIT_DEPTH == 8
                    if (db_have_neon()) {
                        y264_deblock_luma_v4_neon(Y + (by0 * 4 + yb * 4) * rs + lx,
                                                  rs, bs, qa, qb,
                                                  qtc[bs < 4 ? bs - 1 : 0]);
                        continue;
                    }
#endif
                    for (int ln = 0; ln < 4; ln++)
                        filter_line(Y + (by0 * 4 + yb * 4 + ln) * rs + lx, 1,
                                    bs, qa, qb, qtc[bs < 4 ? bs - 1 : 0], 0);
                }
            }
            /* luma horizontal edges (yb = 0,1,2,3 -> y = 0,4,8,12) */
            for (int yb = 0; yb < 4; yb++) {
                int ly = mby * 16 + yb * 4;
                if (ly == 0) continue;
                if (tr8 && (yb & 1)) continue;  /* 8x8 transform: no internal 4x4 edge */
                if (!bs_any(bsg.h[yb])) continue;
                int mb_edge = (yb == 0), q = mb_edge ? qph : qpc;
                int qa = ALPHA[q], qb = BETA[q];
                const uint8_t *qtc = TC0[q];
                for (int xb = 0; xb < 4; xb++) {
                    int bs = bsg.h[yb][xb];
                    if (!bs) continue;
#if defined(__aarch64__) && Y264_BIT_DEPTH == 8
                    if (db_have_neon()) {
                        y264_deblock_luma_h4_neon(Y + ly * rs + (mbx * 16 + xb * 4),
                                                  rs, bs, qa, qb,
                                                  qtc[bs < 4 ? bs - 1 : 0]);
                        continue;
                    }
#endif
                    for (int ln = 0; ln < 4; ln++)
                        filter_line(Y + ly * rs + (mbx * 16 + xb * 4 + ln), rs,
                                    bs, qa, qb, qtc[bs < 4 ? bs - 1 : 0], 0);
                }
            }
            /* Chroma (MbWidthC x MbHeightC): edges every 4 chroma samples. A
 * chroma sample maps to luma (cx*SubWidthC, cy*SubHeightC), so the
 * bS is read from luma 4x4 (xb = e*SubWidthC on vertical edges,
 * yb = e*SubHeightC on horizontal edges); each luma 4x4 spans
 * 4/SubWidthC chroma cols and 4/SubHeightC chroma rows. This covers
 * 8x8 (4:2:0), 8x16 (4:2:2) and 16x16 (4:4:4). */
            int cw = 16 / f->sub_w, ch = 16 / f->sub_h;
            int colspan = 4 / f->sub_w, rowspan = 4 / f->sub_h;
            /* 4:4:4 uses the luma-strength filter on chroma (chromaStyleFilteringFlag
 * = chromaEdgeFlag && ChromaArrayType != 3 = 0), still with chroma QP. */
            int cstyle = f->cf_idc == 3 ? 0 : 1;
            for (int c = 0; c < 2; c++) {
                int crs = f->rec_stride[1 + c];
                pixel *C = f->rec[1 + c];
                int cx0 = mbx * cw, cy0 = mby * ch;
                for (int e = 0; e < cw / 4; e++) {
                    int cx = cx0 + e * 4;
                    if (cx == 0) continue;
                    int xb = e * f->sub_w, mb_edge = (e == 0), cq = mb_edge ? cqv : cqc;
                    int ca = ALPHA[cq], cb = BETA[cq];
                    const uint8_t *ctc = TC0[cq];
                    const uint8_t *bs4v = bsg.v[xb];
                    if (!bs_any(bs4v)) continue;
                    for (int yb = 0; yb < 4; yb++) {
                        int bs = bs4v[yb];
                        if (!bs) continue;
                        for (int ln = 0; ln < rowspan; ln++)
                            filter_line(C + (cy0 + yb * rowspan + ln) * crs + cx, 1,
                                        bs, ca, cb, ctc[bs < 4 ? bs - 1 : 0], cstyle);
                    }
                }
                for (int e = 0; e < ch / 4; e++) {
                    int cy = cy0 + e * 4;
                    if (cy == 0) continue;
                    int yb = e * f->sub_h, mb_edge = (e == 0), cq = mb_edge ? cqh : cqc;
                    int ca = ALPHA[cq], cb = BETA[cq];
                    const uint8_t *ctc = TC0[cq];
                    const uint8_t *bs4h = bsg.h[yb];
                    if (!bs_any(bs4h)) continue;
#if Y264_DEBLOCK_CHROMA_NEON
                    if (chroma_edge_neon(cstyle)) {
                        for (int g = 0; g < colspan / 2; g++)
                            y264_deblock_chroma8_h_neon(C + cy * crs + cx0 + g * 8,
                                                        crs, ca, cb, bs4h, ctc, colspan, g);
                        continue;
                    }
#endif
                    for (int xb = 0; xb < 4; xb++) {
                        int bs = bs4h[xb];
                        if (!bs) continue;
                        for (int ln = 0; ln < colspan; ln++)
                            filter_line(C + cy * crs + (cx0 + xb * colspan + ln), crs,
                                        bs, ca, cb, ctc[bs < 4 ? bs - 1 : 0], cstyle);
                    }
                }
            }
        }
    }
#undef MBQP
}

/* Deblock on the row wavefront. The filter's macroblock dependency is left +
 * top: MB (x,y) rewrites the last columns of (x-1,y) and the last rows of
 * (x,y-1), and that macroblock's bottom-RIGHT corner is also touched by
 * (x+1,y-1). ntp_wavefront's ordering -- left done, row above done through
 * col+1 -- dominates all three, so every macroblock sees exactly the pixels the
 * raster loop would have handed it: byte-identical at any thread count.
 *
 * Cells are CHUNKS of macroblocks. Per-MB deblock is only ~0.6us and a 1-MB cell
 * pays the per-cell sync on every one of them; a chunk still satisfies the
 * dependency, since "chunk left done, row above done through chunk+1" dominates
 * every constituent macroblock's requirement. */
#define DB_CHUNK 4
static void deblock_cell(void *ctx, int tid, int r, int c)
{
    y264_frame_t *f = ctx;
    (void)tid;
    int x0 = c * DB_CHUNK, x1 = x0 + DB_CHUNK;
    if (x1 > f->wmb) x1 = f->wmb;
    for (int mbx = x0; mbx < x1; mbx++)
        deblock_mb(f, mbx, r);
}

void y264_deblock_rows(y264_frame_t *f, int mby0, int mby1)
{
    for (int mby = mby0; mby < mby1; mby++)
        for (int mbx = 0; mbx < f->wmb; mbx++)
            deblock_mb(f, mbx, mby);
}

void y264_deblock_frame(y264_frame_t *f)
{
    STG_BEG(STG_DEBLOCK);
    ntp_pool_t *pool = (ntp_pool_t *)f->pool;
    if (pool && ntp_pool_nthreads(pool) > 1) {
        ntp_prof_tag("deblock"); ntp_prio_hint();
        ntp_wavefront(pool, f->hmb, (f->wmb + DB_CHUNK - 1) / DB_CHUNK,
                      NULL, deblock_cell, f);
        STG_END();
        return;
    }
    for (int mby = 0; mby < f->hmb; mby++)
        for (int mbx = 0; mbx < f->wmb; mbx++)
            deblock_mb(f, mbx, mby);
    STG_END();  /* STG_DEBLOCK */
}
