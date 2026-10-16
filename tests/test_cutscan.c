/*
 * test_cutscan.c - the streamed cut scan returns the whole-input scan's IDR
 * map frame for frame, and the streamed shot scan returns its IDR map and
 * shot table bit for bit, at any batch size and thread count.
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "yah264.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } \
} while (0)

#define W 320
#define H 192

static unsigned rnd(unsigned *s) { *s = *s * 1103515245u + 12345u; return *s >> 16; }

/* A textured plane per scene, panned a pixel a frame, with hard cuts, a
 * one-frame flash and a stretch of rising noise (the ramp a falling cut
 * threshold walks onto). */
static void make(unsigned char **fr, int n)
{
    unsigned char *tex[4];
    unsigned seed = 7;
    for (int t = 0; t < 4; t++) {
        tex[t] = malloc((size_t)(W + n) * H);
        for (int i = 0; i < (W + n) * H; i++) tex[t][i] = (unsigned char)(rnd(&seed) & 0xff);
        /* smooth it a little so motion search has something to find */
        for (int y = 0; y < H; y++)
            for (int x = 1; x < W + n; x++)
                tex[t][y * (W + n) + x] = (unsigned char)((tex[t][y * (W + n) + x] + 3 * tex[t][y * (W + n) + x - 1]) / 4);
    }
    for (int f = 0; f < n; f++) {
        int sc = f < 70 ? 0 : f < 150 ? 1 : f < 260 ? 2 : 3;
        if (f == 200) sc = 0;                          /* a flash */
        unsigned char *d = fr[f];
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                int v = tex[sc][y * (W + n) + x + f];
                if (f >= 280 && f < 340) v += (int)(rnd(&seed) % (unsigned)(1 + (f - 280) * 2)) - (f - 280);
                d[y * W + x] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
            }
    }
    for (int t = 0; t < 4; t++) free(tex[t]);
}

static void run(int n, int keyint, int bframes)
{
    yah264_param_t p;
    yah264_param_default(&p);
    p.width = W; p.height = H; p.keyint = keyint; p.bframes = bframes;
    unsigned char **fr = malloc((size_t)n * sizeof *fr);
    const void **luma = malloc((size_t)n * sizeof *luma);
    int *stride = malloc((size_t)n * sizeof *stride);
    for (int i = 0; i < n; i++) { fr[i] = malloc(W * H); luma[i] = fr[i]; stride[i] = W; }
    make(fr, n);
    /* the scans read the library's own sample type, so a high-bit-depth build
     * gets the same picture widened, not 8-bit bytes read as 16-bit samples */
    unsigned short **wide = NULL;
    if (yah264_bit_depth() > 8) {
        wide = malloc((size_t)n * sizeof *wide);
        for (int i = 0; i < n; i++) {
            wide[i] = malloc((size_t)W * H * sizeof **wide);
            for (int j = 0; j < W * H; j++) wide[i][j] = (unsigned short)(fr[i][j] << (yah264_bit_depth() - 8));
            luma[i] = wide[i];
        }
    }
    unsigned char *a = calloc((size_t)n, 1), *b = calloc((size_t)n, 1);
    int na = yah264_scan_idr_frames(&p, luma, stride, n, 1, yah264_bit_depth(), a);
    yah264_cutscan_t *s = yah264_cutscan_open(&p, yah264_bit_depth());
    CHECK(s != NULL, "cutscan_open");
    int nb = 0;
    for (int i = 0; s && i < n; i++) {
        int d = yah264_cutscan_push(s, luma[i], W);
        CHECK(i == 0 ? d == -1 : d >= 0, "push %d returned %d", i, d);
        if (i > 0 && d >= 0) { b[i - 1] = (unsigned char)d; nb += d; }
    }
    if (s) { int d = yah264_cutscan_flush(s); CHECK(d >= 0, "flush"); b[n - 1] = (unsigned char)(d > 0); nb += d > 0; }
    yah264_cutscan_close(s);
    int cuts = 0;
    for (int i = 0; i < n; i++) {
        CHECK(a[i] == b[i], "n=%d keyint=%d bframes=%d frame %d: whole %d streamed %d", n, keyint, bframes, i, a[i], b[i]);
        cuts += a[i] && i % keyint;
    }
    CHECK(na == nb, "counts %d vs %d", na, nb);
    CHECK(cuts >= 2, "the clip should hold cuts (%d)", cuts);

    yah264_shot_t *sa = calloc((size_t)n, sizeof *sa), *sb = calloc((size_t)n, sizeof *sb);
    int nsa = yah264_scan_shots(&p, luma, stride, n, 1, yah264_bit_depth(), NULL, sa, n);
    CHECK(nsa >= 3, "the clip should hold shots (%d)", nsa);
    static const int shape[][2] = { { 1, 1 }, { 3, 1 }, { 7, 4 }, { 16, 8 } };
    for (int c = 0; c < 4; c++) {
        int batch = shape[c][0], thr = shape[c][1];
        yah264_shotscan_t *ss = yah264_shotscan_open(&p, yah264_bit_depth(), thr, batch);
        CHECK(ss != NULL, "shotscan_open");
        if (!ss) continue;
        for (int i = 0; i < n; i += batch) {
            int k = n - i < batch ? n - i : batch;
            CHECK(yah264_shotscan_push(ss, luma + i, stride + i, k) == 0, "shotscan push %d", i);
        }
        CHECK(yah264_shotscan_finish(ss) == n, "shotscan finish");
        memset(b, 0, (size_t)n);
        int nc = yah264_shotscan_idr(ss, b, n);
        CHECK(nc == na, "batch %d: IDR count %d vs %d", batch, nc, na);
        CHECK(!memcmp(a, b, (size_t)n), "batch %d threads %d: IDR map differs", batch, thr);
        int nsb = yah264_shotscan_shots(ss, NULL, 0);
        CHECK(nsb == nsa, "batch %d: %d shots vs %d", batch, nsb, nsa);
        if (nsb == nsa) {
            memset(sb, 0, (size_t)n * sizeof *sb);
            yah264_shotscan_shots(ss, sb, nsb);
            CHECK(!memcmp(sa, sb, (size_t)nsa * sizeof *sa), "batch %d threads %d: shot table differs", batch, thr);
        }
        yah264_shotscan_close(ss);
    }
    free(sa); free(sb);
    for (int i = 0; i < n; i++) { free(fr[i]); if (wide) free(wide[i]); }
    free(wide);
    free(fr); free(luma); free(stride); free(a); free(b);
}

int main(void)
{
    run(360, 250, 3);
    run(360, 100, 3);
    run(360, 250, 0);
    run(151, 60, 3);
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("cutscan: OK\n");
    return 0;
}
