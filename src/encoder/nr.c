/*
 * nr.c - coefficient-domain noise reduction: the estimator and the offsets.
 * nr.h says what it is.
 *
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "nr.h"
#include "../dsp/transform.h"

/* Per-position weight, Q8, normalised so the smallest gain reads 256: the
 * squared gain of each coefficient of the forward integer transform over the
 * pixel domain, measured by pushing unit impulses through the C transform
 * once. Measured rather than tabulated so it cannot drift from the transform
 * the encoder actually runs. */
static uint16_t g_w[80];
static pthread_once_t g_w_once = PTHREAD_ONCE_INIT;

static void nr_weights_init(void)
{
    double g4[16] = { 0 }, g8[64] = { 0 };
    for (int p = 0; p < 16; p++) {
        dctcoef d[16] = { 0 }, c[16];
        d[p] = 64;
        y264_fdct4x4_c(d, c);
        for (int i = 0; i < 16; i++)
            g4[i] += (double)c[i] * c[i] / (64.0 * 64.0);
    }
    for (int p = 0; p < 64; p++) {
        dctcoef d[64] = { 0 }, c[64];
        d[p] = 64;
        y264_fdct8x8_c(d, c);
        for (int i = 0; i < 64; i++)
            g8[i] += (double)c[i] * c[i] / (64.0 * 64.0);
    }
    /* Y264_NR_FLATW=1: every position weighs the same (the unweighted
     * reciprocal), kept as the arm the gain weighting was measured against */
    const char *fw = getenv("Y264_NR_FLATW");
    if (fw && atoi(fw)) {
        for (int i = 0; i < 80; i++) g_w[i] = 256;
        return;
    }
    double m4 = g4[0], m8 = g8[0];
    for (int i = 0; i < 16; i++) if (g4[i] < m4) m4 = g4[i];
    for (int i = 0; i < 64; i++) if (g8[i] < m8) m8 = g8[i];
    /* the energy ratio IS the squared amplitude gain, which is what the
     * offset is scaled by (nr.h) */
    for (int i = 0; i < 16; i++) {
        double r = g4[i] / m4;
        g_w[i] = (uint16_t)(256.0 * r + 0.5);
    }
    for (int i = 0; i < 64; i++) {
        double r = g8[i] / m8;
        g_w[16 + i] = (uint16_t)(256.0 * r + 0.5);
    }
}

static void nr_weights(void)
{
    pthread_once(&g_w_once, nr_weights_init);
}

void y264_nr_fold(y264_nr *nr, const y264_nr_tally *t)
{
    for (int i = 0; i < Y264_NR_SLOTS; i++)
        nr->sum[i] += t->sum[i];
    for (int c = 0; c < Y264_NR_CATS; c++)
        nr->cnt[c] += t->cnt[c];
    /* halving both halves of the ratio at once leaves the offset where it
     * was; the estimate only starts forgetting */
    for (int c = 0; c < Y264_NR_CATS; c++) {
        if (nr->cnt[c] < Y264_NR_HALVE)
            continue;
        int g = c >> 1, big = c & 1;
        int base = g * Y264_NR_GROUP + (big ? 16 : 0), n = big ? 64 : 16;
        for (int i = 0; i < n; i++)
            nr->sum[base + i] >>= 1;
        nr->cnt[c] >>= 1;
    }
}

void y264_nr_offsets(y264_nr *nr, int intra, int inter)
{
    nr_weights();
    for (int c = 0; c < Y264_NR_CATS; c++) {
        int g = c >> 1, big = c & 1;
        int base = g * Y264_NR_GROUP + (big ? 16 : 0), n = big ? 64 : 16;
        int str = g < 2 ? intra : inter;
        uint64_t cnt = nr->cnt[c];
        if (str <= 0 || cnt == 0) {
            memset(nr->off + base, 0, (size_t)n * sizeof(uint16_t));
            continue;
        }
        const uint16_t *w = g_w + (big ? 16 : 0);
        for (int i = 0; i < n; i++) {
            uint64_t s = nr->sum[base + i];
            /* offset = strength * weight / (mean + 1), carried as
             * strength * weight * cnt / (sum + cnt) so nothing floats */
            uint64_t num = (uint64_t)str * w[i] * cnt;
            uint64_t den = (s + cnt) * 256;
            uint64_t o = (num + den / 2) / den;
            /* never take off more than the position carries on average:
             * past that it stops being attenuation and deletes the band,
             * which is what a shot change would otherwise ask for while
             * the mean catches up */
            uint64_t m = s / cnt;
            if (o > m) o = m;
            if (o > 32767) o = 32767;
            nr->off[base + i] = (uint16_t)o;
        }
    }
}

/* The spatial noise reading the NR gate uses: the 25th percentile over 8x8
 * blocks of the block's mean absolute Laplacian, /4, which for white noise of
 * deviation s reads 0.89 s (the Laplacian's deviation is 4.47 s, a quarter of
 * it 1.12 s, and a normal's mean absolute value is 0.80 of its deviation).
 * Blocks near black or white are left out, since clipping takes the noise with
 * it and a letterboxed picture would otherwise read clean. A clean digital
 * source -- rendered animation above all -- reads at or near zero, and that
 * is the population where attenuating "quiet" positions removes structure,
 * not noise. Q8, 8-bit levels. */
int y264_nr_noise_q8(const pixel *p, int st, int w, int h)
{
    enum { NBIN = 512 };
    int hist[NBIN];
    int n = 0, all = 0;
    memset(hist, 0, sizeof hist);
    for (int by = 8; by + 16 <= h; by += 16)
        for (int bx = 8; bx + 16 <= w; bx += 16) {
            const pixel *b = p + (size_t)by * st + bx;
            unsigned acc = 0, sum = 0;
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    const pixel *v = b + (size_t)y * st + x;
                    int d = 4 * (int)v[0] - (int)v[-st] - (int)v[st] - (int)v[-1] - (int)v[1];
                    acc += (unsigned)(d < 0 ? -d : d);
                    sum += v[0];
                }
            all++;
            unsigned mean = (sum / 64u) >> (Y264_BIT_DEPTH - 8);
            if (mean < 24 || mean > 232)
                continue;
            unsigned q4 = (acc * 16u / 64u / 4u) >> (Y264_BIT_DEPTH - 8);
            hist[q4 < NBIN - 1 ? q4 : NBIN - 1]++;
            n++;
        }
    if (!n || n * 8 < all)
        return 0;
    int want = n / 4, run = 0;
    for (int i = 0; i < NBIN; i++) {
        run += hist[i];
        if (run > want)
            return i * 16;
    }
    return (NBIN - 1) * 16;
}
