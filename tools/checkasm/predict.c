/*
 * checkasm: intra prediction builders.
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Every mode under every availability combination the encoder can request. The
 * mode gating mirrors the encoder's own i4 / i16 / chroma rules, because a mode
 * whose neighbours do not exist has no defined answer to compare; everything
 * the gate admits is checked, including combinations the mode decision happens
 * never to ask for on real content.
 *
 * 4x4 has no NEON builder -- the blocks are too small to amortize the
 * edge-filter precompute and it measured a net loss -- so its row is portable
 * and guards the dispatching wrapper rather than a kernel.
 */

#include "checkasm.h"
#include "dsp/arch.h"
#include "dsp/predict.h"

#define PORG (8 * STRIDE + 16)

static pixel *rec;

static void recplane(void)
{
    if (!rec)
        rec = ca_guard_alloc(STRIDE * PLANE_H * sizeof(pixel));
    ca_fill(rec, STRIDE * PLANE_H);
}

/* The encoder's availability gate for the nine 4x4 / 8x8 directional modes. */
static int i4_allowed(int mode, int ht, int hl)
{
    switch (mode) {
    case Y264_I4_VERT: case Y264_I4_DDL: case Y264_I4_VL: return ht;
    case Y264_I4_HORIZ: case Y264_I4_HU:                  return hl;
    case Y264_I4_DC:                                      return 1;
    default:                                              return ht && hl;
    }
}

#if Y264_HAVE_NEON

/* The 8x8 NEON builder covers five of the nine modes -- the ones whose
 * diagonals pay for the edge filter -- and the dispatcher routes only those to
 * it. Handing it a mode it does not implement would be testing something the
 * encoder never asks for, so the row asks the same question the dispatcher
 * does. */
static int neon8_mode(int mode)
{
    return mode == Y264_I4_VERT || mode == Y264_I4_DDR || mode == Y264_I4_VR ||
           mode == Y264_I4_HD   || mode == Y264_I4_VL;
}

static int t_intra16x16(void)
{
    pixel *o1 = ca_guard_alloc(256 * sizeof(pixel));
    pixel o2[256];
    int bad = 0;
    for (int t = 0; t < TRIALS; t++) {
        recplane();
        const pixel *rc = rec + PORG;
        for (int ht = 0; ht <= 1; ht++)
            for (int hl = 0; hl <= 1; hl++)
                for (int mode = 0; mode < 4; mode++) {
                    int ok = (mode == Y264_I16_DC) ||
                             (mode == Y264_I16_VERT && ht) ||
                             (mode == Y264_I16_HORIZ && hl) ||
                             (mode == Y264_I16_PLANE && ht && hl);
                    if (!ok)
                        continue;
                    ca_guard_poison(o1, 256 * sizeof(pixel));
                    memset(o1, 0, 256 * sizeof(pixel));
                    memset(o2, 0, sizeof(o2));
                    y264_intra16x16_neon(o1, rc, STRIDE, mode, ht, hl);
                    y264_intra16x16_c(o2, rc, STRIDE, mode, ht, hl);
                    if (memcmp(o1, o2, sizeof(o2))) {
                        if (!bad) ca_fail("intra16x16: mode %d avail %d%d", mode, ht, hl);
                        bad++;
                    }
                    if (!bad && ca_guard_check(o1, 256 * sizeof(pixel), "intra16x16"))
                        bad++;
                }
    }
    {
        const pixel *rc = rec + PORG;
        CA_BENCH2("intra16_vert",
                  y264_intra16x16_neon(o1, rc, STRIDE, Y264_I16_VERT, 1, 1),
                  y264_intra16x16_c(o2, rc, STRIDE, Y264_I16_VERT, 1, 1));
        CA_BENCH2("intra16_plane",
                  y264_intra16x16_neon(o1, rc, STRIDE, Y264_I16_PLANE, 1, 1),
                  y264_intra16x16_c(o2, rc, STRIDE, Y264_I16_PLANE, 1, 1));
    }
    ca_guard_free(o1);
    return bad;
}

/* Both chroma geometries the encoder builds: 4:2:0's 8x8 and 4:2:2's 8x16. */
static int t_intra_chroma(void)
{
    pixel *o1 = ca_guard_alloc(8 * 16 * sizeof(pixel));
    pixel o2[8 * 16];
    int bad = 0;
    for (int t = 0; t < TRIALS; t++) {
        recplane();
        const pixel *rc = rec + PORG;
        for (int ht = 0; ht <= 1; ht++)
            for (int hl = 0; hl <= 1; hl++)
                for (int mode = 0; mode < 4; mode++) {
                    int ok = (mode == Y264_IC_DC) ||
                             (mode == Y264_IC_VERT && ht) ||
                             (mode == Y264_IC_HORIZ && hl) ||
                             (mode == Y264_IC_PLANE && ht && hl);
                    if (!ok)
                        continue;
                    for (int ch = 8; ch <= 16; ch += 8) {
                        ca_guard_poison(o1, (size_t)(8 * ch) * sizeof(pixel));
                        memset(o1, 0, (size_t)(8 * ch) * sizeof(pixel));
                        memset(o2, 0, sizeof(o2));
                        y264_intra_chroma_neon(o1, rc, STRIDE, mode, ht, hl, 8, ch);
                        y264_intra_chroma_c(o2, rc, STRIDE, mode, ht, hl, 8, ch);
                        if (memcmp(o1, o2, (size_t)(8 * ch) * sizeof(pixel))) {
                            if (!bad)
                                ca_fail("intra_chroma: mode %d avail %d%d 8x%d",
                                        mode, ht, hl, ch);
                            bad++;
                        }
                        if (!bad && ca_guard_check(o1, (size_t)(8 * ch) * sizeof(pixel),
                                                   "intra_chroma"))
                            bad++;
                    }
                }
    }
    {
        const pixel *rc = rec + PORG;
        CA_BENCH2("intra_ch_plane",
                  y264_intra_chroma_neon(o1, rc, STRIDE, Y264_IC_PLANE, 1, 1, 8, 8),
                  y264_intra_chroma_c(o2, rc, STRIDE, Y264_IC_PLANE, 1, 1, 8, 8));
    }
    ca_guard_free(o1);
    return bad;
}

static int t_intra8x8(void)
{
    pixel *o1 = ca_guard_alloc(64 * sizeof(pixel));
    pixel o2[64];
    int bad = 0;
    for (int t = 0; t < TRIALS; t++) {
        recplane();
        const pixel *rc = rec + PORG;
        for (int ht = 0; ht <= 1; ht++)
            for (int hl = 0; hl <= 1; hl++)
                for (int htl = 0; htl <= (ht && hl); htl++)
                    for (int htr = 0; htr <= ht; htr++)
                        for (int mode = 0; mode < 9; mode++) {
                            if (!i4_allowed(mode, ht, hl) || !neon8_mode(mode))
                                continue;
                            ca_guard_poison(o1, 64 * sizeof(pixel));
                            memset(o1, 0, 64 * sizeof(pixel));
                            memset(o2, 0, sizeof(o2));
                            y264_intra8x8_neon(o1, rc, STRIDE, mode, ht, hl, htl, htr);
                            y264_intra8x8_c(o2, rc, STRIDE, mode, ht, hl, htl, htr);
                            if (memcmp(o1, o2, sizeof(o2))) {
                                if (!bad)
                                    ca_fail("intra8x8: mode %d avail %d%d%d%d",
                                            mode, ht, hl, htl, htr);
                                bad++;
                            }
                            if (!bad && ca_guard_check(o1, 64 * sizeof(pixel), "intra8x8"))
                                bad++;
                            /* The decision loop's shape: ONE edge derivation
                             * feeding every mode. The kernel takes the flat
                             * edge array; the reference takes the struct. */
                            {
                                y264_i8_edge_t ed;
                                y264_intra8x8_edge_c(&ed, rc, STRIDE, ht, hl, htl, htr);
                                memset(o1, 0, 64 * sizeof(pixel));
                                y264_intra8x8_from_edge_neon(o1, ed.f, mode);
                                if (memcmp(o1, o2, sizeof(o2))) {
                                    if (!bad)
                                        ca_fail("intra8x8_from_edge: mode %d "
                                                "avail %d%d%d%d", mode, ht, hl,
                                                htl, htr);
                                    bad++;
                                }
                            }
                        }
    }
    {
        const pixel *rc = rec + PORG;
        CA_BENCH2("intra8x8_vr",
                  y264_intra8x8_neon(o1, rc, STRIDE, Y264_I4_VR, 1, 1, 1, 1),
                  y264_intra8x8_c(o2, rc, STRIDE, Y264_I4_VR, 1, 1, 1, 1));
        CA_BENCH2("intra8x8_hd",
                  y264_intra8x8_neon(o1, rc, STRIDE, Y264_I4_HD, 1, 1, 1, 1),
                  y264_intra8x8_c(o2, rc, STRIDE, Y264_I4_HD, 1, 1, 1, 1));
        /* The two rows above include the edge derivation and its filter, which
         * they pay on every call. The decision loop does NOT: it derives the
         * edge once and feeds every mode from it, which is the from_edge form
         * below and the only shape a bare predictor elsewhere compares with. */
        {
            y264_i8_edge_t ed;
            y264_intra8x8_edge_c(&ed, rc, STRIDE, 1, 1, 1, 1);
            CA_BENCH2("intra8x8_vr from edge",
                      y264_intra8x8_from_edge_neon(o1, ed.f, Y264_I4_VR),
                      y264_intra8x8_c(o2, rc, STRIDE, Y264_I4_VR, 1, 1, 1, 1));
            CA_BENCH2("intra8x8_hd from edge",
                      y264_intra8x8_from_edge_neon(o1, ed.f, Y264_I4_HD),
                      y264_intra8x8_c(o2, rc, STRIDE, Y264_I4_HD, 1, 1, 1, 1));
        }
    }
    ca_guard_free(o1);
    return bad;
}

#endif /* Y264_HAVE_NEON */

/* ---- the portable rows ---------------------------------------------------- */

/* 4x4 is C everywhere, so this row checks the dispatching wrapper against the
 * reference builder rather than a kernel. It stays because the wrapper is where
 * a future x86 4x4 builder will be routed, and because the availability gate
 * itself is worth pinning. */
static int t_intra4x4(void)
{
    pixel *o1 = ca_guard_alloc(16 * sizeof(pixel));
    pixel o2[16];
    int bad = 0;
    for (int t = 0; t < TRIALS; t++) {
        recplane();
        const pixel *rc = rec + PORG;
        for (int ht = 0; ht <= 1; ht++)
            for (int hl = 0; hl <= 1; hl++)
                for (int htl = 0; htl <= (ht && hl); htl++)
                    for (int htr = 0; htr <= ht; htr++)
                        for (int mode = 0; mode < 9; mode++) {
                            if (!i4_allowed(mode, ht, hl))
                                continue;
                            ca_guard_poison(o1, 16 * sizeof(pixel));
                            memset(o1, 0, 16 * sizeof(pixel));
                            memset(o2, 0, sizeof(o2));
                            y264_intra4x4(o1, rc, STRIDE, mode, ht, hl, htl, htr);
                            y264_intra4x4_c(o2, rc, STRIDE, mode, ht, hl, htl, htr);
                            if (memcmp(o1, o2, sizeof(o2))) {
                                if (!bad)
                                    ca_fail("intra4x4: mode %d avail %d%d%d%d",
                                            mode, ht, hl, htl, htr);
                                bad++;
                            }
                            if (!bad && ca_guard_check(o1, 16 * sizeof(pixel), "intra4x4"))
                                bad++;
                        }
    }
    ca_guard_free(o1);
    return bad;
}

/* The edge derivation: one precomputed edge must build the same block as the
 * whole-function builder, on both the C and the dispatched side. This is what
 * makes the decision loop's precompute legitimate. */
static int t_intra8x8_edge(void)
{
    pixel whole[64], from_c[64], from_disp[64];
    int bad = 0;
    for (int t = 0; t < TRIALS; t++) {
        recplane();
        const pixel *rc = rec + PORG;
        for (int ht = 0; ht <= 1; ht++)
            for (int hl = 0; hl <= 1; hl++)
                for (int htl = 0; htl <= (ht && hl); htl++)
                    for (int htr = 0; htr <= ht; htr++) {
                        y264_i8_edge_t ed;
                        y264_intra8x8_edge_c(&ed, rc, STRIDE, ht, hl, htl, htr);
                        for (int mode = 0; mode < 9; mode++) {
                            if (!i4_allowed(mode, ht, hl))
                                continue;
                            memset(whole, 0, sizeof(whole));
                            memset(from_c, 0, sizeof(from_c));
                            memset(from_disp, 0, sizeof(from_disp));
                            y264_intra8x8_c(whole, rc, STRIDE, mode, ht, hl, htl, htr);
                            y264_intra8x8_from_edge_c(from_c, &ed, mode, ht, hl);
                            y264_intra8x8_from_edge(from_disp, &ed, mode, ht, hl);
                            if (memcmp(whole, from_c, sizeof(whole)) ||
                                memcmp(whole, from_disp, sizeof(whole))) {
                                if (!bad)
                                    ca_fail("intra8x8_edge: mode %d avail %d%d%d%d",
                                            mode, ht, hl, htl, htr);
                                bad++;
                            }
                        }
                    }
    }
    return bad;
}

const ca_test ca_predict_tests[] = {
#if Y264_HAVE_NEON
    { "intra16x16",    "predict", Y264_CPU_NEON, t_intra16x16 },
    { "intra_chroma",  "predict", Y264_CPU_NEON, t_intra_chroma },
    { "intra8x8",      "predict", Y264_CPU_NEON, t_intra8x8 },
#endif
    { "intra4x4",      "predict", 0, t_intra4x4 },
    { "intra8x8_edge", "predict", 0, t_intra8x8_edge },
    { NULL, "predict", 0, NULL },
};
