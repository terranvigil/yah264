/*
 * test_denoise.c - the two pre-quantisation denoisers: --nr's offsets and
 * --mcstf's queue.
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "encoder/nr.h"
#include "encoder/mcstf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } \
} while (0)

static void test_nr(void)
{
    y264_nr nr;
    y264_nr_tally t;
    memset(&nr, 0, sizeof nr);
    memset(&t, 0, sizeof t);
    /* an empty estimate, or a zero strength, is the plain quantiser */
    y264_nr_offsets(&nr, 500, 500);
    for (int i = 0; i < Y264_NR_SLOTS; i++)
        CHECK(nr.off[i] == 0, "empty estimate offset[%d] = %d", i, nr.off[i]);
    /* feed inter luma 4x4 blocks whose every position averages 8 */
    dctcoef c[16];
    for (int b = 0; b < 100; b++) {
        for (int i = 0; i < 16; i++) c[i] = (dctcoef)((b & 1) ? 8 : -8);
        y264_nr_block(nr.off, &t, c, 16, 0, 0, 0);
    }
    y264_nr_fold(&nr, &t);
    y264_nr_offsets(&nr, 0, 0);
    for (int i = 0; i < Y264_NR_SLOTS; i++)
        CHECK(nr.off[i] == 0, "zero strength offset[%d] = %d", i, nr.off[i]);
    y264_nr_offsets(&nr, 2000, 2000);
    const uint16_t *o = nr.off + y264_nr_group(0, 0) * Y264_NR_GROUP;
    for (int i = 0; i < 16; i++)
        CHECK(o[i] <= 8, "offset[%d] = %d exceeds the position mean 8", i, o[i]);
    y264_nr_offsets(&nr, 20, 20);
    /* the gain weighting: position (1,1) carries the largest gain of a 4x4 */
    CHECK(o[5] > o[0], "gain weight: offset(1,1) %d <= offset(0,0) %d", o[5], o[0]);
    /* the subtraction stops at zero and keeps the sign */
    for (int i = 0; i < 16; i++) c[i] = (dctcoef)(i & 1 ? -3 : 40);
    y264_nr_block(nr.off, NULL, c, 16, 0, 0, 0);
    for (int i = 0; i < 16; i++) {
        CHECK((i & 1) ? c[i] <= 0 : c[i] >= 0, "sign flipped at %d: %d", i, c[i]);
        CHECK((i & 1) ? c[i] >= -3 : c[i] <= 40, "grew at %d: %d", i, c[i]);
    }
    /* intra statistics are untouched by inter blocks */
    const uint16_t *oi = nr.off + y264_nr_group(1, 0) * Y264_NR_GROUP;
    for (int i = 0; i < 16; i++)
        CHECK(oi[i] == 0, "intra offset[%d] = %d with no intra blocks", i, oi[i]);
}

static void test_mcstf(void)
{
    enum { W = 64, H = 48, N = 11 };
    y264_mcstf_cfg cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.range = 2; cfg.period = 1; cfg.sigma_q8 = 6 * 256; cfg.clean_q8 = 0;
    cfg.adapt = 1; cfg.adapt_k = 5; cfg.subpel = 1; cfg.cut = 1;
    y264_mcstf *m = y264_mcstf_open(&cfg, W, H, 1, 1, 26.0);
    CHECK(m != NULL, "mcstf_open");
    if (!m)
        return;
    static pixel pl[3][W * H];
    for (int i = 0; i < W * H; i++) pl[0][i] = (pixel)((i * 37) & 255);
    for (int i = 0; i < W * H / 4; i++) { pl[1][i] = (pixel)(i & 255); pl[2][i] = (pixel)(200 - (i & 63)); }
    const pixel *in[3] = { pl[0], pl[1], pl[2] };
    int st[3] = { W, W / 2, W / 2 };
    int got = 0, same = 1;
    for (int f = 0; f <= N; f++) {
        if (f < N)
            y264_mcstf_push(m, in, st, 1000 + f);
        while (y264_mcstf_ready(m, f == N)) {
            const pixel *op[3];
            int os[3], filt;
            int64_t pts;
            y264_mcstf_pop(m, op, os, &pts, &filt);
            CHECK(pts == 1000 + got, "pts %lld, want %d", (long long)pts, 1000 + got);
            /* a still picture filters to itself exactly */
            for (int y = 0; y < H; y++)
                if (memcmp(op[0] + (size_t)y * os[0], pl[0] + y * W, W * sizeof(pixel)))
                    same = 0;
            got++;
        }
    }
    CHECK(got == N, "popped %d of %d pictures", got, N);
    CHECK(same, "a still sequence came out changed");
    y264_mcstf_close(m);
}

int main(void)
{
    test_nr();
    test_mcstf();
    if (fails) {
        printf("%d failure(s)\n", fails);
        return 1;
    }
    printf("denoise: all passed\n");
    return 0;
}
