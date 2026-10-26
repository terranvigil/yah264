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
    CHECK(fabs(y264_aq_dark_gate(0.14, 0.425) - 0.5) < 1e-9);    /* the larger, not the sum */
    /* grain counts dark, smooth blocks that aren't flat */
    CHECK(y264_aq_grain(M(30.0), V(2.0)) == 1);
    CHECK(y264_aq_grain(M(30.0), V(0.1)) == 0);     /* a flat fill can't band */
    CHECK(y264_aq_grain(M(120.0), V(2.0)) == 0);    /* not dark */
    CHECK(y264_aq_grain(M(30.0), V(100.0)) == 0);   /* textured */

    /* bars: a 16x18 frame (W x H macroblocks), then 80x45 (1280x720) */
    {
        enum { W = 16, H = 18 };
        float m[W * H], v[W * H];
        int e[4];
#define FILL(cond, mb, vb, mp, vp)                                   \
        for (int y = 0; y < H; y++)                                  \
            for (int x = 0; x < W; x++) {                            \
                int bar = (cond);                                    \
                m[y * W + x] = bar ? M(mb) : M(mp);                  \
                v[y * W + x] = bar ? V(vb) : V(vp);                  \
            }
        /* flat black bars around a dark grain picture: the share is the picture's */
        FILL(y < 3 || y >= H - 3, 16.0, 0.0, 30.0, 6.0)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[0] == 3 && e[1] == 3 && e[2] == 0 && e[3] == 0);
        CHECK(y264_aq_grain_share(m, v, W, H) == 1.0);
        /* a subtitle in the bottom bar doesn't end it */
        for (int x = 5; x < 11; x++) { m[(H - 2) * W + x] = M(180.0); v[(H - 2) * W + x] = V(300.0); }
        CHECK(y264_aq_grain_share(m, v, W, H) == 1.0);
        /* flat all over: deeper than the cap, so no bars */
        FILL(1, 16.0, 0.0, 16.0, 0.0)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[0] == 0 && e[1] == 0 && e[2] == 0 && e[3] == 0);
        /* a bar on one edge only isn't a letterbox */
        FILL(y < 2, 16.0, 0.0, 30.0, 6.0)
        CHECK(fabs(y264_aq_grain_share(m, v, W, H) - (H - 2.0) / H) < 1e-9);
        /* grainy bars aren't bars: they count as grain */
        FILL(y < 3 || y >= H - 3, 16.0, 1.44, 140.0, 80.0)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[0] == 0 && e[1] == 0);
        /* mattes of any color, and pillarboxes */
        FILL(y < 3 || y >= H - 3, 200.0, 0.1, 90.0, 60.0)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[0] == 3 && e[1] == 3);
        FILL(y < 3 || y >= H - 3, y < 3 ? 200.0 : 203.0, 0.1, 90.0, 60.0)   /* two colors */
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[0] == 0 && e[1] == 0);
        /* the outermost line must be nearly all matte */
        FILL(y < 3 || y >= H - 3, 16.0, 0.0, 90.0, 60.0)
        for (int x = 0; x < 4; x++) { m[x] = M(120.0); v[x] = V(60.0); }
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[0] == 0 && e[1] == 0);
        /* a windowbox keeps its top and bottom bars alone */
        FILL(y < 3 || y >= H - 3 || x < 2 || x >= W - 2, 16.0, 0.0, 90.0, 60.0)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[0] == 3 && e[1] == 3 && e[2] == 0 && e[3] == 0);
        FILL(x < 2 || x >= W - 2, 128.0, 0.0, 90.0, 60.0)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[0] == 0 && e[1] == 0 && e[2] == 2 && e[3] == 2);
#undef FILL
    }
    {
        enum { W = 80, H = 45 };
        static float m[W * H], v[W * H];
        int e[4];
#define FILL(cond, mb, vb, mp, vp)                                   \
        for (int y = 0; y < H; y++)                                  \
            for (int x = 0; x < W; x++) {                            \
                int bar = (cond);                                    \
                m[y * W + x] = (float)((bar) ? M(mb) : M(mp));       \
                v[y * W + x] = (float)((bar) ? V(vb) : V(vp));       \
            }
        /* a flat vertical gradient darkest at both edges drifts: at most its
         * edge line reads as bar, and that line is flat, so harmless */
        FILL(0, 0.0, 0.0, 20.0 + 30.0 * (1.0 - fabs(y - 22.0) / 22.0), 0.1)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[0] <= 1 && e[1] <= 1);
        /* the same for a flat vignette */
        FILL(0, 0.0, 0.0, 48.0 - 28.0 * pow(fabs(x - 39.5) / 39.5, 2), 0.1)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[2] <= 1 && e[3] <= 1);
        /* letterbox bars past three eighths of the height aren't bars */
        FILL(y < 18 || y >= H - 18, 16.0, 0.0, 120.0, 60.0)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[0] == 0 && e[1] == 0);
        /* a letterbox a few rows off center pairs (3 and 5 rows: 62 and 86 px
         * around a 572-row picture), bars of clearly unequal depth don't, and a
         * bar on one edge only isn't a letterbox */
        FILL(y < 3 || y >= H - 5, 16.0, 0.0, 120.0, 60.0)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[0] == 3 && e[1] == 5);
        FILL(y < 2 || y >= H - 5, 16.0, 0.0, 120.0, 60.0)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[0] == 0 && e[1] == 0);
        FILL(y < 1 || (y == H - 1 && x < 50), 16.0, 0.0, 120.0, 60.0)  /* bottom edge part matte */
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[0] == 0 && e[1] == 0);
        /* pillars of two colors aren't a matte */
        FILL(x < 4 || x >= W - 4, x < 4 ? 128.0 : 131.0, 0.0, 120.0, 60.0)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[2] == 0 && e[3] == 0);
        /* grain at the bar's own level is picture: the bar ends there */
        FILL(y < 4 || y >= H - 4, 16.0, 0.0, y < 10 || y >= H - 10 ? 16.0 : 120.0,
             y < 10 || y >= H - 10 ? 2.0 : 60.0)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[0] == 4 && e[1] == 4);
        /* the pairing allows a row of rounding and a third of the shallower side */
        FILL(x < 6 || x >= W - 9, 16.0, 0.0, 120.0, 60.0)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[2] == 6 && e[3] == 9);
        FILL(x < 6 || x >= W - 10, 16.0, 0.0, 120.0, 60.0)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[2] == 0 && e[3] == 0);
        /* a flat sky and ground a level apart aren't a matte, at any depths */
        FILL(y < 6 || y >= H - 9, y < 6 ? 20.0 : 21.0, 0.0, 120.0, 60.0)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[0] == 0 && e[1] == 0);
        FILL(y < 6 || y >= H - 7, y < 6 ? 20.0 : 21.0, 0.0, 120.0, 60.0)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[0] == 0 && e[1] == 0);
        /* one color means within half a level: 0.4 apart pairs, 0.7 doesn't */
        FILL(y < 6 || y >= H - 9, y < 6 ? 20.0 : 20.4, 0.0, 120.0, 60.0)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[0] == 6 && e[1] == 9);
        FILL(y < 6 || y >= H - 6, y < 6 ? 20.0 : 20.7, 0.0, 120.0, 60.0)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[0] == 0 && e[1] == 0);
        /* sides a few blocks apart pair (a crop off center): 8 and 6 deep */
        FILL(x < 8 || x >= W - 6, 16.0, 0.0, 120.0, 60.0)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[2] == 8 && e[3] == 6);
        FILL(x < 2 || x >= W - 5, 16.0, 0.0, 120.0, 60.0)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[2] == 0 && e[3] == 0);
        /* a flat dark gradient across the line isn't uniform along it */
        FILL(y < 8 || y >= H - 8, 13.5 + 6.0 * x / (W - 1.0), 0.1, 120.0, 80.0)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[0] == 0 && e[1] == 0);
        /* a slowly graded flat sky stops at half a level of drift */
        FILL(y < 8 || y >= H - 8, 16.0 + 0.3 * (y < 8 ? y : H - 1 - y), 0.1, 185.0, 100.0)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[0] <= 2 && e[1] <= 2);
        /* black bars beside a crushed, faintly grainy sky end at the sky */
        FILL(y < 6 || y >= H - 6, 16.0, 0.0, y < 22 ? 16.9 : 150.0, y < 22 ? 0.5 : 90.0)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[0] == 6 && e[1] == 6);
        CHECK(fabs(y264_aq_grain_share(m, v, W, H) - 16.0 / 33.0) < 1e-9);
        /* black bars beside dark grain */
        FILL(y < 6 || y >= H - 6, 16.0, 0.0, 24.0, 3.0)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[0] == 6 && e[1] == 6);
        /* a pillar that walks into flat, mostly black picture still pairs at
         * its solid depth */
        FILL(x < 15 || x >= W - 15 || (x >= W - 22 && y % 10 < 7), 16.0, 0.0,
             x >= W - 22 ? 24.0 : 120.0, x >= W - 22 ? 3.0 : 60.0)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[2] == 15 && e[3] == 15);
        /* a bar that walks into dark picture also pairs with one holding a soft,
         * quiet subtitle, whose own solid depth is 1 */
        FILL(((y < 6 || y >= H - 6) && !(y == H - 2 && x >= 20 && x < 60)) ||
             (y >= 6 && y < 13 && x % 10 < 7), 16.0, 0.0,
             y == H - 2 ? 60.0 : (y < 13 ? 24.0 : 120.0),
             y == H - 2 ? 10.0 : (y < 13 ? 3.0 : 60.0))
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[0] == 6 && e[1] == 6);
        /* busy subtitle text doesn't end a solid depth, so a bar holding one can
         * walk into dark picture too */
        FILL(((y < 6 || y >= H - 6) && !(y == H - 3 && x >= 20 && x < 60)) ||
             (y >= H - 13 && y < H - 6 && x % 10 < 7), 16.0, 0.0,
             y == H - 3 ? 200.0 : (y >= H - 13 ? 24.0 : 120.0),
             y == H - 3 ? 500.0 : (y >= H - 13 ? 3.0 : 60.0))
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[0] == 6 && e[1] == 6);
        /* one walk past the cap still pairs at its solid depth */
        FILL(y < 6 || y >= H - 6 || (y < 25 && x % 10 < 7), 16.0, 0.0,
             y < 25 ? 24.0 : 120.0, y < 25 ? 3.0 : 60.0)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[0] == 6 && e[1] == 6);
        /* the soft subtitle in the top bar and the walk below, the other way round */
        FILL(((y < 6 || y >= H - 6) && !(y == 1 && x >= 20 && x < 60)) ||
             (y >= H - 13 && y < H - 6 && x % 10 < 7), 16.0, 0.0,
             y == 1 ? 60.0 : (y >= H - 13 ? 24.0 : 120.0),
             y == 1 ? 10.0 : (y >= H - 13 ? 3.0 : 60.0))
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[0] == 6 && e[1] == 6);
        /* solid depths a block of rounding apart pair (1080p's 138-pixel bars) */
        FILL(y < 8 || y >= H - 9 || (y < 20 && x % 10 < 6), 16.0, 0.0, 24.0, 3.0)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[0] == 8 && e[1] == 9);
        /* both bars walk, to unequal depths: the solid ones pair */
        FILL(y < 6 || y >= H - 6 || (y < 16 && x % 10 < 6) || (y >= H - 9 && x % 10 < 6),
             16.0, 0.0, 24.0, 3.0)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[0] == 6 && e[1] == 6);
        /* a faint logo block in a matte line doesn't end the solid depth */
        FILL((y < 6 || y >= H - 6 || (y < 16 && x % 10 < 6) || (y >= H - 9 && x % 10 < 6)) &&
             !(x == 40 && y == H - 3), 16.0, 0.0, x == 40 && y == H - 3 ? 40.0 : 24.0,
             x == 40 && y == H - 3 ? 0.0 : 3.0)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[0] == 6 && e[1] == 6);
        /* a letterbox beside thin black picture edges stays a letterbox, and so,
         * the commoner matte winning, does a pillarbox whose picture's own top
         * and bottom are black */
        FILL(y < 6 || y >= H - 6 || (y < 12 && x % 10 < 6) || x < 3 || x >= W - 3, 16.0, 0.0,
             x % 3 ? 24.0 : 120.0, x % 3 ? 3.0 : 60.0)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[0] == 6 && e[1] == 6 && e[2] == 0 && e[3] == 0);
        FILL(x < 10 || x >= W - 10 || y < 4 || y >= H - 4 || (y < 14 && x % 10 < 4) ||
             (y >= 37 && x % 10 < 4), 16.0, 0.0, 24.0, 3.0)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[0] == 4 && e[1] == 4 && e[2] == 0 && e[3] == 0);
        /* a dark subject in a black void isn't letterboxed: both walks pass the cap */
        FILL(!(x >= 20 && x < 60 && y >= 12 && y < 33), 16.0, 0.0, 30.0, 3.0)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[0] == 0 && e[1] == 0 && e[2] == 0 && e[3] == 0);
        /* nor is one off center, where one walk passes the cap and the other doesn't */
        FILL(!((x >= 20 && x < 32 && y >= 15 && y < 30) || (x >= 32 && x < 58 && y >= 5 && y < 40)),
             16.0, 0.0, 26.0, 3.0)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[0] == 0 && e[1] == 0 && e[2] == 0 && e[3] == 0);
        FILL(!((y >= 10 && y < 18 && x >= 40 && x < 60) || (y >= 18 && y < 37 && x >= 10 && x < 55)),
             16.0, 0.0, 26.0, 3.0)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[0] == 0 && e[1] == 0 && e[2] == 0 && e[3] == 0);
        /* nor are bars past the cap rescued at a partly matte line inside them */
        FILL((y < 18 || y >= H - 18) && !((y == 3 || y == H - 4) && x < 30), 16.0, 0.0,
             24.0, 3.0)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[0] == 0 && e[1] == 0);
        /* 2.39:1 inside 4:3, and a 9:16 video pillarboxed in 16:9 */
        FILL(y < 10 || y >= H - 10, 16.0, 0.0, 120.0, 60.0)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[0] == 10 && e[1] == 10);
        FILL(x < 28 || x >= W - 28, 16.0, 0.0, 120.0, 60.0)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[2] == 28 && e[3] == 28);
        /* pillars past three eighths of the width aren't bars */
        FILL(x < 31 || x >= W - 31, 16.0, 0.0, 120.0, 60.0)
        y264_aq_bars(m, v, W, H, e);
        CHECK(e[2] == 0 && e[3] == 0);
#undef FILL
    }

    if (fails) fprintf(stderr, "%d failure(s)\n", fails);
    else printf("darkaq: ok\n");
    return fails ? 1 : 0;
}
