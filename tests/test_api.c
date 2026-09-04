// Copyright (c) 2026, the yah264 authors
// SPDX-License-Identifier: BSD-2-Clause
//
// The public API, end to end: defaults -> open -> headers -> encode -> close,
// on a synthetic picture, plus the cases open must refuse. Nothing else in
// the unit tests reaches yah264_encoder_open; this is the smoke test a caller
// of the library would write first.

#include <yah264.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void fill(pixel *p, int w, int h, int seed)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            p[(size_t)y * w + x] = (pixel)((x * 3 + y * 5 + seed * 7) & 0xff);
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
    pixel *y = malloc((size_t)W * H * sizeof(pixel));
    pixel *u = malloc((size_t)W * H / 4 * sizeof(pixel) + 64);
    pixel *v = malloc((size_t)W * H / 4 * sizeof(pixel) + 64);
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

    if (fails) { fprintf(stderr, "test_api: %d failure(s)\n", fails); return 1; }
    printf("test_api: ok\n");
    return 0;
}
