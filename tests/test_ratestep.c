// Copyright (c) 2026, the yah264 authors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The rate control's handling of a large live bitrate step, read from the
// inside: the refit and its short memory end when a later step replaces the
// move they were measuring, a buffered step up never refits, the whole-step
// slew window after a step closes once the ledger has aged two seconds past
// it, however much of its history the short memory dropped, a scene cut
// between the measured groups stops the refit, and the short memory decays
// the history the no-lookahead rule set aside.
#include "../src/encoder/rate.h"
#include <yah264.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if Y264_BIT_DEPTH > 8
typedef uint16_t sample_t;
#else
typedef uint8_t  sample_t;
#endif

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define W 128
#define H 96
#define NF 120

static sample_t y[W * H], u[W * H / 4], v[W * H / 4];

static int cut_at = -1;     /* from this frame on, a different picture */

static void fill(sample_t *p, int w, int h, int n, int plane)
{
    if (cut_at >= 0 && n >= cut_at) {
        for (int r = 0; r < h; r++)
            for (int c = 0; c < w; c++) {
                unsigned x = (unsigned)((c / 8 + r / 8 + n) * 2654435761u) >> 24;
                p[(size_t)r * w + c] = (sample_t)(plane ? 128 : (x & 0xff));
            }
        return;
    }
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++) {
            unsigned x = (unsigned)((c + n) * 7 + (r + n / 2) * 13 + plane * 50);
            x ^= (unsigned)((c * r + n) * 2654435761u) >> 26;
            p[(size_t)r * w + c] = (sample_t)(x & 0xff);
        }
}

static int la_off;

static yah264_encoder_t *open_abr(int kbps, int vbv)
{
    yah264_param_t p;
    memset(&p, 0, sizeof p);
    yah264_param_default(&p);
    p.width = W; p.height = H; p.csp = YAH264_CSP_I420;
    p.timebase.fps_num = 25; p.timebase.fps_den = 1;
    p.threads = 1; p.keyint = 250; p.bframes = 3;
    p.rc.method = YAH264_RC_ABR;
    p.rc.bitrate = kbps;
    if (vbv) { p.rc.vbv_maxrate = kbps; p.rc.vbv_bufsize = kbps; }
    if (la_off) p.rc.lookahead = 0;
    yah264_encoder_t *e = yah264_encoder_open(&p);
    yah264_nal_t *nal; int cnt;
    if (e) yah264_encoder_headers(e, &nal, &cnt);
    return e;
}

static int push(yah264_encoder_t *e, int n)
{
    fill(y, W, H, n, 0); fill(u, W / 2, H / 2, n, 1); fill(v, W / 2, H / 2, n, 2);
    yah264_picture_t pic; memset(&pic, 0, sizeof pic);
    pic.csp = YAH264_CSP_I420; pic.width = W; pic.height = H; pic.pts = n;
    pic.plane[0] = y; pic.plane[1] = u; pic.plane[2] = v;
    pic.stride[0] = W; pic.stride[1] = W / 2; pic.stride[2] = W / 2;
    yah264_nal_t *nal; int cnt;
    return yah264_encoder_encode(e, &nal, &cnt, &pic) < 0 ? -1 : 0;
}

static void flush(yah264_encoder_t *e)
{
    yah264_nal_t *nal; int cnt;
    for (int g = 0; g < 400; g++) {
        int b = yah264_encoder_encode(e, &nal, &cnt, NULL);
        if (b < 0 || (b == 0 && cnt == 0)) break;
    }
}

/* events that have taken effect so far */
static int landed(yah264_encoder_t *e)
{
    static int n;
    yah264_control_t d[8];
    for (int i = 0; i < 8; i++) yah264_control_init(&d[i], 0);
    int m;
    while ((m = yah264_encoder_controls_applied(e, d, 8)) > 0) n += m;
    return n;
}

static int rate(yah264_encoder_t *e, int64_t f, int k, int m, int b)
{
    yah264_control_t ev; yah264_control_init(&ev, YAH264_CTL_RATE);
    ev.frame = f; ev.bitrate_kbps = k; ev.vbv_maxrate_kbps = m; ev.vbv_bufsize_kbit = b;
    return yah264_encoder_control(e, &ev);
}

int main(void)
{
    /* a 10x step down arms the refit; a 2.8x step back up eight frames later,
     * inside its window, ends it, short memory and all */
    {
        yah264_encoder_t *e = open_abr(400, 0);
        CHECK(e && e->abr_rf2);
        CHECK(rate(e, 30, 40, 0, 0) == YAH264_CTL_OK);
        CHECK(rate(e, 38, 112, 0, 0) == YAH264_CTL_OK);
        int armed = 0, after = 0, base = landed(e), n;
        for (int i = 0; i < NF; i++) {
            CHECK(push(e, i) == 0);
            n = landed(e) - base;
            if (n == 1 && e->rt_fit) armed = 1;
            if (n == 2 && (e->rt_fit || e->rt_fast)) after = 1;
        }
        flush(e);
        CHECK(landed(e) - base == 2);
        CHECK(armed);
        CHECK(!after);
        yah264_encoder_close(e);
    }

    /* CBR: a step down arms the refit; the step back up four frames later
     * doesn't qualify under a buffer, so it ends it and no refit runs */
    {
        yah264_encoder_t *e = open_abr(400, 1);
        CHECK(e && e->vbv_on);
        CHECK(rate(e, 30, 40, 40, 40) == YAH264_CTL_OK);
        CHECK(rate(e, 34, 400, 400, 400) == YAH264_CTL_OK);
        int armed = 0, after = 0, base = landed(e), n;
        for (int i = 0; i < NF; i++) {
            CHECK(push(e, i) == 0);
            n = landed(e) - base;
            if (n == 1 && e->rt_fit) armed = 1;
            if (n == 2 && (e->rt_fit || e->rt_fast)) after = 1;
        }
        flush(e);
        CHECK(landed(e) - base == 2);
        CHECK(armed);
        CHECK(!after);
        yah264_encoder_close(e);
    }

    /* plain ABR, one 10x step down: the refit runs, the short memory drops
     * owed bits, and the whole-step slew window still closes two seconds
     * (50 frames) of ledger age after the landing */
    {
        yah264_encoder_t *e = open_abr(400, 0);
        CHECK(e != NULL);
        CHECK(rate(e, 30, 40, 0, 0) == YAH264_CTL_OK);
        int base = landed(e), fit = 0;
        for (int i = 0; i < NF; i++) {
            CHECK(push(e, i) == 0);
            if (landed(e) > base && e->rt_fit) fit = 1;
        }
        flush(e);
        CHECK(fit && e->rt_owed_end > 0.0);
        CHECK(e->rfm_drop > 0.0);
        CHECK(!rc_rt_slew_open(e, e->rfm_owed));
        /* ...where owed alone, without what the short memory dropped, still
         * reads open: the case the window has to get right */
        CHECK(e->rfm_owed < e->rt_owed_end);
        yah264_encoder_close(e);
    }

    /* a scene cut the detector finds between the groups stops refitting: a
     * 10x step eight frames before a cut refits once and stops at the cut,
     * where the same step on content without one refits twice */
    for (int c = 0; c < 2; c++) {
        cut_at = c ? 60 : -1;
        yah264_encoder_t *e = open_abr(400, 0);
        CHECK(e != NULL);
        CHECK(rate(e, 52, 40, 0, 0) == YAH264_CTL_OK);
        int base = landed(e), refits = 0, seen_cut = 0;
        unsigned lo = e->rt_fit_lo;
        for (int i = 0; i <= NF; i++) {
            if (i < NF) CHECK(push(e, i) == 0);
            else flush(e);
            if (landed(e) > base && e->rt_fit_lo != lo) {   /* each refit moves it */
                refits++;
                lo = e->rt_fit_lo;
            }
        }
        for (int i = 0; i < 128; i++) seen_cut |= e->rt_hist[i].cut;
        CHECK(seen_cut == c);
        if (refits != (c ? 1 : 2))
            fprintf(stderr, "cut %d: %d refits\n", c, refits);
        CHECK(refits == (c ? 1 : 2));
        yah264_encoder_close(e);
    }
    cut_at = -1;

    /* with no lookahead the recent-reading rule may set history aside; the
     * short memory decays that with the ledger, so a later restore doesn't
     * bring it back at full weight */
    {
        la_off = 1;
        yah264_encoder_t *e = open_abr(400, 0);
        la_off = 0;
        CHECK(e && e->abr_rf2 && e->la_depth == 0);
        CHECK(rate(e, 30, 40, 0, 0) == YAH264_CTL_OK);
        int checked = 0, armed = -1;
        for (int i = 0; i < NF; i++) {
            if (armed < 0 && e->rt_fast > 8 && e->rfm_rec_state != 2) {
                e->rfm_rec_lost = 1e6;
                armed = e->rt_fast;
            }
            CHECK(push(e, i) == 0);
            if (armed >= 0 && !checked && e->rt_fast < armed) {
                CHECK(e->rfm_rec_lost < 1e6);
                checked = 1;
            }
        }
        flush(e);
        CHECK(checked);
        yah264_encoder_close(e);
    }

    if (fails) { fprintf(stderr, "ratestep: %d failure(s)\n", fails); return 1; }
    printf("ratestep: all checks passed\n");
    return 0;
}
