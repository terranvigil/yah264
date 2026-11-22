// Copyright (c) 2026, the yah264 authors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Live control through the library (docs/live-control.md): what is refused
// and with which code, where each kind lands, the drain and the status, the
// end-of-stream rule, and that the bytes depend on the landing frame alone,
// not on when the event was handed in. The log grammar has its own checks
// at the end, on the shared parser.

#include <yah264.h>
#include "../cli/ctl_log.h"
#include <stdint.h>
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
#define NF 72

/* moving texture with a cut at 36, so the rate control has work to do */
static void fill(sample_t *p, int w, int h, int n, int plane)
{
    int s = n < 36 ? n : 1000 + 3 * n;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            unsigned v = (unsigned)((x + s) * 7 + (y + s / 2) * 13 + plane * 50);
            v ^= (unsigned)((x * y + s) * 2654435761u) >> 26;
            p[(size_t)y * w + x] = (sample_t)(v & 0xff);
        }
}

static void base_param(yah264_param_t *p, int method)
{
    memset(p, 0, sizeof *p);
    yah264_param_default(p);
    p->width = W; p->height = H; p->csp = YAH264_CSP_I420;
    p->timebase.fps_num = 25; p->timebase.fps_den = 1;
    p->threads = 1;
    p->keyint = 250;
    p->rc.method = method;
    if (method == YAH264_RC_ABR) {
        p->rc.bitrate = 400;
        p->rc.vbv_maxrate = 400;
        p->rc.vbv_bufsize = 400;
    } else if (method == YAH264_RC_CRF) {
        p->rc.rf = 26;
    } else {
        p->rc.qp = 30;
    }
}

struct enc {
    yah264_encoder_t *e;
    unsigned char *buf; long len, cap;
    yah264_frame_stats_t st[NF];
    int have[NF];
    long bytes[NF];
    yah264_control_t done[64];
    int ndone;
    sample_t *y, *u, *v;
    int n;
};

static void take(struct enc *r, yah264_nal_t *nal, int cnt)
{
    int disp[64], nd = 0, slices = 0;
    for (int k = 0; k < cnt; k++) slices += nal[k].type == YAH264_NAL_SLICE || nal[k].type == YAH264_NAL_SLICE_IDR;
    if (slices) nd = yah264_encoder_frame_order(r->e, disp, slices < 64 ? slices : 64);
    for (int k = 0, j = 0; k < cnt && j < nd; k++)
        if (nal[k].type == YAH264_NAL_SLICE || nal[k].type == YAH264_NAL_SLICE_IDR) {
            if (disp[j] >= 0 && disp[j] < NF) r->bytes[disp[j]] += (long)nal[k].size;
            j++;
        }
    for (int k = 0; k < cnt; k++) {
        if (r->len + (long)nal[k].size > r->cap) {
            r->cap = (r->len + (long)nal[k].size) * 2 + 4096;
            r->buf = realloc(r->buf, (size_t)r->cap);
        }
        memcpy(r->buf + r->len, nal[k].payload, nal[k].size);
        r->len += (long)nal[k].size;
    }
    yah264_frame_stats_t s[16];
    int m;
    while ((m = yah264_encoder_frame_stats(r->e, s, 16)) > 0)
        for (int i = 0; i < m; i++)
            if (s[i].disp >= 0 && s[i].disp < NF) { r->st[s[i].disp] = s[i]; r->have[s[i].disp] = 1; }
    for (;;) {
        yah264_control_t d[8];
        for (int i = 0; i < 8; i++) yah264_control_init(&d[i], 0);
        m = yah264_encoder_controls_applied(r->e, d, 8);
        for (int i = 0; i < m && r->ndone < 64; i++) r->done[r->ndone++] = d[i];
        if (m < 8) break;
    }
}

static int open_enc(struct enc *r, const yah264_param_t *p)
{
    memset(r, 0, sizeof *r);
    r->e = yah264_encoder_open(p);
    if (!r->e) return -1;
    r->y = malloc((size_t)W * H * sizeof(sample_t));
    r->u = malloc((size_t)W * H / 4 * sizeof(sample_t));
    r->v = malloc((size_t)W * H / 4 * sizeof(sample_t));
    yah264_nal_t *nal; int cnt;
    if (yah264_encoder_headers(r->e, &nal, &cnt) < 0) return -1;
    take(r, nal, cnt);
    return 0;
}

static int push(struct enc *r)
{
    fill(r->y, W, H, r->n, 0); fill(r->u, W / 2, H / 2, r->n, 1); fill(r->v, W / 2, H / 2, r->n, 2);
    yah264_picture_t pic; memset(&pic, 0, sizeof pic);
    pic.csp = YAH264_CSP_I420; pic.width = W; pic.height = H; pic.pts = r->n;
    pic.plane[0] = r->y; pic.plane[1] = r->u; pic.plane[2] = r->v;
    pic.stride[0] = W; pic.stride[1] = W / 2; pic.stride[2] = W / 2;
    yah264_nal_t *nal; int cnt;
    int b = yah264_encoder_encode(r->e, &nal, &cnt, &pic);
    if (b < 0) return -1;
    take(r, nal, cnt);
    r->n++;
    return 0;
}

static void flush(struct enc *r)
{
    yah264_nal_t *nal; int cnt;
    for (int guard = 0; guard < 400; guard++) {
        int b = yah264_encoder_encode(r->e, &nal, &cnt, NULL);
        if (b < 0) break;
        take(r, nal, cnt);
        if (b == 0 && cnt == 0) break;
    }
}

static void close_enc(struct enc *r)
{
    yah264_encoder_close(r->e);
    free(r->buf); free(r->y); free(r->u); free(r->v);
}

static int ev_rate(yah264_encoder_t *e, int64_t f, int k, int m, int b)
{
    yah264_control_t ev; yah264_control_init(&ev, YAH264_CTL_RATE);
    ev.frame = f; ev.bitrate_kbps = k; ev.vbv_maxrate_kbps = m; ev.vbv_bufsize_kbit = b;
    return yah264_encoder_control(e, &ev);
}

static const yah264_control_t *done_of(const struct enc *r, int kind)
{
    for (int i = 0; i < r->ndone; i++) if (r->done[i].kind == kind) return &r->done[i];
    return NULL;
}

/* the schedule of the landing test, submitted either all up front or each
 * just before its frame; the bytes must not care which */
static void landing_events(yah264_encoder_t *e, int at_frame /* -1 = all */)
{
    yah264_control_t ev;
    if (at_frame < 0 || at_frame == 20) {
        yah264_control_init(&ev, YAH264_CTL_KEYFRAME); ev.frame = 20;
        CHECK(yah264_encoder_control(e, &ev) == YAH264_CTL_OK);
    }
    if (at_frame < 0 || at_frame == 25)
        CHECK(ev_rate(e, 25, 150, 0, 0) == YAH264_CTL_OK);
    if (at_frame < 0 || at_frame == 30) {
        yah264_control_init(&ev, YAH264_CTL_QP_LIMITS); ev.frame = 30; ev.qp_min = 33;
        CHECK(yah264_encoder_control(e, &ev) == YAH264_CTL_OK);
    }
    if (at_frame < 0 || at_frame == 50) {
        yah264_control_init(&ev, YAH264_CTL_ZONE); ev.frame = 50; ev.last = 53; ev.qp_offset = 4;
        CHECK(yah264_encoder_control(e, &ev) == YAH264_CTL_OK);
    }
}

int main(void)
{
    yah264_param_t p;
    struct enc r;

    /* 1. init, codes and refusals that need no frame */
    {
        yah264_control_t ev;
        yah264_control_init(&ev, YAH264_CTL_KEYFRAME);
        CHECK(ev.size == (int)sizeof ev && ev.frame == YAH264_CTL_NEXT && ev.applied_frame == -1);
        CHECK(ev.qp_min == -1 && ev.qp_max == -1);
        for (int c = 0; c >= -8; c--) CHECK(yah264_control_strerror(c) && *yah264_control_strerror(c));

        base_param(&p, YAH264_RC_ABR);
        CHECK(open_enc(&r, &p) == 0);
        yah264_encoder_t *e = r.e;
        CHECK(yah264_encoder_control(NULL, &ev) == YAH264_CTL_EINVAL);
        CHECK(yah264_encoder_control(e, NULL) == YAH264_CTL_EINVAL);
        ev.size = 8;
        CHECK(yah264_encoder_control(e, &ev) == YAH264_CTL_EINVAL);
        ev.size = (int)sizeof ev + 16;
        CHECK(yah264_encoder_control(e, &ev) == YAH264_CTL_EINVAL);
        yah264_control_init(&ev, 99);
        CHECK(yah264_encoder_control(e, &ev) == YAH264_CTL_EUNSUPPORTED);
        yah264_control_init(&ev, 0);
        CHECK(yah264_encoder_control(e, &ev) == YAH264_CTL_EINVAL);
        int unsup[] = { YAH264_CTL_FPS, YAH264_CTL_PRESET, YAH264_CTL_RESOLUTION };
        for (int i = 0; i < 3; i++) {
            yah264_control_init(&ev, unsup[i]);
            ev.fps_num = 30; ev.fps_den = 1; ev.width = 64; ev.height = 48;
            snprintf(ev.preset, sizeof ev.preset, "fast");
            CHECK(yah264_encoder_control(e, &ev) == YAH264_CTL_EUNSUPPORTED);
        }
        yah264_control_init(&ev, YAH264_CTL_CRF); ev.crf = 20;
        CHECK(yah264_encoder_control(e, &ev) == YAH264_CTL_EMODE);       /* not a CRF encode */
        CHECK(ev_rate(e, 0, 0, 0, 0) == YAH264_CTL_EINVAL);              /* nothing to change */
        CHECK(ev_rate(e, 0, 300, 450, 0) == YAH264_CTL_EMODE);           /* CBR stays CBR */
        CHECK(ev_rate(e, 0, 100000, 0, 0) == YAH264_CTL_ELEVEL);         /* past the level */
        yah264_control_init(&ev, YAH264_CTL_QP_LIMITS); ev.qp_min = 40; ev.qp_max = 30;
        CHECK(yah264_encoder_control(e, &ev) == YAH264_CTL_EINVAL);
        yah264_control_init(&ev, YAH264_CTL_ZONE); ev.frame = 10; ev.last = 5;
        CHECK(yah264_encoder_control(e, &ev) == YAH264_CTL_EINVAL);
        ev.last = 15; ev.qp_offset = -2;
        CHECK(yah264_encoder_control(e, &ev) == YAH264_CTL_OK);
        ev.frame = 15; ev.last = 20;
        CHECK(yah264_encoder_control(e, &ev) == YAH264_CTL_ECONFLICT);
        /* NEXT resolves to the next picture, and a handed-in frame is late */
        CHECK(ev_rate(e, YAH264_CTL_NEXT, 300, 0, 0) == YAH264_CTL_OK);
        for (int i = 0; i < 3; i++) CHECK(push(&r) == 0);
        yah264_control_init(&ev, YAH264_CTL_KEYFRAME);
        CHECK(yah264_encoder_control(e, &ev) == YAH264_CTL_OK && ev.frame == 3);
        ev.frame = 2;
        CHECK(yah264_encoder_control(e, &ev) == YAH264_CTL_ELATE);
        yah264_status_t st; memset(&st, 0, sizeof st); st.size = (int)sizeof st;
        CHECK(yah264_encoder_status(e, &st) == 0);
        CHECK(st.frames_in == 3 && st.next_frame == 3);
        CHECK(st.queue_depth == st.frames_in - st.frames_out && st.queue_depth >= 0);
        CHECK(st.target_kbps == 400 && st.vbv_bufsize_kbit == 400);
        CHECK(st.controls_pending >= 2);
        st.size = 4;
        CHECK(yah264_encoder_status(e, &st) < 0);
        close_enc(&r);

        base_param(&p, YAH264_RC_CRF);
        CHECK(open_enc(&r, &p) == 0);
        CHECK(ev_rate(r.e, 0, 300, 0, 0) == YAH264_CTL_EMODE);           /* no bitrate in CRF */
        CHECK(ev_rate(r.e, 0, 0, 300, 0) == YAH264_CTL_EMODE);           /* no buffer at open */
        yah264_control_init(&ev, YAH264_CTL_CRF); ev.crf = 0;
        CHECK(yah264_encoder_control(r.e, &ev) == YAH264_CTL_EINVAL);
        close_enc(&r);

        base_param(&p, YAH264_RC_CRF);
        p.rc.vbv_maxrate = 500; p.rc.vbv_bufsize = 500;                  /* capped CRF */
        CHECK(open_enc(&r, &p) == 0);
        CHECK(ev_rate(r.e, 0, 0, 300, 300) == YAH264_CTL_OK);
        CHECK(ev_rate(r.e, 0, 300, 0, 0) == YAH264_CTL_EMODE);
        close_enc(&r);

        base_param(&p, YAH264_RC_CQP);
        CHECK(open_enc(&r, &p) == 0);
        yah264_control_init(&ev, YAH264_CTL_QP_LIMITS); ev.qp_min = 20;
        CHECK(yah264_encoder_control(r.e, &ev) == YAH264_CTL_EMODE);
        close_enc(&r);

        base_param(&p, YAH264_RC_ABR);
        p.rc.vbv_maxrate = 600; p.rc.vbv_bufsize = 800;
        p.nal_hrd = YAH264_NAL_HRD_VBR;
        CHECK(open_enc(&r, &p) == 0);
        CHECK(ev_rate(r.e, 0, 300, 500, 600) == YAH264_CTL_OK);          /* inside the declaration */
        CHECK(ev_rate(r.e, 0, 0, 700, 0) == YAH264_CTL_EHRD);            /* above it */
        yah264_control_init(&ev, YAH264_CTL_QP_LIMITS); ev.qp_max = 40;
        CHECK(yah264_encoder_control(r.e, &ev) == YAH264_CTL_EHRD);
        close_enc(&r);
        p.nal_hrd = YAH264_NAL_HRD_CBR; p.rc.vbv_maxrate = 400;
        CHECK(open_enc(&r, &p) == 0);
        CHECK(ev_rate(r.e, 0, 300, 300, 0) == YAH264_CTL_EHRD);
        close_enc(&r);
    }

    /* 1b. events handed in out of frame order are judged against the state at
     * their own frame, and against every later queued event of their kind */
    {
        yah264_control_t ev;
        base_param(&p, YAH264_RC_ABR);                  /* capped VBR 800 / 1200 */
        p.rc.bitrate = 800; p.rc.vbv_maxrate = 1200; p.rc.vbv_bufsize = 1200; p.level_idc = 40;
        CHECK(open_enc(&r, &p) == 0);
        CHECK(ev_rate(r.e, 60, 300, 0, 0) == YAH264_CTL_OK);
        CHECK(ev_rate(r.e, 30, 0, 600, 0) == YAH264_CTL_EMODE);   /* 30..59 would cap below 800 */
        CHECK(ev_rate(r.e, 70, 0, 600, 0) == YAH264_CTL_OK);      /* after the step down it fits */
        CHECK(ev_rate(r.e, 65, 700, 0, 0) == YAH264_CTL_EMODE);   /* would break the 70 event */
        close_enc(&r);

        base_param(&p, YAH264_RC_ABR);
        p.rc.bitrate = 600; p.rc.vbv_maxrate = 900; p.rc.vbv_bufsize = 900; p.level_idc = 40;
        CHECK(open_enc(&r, &p) == 0);
        CHECK(ev_rate(r.e, 20, 0, 2000, 0) == YAH264_CTL_OK);
        CHECK(ev_rate(r.e, 10, 1500, 0, 0) == YAH264_CTL_EMODE);  /* 10..19 under the old cap */
        close_enc(&r);

        base_param(&p, YAH264_RC_ABR);                  /* CBR 3000 */
        p.rc.bitrate = 3000; p.rc.vbv_maxrate = 3000; p.rc.vbv_bufsize = 3000; p.level_idc = 40;
        CHECK(open_enc(&r, &p) == 0);
        CHECK(ev_rate(r.e, 300, 1000, 0, 0) == YAH264_CTL_OK);
        CHECK(ev_rate(r.e, 100, 0, 3000, 0) == YAH264_CTL_OK);    /* valid at 100, and 300 still is */
        close_enc(&r);

        base_param(&p, YAH264_RC_ABR);
        CHECK(open_enc(&r, &p) == 0);
        yah264_control_init(&ev, YAH264_CTL_QP_LIMITS); ev.frame = 60; ev.qp_min = 40;
        CHECK(yah264_encoder_control(r.e, &ev) == YAH264_CTL_OK);
        yah264_control_init(&ev, YAH264_CTL_QP_LIMITS); ev.frame = 30; ev.qp_max = 30;
        CHECK(yah264_encoder_control(r.e, &ev) == YAH264_CTL_EINVAL);   /* crosses at 60 */
        yah264_control_init(&ev, YAH264_CTL_QP_LIMITS); ev.frame = 20; ev.qp_min = 25;
        CHECK(yah264_encoder_control(r.e, &ev) == YAH264_CTL_OK);
        yah264_control_init(&ev, YAH264_CTL_QP_LIMITS); ev.frame = 10; ev.qp_max = 24;
        CHECK(yah264_encoder_control(r.e, &ev) == YAH264_CTL_EINVAL);   /* crosses at 20 */
        /* every accepted pair holds on every frame, so nothing is clamped */
        for (int i = 0; i < NF; i++) CHECK(push(&r) == 0);
        flush(&r);
        for (int i = 20; i < NF; i++) CHECK(r.st[i].qp >= 25);
        for (int i = 60; i < NF; i++) CHECK(r.st[i].qp >= 40);
        close_enc(&r);
    }

    /* 1c. a step down from a target the encode could not reach lands */
    {
        base_param(&p, YAH264_RC_ABR);
        p.rc.bitrate = 30000; p.rc.vbv_maxrate = 0; p.rc.vbv_bufsize = 0; p.level_idc = 51;
        p.bframes = 3;
        CHECK(open_enc(&r, &p) == 0);
        CHECK(ev_rate(r.e, 30, 100, 0, 0) == YAH264_CTL_OK);
        for (int i = 0; i < NF; i++) CHECK(push(&r) == 0);
        flush(&r);
        /* 100 kbit/s at 25 fps is 500 bytes a frame: from a second after the
         * step it must be near that, far under what 30 Mbit/s spends */
        long tail = 0;
        for (int i = 55; i < NF; i++) tail += r.bytes[i];
        CHECK(tail / (NF - 55) < 1500);
        long head = 0;
        for (int i = 5; i < 30; i++) head += r.bytes[i];
        CHECK(head / 25 > 4 * (tail / (NF - 55)));
        close_enc(&r);
    }

    /* 1d. control_check agrees with the real queue: one event at a time,
     * and over a batch it names the first event the queue would refuse */
    {
        unsigned rng = 12345;
#define RND(n) ((int)((rng = rng * 1103515245u + 12345u) >> 16) % (n))
        base_param(&p, YAH264_RC_ABR);
        p.rc.bitrate = 800; p.rc.vbv_maxrate = 1200; p.rc.vbv_bufsize = 1200; p.level_idc = 31;
        CHECK(open_enc(&r, &p) == 0);
        int bad = 0, agree = 1, ok = 0;
        CHECK(yah264_encoder_control_check(r.e, NULL, 1, &bad) == YAH264_CTL_EINVAL && bad == -1);
        CHECK(yah264_encoder_control_check(r.e, NULL, -1, &bad) == YAH264_CTL_EINVAL && bad == -1);
        CHECK(yah264_encoder_control_check(r.e, NULL, 0, NULL) == YAH264_CTL_EINVAL);
        CHECK(yah264_encoder_control_check(r.e, NULL, 0, &bad) == YAH264_CTL_OK);
        for (int it = 0; it < 1500; it++) {
            yah264_control_t ev;
            int k = 1 + RND(5);
            yah264_control_init(&ev, k);
            ev.frame = r.n + RND(60);
            if (k == YAH264_CTL_RATE) {
                int w = RND(3);
                if (w == 0) ev.bitrate_kbps = 200 + RND(1400);
                if (w == 1) ev.vbv_maxrate_kbps = 200 + RND(2000);
                if (w == 2) { ev.bitrate_kbps = 200 + RND(1400); ev.vbv_maxrate_kbps = 200 + RND(2000); }
                if (RND(2)) ev.vbv_bufsize_kbit = 300 + RND(2000);
            } else if (k == YAH264_CTL_CRF) {
                ev.crf = 20;
            } else if (k == YAH264_CTL_QP_LIMITS) {
                if (RND(2)) ev.qp_min = RND(52);
                if (RND(2) || ev.qp_min < 0) ev.qp_max = RND(52);
            } else if (k == YAH264_CTL_ZONE) {
                ev.last = ev.frame + RND(5);
                ev.qp_offset = RND(9) - 4;
            }
            int c1 = yah264_encoder_control_check(r.e, &ev, 1, &bad);
            int c2 = yah264_encoder_control(r.e, &ev);
            if (c1 != c2) { agree = 0; fprintf(stderr, "check %d, control %d, kind %d\n", c1, c2, k); }
            ok += c2 == YAH264_CTL_OK;
            if (it % 20 == 19 && r.n < NF) CHECK(push(&r) == 0);    /* let some land */
        }
        CHECK(agree && ok > 100);
        /* a batch: the first refusal's index matches submitting one by one */
        yah264_control_t b[40];
        for (int i = 0; i < 40; i++) {
            yah264_control_init(&b[i], YAH264_CTL_RATE);
            b[i].frame = r.n + 100 + i;
            b[i].vbv_maxrate_kbps = 900 + 10 * i;
            b[i].bitrate_kbps = i == 25 ? 2000 : 0;         /* past the cap: refused */
        }
        int code = yah264_encoder_control_check(r.e, b, 40, &bad);
        int first = -1, fc = 0;
        for (int i = 0; i < 40 && first < 0; i++) {
            int c = yah264_encoder_control(r.e, &b[i]);
            if (c != YAH264_CTL_OK) { first = i; fc = c; }
        }
        CHECK(code != YAH264_CTL_OK && bad == first && code == fc);
        close_enc(&r);
#undef RND
    }

    /* 2. landing, with B frames, the lookahead and a cut */
    unsigned char *ref = NULL; long ref_len = 0;
    {
        base_param(&p, YAH264_RC_ABR);
        p.bframes = 3;
        CHECK(open_enc(&r, &p) == 0);
        landing_events(r.e, -1);
        for (int i = 0; i < NF; i++) CHECK(push(&r) == 0);
        flush(&r);
        for (int i = 0; i < NF; i++) CHECK(r.have[i]);
        CHECK(r.st[20].is_idr);
        const yah264_control_t *d = done_of(&r, YAH264_CTL_KEYFRAME);
        CHECK(d && d->frame == 20 && d->applied_frame == 20);
        d = done_of(&r, YAH264_CTL_QP_LIMITS);
        CHECK(d && d->applied_frame == 30);
        for (int i = 30; i < NF; i++) CHECK(r.st[i].qp >= 33);
        d = done_of(&r, YAH264_CTL_ZONE);
        CHECK(d && d->applied_frame == 50 && d->last == 53);
        /* a rate decision event lands at the first frame in coding order
         * numbered 25 or later: 25 itself, or an anchor after it */
        d = done_of(&r, YAH264_CTL_RATE);
        CHECK(d && d->frame == 25 && d->applied_frame >= 25 && d->applied_frame <= 25 + p.bframes + 1);
        yah264_status_t st; memset(&st, 0, sizeof st); st.size = (int)sizeof st;
        CHECK(yah264_encoder_status(r.e, &st) == 0);
        CHECK(st.frames_in == NF && st.frames_out == NF && st.queue_depth == 0);
        CHECK(st.controls_applied == 4 && st.controls_pending == 0);
        CHECK(st.target_kbps == 150 && st.vbv_maxrate_kbps == 150);
        ref = r.buf; ref_len = r.len; r.buf = NULL;
        close_enc(&r);
    }

    /* 3. the same events handed in just before each frame: the same bytes */
    {
        CHECK(open_enc(&r, &p) == 0);
        for (int i = 0; i < NF; i++) {
            landing_events(r.e, i);
            CHECK(push(&r) == 0);
        }
        flush(&r);
        CHECK(r.len == ref_len && !memcmp(r.buf, ref, (size_t)ref_len));
        close_enc(&r);
    }

    /* 4. no events at all is the stream of an encode that never heard of
     * them; an event that never lands comes back with -1 after the flush */
    {
        base_param(&p, YAH264_RC_ABR);
        p.bframes = 3;
        CHECK(open_enc(&r, &p) == 0);
        for (int i = 0; i < NF; i++) CHECK(push(&r) == 0);
        flush(&r);
        long plain = r.len;
        unsigned char *pb = r.buf; r.buf = NULL;
        close_enc(&r);
        CHECK(open_enc(&r, &p) == 0);
        yah264_control_t ev; yah264_control_init(&ev, YAH264_CTL_KEYFRAME); ev.frame = NF + 10;
        CHECK(yah264_encoder_control(r.e, &ev) == YAH264_CTL_OK);
        CHECK(ev_rate(r.e, NF + 20, 200, 0, 0) == YAH264_CTL_OK);
        for (int i = 0; i < NF; i++) CHECK(push(&r) == 0);
        flush(&r);
        CHECK(r.len == plain && !memcmp(r.buf, pb, (size_t)plain));
        CHECK(r.ndone == 2 && r.done[0].applied_frame == -1 && r.done[1].applied_frame == -1);
        free(pb);
        close_enc(&r);
    }

    /* 5. CRF and a step: frames after the landing code coarser */
    {
        base_param(&p, YAH264_RC_CRF);
        p.bframes = 0;
        CHECK(open_enc(&r, &p) == 0);
        yah264_control_t ev; yah264_control_init(&ev, YAH264_CTL_CRF); ev.frame = 40; ev.crf = 40;
        CHECK(yah264_encoder_control(r.e, &ev) == YAH264_CTL_OK);
        for (int i = 0; i < NF; i++) CHECK(push(&r) == 0);
        flush(&r);
        const yah264_control_t *d = done_of(&r, YAH264_CTL_CRF);
        CHECK(d && d->applied_frame == 40);
        CHECK(r.st[45].qp > r.st[35].qp + 6);
        close_enc(&r);
    }
    free(ref);

    /* 6. the shared log grammar */
    {
        struct ctl_log_event le;
        char err[160], buf[200];
        CHECK(ctl_log_parse("120 rate bitrate=2000 maxrate=2000\n", 0, &le, err, sizeof err) == 1);
        CHECK(le.kind == CTL_KIND_RATE && le.frame == 120 && le.bitrate_kbps == 2000 &&
              le.vbv_maxrate_kbps == 2000 && le.vbv_bufsize_kbit == 0);
        ctl_log_format(&le, buf, sizeof buf);
        CHECK(!strcmp(buf, "120 rate bitrate=2000 maxrate=2000"));
        CHECK(ctl_log_parse("  # a comment", 0, &le, err, sizeof err) == 0);
        CHECK(ctl_log_parse("   \r\n", 0, &le, err, sizeof err) == 0);
        CHECK(ctl_log_parse("7 key", 0, &le, err, sizeof err) == 1 && le.kind == CTL_KIND_KEYFRAME);
        CHECK(ctl_log_parse("next key", 1, &le, err, sizeof err) == 1 && le.frame == CTL_FRAME_NEXT);
        CHECK(ctl_log_parse("next key", 0, &le, err, sizeof err) < 0);
        CHECK(ctl_log_parse("3 zone last=9 offset=-2.5", 0, &le, err, sizeof err) == 1 &&
              le.last == 9 && le.qp_offset == -2.5);
        ctl_log_format(&le, buf, sizeof buf);
        CHECK(!strcmp(buf, "3 zone last=9 offset=-2.5"));
        CHECK(ctl_log_parse("3 crf value=21.25", 0, &le, err, sizeof err) == 1 && le.crf == 21.25);
        CHECK(ctl_log_parse("3 qp max=40", 0, &le, err, sizeof err) == 1 && le.qp_min == -1 && le.qp_max == 40);
        CHECK(ctl_log_parse("3 fps num=30 den=1", 0, &le, err, sizeof err) == 1 && le.kind == CTL_KIND_FPS);
        const char *bad[] = {
            "3", "x key", "-3 key", "3 warp", "3 key type=open", "3 key type=refresh",
            "3 rate", "3 rate bitrate=", "3 rate bitrate=abc", "3 rate bitrate=1 bitrate=2",
            "3 rate speed=4", "3 crf", "3 crf value=nan", "3 qp", "3 qp min=52", "3 zone last=9",
            "3 zone offset=1", "3 rate bitrate=-5", "3 size width=1",
            "3 zone last=9 offset=0x10", "3 zone last=9 offset=1e1", "3 zone last=9 offset=.5",
            "3 zone last=9 offset=5.", "3 crf value=inf", "3 rate bitrate=+5", "3 rate bitrate=0x10",
            "5 key # trailing comment",
        };
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            int rr = ctl_log_parse(bad[i], 1, &le, err, sizeof err);
            if (rr != -1) fprintf(stderr, "FAIL accepted '%s'\n", bad[i]);
            CHECK(rr == -1 && err[0]);
        }
        CHECK(ctl_log_parse("3 zone last=9 offset=+2", 0, &le, err, sizeof err) == 1 && le.qp_offset == 2);
        le.kind = CTL_KIND_ZONE; le.frame = 3; le.last = 9; le.qp_offset = 0.0000001;
        ctl_log_format(&le, buf, sizeof buf);
        CHECK(!strstr(buf, "e-"));
        CHECK(ctl_log_parse(buf, 0, &le, err, sizeof err) == 1 && le.qp_offset == 0.0000001);
        CHECK(ctl_log_header("#livectl 1") == 1);
        CHECK(ctl_log_header("#livectl 2\n") == 2);
        CHECK(ctl_log_header("# livectl 1") == 0);
        CHECK(ctl_log_header("#livectl x") == -1);
    }

    if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
    printf("control: all checks passed\n");
    return 0;
}
