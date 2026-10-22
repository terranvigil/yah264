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

    if (fails) fprintf(stderr, "%d failure(s)\n", fails);
    else printf("darkaq: ok\n");
    return fails ? 1 : 0;
}
