/*
 * test_trellis.c - byte-identity gate for the CABAC Viterbi RDOQ kernels
 * (y264_cabac_trellis_4x4 / _8x8). These kernels are coefficient-preserving:
 * given (contexts, qn, abscoef, unmf, w2, ...) they must produce EXACTLY the
 * same absout regardless of how they are implemented internally. This test
 * drives both kernels over a large deterministic input corpus and folds every
 * output into an FNV-1a hash; the hash is frozen as a golden constant so any
 * kernel rewrite (e.g. the x264-style 4-byte node-state port, see
 * docs/archive/trellis-kernel-plan.md) is gated on producing bit-identical decisions.
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Capture flow: run once with Y264_TRELLIS_PRINT=1 to print the hash, bake it
 * into GOLDEN below, then the test fails on any divergence.
 */
#include "../src/encoder/cabac.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* deterministic PRNG (xorshift32), so the corpus is fixed across runs/machines */
static uint32_t rng = 0x1234567u;
static uint32_t xr(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static int rr(int lo, int hi) { return lo + (int)(xr() % (uint32_t)(hi - lo + 1)); }

static uint64_t H = 1469598103934665603ull;      /* FNV-1a offset basis */
static void fold(int v) {
    uint32_t u = (uint32_t)v;
    for (int b = 0; b < 4; b++) { H ^= (u >> (8 * b)) & 0xff; H *= 1099511628211ull; }
}

/* Fill one block of trellis inputs with realistic-range random values. Ranges
 * are chosen to exercise both the drop (a==0) and keep (a>0) transitions and a
 * spread of level magnitudes; exact values are irrelevant to a regression gate,
 * only that the corpus is fixed and varied. */
static void gen_block(int n, int *qn, int *absc, long *unmf, int *w2) {
    for (int i = 0; i < n; i++) {
        qn[i]   = rr(0, 18);                     /* quantized level (0 => forced drop) */
        absc[i] = rr(0, 3200);                   /* pre-quant |coef| */
        unmf[i] = (long)rr(90, 26000);           /* dequant multiplier */
        w2[i]   = rr(1, 280);                    /* transform weight^2 */
    }
}

int main(void) {
    const char *pr = getenv("Y264_TRELLIS_PRINT");

    /* A pool of distinct context states: init at several qp / init_idc / slice
 * types so the kernels see varied c->ctx starting points. */
    y264_cabac_t pool[8];
    int np = 0;
    for (int st = 0; st <= 2 && np < 8; st++)
        for (int qp = 12; qp <= 44 && np < 8; qp += 16)
            y264_cabac_init_contexts(&pool[np++], st == 2 ? 1 : st, st == 2 ? 1 : 0, qp);

    int qn[64], absc[64], w2[64]; long unmf[64]; int out[64];

    /* 4x4 kernel: all 14 ctxBlockCat indices, both n (15 AC / 16 non-AC) plus a
 * couple of short blocks, nza/nzb both bits, a range of lambda. */
    for (int cat = 0; cat < 14; cat++)
        for (int trial = 0; trial < 600; trial++) {
            const y264_cabac_t *c = &pool[xr() & 7];
            int nlist[4] = {16, 15, 8, 4};
            int n = nlist[xr() & 3];
            int nza = xr() & 1, nzb = xr() & 1;
            long lambda = rr(1, 2000);
            gen_block(n, qn, absc, unmf, w2);
            memset(out, 0x5a, sizeof(out));
            y264_cabac_trellis_4x4(c, cat, nza, nzb, lambda, n, qn, absc, unmf, w2, 0, NULL, 0, out);
            for (int i = 0; i < n; i++) fold(out[i]);
        }

    /* 8x8 kernel: 64-coefficient luma block, no cat/nza/nzb. */
    for (int trial = 0; trial < 4000; trial++) {
        const y264_cabac_t *c = &pool[xr() & 7];
        long lambda = rr(1, 2000);
        gen_block(64, qn, absc, unmf, w2);
        memset(out, 0x5a, sizeof(out));
        y264_cabac_trellis_8x8(c, lambda, qn, absc, unmf, w2, 0, NULL, 0, out);
        for (int i = 0; i < 64; i++) fold(out[i]);
    }

    if (pr) { printf("trellis golden hash = 0x%016llx\n", (unsigned long long)H); return 0; }

    const uint64_t GOLDEN = 0x2a0048d746364c0bull;   /* captured 2026-07-24, pre-rewrite */
    if (H != GOLDEN) {
        fprintf(stderr, "test_trellis: FAIL hash 0x%016llx != golden 0x%016llx\n",
                (unsigned long long)H, (unsigned long long)GOLDEN);
        return 1;
    }
    printf("test_trellis: OK (0x%016llx)\n", (unsigned long long)H);
    return 0;
}
