/*
 * test_mc.c - smoke tests for motion compensation interpolation
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Authoritative validation of the interpolation filters is the recon-match
 * conformance gate; these catch gross errors (wrong integer copy, filter that
 * breaks on a constant field, out-of-range output).
 */
#include "dsp/mc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } \
} while (0)

#define PW 48
#define PH 48

/* y264_mc_luma's fast paths assume encoder-style planes with edge-replicated
 * borders (mc.h); build one around a PW x PH interior. */
#define BSTRIDE (PW + 2 * Y264_LUMA_BORDER)
static pixel plane_buf[(size_t)BSTRIDE * (PH + 2 * Y264_LUMA_BORDER)];
static pixel *make_plane(void)
{
    return plane_buf + (size_t)Y264_LUMA_BORDER * BSTRIDE + Y264_LUMA_BORDER;
}
static void extend_test_plane(pixel *p)
{
    for (int y = 0; y < PH; y++) {
        pixel *row = p + (size_t)y * BSTRIDE;
        pixel lv = row[0], rv = row[PW - 1];
        for (int b = 0; b < Y264_LUMA_BORDER; b++) { row[-Y264_LUMA_BORDER + b] = lv; row[PW + b] = rv; }
    }
    for (int y = 1; y <= Y264_LUMA_BORDER; y++) {
        memcpy(p - (size_t)y * BSTRIDE - Y264_LUMA_BORDER,
               p - Y264_LUMA_BORDER, BSTRIDE * sizeof(pixel));
        memcpy(p + (size_t)(PH - 1 + y) * BSTRIDE - Y264_LUMA_BORDER,
               p + (size_t)(PH - 1) * BSTRIDE - Y264_LUMA_BORDER, BSTRIDE * sizeof(pixel));
    }
}

static uint64_t rng = 0xabcdef1234567ULL;
static pixel rnd8(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (pixel)(rng >> 24);
}

int main(void)
{
    pixel *ref = make_plane();
    for (int y = 0; y < PH; y++)
        for (int x = 0; x < PW; x++) ref[y * BSTRIDE + x] = rnd8();
    extend_test_plane(ref);

    pixel dst[16 * 16];

    /* Integer MV must be an exact block copy from the reference. */
    y264_mc_luma(dst, 16, ref, BSTRIDE, PW, PH, 8, 8, 0, 0, 16, 16);
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            CHECK(dst[y * 16 + x] == ref[(8 + y) * BSTRIDE + (8 + x)],
                  "integer MV copy mismatch at %d,%d", x, y);

    /* Integer MV of (4,-8) quarter-pel = (+1,-2) samples: shifted copy. */
    y264_mc_luma(dst, 16, ref, BSTRIDE, PW, PH, 8, 8, 4, -8, 16, 16);
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            CHECK(dst[y * 16 + x] == ref[(8 - 2 + y) * BSTRIDE + (8 + 1 + x)],
                  "shifted integer MV mismatch at %d,%d", x, y);

    /* On a constant field every fractional position returns the constant. */
    static pixel flat_buf[sizeof(plane_buf) / sizeof(pixel)];
    for (size_t i = 0; i < sizeof(flat_buf) / sizeof(pixel); i++) flat_buf[i] = 137;
    pixel *flat = flat_buf + (size_t)Y264_LUMA_BORDER * BSTRIDE + Y264_LUMA_BORDER;
    for (int fy = 0; fy < 4; fy++)
        for (int fx = 0; fx < 4; fx++) {
            y264_mc_luma(dst, 16, flat, BSTRIDE, PW, PH, 8, 8, fx, fy, 16, 16);
            for (int i = 0; i < 256; i++)
                CHECK(dst[i] == 137, "flat luma frac (%d,%d) => %d", fx, fy, dst[i]);
        }

    /* Chroma: integer eighth-pel copy and constant field. */
    pixel cdst[8 * 8];
    y264_mc_chroma(cdst, 8, ref, BSTRIDE, PW, PH, 8, 8, 0, 0, 8, 8, 2, 2);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            CHECK(cdst[y * 8 + x] == ref[(8 + y) * BSTRIDE + (8 + x)],
                  "chroma integer copy mismatch at %d,%d", x, y);
    y264_mc_chroma(cdst, 8, flat, BSTRIDE, PW, PH, 8, 8, 3, 5, 8, 8, 2, 2);
    for (int i = 0; i < 64; i++)
        CHECK(cdst[i] == 137, "flat chroma => %d", cdst[i]);

    /* The dispatched path (NEON where available) must match the portable
 * reference exactly, for every fractional position and a spread of MVs. */
    for (int y = 0; y < PH; y++)
        for (int x = 0; x < PW; x++) ref[y * BSTRIDE + x] = rnd8();
    extend_test_plane(ref);
    static const int SZ[][2] = { {16,16}, {16,8}, {8,16}, {8,8}, {8,4}, {4,8}, {4,4} };
    static const int BX[] = { 12, PW - 24, PW - 20 };   /* centre + right edge */
    for (size_t bi = 0; bi < sizeof(BX)/sizeof(BX[0]); bi++)
    for (size_t si = 0; si < sizeof(SZ)/sizeof(SZ[0]); si++)
    for (int mvy = -20; mvy <= 20; mvy++) {
        for (int mvx = -20; mvx <= 20; mvx++) {
            int w = SZ[si][0], h = SZ[si][1];
            pixel a[16 * 16], b[16 * 16];
            memset(a, 0xAA, sizeof(a)); memset(b, 0xAA, sizeof(b));
            y264_mc_luma(a, 16, ref, BSTRIDE, PW, PH, BX[bi], 12, mvx, mvy, w, h);
            y264_mc_luma_c(b, 16, ref, BSTRIDE, PW, PH, BX[bi], 12, mvx, mvy, w, h);
            for (int k = 0; k < 256; k++)
                if (a[k] != b[k]) {
                    CHECK(0, "dispatch != ref at mv(%d,%d) idx %d: %d vs %d",
                          mvx, mvy, k, a[k], b[k]);
                    mvx = 999; mvy = 999; break;   /* stop after first block */
                }
        }
    }

    /* Chroma: dispatched (NEON) path must match the reference for all eighth-pel
 * phases and a spread of MVs. */
    for (int mvy = -16; mvy <= 16; mvy++) {
        for (int mvx = -16; mvx <= 16; mvx++) {
            pixel a[8 * 8], b[8 * 8];
            y264_mc_chroma(a, 8, ref, BSTRIDE, PW, PH, 10, 10, mvx, mvy, 8, 8, 2, 2);
            y264_mc_chroma_c(b, 8, ref, BSTRIDE, PW, PH, 10, 10, mvx, mvy, 8, 8, 2, 2);
            for (int k = 0; k < 64; k++)
                if (a[k] != b[k]) {
                    CHECK(0, "chroma dispatch != ref at mv(%d,%d) idx %d: %d vs %d",
                          mvx, mvy, k, a[k], b[k]);
                    mvx = 999; mvy = 999; break;
                }
        }
    }

    if (fails) {
        printf("test_mc: %d failure(s)\n", fails);
        return 1;
    }
    printf("test_mc: all passed\n");
    return 0;
}
