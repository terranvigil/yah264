/*
 * test_transform.c - sanity checks for transforms and quant round-trip
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * These are smoke tests; the authoritative check that transforms/quant match
 * the H.264 spec is the recon-match conformance gate against an independent
 * decoder.
 */
#include "dsp/transform.h"
#include <stdio.h>
#include <stdlib.h>

static int fails = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } \
} while (0)

static uint64_t rng = 0x123456789abcdefULL;
static int rnd_range(int lo, int hi)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return lo + (int)((rng >> 33) % (uint64_t)(hi - lo + 1));
}

static void test_dc_only(void)
{
    /* A constant residual transforms to a pure DC coefficient of 16*C. */
    dctcoef diff[16], coef[16];
    for (int i = 0; i < 16; i++) diff[i] = 5;
    y264_fdct4x4(diff, coef);
    CHECK(coef[0] == 16 * 5, "fdct DC = %d, want %d", coef[0], 16 * 5);
    for (int i = 1; i < 16; i++)
        CHECK(coef[i] == 0, "fdct AC[%d] = %d, want 0", i, coef[i]);

    dctcoef had[16];
    y264_hadamard4x4(diff, had);
    CHECK(had[0] == 16 * 5, "hadamard4x4 DC = %d", had[0]);
    for (int i = 1; i < 16; i++)
        CHECK(had[i] == 0, "hadamard4x4[%d] = %d, want 0", i, had[i]);

    dctcoef c2[4], h2[4];
    for (int i = 0; i < 4; i++) c2[i] = 7;
    y264_hadamard2x2(c2, h2);
    CHECK(h2[0] == 4 * 7, "hadamard2x2 DC = %d", h2[0]);
    CHECK(h2[1] == 0 && h2[2] == 0 && h2[3] == 0, "hadamard2x2 AC nonzero");
}

static void test_roundtrip(void)
{
    /* quant/dequant/transform round-trip error must be bounded and grow with QP.
 * At QP 0 the error should be tiny; we just require it stays sane. */
    for (int qp = 0; qp <= 51; qp += 3) {
        int max_err = 0;
        for (int trial = 0; trial < 200; trial++) {
            dctcoef diff[16], coef[16], lev[16], deq[16], res[16];
            for (int i = 0; i < 16; i++)
                diff[i] = (dctcoef)rnd_range(-64, 64);
            y264_fdct4x4(diff, coef);
            y264_quant_4x4(coef, lev, qp, 1, NULL);
            y264_dequant_4x4(lev, deq, qp, NULL);
            y264_idct4x4(deq, res);
            for (int i = 0; i < 16; i++) {
                int e = abs(res[i] - diff[i]);
                if (e > max_err) max_err = e;
            }
        }
        /* Loose ceiling: dominated by the quant step size at this QP. */
        int ceil = 8 + (1 << (qp / 6)) * 4;
        CHECK(max_err <= ceil, "qp %d round-trip max_err %d > %d", qp, max_err, ceil);
    }
}

static void test_dc_only_8x8(void)
{
    /* A constant residual C transforms to a pure DC of 64*C (8x8 gain). */
    dctcoef diff[64], coef[64];
    for (int i = 0; i < 64; i++) diff[i] = 3;
    y264_fdct8x8(diff, coef);
    CHECK(coef[0] == 64 * 3, "fdct8x8 DC = %d, want %d", coef[0], 64 * 3);
    for (int i = 1; i < 64; i++)
        CHECK(coef[i] == 0, "fdct8x8 AC[%d] = %d, want 0", i, coef[i]);
}

static void test_roundtrip_8x8(void)
{
    for (int qp = 0; qp <= 51; qp += 3) {
        int max_err = 0;
        for (int trial = 0; trial < 100; trial++) {
            dctcoef diff[64], coef[64], lev[64], deq[64], res[64];
            for (int i = 0; i < 64; i++)
                diff[i] = (dctcoef)rnd_range(-64, 64);
            y264_fdct8x8(diff, coef);
            y264_quant_8x8(coef, lev, qp, 1, NULL);
            y264_dequant_8x8(lev, deq, qp, NULL);
            y264_idct8x8(deq, res);
            for (int i = 0; i < 64; i++) {
                int e = abs(res[i] - diff[i]);
                if (e > max_err) max_err = e;
            }
        }
        int ceil = 8 + (1 << (qp / 6)) * 4;
        CHECK(max_err <= ceil, "qp %d 8x8 round-trip max_err %d > %d", qp, max_err, ceil);
    }
}

static void test_zero(void)
{
    dctcoef z[16] = {0}, coef[16], lev[16], deq[16], res[16];
    y264_fdct4x4(z, coef);
    y264_quant_4x4(coef, lev, 26, 1, NULL);
    y264_dequant_4x4(lev, deq, 26, NULL);
    y264_idct4x4(deq, res);
    for (int i = 0; i < 16; i++)
        CHECK(res[i] == 0, "zero residual produced %d at %d", res[i], i);
}

int main(void)
{
    test_dc_only();
    test_dc_only_8x8();
    test_roundtrip();
    test_roundtrip_8x8();
    test_zero();
    if (fails) {
        printf("test_transform: %d failure(s)\n", fails);
        return 1;
    }
    printf("test_transform: all passed\n");
    return 0;
}
