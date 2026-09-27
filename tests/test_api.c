// Copyright (c) 2026, the yah264 authors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The public API, end to end: defaults -> open -> headers -> encode -> close,
// on a synthetic picture, plus the cases open must refuse. Nothing else in
// the unit tests reaches yah264_encoder_open; this is the smoke test a caller
// of the library would write first.

#include <yah264.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Picture planes are void* now (item C3-10bit); the test names its own sample
 * type, the depth this build's library was compiled at. */
#if Y264_BIT_DEPTH > 8
typedef uint16_t sample_t;
#else
typedef uint8_t  sample_t;
#endif

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void fill(sample_t *p, int w, int h, int seed)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            p[(size_t)y * w + x] = (sample_t)((x * 3 + y * 5 + seed * 7) & 0xff);
}

static int encode_n(yah264_param_t *prm, int nframes, long *bytes_out, int *nal_out)
{
    yah264_encoder_t *e = yah264_encoder_open(prm);
    if (!e) return -1;
    yah264_nal_t *nal = NULL; int cnt = 0;
    long bytes = 0; int nals = 0;
    if (yah264_encoder_headers(e, &nal, &cnt) < 0) { yah264_encoder_close(e); return -2; }
    for (int k = 0; k < cnt; k++) { bytes += nal[k].size; nals++; }
    int W = prm->width, H = prm->height;
    sample_t *y = malloc((size_t)W * H * sizeof(sample_t));
    sample_t *u = malloc((size_t)W * H / 4 * sizeof(sample_t) + 64);
    sample_t *v = malloc((size_t)W * H / 4 * sizeof(sample_t) + 64);
    for (int n = 0; n < nframes; n++) {
        fill(y, W, H, n); fill(u, W / 2, H / 2, n + 100); fill(v, W / 2, H / 2, n + 200);
        yah264_picture_t pic; memset(&pic, 0, sizeof pic);
        pic.csp = prm->csp; pic.width = W; pic.height = H; pic.pts = n;
        pic.plane[0] = y; pic.plane[1] = u; pic.plane[2] = v;
        pic.stride[0] = W; pic.stride[1] = W / 2; pic.stride[2] = W / 2;
        int r = yah264_encoder_encode(e, &nal, &cnt, &pic);
        if (r < 0) { free(y); free(u); free(v); yah264_encoder_close(e); return -3; }
        for (int k = 0; k < cnt; k++) { bytes += nal[k].size; nals++; }
    }
    /* flush */
    for (int guard = 0; guard < 64; guard++) {
        int r = yah264_encoder_encode(e, &nal, &cnt, NULL);
        if (r < 0) break;
        if (cnt == 0) break;
        for (int k = 0; k < cnt; k++) { bytes += nal[k].size; nals++; }
    }
    free(y); free(u); free(v);
    yah264_encoder_close(e);
    if (bytes_out) *bytes_out = bytes;
    if (nal_out) *nal_out = nals;
    return 0;
}

/* Section 8: two encoders in one process. A high-contrast picture of 16x16
 * blocks, so an intra DC at QP 0 quantises far past the CAVLC prefix-15
 * bound (2063) and the level cap changes the bytes. */
static void fill_blocks(sample_t *p, int w, int h, int seed)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            p[(size_t)y * w + x] = (sample_t)((((x >> 4) + (y >> 4) + seed) & 1) ? 0xff : 0);
}

struct run { yah264_encoder_t *e; unsigned char *buf; long len, cap; sample_t *y, *u, *v; int n; };

static void run_take(struct run *r, const yah264_nal_t *nal, int cnt)
{
    for (int k = 0; k < cnt; k++) {
        if (r->len + nal[k].size > r->cap) {
            r->cap = 2 * (r->len + nal[k].size);
            r->buf = realloc(r->buf, (size_t)r->cap);
        }
        memcpy(r->buf + r->len, nal[k].payload, (size_t)nal[k].size);
        r->len += nal[k].size;
    }
}

static int run_open(struct run *r, const yah264_param_t *prm)
{
    memset(r, 0, sizeof *r);
    r->e = yah264_encoder_open(prm);
    if (!r->e) return -1;
    int W = prm->width, H = prm->height;
    r->y = malloc((size_t)W * H * sizeof(sample_t));
    r->u = malloc((size_t)W * H / 4 * sizeof(sample_t) + 64);
    r->v = malloc((size_t)W * H / 4 * sizeof(sample_t) + 64);
    yah264_nal_t *nal = NULL; int cnt = 0;
    if (yah264_encoder_headers(r->e, &nal, &cnt) < 0) return -2;
    run_take(r, nal, cnt);
    return 0;
}

static int run_frame(struct run *r, const yah264_param_t *prm)
{
    int W = prm->width, H = prm->height, n = r->n++;
    fill_blocks(r->y, W, H, n); fill_blocks(r->u, W / 2, H / 2, n + 1); fill_blocks(r->v, W / 2, H / 2, n);
    yah264_picture_t pic; memset(&pic, 0, sizeof pic);
    pic.csp = prm->csp; pic.width = W; pic.height = H; pic.pts = n;
    pic.plane[0] = r->y; pic.plane[1] = r->u; pic.plane[2] = r->v;
    pic.stride[0] = W; pic.stride[1] = W / 2; pic.stride[2] = W / 2;
    yah264_nal_t *nal = NULL; int cnt = 0;
    if (yah264_encoder_encode(r->e, &nal, &cnt, &pic) < 0) return -1;
    run_take(r, nal, cnt);
    return 0;
}

static void run_close(struct run *r)
{
    yah264_nal_t *nal = NULL; int cnt = 0;
    for (int guard = 0; guard < 64; guard++) {
        if (yah264_encoder_encode(r->e, &nal, &cnt, NULL) < 0 || cnt == 0) break;
        run_take(r, nal, cnt);
    }
    yah264_encoder_close(r->e); r->e = NULL;
    free(r->y); free(r->u); free(r->v);
}

#define LC_FRAMES 4

/* One encoder alone: the reference its output must match beside another. */
static struct run lc_alone(const yah264_param_t *prm)
{
    struct run r;
    if (run_open(&r, prm) == 0)
        for (int n = 0; n < LC_FRAMES; n++) run_frame(&r, prm);
    if (r.e) run_close(&r);
    return r;
}

/* Two encoders open at once, `a` opened first, frames interleaved. */
static void lc_pair(const yah264_param_t *a, const yah264_param_t *b, struct run *ra, struct run *rb)
{
    CHECK(run_open(ra, a) == 0);
    CHECK(run_open(rb, b) == 0);
    for (int n = 0; n < LC_FRAMES; n++) {
        CHECK(run_frame(ra, a) == 0);
        CHECK(run_frame(rb, b) == 0);
    }
    run_close(ra); run_close(rb);
}

static int same_bytes(const struct run *x, const struct run *y)
{
    return x->len > 0 && x->len == y->len && !memcmp(x->buf, y->buf, (size_t)x->len);
}

int main(void)
{
    yah264_param_t p;
    memset(&p, 0, sizeof p);
    yah264_param_default(&p);
    CHECK(p.keyint >= 1);
    CHECK(p.threads >= 0);

    /* 1. defaults, a small 4:2:0 picture, a few frames, single thread */
    p.width = 64; p.height = 48; p.csp = YAH264_CSP_I420;
    p.timebase.fps_num = 25; p.timebase.fps_den = 1;
    p.threads = 1;
    long bytes = 0; int nals = 0;
    CHECK(encode_n(&p, 6, &bytes, &nals) == 0);
    CHECK(bytes > 0 && nals >= 6);

    /* 2. the same at an automatic thread budget, with the preset applier */
    CHECK(yah264_param_apply_preset(&p, "veryfast") == 0);
    p.threads = 0;
    CHECK(encode_n(&p, 6, &bytes, &nals) == 0);
    CHECK(bytes > 0);

    /* 3. an unknown preset is refused, the params untouched */
    yah264_param_t q = p;
    CHECK(yah264_param_apply_preset(&q, "no-such-preset") != 0);

    /* 4. open must refuse an empty picture; a zero timebase is accepted and
 * treated as 25 fps by the rate control (documented fallback) */
    q = p; q.width = 0;
    CHECK(yah264_encoder_open(&q) == NULL);
    q = p; q.timebase.fps_num = 0; q.timebase.fps_den = 0;
    {
        yah264_encoder_t *e = yah264_encoder_open(&q);
        CHECK(e != NULL);
        yah264_encoder_close(e);
    }

    /* 4b. the ABI-3 size field. Open refuses a struct that is not this
 * library's, in both directions: a caller can be built against an older
 * header (a smaller struct) or a newer one, and either used to open and
 * then encode from fields read at the wrong offsets. The refusal prints to
 * stderr, so this test is noisy on purpose. */
    q = p; q.size = (int)sizeof(q) - 4;
    CHECK(yah264_encoder_open(&q) == NULL);
    q = p; q.size = (int)sizeof(q) + 4;
    CHECK(yah264_encoder_open(&q) == NULL);
    q = p; q.size = 0;                  /* zeroed, param_default never called */
    CHECK(yah264_encoder_open(&q) == NULL);
    CHECK(p.size == (int)sizeof(p));    /* param_default wrote it */

    /* 5. a NULL picture flush on a fresh encoder is not an error */
    {
        yah264_encoder_t *e = yah264_encoder_open(&p);
        CHECK(e != NULL);
        if (e) {
            yah264_nal_t *nal = NULL; int cnt = 0;
            CHECK(yah264_encoder_encode(e, &nal, &cnt, NULL) >= 0);
            yah264_encoder_close(e);
        }
    }

    /* 6. close on NULL is a no-op */
    yah264_encoder_close(NULL);

    /* 7. zones and frame stats (docs/engine-interface.md): overlapping zones
 * are refused; a forced-IDR zone yields an IDR record at that frame; every
 * coded frame has one stats record with a sane QP. */
    {
        yah264_zone_t bad[2] = { { 0, 10, 0, 0.0 }, { 5, 20, 0, 0.0 } };
        yah264_zone_t z[2] = { { 4, 7, YAH264_ZONE_IDR, 0.0 }, { 8, 11, 0, 6.0 } };
        yah264_param_t r = p; r.threads = 1; r.bframes = 0; r.keyint = 250;
        yah264_encoder_t *e = yah264_encoder_open(&r);
        CHECK(e != NULL);
        if (e) {
            CHECK(yah264_encoder_set_zones(e, bad, 2) < 0);
            CHECK(yah264_encoder_set_zones(e, z, 2) == 0);
            int W = r.width, H = r.height;
            sample_t *y = malloc((size_t)W * H * sizeof(sample_t)), *u = malloc((size_t)W * H / 4 * sizeof(sample_t) + 64), *v = malloc((size_t)W * H / 4 * sizeof(sample_t) + 64);
            yah264_nal_t *nal = NULL; int cnt = 0, got = 0, idr_at4 = 0, qp_lo = 0, qp_hi = 0, nlo = 0, nhi = 0;
            yah264_frame_stats_t st[64];
            yah264_encoder_headers(e, &nal, &cnt);
            /* most frames are coded during the flush with a lookahead, so the
 * records are inspected wherever they arrive */
#define TAKE_STATS() do { int m_ = yah264_encoder_frame_stats(e, st, 64); \
                for (int i = 0; i < m_; i++) { \
                    got++; \
                    if (st[i].disp == 4 && st[i].is_idr) idr_at4 = 1; \
                    if (st[i].disp >= 1 && st[i].disp <= 3 && st[i].type == 1) { qp_lo += st[i].qp; nlo++; } \
                    if (st[i].disp >= 9 && st[i].disp <= 11 && st[i].type == 1) { qp_hi += st[i].qp; nhi++; } \
                    CHECK(st[i].qp >= 0 && st[i].qp <= 51); \
                } } while (0)
            for (int n = 0; n < 12; n++) {
                fill(y, W, H, n); fill(u, W / 2, H / 2, n + 100); fill(v, W / 2, H / 2, n + 200);
                yah264_picture_t pic; memset(&pic, 0, sizeof pic);
                pic.csp = r.csp; pic.width = W; pic.height = H; pic.pts = n;
                pic.plane[0] = y; pic.plane[1] = u; pic.plane[2] = v;
                pic.stride[0] = W; pic.stride[1] = W / 2; pic.stride[2] = W / 2;
                CHECK(yah264_encoder_encode(e, &nal, &cnt, &pic) >= 0);
                TAKE_STATS();
            }
            for (int guard = 0; guard < 64; guard++) {
                int rr = yah264_encoder_encode(e, &nal, &cnt, NULL);
                if (rr < 0 || cnt == 0) break;
                TAKE_STATS();
            }
#undef TAKE_STATS
            CHECK(got == 12);
            CHECK(idr_at4);
            CHECK(nlo > 0 && nhi > 0 && qp_hi * nlo > qp_lo * nhi);   /* the +6 zone raised the P QP */
            free(y); free(u); free(v);
            yah264_encoder_close(e);
        }
    }

    /* 8. per-encoder profile state. The CAVLC level cap (below High) and its
 * writer check were process-wide, so a CAVLC encoder beside a CABAC one
 * quantised with whichever cap the last open set. Each encoder's output
 * beside the other, in both opening orders, must equal its output alone. */
    {
        yah264_param_t lv = p;
        lv.width = 128; lv.height = 96; lv.threads = 1; lv.bframes = 0;
        lv.keyint = 250; lv.rc.method = YAH264_RC_CQP; lv.rc.qp = 0;
        yah264_param_t hi = lv;
        lv.cabac = 0;                   /* CAVLC below High: capped at 2063 */
        hi.cabac = 1;                   /* CABAC: no cap */
        struct run lv1 = lc_alone(&lv), hi1 = lc_alone(&hi);
        CHECK(lv1.len > 0 && hi1.len > 0);
        struct run ra, rb;
        lc_pair(&lv, &hi, &ra, &rb);
        CHECK(same_bytes(&ra, &lv1));
        CHECK(same_bytes(&rb, &hi1));
        free(ra.buf); free(rb.buf);
        lc_pair(&hi, &lv, &ra, &rb);
        CHECK(same_bytes(&ra, &hi1));
        CHECK(same_bytes(&rb, &lv1));
        free(ra.buf); free(rb.buf);
        free(lv1.buf); free(hi1.buf);
    }

    if (fails) { fprintf(stderr, "test_api: %d failure(s)\n", fails); return 1; }
    printf("test_api: ok\n");
    return 0;
}
