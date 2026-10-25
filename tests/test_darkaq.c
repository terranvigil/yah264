// Copyright (c) 2026, the yah264 authors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The dark-block guards against banding: by default a dark, smooth
// macroblock gets a raised psy-rd weight and a QP cut on top of the variance
// offset, and a bright or a textured one gets neither. VMAF-NEG does not see
// banding, so a sweep scored on it alone would happily tune these to nothing;
// this test is what stops that from happening silently. The cases are written
// on the 8-bit scale and moved to the build's pixel scale before the call.
#include "../src/encoder/mbcode.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* an 8-bit luma mean and variance on this build's pixel scale */
#define SC ((double)(1 << (Y264_BIT_DEPTH - 8)))
#define M(m) ((m) * SC)
#define V(v) ((v) * SC * SC)

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
    unsetenv("Y264_PSY_DARK");
    unsetenv("Y264_AQ_DARK");
    unsetenv("Y264_AQ_DARK_M");
    unsetenv("Y264_AQ_DARK_V");
    unsetenv("Y264_AQ_DARK_B0");
    unsetenv("Y264_AQ_DARK_B1");
    unsetenv("Y264_AQ_DARK_S0");
    unsetenv("Y264_AQ_DARK_S1");

    /* a dark gradient with faint grain: luma mean 30, standard deviation 1.4 */
    CHECK(y264_psy_dark_gain(M(30.0), V(2.0)) >= 3.0);
    CHECK(y264_aq_dark(M(30.0), V(2.0)) <= -2.0);
    /* black with a little noise still counts */
    CHECK(y264_psy_dark_gain(M(18.0), V(1.0)) >= 3.0);
    /* the ramps: halfway up the mean ramp is half the boost */
    CHECK(y264_psy_dark_gain(M(48.0), V(2.0)) > 1.0 && y264_psy_dark_gain(M(48.0), V(2.0)) < y264_psy_dark_gain(M(30.0), V(2.0)));
    CHECK(y264_aq_dark(M(48.0), V(2.0)) < 0.0 && y264_aq_dark(M(48.0), V(2.0)) > y264_aq_dark(M(30.0), V(2.0)));

    /* bright and smooth: a sky, a wall */
    CHECK(y264_psy_dark_gain(M(120.0), V(2.0)) == 1.0);
    CHECK(y264_aq_dark(M(120.0), V(2.0)) == 0.0);
    /* dark and textured: the variance offset already handles it */
    CHECK(y264_psy_dark_gain(M(30.0), V(100.0)) == 1.0);
    CHECK(y264_aq_dark(M(30.0), V(100.0)) == 0.0);

    /* the frame gate: closed with neither bright areas nor much dark grain,
     * open on either ramp, the larger of the two */
    CHECK(y264_aq_dark_gate(0.0, 0.0) == 0.0);
    CHECK(y264_aq_dark_gate(0.20, 0.0) == 1.0);
    CHECK(y264_aq_dark_gate(0.0, 0.50) == 1.0);
    CHECK(y264_aq_dark_gate(0.0, 0.35) == 0.0);
    CHECK(y264_aq_dark_gate(0.14, 0.0) > 0.4 && y264_aq_dark_gate(0.14, 0.0) < 0.6);
    CHECK(y264_aq_dark_gate(0.14, 0.50) == 1.0);
    /* grain counts dark, smooth blocks that aren't flat */
    CHECK(y264_aq_grain(M(30.0), V(2.0)) == 1);
    CHECK(y264_aq_grain(M(30.0), V(0.1)) == 0);     /* a flat fill can't band */
    CHECK(y264_aq_grain(M(120.0), V(2.0)) == 0);    /* not dark */
    CHECK(y264_aq_grain(M(30.0), V(100.0)) == 0);   /* textured */

    /* the grain share leaves letterbox bars out: a 16x18 frame whose top and
     * bottom three rows are near-black, low-variance and whose middle is bright */
    {
        enum { W = 16, H = 18 };
        float m[W * H], v[W * H];
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                int bar = y < 3 || y >= H - 3;
                m[y * W + x] = bar ? M(16.0) : M(120.0);
                v[y * W + x] = bar ? V(0.5) : V(40.0);
            }
        CHECK(y264_aq_grain_share(m, v, W, H, NULL) == 0.0);
        /* the same bars around a dark grain picture: share over the picture */
        for (int y = 3; y < H - 3; y++)
            for (int x = 0; x < W; x++) { m[y * W + x] = M(30.0); v[y * W + x] = V(6.0); }
        CHECK(y264_aq_grain_share(m, v, W, H, NULL) == 1.0);
        /* dark grain all over: no bars, every block counts */
        for (int i = 0; i < W * H; i++) { m[i] = M(16.0); v[i] = V(1.0); }
        CHECK(y264_aq_grain_share(m, v, W, H, NULL) == 1.0);
        /* one bright block in dark grain isn't the edge of a bar: the rows
         * and columns around it stay picture, so the share stays near 1 */
        m[8 * W + 7] = M(200.0); v[8 * W + 7] = V(40.0);
        CHECK(y264_aq_grain_share(m, v, W, H, NULL) > 0.99);
        /* a subtitle in the bottom bar doesn't end it */
        for (int i = 0; i < W * H; i++) { m[i] = M(30.0); v[i] = V(6.0); }
        for (int y = 0; y < H; y++)
            if (y < 3 || y >= H - 3)
                for (int x = 0; x < W; x++) {
                    int sub = y == H - 2 && x >= 5 && x < 11;
                    m[y * W + x] = sub ? M(180.0) : M(16.0);
                    v[y * W + x] = sub ? V(300.0) : V(0.5);
                }
        CHECK(y264_aq_grain_share(m, v, W, H, NULL) == 1.0);
        /* near-black with one grainy band: bars that deep aren't bars, so the
         * band counts against the whole frame */
        for (int i = 0; i < W * H; i++) { m[i] = M(10.0); v[i] = V(0.1); }
        for (int x = 0; x < W; x++) { m[8 * W + x] = M(30.0); v[8 * W + x] = V(6.0); }
        CHECK(y264_aq_grain_share(m, v, W, H, NULL) < 0.1);
        /* mattes of any color are bars, both letterbox and pillarbox; edges of
         * two different colors aren't a matte */
        {
            int e[4];
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++) {
                    int bar = y < 3 || y >= H - 3;
                    m[y * W + x] = bar ? M(200.0) : M(90.0);
                    v[y * W + x] = bar ? V(0.5) : V(60.0);
                }
            y264_aq_bars(m, v, W, H, e);
            CHECK(e[0] == 3 && e[1] == 3 && e[2] == 0 && e[3] == 0);
            for (int x = 0; x < W; x++) m[(H - 1) * W + x] = M(100.0);
            y264_aq_bars(m, v, W, H, e);
            CHECK(e[0] == 0 && e[1] == 0);
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++) {
                    int bar = x < 2 || x >= W - 2;
                    m[y * W + x] = bar ? M(128.0) : M(90.0);
                    v[y * W + x] = bar ? V(0.5) : V(60.0);
                }
            y264_aq_bars(m, v, W, H, e);
            CHECK(e[0] == 0 && e[1] == 0 && e[2] == 2 && e[3] == 2);
        }
        /* a dark vertical gradient, darkest at both edges, drifts from the
         * edge color without a sharp step: not bars, and a vignette isn't
         * pillars */
        {
            enum { GW = 80, GH = 45 };
            static float gm[GW * GH], gv[GW * GH];
            int e[4];
            for (int y = 0; y < GH; y++)
                for (int x = 0; x < GW; x++) {
                    gm[y * GW + x] = (float)M(20.0 + 30.0 * (1.0 - fabs(y - 22.0) / 22.0));
                    gv[y * GW + x] = (float)V(0.5);
                }
            y264_aq_bars(gm, gv, GW, GH, e);
            CHECK(e[0] == 0 && e[1] == 0 && e[2] == 0 && e[3] == 0);
            for (int y = 0; y < GH; y++)
                for (int x = 0; x < GW; x++) {
                    double r = fabs(x - 39.5) / 39.5;
                    gm[y * GW + x] = (float)M(48.0 - 28.0 * r * r);
                }
            y264_aq_bars(gm, gv, GW, GH, e);
            CHECK(e[0] == 0 && e[1] == 0 && e[2] == 0 && e[3] == 0);
        }
        /* a bar on one edge only isn't a letterbox */
        for (int i = 0; i < W * H; i++) { m[i] = M(30.0); v[i] = V(6.0); }
        for (int x = 0; x < W; x++) { m[x] = M(16.0); v[x] = V(1.0); m[W + x] = M(16.0); v[W + x] = V(1.0); }
        CHECK(y264_aq_grain_share(m, v, W, H, NULL) == 1.0);
    }

    if (fails) fprintf(stderr, "%d failure(s)\n", fails);
    else printf("darkaq: ok\n");
    return fails ? 1 : 0;
}
