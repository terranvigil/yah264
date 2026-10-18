// Copyright (c) 2026, the yah264 authors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The frame-level AQ strength each rate mode opens with: 1.3 under uncapped
// CRF, 1.04 under capped CRF and whenever a bitrate target drives rate
// control, and Y264_AQ_DC above both. Opening is enough; nothing is encoded.
#include <yah264.h>
#include "../src/encoder/encoder.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void base(yah264_param_t *p)
{
    memset(p, 0, sizeof *p);
    yah264_param_default(p);
    p->width = 64; p->height = 48; p->csp = YAH264_CSP_I420;
    p->timebase.fps_num = 25; p->timebase.fps_den = 1;
    p->threads = 1;
}

static double strength(yah264_param_t *p)
{
    yah264_encoder_t *e = yah264_encoder_open(p);
    if (!e) return -1.0;
    double s = e->aq_dcstr;
    yah264_encoder_close(e);
    return s;
}

static int near(double a, double b) { return fabs(a - b) < 1e-9; }

int main(void)
{
    yah264_param_t p;
    unsetenv("Y264_AQ_DC");

    base(&p); p.rc.method = YAH264_RC_CRF; p.rc.rf = 23.0;
    CHECK(near(strength(&p), 1.3));                       /* CRF */

    base(&p); p.rc.method = YAH264_RC_CRF; p.rc.rf = 23.0;
    p.rc.vbv_maxrate = 400; p.rc.vbv_bufsize = 800;
    CHECK(near(strength(&p), 1.04));                      /* capped CRF */

    base(&p); p.rc.method = YAH264_RC_ABR; p.rc.bitrate = 300;
    CHECK(near(strength(&p), 1.04));                      /* ABR */

    base(&p); p.rc.method = YAH264_RC_ABR; p.rc.bitrate = 300;
    p.rc.vbv_maxrate = 450; p.rc.vbv_bufsize = 900;
    CHECK(near(strength(&p), 1.04));                      /* capped VBR */

    base(&p); p.rc.method = YAH264_RC_ABR; p.rc.bitrate = 300;
    p.rc.vbv_maxrate = 300; p.rc.vbv_bufsize = 300;
    CHECK(near(strength(&p), 1.04));                      /* CBR */

    const char *stats = "test_aqdc.stats";
    base(&p); p.rc.method = YAH264_RC_2PASS; p.rc.bitrate = 300;
    p.rc.pass = 1; p.rc.stats = stats;
    CHECK(near(strength(&p), 1.04));                      /* two-pass, pass 1 */
    remove(stats);

    setenv("Y264_AQ_DC", "0.75", 1);                      /* the override wins in both */
    base(&p); p.rc.method = YAH264_RC_CRF; p.rc.rf = 23.0;
    CHECK(near(strength(&p), 0.75));
    base(&p); p.rc.method = YAH264_RC_ABR; p.rc.bitrate = 300;
    CHECK(near(strength(&p), 0.75));
    unsetenv("Y264_AQ_DC");

    if (fails) { fprintf(stderr, "test_aqdc: %d failure(s)\n", fails); return 1; }
    printf("test_aqdc: ok\n");
    return 0;
}
