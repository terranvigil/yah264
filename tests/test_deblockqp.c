// Copyright (c) 2026, the yah264 authors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The chroma edge QP the deblocking filter indexes its tables with, against
// the spec's derivation (8.7.2.2 with 8.5.8 and Table 8-15) written out here
// on its own. Above 8 bits a near-zero QPY maps to a QPc below zero, and the
// spec clips only indexA/indexB; clipping each side's QPc first, or the mean,
// filters the edge with thresholds the decoder doesn't use.
#include "../src/encoder/loopfilter.h"
#include <stdio.h>

#define BD_OFFSET_C (6 * (Y264_BIT_DEPTH - 8))

static int clip3(int lo, int hi, int v) { return v < lo ? lo : v > hi ? hi : v; }

/* Table 8-15, qPI 30..51 */
static int spec_qpc(int qpy, int cqo)
{
    static const int tab[22] = { 29, 30, 31, 32, 32, 33, 34, 34, 35, 35, 36,
                                 36, 37, 37, 37, 38, 38, 38, 39, 39, 39, 39 };
    int qpi = clip3(-BD_OFFSET_C, 51, qpy + cqo);
    return qpi < 30 ? qpi : tab[qpi - 30];
}

static int spec_index(int qpy_p, int qpy_q, int cqo, int off)
{
    int qpav = (spec_qpc(qpy_p, cqo) + spec_qpc(qpy_q, cqo) + 1) >> 1;
    return clip3(0, 51, qpav + off);
}

int main(void)
{
    int fails = 0, below0 = 0;
    for (int cqo = -12; cqo <= 12; cqo++)
        for (int p = 0; p <= 51; p++)
            for (int q = 0; q <= 51; q++) {
                int qpav = y264_deblock_chroma_qpav(p, q, cqo);
                below0 += qpav < 0;
                for (int off = -12; off <= 12; off += 2) {
                    int got = clip3(0, 51, qpav + off), want = spec_index(p, q, cqo, off);
                    if (got != want && fails++ < 10)
                        fprintf(stderr, "FAIL qpy %d|%d cqo %d off %d: index %d, spec %d\n",
                                p, q, cqo, off, got, want);
                }
            }
#if Y264_BIT_DEPTH > 8
    /* the case that broke: QPY 0 beside QPY 30 at offset -12 is a QPc of -12
     * beside 18, a qPav of 3; clipping -12 to 0 first gives 9. both orders,
     * since only the macroblock's own side used to be clipped */
    if (y264_deblock_chroma_qpav(0, 30, -12) != 3 || y264_deblock_chroma_qpav(30, 0, -12) != 3) {
        fprintf(stderr, "FAIL qPav(0|30, -12) = %d|%d, want 3\n",
                y264_deblock_chroma_qpav(0, 30, -12), y264_deblock_chroma_qpav(30, 0, -12));
        fails++;
    }
    if (!below0) {
        fprintf(stderr, "FAIL no qPav below 0 reached at %d bits\n", Y264_BIT_DEPTH);
        fails++;
    }
#endif
    if (fails) {
        fprintf(stderr, "%d failures\n", fails);
        return 1;
    }
    printf("deblockqp: chroma edge index matches the spec at %d bits\n", Y264_BIT_DEPTH);
    return 0;
}
