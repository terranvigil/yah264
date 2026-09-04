/*
 * checkasm.c - kernel correctness and benchmark harness
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Validates every dispatched DSP kernel against its portable C reference on
 * randomized inputs, then benchmarks both. A kernel that disagrees with the
 * reference fails the run (non-zero exit), which is what gates a merge in CI.
 */
#include "dsp/pixel.h"
#include "dsp/transform.h"
#include "dsp/mc.h"
#include "dsp/predict.h"
#include "dsp/deblock.h"
#include "common/cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#ifdef __APPLE__
#include <pthread.h>
#include <sys/qos.h>
#include "encoder/cabac.h"
#endif

#if defined(__aarch64__) && Y264_BIT_DEPTH == 8
void y264_deblock_luma_v4_neon(pixel *q0, int stride, int bs, int alpha,
                               int beta, int tc0);
void y264_deblock_luma_h4_neon(pixel *q0, int stride, int bs, int alpha,
                               int beta, int tc0);
void y264_deblock_chroma8_h_neon(pixel *q0, int stride, int alpha, int beta,
                                 const uint8_t bs[4], const uint8_t tc0tab[3],
                                 int span, int g);
#endif

#define STRIDE 64
#define PLANE_H 32
#define TRIALS 64
#define BENCH_ITERS 200000
#define BENCH_REPS 4            /* best-of reps: absorbs DVFS ramp + scheduler noise */

static uint64_t rng = 0x2545F4914F6CDD1DULL;
static pixel rnd8(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (pixel)(rng >> 24);
}

static void fill_random(pixel *p, int n)
{
    /* Full sample range for the build's bit depth. rnd8() alone left a 10-bit
     * build exercising a quarter of the range, which is where a clip bug
     * hides. */
    for (int i = 0; i < n; i++)
#if Y264_BIT_DEPTH > 8
        p[i] = (pixel)(((rnd8() << 8) | rnd8()) & PIXEL_MAX);
#else
        p[i] = rnd8();
#endif
}

#if defined(__aarch64__) && Y264_BIT_DEPTH == 8
/* Definitional per-line chroma edge filter, the shape deblock.c ran before
 * the whole-edge kernel: eight lines, one call each. The bench's `ref`. */
static void chroma_edge_lines_c(pixel *q0p, int st, int ls, int alpha, int beta,
                                const uint8_t bs[4], const uint8_t tc0tab[3],
                                int span)
{
    for (int i = 0; i < 8; i++) {
        int b = bs[i / span];
        if (!b) continue;
        int tc0 = tc0tab[b < 4 ? b - 1 : 0];
        pixel *q0 = q0p + i * ls;
        int p0 = q0[-st], p1 = q0[-2*st], Q0 = q0[0], Q1 = q0[st];
        if (abs(p0-Q0) >= alpha || abs(p1-p0) >= beta || abs(Q1-Q0) >= beta)
            continue;
        if (b < 4) {
            int tc = tc0 + 1;
            int d = ((Q0-p0)*4 + (p1-Q1) + 4) >> 3;
            d = d < -tc ? -tc : d > tc ? tc : d;
            int p0n = p0 + d, q0n = Q0 - d;
            q0[-st] = (pixel)(p0n < 0 ? 0 : p0n > 255 ? 255 : p0n);
            q0[0]   = (pixel)(q0n < 0 ? 0 : q0n > 255 ? 255 : q0n);
        } else {
            q0[-st] = (pixel)((2*p1 + p0 + Q1 + 2) >> 2);
            q0[0]   = (pixel)((2*Q1 + Q0 + p1 + 2) >> 2);
        }
    }
}
#endif

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* Pin the bench to a performance core at ramped frequency: without the QoS
 * boost macOS may run the process on an efficiency core / low DVFS state,
 * which measured our SAD kernels 4x slower than their true P-core speed (and
 * inverted before/after comparisons). The warm-up spin ramps the clock. */
static void bench_prep(void)
{
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    volatile uint64_t x = 1;
    uint64_t t0 = now_ns();
    while (now_ns() - t0 < 300000000ull)    /* ~300ms spin */
        x = x * 6364136223846793005ull + 1442695040888963407ull;
}

/* Best-of-BENCH_REPS timing of `fn(a, sa, b, sb)` over BENCH_ITERS calls. */
typedef int (*bench2_fn)(const pixel *, int, const pixel *, int);
static double bench2(bench2_fn fn, const pixel *a, const pixel *b)
{
    double best = 1e30;
    volatile int sink = 0;
    for (int r = 0; r < BENCH_REPS; r++) {
        uint64_t t0 = now_ns();
        for (int i = 0; i < BENCH_ITERS; i++)
            sink += fn(a, STRIDE, b, STRIDE);
        uint64_t t1 = now_ns();
        double ns = (double)(t1 - t0) / BENCH_ITERS;
        if (ns < best) best = ns;
    }
    (void)sink;
    return best;
}

int main(int argc, char **argv)
{
    int do_bench = (argc > 1 && strcmp(argv[1], "--bench") == 0);

    uint32_t cpu = y264_cpu_detect();
    char name[128];
    y264_cpu_name(cpu, name, sizeof(name));
    printf("checkasm: cpu features: %s\n", name);

    y264_pixel_fn_t ref, opt;
    y264_pixel_init_c(&ref);
    y264_pixel_init(cpu, &opt);
    /* The composed NEON satd8x8/16x16 dispatch through the global y264_dsp table,
 * so it must be initialized before those kernels are called. */
    y264_dsp_init();

    pixel *a = malloc(STRIDE * PLANE_H);
    pixel *b = malloc(STRIDE * PLANE_H);
    if (!a || !b)
        return 2;

    int failures = 0;

    /* SATD 4x4: dispatched kernel must match the C reference exactly. */
    {
        int mism = 0;
        for (int t = 0; t < TRIALS; t++) {
            fill_random(a, STRIDE * PLANE_H);
            fill_random(b, STRIDE * PLANE_H);
            if (t == 0) memcpy(b, a, STRIDE * PLANE_H);
            int r = ref.satd4x4(a, STRIDE, b, STRIDE);
            int o = opt.satd4x4(a, STRIDE, b, STRIDE);
            if (r != o) { if (!mism) printf("  FAIL satd4x4: ref=%d opt=%d\n", r, o); mism++; }
        }
        if (mism) failures++;
        else printf("  ok   satd4x4 (%d trials)\n", TRIALS);
    }

    /* SATD 8x8/16x16 + SA8D: dispatched kernel must match the C reference
 * exactly (the NEON max-identity and butterfly reorderings are exact). */
    {
        struct { const char *n; y264_satd_fn r, o; } sk[] = {
            { "satd8x8",   ref.satd8x8,   opt.satd8x8   },
            { "satd16x16", ref.satd16x16, opt.satd16x16 },
            { "sa8d8x8",   ref.sa8d8x8,   opt.sa8d8x8   },
            { "sa8d16x16", ref.sa8d16x16, opt.sa8d16x16 },
        };
        for (int k = 0; k < 4; k++) {
            int mism = 0;
            for (int t = 0; t < TRIALS; t++) {
                fill_random(a, STRIDE * PLANE_H);
                fill_random(b, STRIDE * PLANE_H);
                if (t == 0) memcpy(b, a, STRIDE * PLANE_H);
                if (t == 1) { memset(a, 0, STRIDE * PLANE_H);
                              memset(b, 255, STRIDE * PLANE_H); }
                int r = sk[k].r(a, STRIDE, b, STRIDE);
                int o = sk[k].o(a, STRIDE, b, STRIDE);
                if (r != o) { if (!mism) printf("  FAIL %s: ref=%d opt=%d\n", sk[k].n, r, o); mism++; }
            }
            if (mism) failures++;
            else printf("  ok   %-9s (%d trials)\n", sk[k].n, TRIALS);
        }
    }

    /* hadamard_ac (single-plane psy texture term). */
    {
        int mism = 0;
        for (int t = 0; t < TRIALS; t++) {
            fill_random(a, STRIDE * PLANE_H);
            if (t == 0) memset(a, 0, STRIDE * PLANE_H);
            if (t == 1) memset(a, 255, STRIDE * PLANE_H);
            long r = ref.hadamard_ac8x8(a, STRIDE);
            long o = opt.hadamard_ac8x8(a, STRIDE);
            if (r != o) { if (!mism) printf("  FAIL hadamard_ac8x8: ref=%ld opt=%ld\n", r, o); mism++; }
        }
        if (mism) failures++;
        else printf("  ok   hadamard_ac8x8 (%d trials)\n", TRIALS);
    }

    /* texture_ac4_16x16 (psy texture term, SATD support). Flat and saturated
 * planes exercise the rounded-mean DC correction at both extremes. */
    {
        int mism = 0;
        for (int t = 0; t < TRIALS; t++) {
            fill_random(a, STRIDE * PLANE_H);
            if (t == 0) memset(a, 0, STRIDE * PLANE_H);
            if (t == 1) memset(a, 255, STRIDE * PLANE_H);
            if (t == 2) for (int i = 0; i < STRIDE * PLANE_H; i++) a[i] = (pixel)(i & 1 ? 8 : 7);
            long r = ref.texture_ac4_16x16(a, STRIDE);
            long o = opt.texture_ac4_16x16(a, STRIDE);
            if (r != o) { if (!mism) printf("  FAIL texture_ac4: ref=%ld opt=%ld\n", r, o); mism++; }
        }
        if (mism) failures++;
        else printf("  ok   texture_ac4 (%d trials, 16x16)\n", TRIALS);
    }

    /* texture_ac48_16x16 (both psy terms in one pass). Checked against the two
 * separate kernels as well as opt-vs-ref: the fused form derives the 8x8
 * coefficients from the 4x4 tiles, and binary noise is the input class that
 * stresses its packed-lane bounds hardest. */
    {
        int mism = 0;
        const int trials = TRIALS * 64;         /* cheap kernel, wide net */
        for (int t = 0; t < trials; t++) {
            if (t & 1) for (int i = 0; i < STRIDE * PLANE_H; i++) a[i] = (pixel)((rnd8() & 1) ? PIXEL_MAX : 0);
            else fill_random(a, STRIDE * PLANE_H);
            if (t == 0) memset(a, 0, STRIDE * PLANE_H);
            if (t == 2) for (int i = 0; i < STRIDE * PLANE_H; i++) a[i] = (pixel)(i & 1 ? 8 : 7);
            if (t == 4) for (int i = 0; i < STRIDE * PLANE_H; i++) a[i] = PIXEL_MAX;
            long ro[2], oo[2];
            ref.texture_ac48_16x16(a, STRIDE, ro);
            opt.texture_ac48_16x16(a, STRIDE, oo);
            long sep4 = ref.texture_ac4_16x16(a, STRIDE), sep8 = 0;
            for (int by = 0; by < 16; by += 8)
                for (int bx = 0; bx < 16; bx += 8)
                    sep8 += ref.hadamard_ac8x8(a + by * STRIDE + bx, STRIDE);
            if (ro[0] != oo[0] || ro[1] != oo[1] || ro[0] != sep4 || ro[1] != sep8) {
                if (!mism) printf("  FAIL texture_ac48: ref=%ld/%ld opt=%ld/%ld split=%ld/%ld\n",
                                  ro[0], ro[1], oo[0], oo[1], sep4, sep8);
                mism++;
            }
        }
        if (mism) failures++;
        else printf("  ok   texture_ac48 (%d trials, 16x16, vs split kernels)\n", trials);
    }

    /* var16x16 (pixel sum + sum of squares, the AQ / mb-tree variance grid). */
    {
        int mism = 0;
        for (int t = 0; t < TRIALS; t++) {
            fill_random(a, STRIDE * PLANE_H);
            if (t == 0) memset(a, 0, STRIDE * PLANE_H);
            if (t == 1) memset(a, PIXEL_MAX, STRIDE * PLANE_H);
            uint32_t r[2], o[2];
            ref.var16x16(a, STRIDE, r);
            opt.var16x16(a, STRIDE, o);
            if (r[0] != o[0] || r[1] != o[1]) {
                if (!mism) printf("  FAIL var16x16: ref=%u/%u opt=%u/%u\n", r[0], r[1], o[0], o[1]);
                mism++;
            }
        }
        if (mism) failures++;
        else printf("  ok   var16x16 (%d trials)\n", TRIALS);
    }

    /* intra4x4_x9 (all nine Intra4x4 mode costs in one pass). Every mode is
 * checked under every availability combination -- including the ones the
 * encoder's gate forbids, since the kernel computes them anyway and a
 * wrong value there would be a latent trap for any future caller. */
    {
        enum { PORG = 8 * STRIDE + 16 };
        int mism = 0;
        for (int t = 0; t < TRIALS; t++) {
            fill_random(a, STRIDE * PLANE_H);
            fill_random(b, STRIDE * PLANE_H);
            if (t == 0) memset(a, 0, STRIDE * PLANE_H);
            if (t == 1) { memset(a, 0, STRIDE * PLANE_H); memset(b, PIXEL_MAX, STRIDE * PLANE_H); }
            const pixel *rc = a + PORG;
            for (int ht = 0; ht <= 1; ht++)
                for (int hl = 0; hl <= 1; hl++)
                    for (int htl = 0; htl <= (ht && hl); htl++)
                        for (int htr = 0; htr <= ht; htr++) {
                            int rcost[9], ocost[9];
                            ref.intra4x4_x9(b, STRIDE, rc, STRIDE, ht, hl, htl, htr, rcost);
                            opt.intra4x4_x9(b, STRIDE, rc, STRIDE, ht, hl, htl, htr, ocost);
                            for (int m = 0; m < 9; m++)
                                if (rcost[m] != ocost[m]) {
                                    if (!mism)
                                        printf("  FAIL intra4x4_x9: mode %d avail %d%d%d%d ref=%d opt=%d\n",
                                               m, ht, hl, htl, htr, rcost[m], ocost[m]);
                                    mism++;
                                }
                        }
        }
        if (mism) failures++;
        else printf("  ok   intra4x4_x9 (%d trials, 9 modes x all avail)\n", TRIALS);
    }

    /* intra_satd_x3_16x16 (V/H/DC I16x16 costs in one pass). The DC value is
 * swept over the whole pixel range as well as the derivations the encoder
 * actually passes, since the kernel takes it as an input. */
    {
        int mism = 0;
        for (int t = 0; t < TRIALS; t++) {
            fill_random(a, STRIDE * PLANE_H);
            fill_random(b, STRIDE * PLANE_H);
            if (t == 0) { memset(a, 0, STRIDE * PLANE_H); memset(b, PIXEL_MAX, STRIDE * PLANE_H); }
            if (t == 1) { memset(a, PIXEL_MAX, STRIDE * PLANE_H); memset(b, 0, STRIDE * PLANE_H); }
            const pixel *top = a + 3 * STRIDE + 5, *left = a + 9 * STRIDE + 1;
            static const int dcs[] = { 0, 1, 128, PIXEL_MAX };
            for (unsigned k = 0; k < sizeof(dcs)/sizeof(dcs[0]); k++) {
                int rcost[3], ocost[3];
                ref.intra_satd_x3_16(b, STRIDE, top, left, dcs[k], rcost);
                opt.intra_satd_x3_16(b, STRIDE, top, left, dcs[k], ocost);
                for (int m = 0; m < 3; m++)
                    if (rcost[m] != ocost[m]) {
                        if (!mism)
                            printf("  FAIL intra_satd_x3_16: mode %d dc %d ref=%d opt=%d\n",
                                   m, dcs[k], rcost[m], ocost[m]);
                        mism++;
                    }
            }
        }
        if (mism) failures++;
        else printf("  ok   intra_satd_x3_16 (%d trials, 3 modes x 4 DCs)\n", TRIALS);
    }

    for (int pu = 0; pu < Y264_PU_COUNT; pu++) {
        int mism = 0;
        for (int t = 0; t < TRIALS; t++) {
            fill_random(a, STRIDE * PLANE_H);
            fill_random(b, STRIDE * PLANE_H);
            /* Cover the corner cases the randomizer rarely hits. */
            if (t == 0) memcpy(b, a, STRIDE * PLANE_H);          /* identical -> 0 */
            if (t == 1) { memset(a, 0, STRIDE * PLANE_H);
                          memset(b, 255, STRIDE * PLANE_H); }    /* max diff */
            int r = ref.sad[pu](a, STRIDE, b, STRIDE);
            int o = opt.sad[pu](a, STRIDE, b, STRIDE);
            if (r != o) {
                if (!mism)
                    printf("  FAIL sad_%s: ref=%d opt=%d\n",
                           y264_pu_name[pu], r, o);
                mism++;
            }
        }
        if (mism) {
            failures++;
        } else {
            printf("  ok   sad_%-5s (%d trials)\n", y264_pu_name[pu], TRIALS);
        }
    }

    /* Batched sad_x4 vs four single reference SADs, at four distinct
 * candidate offsets within the plane. */
    for (int pu = 0; pu < Y264_PU_COUNT; pu++) {
        int mism = 0;
        for (int t = 0; t < TRIALS; t++) {
            fill_random(a, STRIDE * PLANE_H);
            fill_random(b, STRIDE * PLANE_H);
            if (t == 0) memcpy(b, a, STRIDE * PLANE_H);
            const pixel *r0 = b, *r1 = b + 1, *r2 = b + STRIDE, *r3 = b + STRIDE + 3;
            int s[4];
            opt.sad_x4[pu](a, STRIDE, r0, r1, r2, r3, STRIDE, s);
            int e0 = ref.sad[pu](a, STRIDE, r0, STRIDE);
            int e1 = ref.sad[pu](a, STRIDE, r1, STRIDE);
            int e2 = ref.sad[pu](a, STRIDE, r2, STRIDE);
            int e3 = ref.sad[pu](a, STRIDE, r3, STRIDE);
            if (s[0] != e0 || s[1] != e1 || s[2] != e2 || s[3] != e3) {
                if (!mism)
                    printf("  FAIL sad_x4_%s: {%d,%d,%d,%d} vs {%d,%d,%d,%d}\n",
                           y264_pu_name[pu], s[0], s[1], s[2], s[3], e0, e1, e2, e3);
                mism++;
            }
        }
        if (mism) failures++;
        else printf("  ok   sad_x4_%-5s (%d trials)\n", y264_pu_name[pu], TRIALS);
    }

    /* Batched satd_x4_8x8 vs four single reference SATDs. Gated against
 * ref.satd8x8, not against opt.satd8x8: the claim the encoder relies on is
 * that batching is byte-identical to the scalar C metric, and comparing two
 * NEON kernels to each other would not test that. */
    {
        int mism = 0;
        for (int t = 0; t < TRIALS; t++) {
            fill_random(a, STRIDE * PLANE_H);
            fill_random(b, STRIDE * PLANE_H);
            if (t == 0) memcpy(b, a, STRIDE * PLANE_H);     /* the all-zero residual */
            const pixel *r0 = b, *r1 = b + 1, *r2 = b + STRIDE, *r3 = b + STRIDE + 3;
            int s[4];
            opt.satd_x4_8x8(a, STRIDE, r0, r1, r2, r3, STRIDE, s);
            int e0 = ref.satd8x8(a, STRIDE, r0, STRIDE);
            int e1 = ref.satd8x8(a, STRIDE, r1, STRIDE);
            int e2 = ref.satd8x8(a, STRIDE, r2, STRIDE);
            int e3 = ref.satd8x8(a, STRIDE, r3, STRIDE);
            if (s[0] != e0 || s[1] != e1 || s[2] != e2 || s[3] != e3) {
                if (!mism)
                    printf("  FAIL satd_x4_8x8: {%d,%d,%d,%d} vs {%d,%d,%d,%d}\n",
                           s[0], s[1], s[2], s[3], e0, e1, e2, e3);
                mism++;
            }
        }
        if (mism) failures++;
        else printf("  ok   satd_x4_8x8 (%d trials)\n", TRIALS);
    }

    /* Transforms + quant: the public entry points dispatch (NEON when
 * detected); compare against the _c references. Forward-dct inputs stay
 * in the pixel-diff domain [-255,255] (the NEON exactness domain and the
 * only domain the encoder feeds); idct gets full-range int16 coefs, and
 * quant compares the !w fast path against the w=flat-16 scalar path
 * (identical multipliers by construction: (16*mf+8)/16 == mf). */
    {
        dctcoef in4[16], o1[16], o2[16], in8[64], p1[64], p2[64];
        uint8_t w16[64]; memset(w16, 16, sizeof(w16));
        int mism4f = 0, mism4i = 0, mism8f = 0, mism8i = 0;
        int mq4 = 0, mq8 = 0, mq4f = 0, mq8f = 0;
        for (int t = 0; t < TRIALS * 4; t++) {
            int qp = (int)(rng % 52); rnd8();
            int intra = t & 1;
            for (int i = 0; i < 16; i++) in4[i] = (dctcoef)((int)rnd8() - (int)rnd8());
            y264_fdct4x4(in4, o1); y264_fdct4x4_c(in4, o2);
            if (memcmp(o1, o2, sizeof(o1))) mism4f++;
            for (int i = 0; i < 16; i++) in4[i] = (dctcoef)((int16_t)(rnd8() | ((uint16_t)rnd8() << 8)));
            y264_idct4x4(in4, o1); y264_idct4x4_c(in4, o2);
            if (memcmp(o1, o2, sizeof(o1))) mism4i++;
            y264_quant_4x4(in4, o1, qp, intra, NULL);
            y264_quant_4x4(in4, o2, qp, intra, w16);
            if (memcmp(o1, o2, sizeof(o1))) mq4++;
            y264_quant_4x4_f64(in4, o1, qp, 32, NULL);
            y264_quant_4x4_f64(in4, o2, qp, 32, w16);
            if (memcmp(o1, o2, sizeof(o1))) mq4f++;
            for (int i = 0; i < 64; i++) in8[i] = (dctcoef)((int)rnd8() - (int)rnd8());
            y264_fdct8x8(in8, p1); y264_fdct8x8_c(in8, p2);
            if (memcmp(p1, p2, sizeof(p1))) mism8f++;
            for (int i = 0; i < 64; i++) in8[i] = (dctcoef)((int16_t)(rnd8() | ((uint16_t)rnd8() << 8)));
            y264_idct8x8(in8, p1); y264_idct8x8_c(in8, p2);
            if (memcmp(p1, p2, sizeof(p1))) mism8i++;
            y264_quant_8x8(in8, p1, qp, intra, NULL);
            y264_quant_8x8(in8, p2, qp, intra, w16);
            if (memcmp(p1, p2, sizeof(p1))) mq8++;
            y264_quant_8x8_f64(in8, p1, qp, 32, NULL);
            y264_quant_8x8_f64(in8, p2, qp, 32, w16);
            if (memcmp(p1, p2, sizeof(p1))) mq8f++;
        }
        struct { const char *n; int m; } tr[] = {
            { "fdct4x4", mism4f }, { "idct4x4", mism4i },
            { "fdct8x8", mism8f }, { "idct8x8", mism8i },
            { "quant_4x4", mq4 }, { "quant_4x4_f64", mq4f },
            { "quant_8x8", mq8 }, { "quant_8x8_f64", mq8f },
        };
        for (unsigned k = 0; k < sizeof(tr)/sizeof(tr[0]); k++) {
            if (tr[k].m) { printf("  FAIL %s: %d mismatching trials\n", tr[k].n, tr[k].m); failures++; }
            else printf("  ok   %-13s (%d trials)\n", tr[k].n, TRIALS * 4);
        }
    }

#if defined(__aarch64__) && Y264_BIT_DEPTH == 8
    /* SSD 16xh / 8xh: the NEON kernels against a scalar loop (they are
 * dispatched from the macroblock code, not the pixel table). */
    if (y264_asm_on(Y264_ASM_SSD)) {
        int mism = 0;
        for (int t = 0; t < TRIALS; t++) {
            fill_random(a, STRIDE * 16); fill_random(b, STRIDE * 16);
            for (int w = 8; w <= 16; w += 8) for (int h = 4; h <= 16; h += 4) {
                int r = 0;
                for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                    int d = a[y * STRIDE + x] - b[y * STRIDE + x]; r += d * d; }
                int o = w == 16 ? y264_ssd_16xh_neon(a, STRIDE, b, STRIDE, h)
                                : y264_ssd_8xh_neon(a, STRIDE, b, STRIDE, h);
                if (r != o) { if (!mism) printf("  FAIL ssd_%dx%d: ref=%d opt=%d\n", w, h, r, o); mism++; }
            }
        }
        if (mism) { printf("  FAIL ssd (%d mismatches)\n", mism); failures++; }
        else printf("  ok   ssd 16xh/8xh (%d trials)\n", TRIALS);
    }
    /* Dequant 4x4 / 8x8: the dispatched kernel (ablation bit on) against the
 * C path (bit off) through the public entry points, flat matrix, every qp. */
    if (y264_asm_on(Y264_ASM_QUANT)) {
        int mism = 0;
        for (int t = 0; t < TRIALS; t++) {
            dctcoef lev4[16], c4a[16], c4b[16], lev8[64], c8a[64], c8b[64];
            /* Levels bounded so the dequantised coefficient stays inside 16
 * bits (the largest flat scale is 45, at the 8x8 positions with qp%6 == 5, times 2^(qp/6)):
 * beyond that the C path wraps and the NEON path saturates, and no
 * conforming stream carries such a level. */
            int qp = t % 52;
            int lim = 32767 / (45 << (qp / 6)); if (lim > 4095) lim = 4095; if (lim < 1) lim = 1;
            for (int k = 0; k < 16; k++) lev4[k] = (dctcoef)(((int)rnd8() * 33 + (int)rnd8()) % (2 * lim + 1) - lim);
            for (int k = 0; k < 64; k++) lev8[k] = (dctcoef)(((int)rnd8() * 33 + (int)rnd8()) % (2 * lim + 1) - lim);
            y264_asm_off_ |= Y264_ASM_QUANT;
            y264_dequant_4x4(lev4, c4a, qp, NULL); y264_dequant_8x8(lev8, c8a, qp, NULL);
            y264_asm_off_ &= ~Y264_ASM_QUANT;
            y264_dequant_4x4(lev4, c4b, qp, NULL); y264_dequant_8x8(lev8, c8b, qp, NULL);
            if (memcmp(c4a, c4b, sizeof c4a)) {
                if (!mism) { for (int k = 0; k < 16; k++) if (c4a[k] != c4b[k]) { printf("  FAIL dequant_4x4 qp=%d k=%d lev=%d c=%d neon=%d\n", qp, k, (int)lev4[k], (int)c4a[k], (int)c4b[k]); break; } }
                mism++;
            }
            if (memcmp(c8a, c8b, sizeof c8a)) { if (!mism) printf("  FAIL dequant_8x8 qp=%d\n", qp); mism++; }
        }
        if (mism) { printf("  FAIL dequant (%d mismatches)\n", mism); failures++; }
        else printf("  ok   dequant 4x4/8x8 (%d trials)\n", TRIALS);
    }
#endif

    /* Fused sub-dct / add-idct: dispatched vs the _c references, on pixel
 * blocks at the harness strides. Forward inputs are pixels (the exactness
 * domain); the inverse gets full-range int16 coefficients (recon path --
 * the add+clip must match the scalar truncate-then-clip exactly, including
 * the int16-overflow corner the saturating add covers). */
    {
        dctcoef c1[64], c2[64];
        pixel d1[64], d2[64];
        int msf4 = 0, msi4 = 0, msf8 = 0, msi8 = 0;
        for (int t = 0; t < TRIALS * 4; t++) {
            fill_random(a, STRIDE * PLANE_H);
            fill_random(b, STRIDE * PLANE_H);
            y264_sub4x4_dct(c1, a, STRIDE, b, STRIDE);
            y264_sub4x4_dct_c(c2, a, STRIDE, b, STRIDE);
            if (memcmp(c1, c2, 16 * sizeof(dctcoef))) msf4++;
            y264_sub8x8_dct8(c1, a, STRIDE, b, STRIDE);
            y264_sub8x8_dct8_c(c2, a, STRIDE, b, STRIDE);
            if (memcmp(c1, c2, 64 * sizeof(dctcoef))) msf8++;
            for (int i = 0; i < 64; i++)
                c1[i] = (dctcoef)((int16_t)(rnd8() | ((uint16_t)rnd8() << 8)));
            if (t == 0) { c1[0] = 32767; c1[1] = -32768; }  /* saturation corner */
            memset(d1, 0, sizeof(d1)); memset(d2, 0, sizeof(d2));
            y264_add4x4_idct(d1, 8, b, STRIDE, c1);
            y264_add4x4_idct_c(d2, 8, b, STRIDE, c1);
            if (memcmp(d1, d2, sizeof(d1))) msi4++;
            memset(d1, 0, sizeof(d1)); memset(d2, 0, sizeof(d2));
            y264_add8x8_idct8(d1, 8, b, STRIDE, c1);
            y264_add8x8_idct8_c(d2, 8, b, STRIDE, c1);
            if (memcmp(d1, d2, sizeof(d1))) msi8++;
        }
        /* Batched forward transform: every grid shape the encoder asks for
 * (16x16 luma, 4:2:0 and 4:2:2 chroma), against the per-block C. */
        int msb = 0;
        {
            static const struct { int w, h; } grid[] = { { 4, 4 }, { 2, 2 }, { 2, 4 } };
            dctcoef g1[16][16], g2[16][16];
            for (int t = 0; t < TRIALS * 4; t++) {
                fill_random(a, STRIDE * PLANE_H);
                fill_random(b, STRIDE * PLANE_H);
                for (unsigned k = 0; k < sizeof(grid)/sizeof(grid[0]); k++) {
                    int n = grid[k].w * grid[k].h;
                    memset(g1, 0x5a, sizeof(g1)); memset(g2, 0xa5, sizeof(g2));
                    y264_sub_dct4_blocks(g1, grid[k].w, grid[k].h, a, STRIDE, b, STRIDE);
                    y264_sub_dct4_blocks_c(g2, grid[k].w, grid[k].h, a, STRIDE, b, STRIDE);
                    if (memcmp(g1, g2, n * 16 * sizeof(dctcoef))) msb++;
                }
            }
        }
        /* Zig-zag scan kernels. Levels are drawn small and sparse, the shape
 * they really have after quant, but the corners matter: -32768 is the
 * value an abs-based |level| >= 2 test would get wrong, and an all-zero
 * / all-big block pins both ends of the mask and the big flag. */
        int mszb = 0, msz8 = 0, msm8 = 0;
        {
            dctcoef z4[16], z8[64], o1[64], o2[64];
            int i1[64], i2[64];
            for (int t = 0; t < TRIALS * 4; t++) {
                for (int i = 0; i < 64; i++) {
                    unsigned r = rnd8();
                    z8[i] = (dctcoef)((r & 3) ? 0 : (int)(rnd8()) - 128);
                }
                if (t == 0) memset(z8, 0, sizeof(z8));
                if (t == 1) for (int i = 0; i < 64; i++) z8[i] = -32768;
                if (t == 2) for (int i = 0; i < 64; i++) z8[i] = (dctcoef)(i & 1 ? 1 : -1);
                if (t == 3) for (int i = 0; i < 64; i++) z8[i] = 32767;
                for (int i = 0; i < 16; i++) z4[i] = z8[i];

                memset(i1, 0x5a, sizeof(i1)); memset(i2, 0xa5, sizeof(i2));
                y264_zigzag_abs_8x8(i1, z8); y264_zigzag_abs_8x8_c(i2, z8);
                if (memcmp(i1, i2, 64 * sizeof(int))) msz8++;

                uint64_t k1, k2; int b1, b2;
                y264_scan_mask_8x8(z8, &k1, &b1); y264_scan_mask_8x8_c(z8, &k2, &b2);
                if (k1 != k2 || b1 != b2) msm8++;

                uint32_t m1, m2;
                memset(o1, 0x5a, sizeof(o1)); memset(o2, 0xa5, sizeof(o2));
                y264_zigzag_scan_4x4(o1, z4, &m1, &b1);
                y264_zigzag_scan_4x4_c(o2, z4, &m2, &b2);
                if (m1 != m2 || b1 != b2 || memcmp(o1, o2, 16 * sizeof(dctcoef))) mszb++;
            }
        }
        struct { const char *n; int m; } fu[] = {
            { "sub4x4_dct", msf4 }, { "add4x4_idct", msi4 },
            { "sub8x8_dct8", msf8 }, { "add8x8_idct8", msi8 },
            { "sub_dct4_blocks", msb },
            { "zigzag_abs_8x8", msz8 },
            { "scan_mask_8x8", msm8 }, { "zigzag_scan_4x4", mszb },
        };
        for (unsigned k = 0; k < sizeof(fu)/sizeof(fu[0]); k++) {
            if (fu[k].m) { printf("  FAIL %s: %d mismatching trials\n", fu[k].n, fu[k].m); failures++; }
            else printf("  ok   %-13s (%d trials)\n", fu[k].n, TRIALS * 4);
        }
    }

    /* Deblock luma 4-line edge kernels vs a definitional scalar filter_line
 * (spec 8.7.2.3/8.7.2.4, mirroring deblock.c). Random content is biased
 * toward small cross-edge deltas so every branch (filt on/off, ap/aq,
 * strong/weak) is exercised; alpha/beta/tc0 sweep the real table range. */
#if defined(__aarch64__) && Y264_BIT_DEPTH == 8
    {
        static const uint8_t A_[52] = {
            0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,4,5,6,7,8,9,10,12,13,
            15,17,20,22,25,28,32,36,40,45,50,56,63,71,80,90,101,113,
            127,144,162,182,203,226,255,255 };
        static const uint8_t B_[52] = {
            0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,3,3,3,3,4,4,4,6,6,
            7,7,8,8,9,9,10,10,11,11,12,12,13,13,14,14,15,15,16,16,17,17,18,18 };
        static const uint8_t TC_[52][3] = {
            {0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},
            {0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},
            {0,0,0},{0,0,1},{0,0,1},{0,0,1},{0,0,1},{0,1,1},{0,1,1},{1,1,1},
            {1,1,1},{1,1,1},{1,1,1},{1,1,2},{1,1,2},{1,1,2},{1,1,2},{1,2,3},
            {1,2,3},{2,2,3},{2,2,4},{2,3,4},{2,3,4},{3,3,5},{3,4,6},{3,4,6},
            {4,5,7},{4,5,8},{4,6,9},{5,7,10},{6,8,11},{6,8,13},{7,10,14},
            {8,11,16},{9,12,18},{10,13,20},{11,15,23},{13,17,25} };
        pixel *pa = malloc(STRIDE * PLANE_H), *pb = malloc(STRIDE * PLANE_H);
        int mismv = 0, mismh = 0;
        for (int t = 0; t < TRIALS * 4 && pa && pb; t++) {
            int qi = 16 + (int)(rng % 36); rnd8();
            int bs = 1 + (int)(rng % 4); rnd8();
            int alpha = A_[qi], beta = B_[qi];
            int tc0 = TC_[qi][bs < 4 ? bs - 1 : 0];
            for (int i = 0; i < STRIDE * PLANE_H; i++) {
                /* random walk keeps neighbour deltas mostly under beta */
                pa[i] = (pixel)(128 + ((int)rnd8() % 32) - 16 +
                                ((t & 1) ? ((int)rnd8() % 200) - 100 : 0));
            }
            memcpy(pb, pa, STRIDE * PLANE_H);
            pixel *edge_a = pa + 8 * STRIDE + 8, *edge_b = pb + 8 * STRIDE + 8;
            /* vertical edge: step 1, lines down the plane */
            for (int ln = 0; ln < 4; ln++) {
                pixel *q0 = edge_a + ln * STRIDE;
                int p0 = q0[-1], p1 = q0[-2], p2 = q0[-3], p3 = q0[-4];
                int Q0 = q0[0], Q1 = q0[1], Q2 = q0[2], Q3 = q0[3];
                if (abs(p0-Q0) >= alpha || abs(p1-p0) >= beta || abs(Q1-Q0) >= beta)
                    continue;
                int ap = abs(p2-p0), aq = abs(Q2-Q0);
                if (bs < 4) {
                    int tc = tc0 + (ap < beta) + (aq < beta);
                    int d = ((Q0-p0)*4 + (p1-Q1) + 4) >> 3;
                    d = d < -tc ? -tc : d > tc ? tc : d;
                    int p0n = p0 + d, q0n = Q0 - d;
                    q0[-1] = (pixel)(p0n < 0 ? 0 : p0n > 255 ? 255 : p0n);
                    q0[0]  = (pixel)(q0n < 0 ? 0 : q0n > 255 ? 255 : q0n);
                    if (ap < beta) {
                        int dl = (p2 + ((p0+Q0+1)>>1) - 2*p1) >> 1;
                        dl = dl < -tc0 ? -tc0 : dl > tc0 ? tc0 : dl;
                        q0[-2] = (pixel)(p1 + dl);
                    }
                    if (aq < beta) {
                        int dl = (Q2 + ((p0+Q0+1)>>1) - 2*Q1) >> 1;
                        dl = dl < -tc0 ? -tc0 : dl > tc0 ? tc0 : dl;
                        q0[1] = (pixel)(Q1 + dl);
                    }
                } else {
                    int strong = abs(p0-Q0) < ((alpha >> 2) + 2);
                    if (ap < beta && strong) {
                        q0[-1] = (pixel)((p2 + 2*p1 + 2*p0 + 2*Q0 + Q1 + 4) >> 3);
                        q0[-2] = (pixel)((p2 + p1 + p0 + Q0 + 2) >> 2);
                        q0[-3] = (pixel)((2*p3 + 3*p2 + p1 + p0 + Q0 + 4) >> 3);
                    } else {
                        q0[-1] = (pixel)((2*p1 + p0 + Q1 + 2) >> 2);
                    }
                    if (aq < beta && strong) {
                        q0[0] = (pixel)((Q2 + 2*Q1 + 2*Q0 + 2*p0 + p1 + 4) >> 3);
                        q0[1] = (pixel)((Q2 + Q1 + Q0 + p0 + 2) >> 2);
                        q0[2] = (pixel)((2*Q3 + 3*Q2 + Q1 + Q0 + p0 + 4) >> 3);
                    } else {
                        q0[0] = (pixel)((2*Q1 + Q0 + p1 + 2) >> 2);
                    }
                }
            }
            y264_deblock_luma_v4_neon(edge_b, STRIDE, bs, alpha, beta, tc0);
            if (memcmp(pa, pb, STRIDE * PLANE_H)) mismv++;
            /* horizontal edge: step STRIDE, lines across; rebuild content */
            memcpy(pb, pa, STRIDE * PLANE_H);
            for (int ln = 0; ln < 4; ln++) {
                pixel *q0 = edge_a + ln;
                const int st = STRIDE;
                int p0 = q0[-st], p1 = q0[-2*st], p2 = q0[-3*st], p3 = q0[-4*st];
                int Q0 = q0[0], Q1 = q0[st], Q2 = q0[2*st], Q3 = q0[3*st];
                if (abs(p0-Q0) >= alpha || abs(p1-p0) >= beta || abs(Q1-Q0) >= beta)
                    continue;
                int ap = abs(p2-p0), aq = abs(Q2-Q0);
                if (bs < 4) {
                    int tc = tc0 + (ap < beta) + (aq < beta);
                    int d = ((Q0-p0)*4 + (p1-Q1) + 4) >> 3;
                    d = d < -tc ? -tc : d > tc ? tc : d;
                    int p0n = p0 + d, q0n = Q0 - d;
                    q0[-st] = (pixel)(p0n < 0 ? 0 : p0n > 255 ? 255 : p0n);
                    q0[0]   = (pixel)(q0n < 0 ? 0 : q0n > 255 ? 255 : q0n);
                    if (ap < beta) {
                        int dl = (p2 + ((p0+Q0+1)>>1) - 2*p1) >> 1;
                        dl = dl < -tc0 ? -tc0 : dl > tc0 ? tc0 : dl;
                        q0[-2*st] = (pixel)(p1 + dl);
                    }
                    if (aq < beta) {
                        int dl = (Q2 + ((p0+Q0+1)>>1) - 2*Q1) >> 1;
                        dl = dl < -tc0 ? -tc0 : dl > tc0 ? tc0 : dl;
                        q0[st] = (pixel)(Q1 + dl);
                    }
                } else {
                    int strong = abs(p0-Q0) < ((alpha >> 2) + 2);
                    if (ap < beta && strong) {
                        q0[-st]   = (pixel)((p2 + 2*p1 + 2*p0 + 2*Q0 + Q1 + 4) >> 3);
                        q0[-2*st] = (pixel)((p2 + p1 + p0 + Q0 + 2) >> 2);
                        q0[-3*st] = (pixel)((2*p3 + 3*p2 + p1 + p0 + Q0 + 4) >> 3);
                    } else {
                        q0[-st] = (pixel)((2*p1 + p0 + Q1 + 2) >> 2);
                    }
                    if (aq < beta && strong) {
                        q0[0]    = (pixel)((Q2 + 2*Q1 + 2*Q0 + 2*p0 + p1 + 4) >> 3);
                        q0[st]   = (pixel)((Q2 + Q1 + Q0 + p0 + 2) >> 2);
                        q0[2*st] = (pixel)((2*Q3 + 3*Q2 + Q1 + Q0 + p0 + 4) >> 3);
                    } else {
                        q0[0] = (pixel)((2*Q1 + Q0 + p1 + 2) >> 2);
                    }
                }
            }
            y264_deblock_luma_h4_neon(edge_b, STRIDE, bs, alpha, beta, tc0);
            if (memcmp(pa, pb, STRIDE * PLANE_H)) mismh++;
        }
        free(pa); free(pb);
        if (mismv) { printf("  FAIL deblock_luma_v4: %d mismatching trials\n", mismv); failures++; }
        else printf("  ok   deblock_luma_v4 (%d trials)\n", TRIALS * 4);
        if (mismh) { printf("  FAIL deblock_luma_h4: %d mismatching trials\n", mismh); failures++; }
        else printf("  ok   deblock_luma_h4 (%d trials)\n", TRIALS * 4);

        /* Whole-edge chroma filters vs the definitional per-line scalar
 * (chromaStyleFilteringFlag == 1: only p0/q0 move, tc is tc0 + 1). Every
 * bS combination over the four groups is drawn, including all-zero and
 * mixed bS==4, since the kernel's whole point is that the four groups
 * share one pass with per-lane parameters. */
        {
            pixel *pa = malloc(STRIDE * PLANE_H), *pb = malloc(STRIDE * PLANE_H);
            int mismv = 0, mismh = 0;
            for (int t = 0; t < TRIALS * 8 && pa && pb; t++) {
                int qi = 16 + (int)(rng % 36); rnd8();
                int alpha = A_[qi], beta = B_[qi];
                const uint8_t *tc0tab = TC_[qi];
                uint8_t bs[4];
                for (int k = 0; k < 4; k++) { bs[k] = (uint8_t)(rng % 5); rnd8(); }
                if (t == 0) { bs[0] = bs[1] = bs[2] = bs[3] = 4; }
                if (t == 1) { bs[0] = bs[1] = bs[2] = bs[3] = 0; }
                int span = (t & 1) ? 2 : 4;         /* 4:2:0 and 4:2:2 line spans */
                /* span 4 (a 4:2:2 vertical edge) is sixteen lines in two
 * groups; group 1 is where the second pair of bS entries is
 * read, so both groups have to be drawn. */
                int grp = (span == 4) ? (int)((rng >> 5) & 1) : 0; rnd8();
                for (int i = 0; i < STRIDE * PLANE_H; i++)
                    pa[i] = (pixel)(128 + ((int)rnd8() % 32) - 16 +
                                    ((t & 2) ? ((int)rnd8() % 200) - 100 : 0));
                memcpy(pb, pa, STRIDE * PLANE_H);

                for (int dir = 0; dir < 2; dir++) {
                    int st = dir ? STRIDE : 1;      /* stride ACROSS the edge */
                    int ls = dir ? 1 : STRIDE;      /* stride ALONG it */
                    pixel *ea = pa + 8 * STRIDE + 8, *eb = pb + 8 * STRIDE + 8;
                    for (int i = 0; i < 8; i++) {
                        int b = bs[(grp * 8 + i) / span];
                        if (!b) continue;
                        int tc0 = tc0tab[b < 4 ? b - 1 : 0];
                        pixel *q0 = ea + i * ls;
                        int p0 = q0[-st], p1 = q0[-2*st], Q0 = q0[0], Q1 = q0[st];
                        if (abs(p0-Q0) >= alpha || abs(p1-p0) >= beta || abs(Q1-Q0) >= beta)
                            continue;
                        if (b < 4) {
                            int tc = tc0 + 1;
                            int d = ((Q0-p0)*4 + (p1-Q1) + 4) >> 3;
                            d = d < -tc ? -tc : d > tc ? tc : d;
                            int p0n = p0 + d, q0n = Q0 - d;
                            q0[-st] = (pixel)(p0n < 0 ? 0 : p0n > 255 ? 255 : p0n);
                            q0[0]   = (pixel)(q0n < 0 ? 0 : q0n > 255 ? 255 : q0n);
                        } else {
                            q0[-st] = (pixel)((2*p1 + p0 + Q1 + 2) >> 2);
                            q0[0]   = (pixel)((2*Q1 + Q0 + p1 + 2) >> 2);
                        }
                    }
                    if (!dir) { memcpy(pa, pb, STRIDE * PLANE_H); continue; }
                    y264_deblock_chroma8_h_neon(eb, STRIDE, alpha, beta, bs, tc0tab, span, grp);
                    if (memcmp(pa, pb, STRIDE * PLANE_H)) mismh++;
                    memcpy(pb, pa, STRIDE * PLANE_H);
                }
            }
            free(pa); free(pb);
            (void)mismv;
            if (mismh) { printf("  FAIL deblock_chroma8_h: %d mismatching trials\n", mismh); failures++; }
            else printf("  ok   deblock_chroma8_h (%d trials)\n", TRIALS * 8);
        }
    }

#endif

#if defined(__aarch64__) && Y264_BIT_DEPTH == 8
    /* Boundary strengths (8.7.2.1), whole macroblock: dispatched vs C over a
 * random motion/coefficient field. The field is deliberately degenerate --
 * few reference indices, small MVs, mostly-zero nnz -- because a uniformly
 * random one would make almost every edge intra or coded and never reach
 * the reference/MV branches the kernel actually has to get right. Every
 * transform-flag and neighbour-availability combination is exercised. */
    {
        enum { GW = 8, GH = 8, GORG = 2 * GW + 2 };   /* 2 blocks of margin */
        int8_t ref0[GW * GH], ref1[GW * GH], nnz[GW * GH];
        int16_t mx0[GW * GH], my0[GW * GH], mx1[GW * GH], my1[GW * GH];
        int mism = 0;
        for (int t = 0; t < TRIALS; t++) {
            int bslice = t & 1;
            for (int i = 0; i < GW * GH; i++) {
                int intra = (rnd8() % 5) == 0;
                ref0[i] = intra ? -1 : (int8_t)(rnd8() % 3);
                ref1[i] = (intra || !bslice) ? -1 : (int8_t)(rnd8() % 2 ? 0 : -1);
                nnz[i]  = (rnd8() % 4) ? 0 : (int8_t)(1 + rnd8() % 15);
                mx0[i] = (int16_t)(rnd8() % 17 - 8); my0[i] = (int16_t)(rnd8() % 17 - 8);
                mx1[i] = (int16_t)(rnd8() % 17 - 8); my1[i] = (int16_t)(rnd8() % 17 - 8);
            }
            struct y264_bs_ctx c = {
                .ref0 = ref0 + GORG, .ref1 = ref1 + GORG,
                .mvx0 = mx0 + GORG,  .mvy0 = my0 + GORG,
                .mvx1 = mx1 + GORG,  .mvy1 = my1 + GORG,
                .mv_stride = GW,
                .nnz = nnz + GORG,   .nnz_stride = GW,
                .tr8_cur = (uint8_t)(t >> 1 & 1), .tr8_left = (uint8_t)(t >> 2 & 1),
                .tr8_top = (uint8_t)(t >> 3 & 1),
                .have_left = (uint8_t)(t >> 4 & 1), .have_top = (uint8_t)(t >> 5 & 1),
            };
            uint8_t av[4][4], ah[4][4], bv[4][4], bh[4][4];
            memset(av, 0xA5, 16); memset(ah, 0xA5, 16);
            memset(bv, 0x5A, 16); memset(bh, 0x5A, 16);
            y264_deblock_strength_c(&c, av, ah);
            y264_deblock_strength_neon(&c, bv, bh);
            if (memcmp(av, bv, 16) || memcmp(ah, bh, 16)) mism++;
        }
        if (mism) { printf("  FAIL deblock_strength: %d mismatching trials\n", mism); failures++; }
        else printf("  ok   deblock_strength (%d trials)\n", TRIALS);
    }
#endif

    /* Intra prediction builders: dispatched vs _c, every mode x availability
 * combination the encoder can request (mode gating mirrors the encoder's
 * i4_mode_allowed / i16 / chroma mode-set rules; prediction reads recon
 * neighbours from a random plane). */
    {
        enum { PORG = 8 * STRIDE + 16 };
        pixel o1[256], o2[256];
        int m16 = 0, mch = 0, m4 = 0, m8 = 0, m8e = 0;
        for (int t = 0; t < TRIALS; t++) {
            fill_random(a, STRIDE * PLANE_H);
            const pixel *rc = a + PORG;
            for (int ht = 0; ht <= 1; ht++)
                for (int hl = 0; hl <= 1; hl++) {
                    /* I16 + chroma (4:2:0 8x8 and 4:2:2 8x16 geometries) */
                    for (int mode = 0; mode < 4; mode++) {
                        int ok16 = (mode == Y264_I16_DC) ||
                                   (mode == Y264_I16_VERT && ht) ||
                                   (mode == Y264_I16_HORIZ && hl) ||
                                   (mode == Y264_I16_PLANE && ht && hl);
                        if (ok16) {
                            memset(o1, 0, 256); memset(o2, 0, 256);
                            y264_intra16x16(o1, rc, STRIDE, mode, ht, hl);
                            y264_intra16x16_c(o2, rc, STRIDE, mode, ht, hl);
                            if (memcmp(o1, o2, 256)) m16++;
                        }
                        int okc = (mode == Y264_IC_DC) ||
                                  (mode == Y264_IC_VERT && ht) ||
                                  (mode == Y264_IC_HORIZ && hl) ||
                                  (mode == Y264_IC_PLANE && ht && hl);
                        if (okc)
                            for (int chh = 8; chh <= 16; chh += 8) {
                                memset(o1, 0, 256); memset(o2, 0, 256);
                                y264_intra_chroma(o1, rc, STRIDE, mode, ht, hl, 8, chh);
                                y264_intra_chroma_c(o2, rc, STRIDE, mode, ht, hl, 8, chh);
                                if (memcmp(o1, o2, 8 * chh)) mch++;
                            }
                    }
                    /* I4 / I8: all 9 modes under the encoder's availability
 * gate, with all topleft/topright variants. */
                    for (int htl = 0; htl <= (ht && hl); htl++)
                        for (int htr = 0; htr <= ht; htr++)
                            for (int mode = 0; mode < 9; mode++) {
                                int ok;
                                switch (mode) {
                                case Y264_I4_VERT: case Y264_I4_DDL: case Y264_I4_VL:
                                    ok = ht; break;
                                case Y264_I4_HORIZ: case Y264_I4_HU:
                                    ok = hl; break;
                                case Y264_I4_DC: ok = 1; break;
                                default: ok = ht && hl; break;
                                }
                                if (!ok) continue;
                                memset(o1, 0, 64); memset(o2, 0, 64);
                                y264_intra4x4(o1, rc, STRIDE, mode, ht, hl, htl, htr);
                                y264_intra4x4_c(o2, rc, STRIDE, mode, ht, hl, htl, htr);
                                if (memcmp(o1, o2, 16)) m4++;
                                memset(o1, 0, 64); memset(o2, 0, 64);
                                y264_intra8x8(o1, rc, STRIDE, mode, ht, hl, htl, htr);
                                y264_intra8x8_c(o2, rc, STRIDE, mode, ht, hl, htl, htr);
                                if (memcmp(o1, o2, 64)) m8++;
                                /* The decision loop's form: one edge
 * derivation feeding every mode, dispatched
 * and C alike, against the whole-function
 * builder above. */
                                {
                                    y264_i8_edge_t ed;
                                    y264_intra8x8_edge_c(&ed, rc, STRIDE, ht, hl, htl, htr);
                                    memset(o1, 0, 64);
                                    y264_intra8x8_from_edge(o1, &ed, mode, ht, hl);
                                    if (memcmp(o1, o2, 64)) m8e++;
                                    memset(o1, 0, 64);
                                    y264_intra8x8_from_edge_c(o1, &ed, mode, ht, hl);
                                    if (memcmp(o1, o2, 64)) m8e++;
                                }
                            }
                }
        }
        struct { const char *n; int m; } ip[] = {
            { "intra16x16", m16 }, { "intra_chroma", mch },
            { "intra4x4", m4 }, { "intra8x8", m8 },
            { "intra8x8_edge", m8e },
        };
        for (unsigned k = 0; k < sizeof(ip)/sizeof(ip[0]); k++) {
            if (ip[k].m) { printf("  FAIL %s: %d mismatches\n", ip[k].n, ip[k].m); failures++; }
            else printf("  ok   %-13s (%d trials, all modes/avail)\n", ip[k].n, TRIALS);
        }
    }

    /* Chroma MC: dispatched vs C over every partition-derived block shape
 * and chroma format axis split, random in-window MVs (both paths then
 * read the same samples, so equality is exact and unconditional). */
    {
        struct { int w, h; } shp[] = {
            { 8, 8 }, { 8, 4 }, { 8, 16 }, { 4, 8 }, { 4, 4 }, { 4, 2 }, { 2, 4 }, { 2, 2 },
        };
        struct { int sw, sh; } fmt[] = { { 2, 2 }, { 2, 1 }, { 1, 1 } };
        int mism = 0;
        pixel d1[16 * 16], d2[16 * 16];
        for (int t = 0; t < TRIALS; t++) {
            fill_random(a, STRIDE * PLANE_H);
            const pixel *rf = a + 4 * STRIDE + 12;
            int pw = 40, ph = 24;
            for (unsigned si = 0; si < sizeof(shp)/sizeof(shp[0]); si++)
                for (unsigned fi = 0; fi < sizeof(fmt)/sizeof(fmt[0]); fi++) {
                    int mvx = (int)(rng % 33) - 16; rnd8();
                    int mvy = (int)(rng % 33) - 16; rnd8();
                    memset(d1, 1, sizeof(d1)); memset(d2, 2, sizeof(d2));
                    y264_mc_chroma(d1, 16, rf, STRIDE, pw, ph, 8, 8, mvx, mvy,
                                   shp[si].w, shp[si].h, fmt[fi].sw, fmt[fi].sh);
                    y264_mc_chroma_c(d2, 16, rf, STRIDE, pw, ph, 8, 8, mvx, mvy,
                                     shp[si].w, shp[si].h, fmt[fi].sw, fmt[fi].sh);
                    for (int y = 0; y < shp[si].h; y++)
                        if (memcmp(d1 + y * 16, d2 + y * 16, shp[si].w)) { mism++; break; }
                }
        }
        if (mism) { printf("  FAIL mc_chroma: %d mismatching shape trials\n", mism); failures++; }
        else printf("  ok   mc_chroma     (%d trials, 8 shapes x 3 formats)\n", TRIALS);
    }

    /* Bi-prediction weighted average, over every (w0, w1) pair the implicit
 * weight derivation can produce (w1 in [-64, 128], w0 = 64 - w1) plus the
 * unweighted (32, 32) fast path, and every packed block length the
 * encoder passes. The extreme weights are where the int16-lane bound and
 * the saturating narrow are load-bearing. */
    {
        int mism = 0;
        static const int len[] = { 64, 128, 256 };
        pixel d1[256], d2[256];
        for (int t = 0; t < TRIALS; t++) {
            fill_random(a, STRIDE * PLANE_H);
            fill_random(b, STRIDE * PLANE_H);
            if (t == 0) { memset(a, 0, 256); memset(b, PIXEL_MAX, 256); }
            if (t == 1) { memset(a, PIXEL_MAX, 256); memset(b, 0, 256); }
            for (int w1 = -64; w1 <= 128; w1++) {
                int w0 = 64 - w1;
                for (unsigned k = 0; k < sizeof(len)/sizeof(len[0]); k++) {
                    memset(d1, 0x5a, sizeof(d1)); memset(d2, 0xa5, sizeof(d2));
                    y264_pixel_avg_wt(d1, a, b, len[k], w0, w1);
                    y264_pixel_avg_wt_c(d2, a, b, len[k], w0, w1);
                    if (memcmp(d1, d2, len[k])) {
                        if (!mism) printf("  FAIL pixel_avg_wt: w0=%d w1=%d n=%d\n", w0, w1, len[k]);
                        mism++;
                    }
                }
            }
        }
        if (mism) failures++;
        else printf("  ok   pixel_avg_wt (%d trials, 193 weight pairs x 3 lengths)\n", TRIALS);
    }

    /* Half-pel plane fetch: strided copy and 2-tap average, every partition
 * shape the subpel path passes, at odd source strides and odd source
 * offsets so no lane alignment can be assumed. The destination is written
 * past its width to catch a kernel that writes a whole vector into a
 * narrower block. */
    {
        int mism = 0;
        static const int ws[] = { 16, 8, 4 }, hs[] = { 16, 8, 4, 2 };
        pixel d1[16 * 16], d2[16 * 16];
        for (int t = 0; t < TRIALS; t++) {
            fill_random(a, STRIDE * PLANE_H);
            fill_random(b, STRIDE * PLANE_H);
            for (unsigned wi = 0; wi < sizeof(ws)/sizeof(ws[0]); wi++)
                for (unsigned hi = 0; hi < sizeof(hs)/sizeof(hs[0]); hi++) {
                    int w = ws[wi], h = hs[hi];
                    int off = (t * 7) % 5, ss = STRIDE - (t & 1);
                    memset(d1, 0x5a, sizeof(d1)); memset(d2, 0x5a, sizeof(d2));
                    y264_pred_copy(d1, 16, a + off, ss, w, h);
                    y264_pred_copy_c(d2, 16, a + off, ss, w, h);
                    if (memcmp(d1, d2, sizeof(d1))) {
                        if (!mism) printf("  FAIL pred_copy: w=%d h=%d\n", w, h);
                        mism++;
                    }
                    memset(d1, 0x5a, sizeof(d1)); memset(d2, 0x5a, sizeof(d2));
                    y264_pred_avg2(d1, 16, a + off, b + off + 1, ss, w, h);
                    y264_pred_avg2_c(d2, 16, a + off, b + off + 1, ss, w, h);
                    if (memcmp(d1, d2, sizeof(d1))) {
                        if (!mism) printf("  FAIL pred_avg2: w=%d h=%d\n", w, h);
                        mism++;
                    }
                }
        }
        if (mism) failures++;
        else printf("  ok   pred_copy/avg2 (%d trials, 3 widths x 4 heights)\n", TRIALS);
    }

    /* Luma MC, dispatched vs the portable clamped reference, over every
 * partition shape x all sixteen sub-pel phases x positions that force each
 * of the three paths: the NEON window, the in-border C body, and the
 * out-of-window tile gather (a block whose integer position is past the
 * plane's replicated border, which is 55% of the calls on bus_cif). There
 * was NO mc_luma test before this; the border replication below is what
 * makes reading past the frame equal the spec's coordinate clamp, exactly
 * as the encoder's reference planes do. */
    {
        enum { MW = 48, MH = 32, MB = Y264_LUMA_BORDER,
               MST = MW + 2 * MB, MPH = MH + 2 * MB };
        static pixel mbuf[MPH * MST];
        pixel *mref = mbuf + (size_t)MB * MST + MB;
        for (int y = 0; y < MH; y++)
            fill_random(mref + (size_t)y * MST, MW);
        for (int y = 0; y < MH; y++) {
            pixel *row = mref + (size_t)y * MST;
            for (int x = -MB; x < 0; x++) row[x] = row[0];
            for (int x = MW; x < MW + MB; x++) row[x] = row[MW - 1];
        }
        for (int y = -MB; y < 0; y++)
            memcpy(mref + (size_t)y * MST - MB, mref - MB, MST * sizeof(pixel));
        for (int y = MH; y < MH + MB; y++)
            memcpy(mref + (size_t)y * MST - MB,
                   mref + (size_t)(MH - 1) * MST - MB, MST * sizeof(pixel));

        struct { int w, h; } msh[] = {
            { 16, 16 }, { 16, 8 }, { 8, 16 }, { 8, 8 }, { 8, 4 }, { 4, 8 }, { 4, 4 },
        };
        /* integer offsets from the block position, in PIXELS: inside, just
 * inside/outside the border on each axis, and far outside. */
        static const int off[] = { 0, -20, -29, -30, -31, -40, -80, 20, 29, 31, 40, 80 };
        int mism = 0, ntrial = 0;
        pixel d1[16 * 16], d2[16 * 16];
        for (unsigned si = 0; si < sizeof(msh)/sizeof(msh[0]); si++)
            for (int ph_ = 0; ph_ < 16; ph_++)
                for (unsigned ox = 0; ox < sizeof(off)/sizeof(off[0]); ox++)
                    for (unsigned oy = 0; oy < sizeof(off)/sizeof(off[0]); oy++) {
                        int bx = 16, by = 16;
                        int mvx = off[ox] * 4 + (ph_ & 3);
                        int mvy = off[oy] * 4 + (ph_ >> 2);
                        memset(d1, 1, sizeof(d1)); memset(d2, 2, sizeof(d2));
                        y264_mc_luma(d1, 16, mref, MST, MW, MH, bx, by,
                                     mvx, mvy, msh[si].w, msh[si].h);
                        y264_mc_luma_c(d2, 16, mref, MST, MW, MH, bx, by,
                                       mvx, mvy, msh[si].w, msh[si].h);
                        ntrial++;
                        for (int y = 0; y < msh[si].h; y++)
                            if (memcmp(d1 + y * 16, d2 + y * 16,
                                       (size_t)msh[si].w * sizeof(pixel))) { mism++; break; }
                    }
        if (mism) { printf("  FAIL mc_luma: %d/%d mismatching trials\n", mism, ntrial); failures++; }
        else printf("  ok   mc_luma       (%d trials, 7 shapes x 16 phases x 144 positions)\n", ntrial);
    }

    /* hpel plane build (dispatched, NEON rows when detected) against the
 * definitional clamped 6-tap formulas -- H/V/C at every bordered
 * position. Recon-path: must be exact. */
    {
        /* The builder's near-taps read past the frame unclamped: like the
 * encoder's ref planes, the source must carry edge-REPLICATED borders
 * (replication == the spec's coordinate clamp). */
        enum { HPW = 64, HPH = 48, HB = 8, HPAD = HB + 8,
               HRST = HPW + 2 * HPAD, HST = HPW + 2 * HB };
        static pixel refbuf[(HPH + 2 * HPAD) * HRST];
        static pixel Hp[(HPH + 2 * HB) * HST], Vp[(HPH + 2 * HB) * HST], Cp[(HPH + 2 * HB) * HST];
        static int32_t scratch[HST * (HPH + 2 * HB + 5)];
        pixel *refp = refbuf + (size_t)HPAD * HRST + HPAD;
        for (int y = 0; y < HPH; y++)
            fill_random(refp + (size_t)y * HRST, HPW);
        for (int y = 0; y < HPH; y++) {          /* left/right replication */
            pixel *row = refp + (size_t)y * HRST;
            for (int x = -HPAD; x < 0; x++) row[x] = row[0];
            for (int x = HPW; x < HPW + HPAD; x++) row[x] = row[HPW - 1];
        }
        for (int y = -HPAD; y < 0; y++)          /* top/bottom replication */
            memcpy(refp + (size_t)y * HRST - HPAD, refp - HPAD, HRST * sizeof(pixel));
        for (int y = HPH; y < HPH + HPAD; y++)
            memcpy(refp + (size_t)y * HRST - HPAD,
                   refp + (size_t)(HPH - 1) * HRST - HPAD, HRST * sizeof(pixel));
        size_t org = (size_t)HB * HST + HB;
        y264_mc_build_hpel(Hp + org, Vp + org, Cp + org, HST,
                           refp, HRST, HPW, HPH, HB, scratch, HST);
        int mism = 0;
#define HCL(v, n) ((v) < 0 ? 0 : (v) >= (n) ? (n) - 1 : (v))
#define HR(x, y) refp[(size_t)HCL(y, HPH) * HRST + HCL(x, HPW)]
#define HT6(a, b, c, d, e, f) ((a) - 5*(b) + 20*(c) + 20*(d) - 5*(e) + (f))
#define HCLIP(v) ((v) < 0 ? 0 : (v) > PIXEL_MAX ? PIXEL_MAX : (v))
        for (int y = -HB; y < HPH + HB && mism < 4; y++)
            for (int x = -HB; x < HPW + HB; x++) {
                int hv = HCLIP((HT6(HR(x-2,y), HR(x-1,y), HR(x,y), HR(x+1,y), HR(x+2,y), HR(x+3,y)) + 16) >> 5);
                int vv = HCLIP((HT6(HR(x,y-2), HR(x,y-1), HR(x,y), HR(x,y+1), HR(x,y+2), HR(x,y+3)) + 16) >> 5);
                int cc[6];
                for (int r = -2; r <= 3; r++)
                    cc[r+2] = HT6(HR(x-2,y+r), HR(x-1,y+r), HR(x,y+r), HR(x+1,y+r), HR(x+2,y+r), HR(x+3,y+r));
                int cv = HCLIP((HT6(cc[0], cc[1], cc[2], cc[3], cc[4], cc[5]) + 512) >> 10);
                size_t o = org + (size_t)y * HST + x;
                if (Hp[o] != hv || Vp[o] != vv || Cp[o] != cv) {
                    if (!mism)
                        printf("  FAIL hpel_build at (%d,%d): H %d/%d V %d/%d C %d/%d\n",
                               x, y, Hp[o], hv, Vp[o], vv, Cp[o], cv);
                    mism++;
                }
            }
#undef HCL
#undef HR
#undef HT6
#undef HCLIP
        if (mism) failures++;
        else printf("  ok   hpel_build    (%dx%d +%d border, all positions)\n", HPW, HPH, HB);
    }

    if (do_bench && !failures) {
        bench_prep();
        printf("checkasm: benchmarks (ns/call, best of %d reps, lower is better)\n",
               BENCH_REPS);
        fill_random(a, STRIDE * PLANE_H);
        fill_random(b, STRIDE * PLANE_H);
        for (int pu = 0; pu < Y264_PU_COUNT; pu++) {
            double opt_ns = bench2(opt.sad[pu], a, b);
            double ref_ns = bench2(ref.sad[pu], a, b);
            printf("  sad_%-7s  opt %6.2f  ref %6.2f  (%.2fx)\n",
                   y264_pu_name[pu], opt_ns, ref_ns,
                   opt_ns > 0 ? ref_ns / opt_ns : 0.0);
        }
        /* sad_x4: batched vs 4 dispatched singles (per-4-SADs time). */
        for (int pu = 0; pu < Y264_PU_COUNT; pu++) {
            double bo = 1e30, bs = 1e30;
            int s[4];
            volatile int sink = 0;
            for (int r = 0; r < BENCH_REPS; r++) {
                uint64_t t0 = now_ns();
                for (int i = 0; i < BENCH_ITERS; i++) {
                    opt.sad_x4[pu](a, STRIDE, b, b + 1, b + STRIDE, b + STRIDE + 3, STRIDE, s);
                    sink += s[0];
                }
                uint64_t t1 = now_ns();
                for (int i = 0; i < BENCH_ITERS; i++) {
                    sink += opt.sad[pu](a, STRIDE, b, STRIDE);
                    sink += opt.sad[pu](a, STRIDE, b + 1, STRIDE);
                    sink += opt.sad[pu](a, STRIDE, b + STRIDE, STRIDE);
                    sink += opt.sad[pu](a, STRIDE, b + STRIDE + 3, STRIDE);
                }
                uint64_t t2 = now_ns();
                double o = (double)(t1 - t0) / BENCH_ITERS;
                double c = (double)(t2 - t1) / BENCH_ITERS;
                if (o < bo) bo = o;
                if (c < bs) bs = c;
            }
            (void)sink;
            printf("  sad_x4_%-5s  opt %6.2f  4xsingle %6.2f  (%.2fx)\n",
                   y264_pu_name[pu], bo, bs, bo > 0 ? bs / bo : 0.0);
        }
        {   /* satd_x4_8x8: batched vs 4 dispatched singles (per-4-SATDs time) */
            double bo = 1e30, bs = 1e30;
            int s[4];
            volatile int sink = 0;
            for (int r = 0; r < BENCH_REPS; r++) {
                uint64_t t0 = now_ns();
                for (int i = 0; i < BENCH_ITERS; i++) {
                    opt.satd_x4_8x8(a, STRIDE, b, b + 1, b + STRIDE, b + STRIDE + 3,
                                    STRIDE, s);
                    sink += s[0];
                }
                uint64_t t1 = now_ns();
                for (int i = 0; i < BENCH_ITERS; i++) {
                    sink += opt.satd8x8(a, STRIDE, b, STRIDE);
                    sink += opt.satd8x8(a, STRIDE, b + 1, STRIDE);
                    sink += opt.satd8x8(a, STRIDE, b + STRIDE, STRIDE);
                    sink += opt.satd8x8(a, STRIDE, b + STRIDE + 3, STRIDE);
                }
                uint64_t t2 = now_ns();
                double o = (double)(t1 - t0) / BENCH_ITERS;
                double c = (double)(t2 - t1) / BENCH_ITERS;
                if (o < bo) bo = o;
                if (c < bs) bs = c;
            }
            (void)sink;
            printf("  satd_x4_8x8    opt %6.2f  4xsingle %6.2f  (%.2fx)\n",
                   bo, bs, bo > 0 ? bs / bo : 0.0);
        }
        struct { const char *n; y264_satd_fn r, o; } sk[] = {
            { "satd4x4  ", ref.satd4x4,   opt.satd4x4   },
            { "satd8x8  ", ref.satd8x8,   opt.satd8x8   },
            { "satd16x16", ref.satd16x16, opt.satd16x16 },
            { "sa8d8x8  ", ref.sa8d8x8,   opt.sa8d8x8   },
            { "sa8d16x16", ref.sa8d16x16, opt.sa8d16x16 },
        };
        for (unsigned k = 0; k < sizeof(sk)/sizeof(sk[0]); k++) {
            double opt_ns = bench2(sk[k].o, a, b);
            double ref_ns = bench2(sk[k].r, a, b);
            printf("  %s    opt %6.2f  ref %6.2f  (%.2fx)\n",
                   sk[k].n, opt_ns, ref_ns, opt_ns > 0 ? ref_ns / opt_ns : 0.0);
        }
        {   /* hadamard_ac has its own signature; bench inline */
            double best_o = 1e30, best_r = 1e30;
            volatile long sink = 0;
            for (int r = 0; r < BENCH_REPS; r++) {
                uint64_t t0 = now_ns();
                for (int i = 0; i < BENCH_ITERS; i++) sink += opt.hadamard_ac8x8(a, STRIDE);
                uint64_t t1 = now_ns();
                for (int i = 0; i < BENCH_ITERS; i++) sink += ref.hadamard_ac8x8(a, STRIDE);
                uint64_t t2 = now_ns();
                double o = (double)(t1 - t0) / BENCH_ITERS, c = (double)(t2 - t1) / BENCH_ITERS;
                if (o < best_o) best_o = o;
                if (c < best_r) best_r = c;
            }
            (void)sink;
            printf("  had_ac8x8      opt %6.2f  ref %6.2f  (%.2fx)\n",
                   best_o, best_r, best_o > 0 ? best_r / best_o : 0.0);
        }
        {   /* texture_ac4_16x16: same inline bench shape */
            double best_o = 1e30, best_r = 1e30;
            volatile long sink = 0;
            for (int r = 0; r < BENCH_REPS; r++) {
                uint64_t t0 = now_ns();
                for (int i = 0; i < BENCH_ITERS; i++) sink += opt.texture_ac4_16x16(a, STRIDE);
                uint64_t t1 = now_ns();
                for (int i = 0; i < BENCH_ITERS; i++) sink += ref.texture_ac4_16x16(a, STRIDE);
                uint64_t t2 = now_ns();
                double o = (double)(t1 - t0) / BENCH_ITERS, c = (double)(t2 - t1) / BENCH_ITERS;
                if (o < best_o) best_o = o;
                if (c < best_r) best_r = c;
            }
            (void)sink;
            printf("  texture_ac4    opt %6.2f  ref %6.2f  (%.2fx)\n",
                   best_o, best_r, best_o > 0 ? best_r / best_o : 0.0);
        }
        {   /* var16x16: pixel sum + sum of squares, same inline bench shape */
            double best_o = 1e30, best_r = 1e30;
            volatile long sink = 0;
            uint32_t v2[2];
            for (int r = 0; r < BENCH_REPS; r++) {
                uint64_t t0 = now_ns();
                for (int i = 0; i < BENCH_ITERS; i++) { opt.var16x16(a, STRIDE, v2); sink += (long)v2[0]; }
                uint64_t t1 = now_ns();
                for (int i = 0; i < BENCH_ITERS; i++) { ref.var16x16(a, STRIDE, v2); sink += (long)v2[0]; }
                uint64_t t2 = now_ns();
                double o = (double)(t1 - t0) / BENCH_ITERS, c = (double)(t2 - t1) / BENCH_ITERS;
                if (o < best_o) best_o = o;
                if (c < best_r) best_r = c;
            }
            (void)sink;
            printf("  var16x16       opt %6.2f  ref %6.2f  (%.2fx)\n",
                   best_o, best_r, best_o > 0 ? best_r / best_o : 0.0);
        }
        {   /* intra4x4_x9: the nine-mode intra cost of one 4x4 block. Two
 * baselines, because they answer different questions: `ref` is the
 * all-C loop this replaces (the pure-C board), `prev` is the
 * as-shipped path it actually displaced -- C mode builders feeding
 * the dispatched NEON satd4x4. */
            double best_o = 1e30, best_r = 1e30, best_p = 1e30;
            volatile long sink = 0;
            const pixel *rc = a + 8 * STRIDE + 16;
            int c9[9];
            for (int r = 0; r < BENCH_REPS; r++) {
                uint64_t t0 = now_ns();
                for (int i = 0; i < BENCH_ITERS; i++) {
                    opt.intra4x4_x9(b, STRIDE, rc, STRIDE, 1, 1, 1, 1, c9); sink += c9[0];
                }
                uint64_t t1 = now_ns();
                for (int i = 0; i < BENCH_ITERS; i++) {
                    ref.intra4x4_x9(b, STRIDE, rc, STRIDE, 1, 1, 1, 1, c9); sink += c9[0];
                }
                uint64_t t2 = now_ns();
                for (int i = 0; i < BENCH_ITERS; i++) {
                    pixel pr[16];
                    for (int m = 0; m < 9; m++) {
                        y264_intra4x4(pr, rc, STRIDE, m, 1, 1, 1, 1);
                        sink += opt.satd4x4(b, STRIDE, pr, 4);
                    }
                }
                uint64_t t3 = now_ns();
                double o = (double)(t1 - t0) / BENCH_ITERS, c = (double)(t2 - t1) / BENCH_ITERS;
                double p = (double)(t3 - t2) / BENCH_ITERS;
                if (o < best_o) best_o = o;
                if (c < best_r) best_r = c;
                if (p < best_p) best_p = p;
            }
            (void)sink;
            printf("  intra4x4_x9    opt %6.2f  ref %6.2f  (%.2fx)  prev %6.2f  (%.2fx)\n",
                   best_o, best_r, best_o > 0 ? best_r / best_o : 0.0,
                   best_p, best_o > 0 ? best_p / best_o : 0.0);
        }
        {   /* texture_ac48_16x16: both psy terms, one pass */
            double best_o = 1e30, best_r = 1e30;
            volatile long sink = 0;
            for (int r = 0; r < BENCH_REPS; r++) {
                long o2[2];
                uint64_t t0 = now_ns();
                for (int i = 0; i < BENCH_ITERS; i++) { opt.texture_ac48_16x16(a, STRIDE, o2); sink += o2[0]; }
                uint64_t t1 = now_ns();
                for (int i = 0; i < BENCH_ITERS; i++) { ref.texture_ac48_16x16(a, STRIDE, o2); sink += o2[0]; }
                uint64_t t2 = now_ns();
                double o = (double)(t1 - t0) / BENCH_ITERS, c = (double)(t2 - t1) / BENCH_ITERS;
                if (o < best_o) best_o = o;
                if (c < best_r) best_r = c;
            }
            (void)sink;
            printf("  texture_ac48   opt %6.2f  ref %6.2f  (%.2fx)\n",
                   best_o, best_r, best_o > 0 ? best_r / best_o : 0.0);
        }
        {   /* transforms: dispatched vs _c reference */
            dctcoef in8[64], out8[64], in4[16], out4[16];
            for (int i = 0; i < 64; i++) in8[i] = (dctcoef)((int)rnd8() - (int)rnd8());
            for (int i = 0; i < 16; i++) in4[i] = (dctcoef)((int)rnd8() - (int)rnd8());
#define BENCH_TR(name, opt_call, ref_call) do {                              \
            double bo = 1e30, br = 1e30;                                     \
            for (int r = 0; r < BENCH_REPS; r++) {                           \
                uint64_t t0 = now_ns();                                      \
                for (int i = 0; i < BENCH_ITERS; i++) { opt_call; }          \
                uint64_t t1 = now_ns();                                      \
                for (int i = 0; i < BENCH_ITERS; i++) { ref_call; }          \
                uint64_t t2 = now_ns();                                      \
                double o = (double)(t1 - t0) / BENCH_ITERS;                  \
                double c = (double)(t2 - t1) / BENCH_ITERS;                  \
                if (o < bo) bo = o;                                          \
                if (c < br) br = c;                                          \
            }                                                                \
            printf("  %-13s  opt %6.2f  ref %6.2f  (%.2fx)\n", name, bo, br, \
                   bo > 0 ? br / bo : 0.0);                                  \
        } while (0)
            BENCH_TR("fdct4x4", y264_fdct4x4(in4, out4), y264_fdct4x4_c(in4, out4));
            BENCH_TR("idct4x4", y264_idct4x4(in4, out4), y264_idct4x4_c(in4, out4));
            BENCH_TR("fdct8x8", y264_fdct8x8(in8, out8), y264_fdct8x8_c(in8, out8));
            BENCH_TR("idct8x8", y264_idct8x8(in8, out8), y264_idct8x8_c(in8, out8));
            {
                pixel drec[64];
                for (int i = 0; i < 64; i++) in8[i] = (dctcoef)((int)rnd8() - (int)rnd8());
                for (int i = 0; i < 16; i++) in4[i] = in8[i];
                BENCH_TR("sub4x4_dct", y264_sub4x4_dct(out4, a, STRIDE, b, STRIDE),
                                       y264_sub4x4_dct_c(out4, a, STRIDE, b, STRIDE));
                BENCH_TR("add4x4_idct", y264_add4x4_idct(drec, 8, b, STRIDE, in4),
                                        y264_add4x4_idct_c(drec, 8, b, STRIDE, in4));
                BENCH_TR("sub8x8_dct8", y264_sub8x8_dct8(out8, a, STRIDE, b, STRIDE),
                                        y264_sub8x8_dct8_c(out8, a, STRIDE, b, STRIDE));
                BENCH_TR("add8x8_idct8", y264_add8x8_idct8(drec, 8, b, STRIDE, in8),
                                         y264_add8x8_idct8_c(drec, 8, b, STRIDE, in8));
                {
                    int zi[64]; uint64_t zk; uint32_t zm; int zb;
                    BENCH_TR("zigzag_abs_8x8", y264_zigzag_abs_8x8(zi, in8),
                                               y264_zigzag_abs_8x8_c(zi, in8));
                    BENCH_TR("scan_mask_8x8", y264_scan_mask_8x8(in8, &zk, &zb),
                                              y264_scan_mask_8x8_c(in8, &zk, &zb));
                    BENCH_TR("zigzag_scan_4x4", y264_zigzag_scan_4x4(out4, in4, &zm, &zb),
                                                y264_zigzag_scan_4x4_c(out4, in4, &zm, &zb));
                }
                {   /* Bi-prediction average of a 16x16 luma block, both the
 * unweighted fast path and an implicit-weight pair. */
                    pixel av[256];
                    BENCH_TR("pixel_avg 32/32", y264_pixel_avg_wt(av, a, b, 256, 32, 32),
                                                y264_pixel_avg_wt_c(av, a, b, 256, 32, 32));
                    BENCH_TR("pixel_avg 21/43", y264_pixel_avg_wt(av, a, b, 256, 21, 43),
                                                y264_pixel_avg_wt_c(av, a, b, 256, 21, 43));
                }
                {   /* Fused I16x16 V/H/DC cost. Same two baselines as
 * intra4x4_x9: `ref` is the all-C loop, `prev` the
 * as-shipped path -- the NEON I16x16 builder writing 256
 * bytes per mode, then the dispatched NEON satd16x16. */
                    double best_o = 1e30, best_r = 1e30, best_p = 1e30;
                    volatile long sink3 = 0;
                    const pixel *rc3 = a + 8 * STRIDE + 16;
                    int c3[3];
                    for (int r = 0; r < BENCH_REPS; r++) {
                        uint64_t t0 = now_ns();
                        for (int i = 0; i < BENCH_ITERS; i++) {
                            opt.intra_satd_x3_16(b, STRIDE, a, a + STRIDE, 128, c3); sink3 += c3[0];
                        }
                        uint64_t t1 = now_ns();
                        for (int i = 0; i < BENCH_ITERS; i++) {
                            ref.intra_satd_x3_16(b, STRIDE, a, a + STRIDE, 128, c3); sink3 += c3[0];
                        }
                        uint64_t t2 = now_ns();
                        for (int i = 0; i < BENCH_ITERS; i++) {
                            pixel pr[256];
                            static const int md[3] = { Y264_I16_VERT, Y264_I16_HORIZ, Y264_I16_DC };
                            for (int m = 0; m < 3; m++) {
                                y264_intra16x16(pr, rc3, STRIDE, md[m], 1, 1);
                                sink3 += opt.satd16x16(b, STRIDE, pr, 16);
                            }
                        }
                        uint64_t t3 = now_ns();
                        double o = (double)(t1 - t0) / BENCH_ITERS, c = (double)(t2 - t1) / BENCH_ITERS;
                        double pv = (double)(t3 - t2) / BENCH_ITERS;
                        if (o < best_o) best_o = o;
                        if (c < best_r) best_r = c;
                        if (pv < best_p) best_p = pv;
                    }
                    (void)sink3;
                    printf("  intra_satd_x3_16 opt %6.2f  ref %6.2f  (%.2fx)  prev %6.2f  (%.2fx)\n",
                           best_o, best_r, best_o > 0 ? best_r / best_o : 0.0,
                           best_p, best_o > 0 ? best_p / best_o : 0.0);
                }
                {   /* Half-pel plane fetch, the three widths the subpel path
 * passes. The C reference is what clang auto-vectorises
 * today, so these ratios are the honest coverage read. */
                    pixel pv[256];
                    BENCH_TR("pred_copy 16x16", y264_pred_copy(pv, 16, a, STRIDE, 16, 16),
                                                y264_pred_copy_c(pv, 16, a, STRIDE, 16, 16));
                    BENCH_TR("pred_avg2 16x16", y264_pred_avg2(pv, 16, a, b, STRIDE, 16, 16),
                                                y264_pred_avg2_c(pv, 16, a, b, STRIDE, 16, 16));
                    BENCH_TR("pred_avg2 8x8", y264_pred_avg2(pv, 16, a, b, STRIDE, 8, 8),
                                              y264_pred_avg2_c(pv, 16, a, b, STRIDE, 8, 8));
                    BENCH_TR("pred_avg2 4x4", y264_pred_avg2(pv, 16, a, b, STRIDE, 4, 4),
                                              y264_pred_avg2_c(pv, 16, a, b, STRIDE, 4, 4));
                }
                {   /* Batched 16x16 forward transform. `ref` is the all-C
 * batch; the displaced path was sixteen dispatched
 * y264_sub4x4_dct calls, benched here as `prev`. */
                    dctcoef gb[16][16];
                    double best_o = 1e30, best_r = 1e30, best_p = 1e30;
                    volatile long sink = 0;
                    int iters = BENCH_ITERS / 8;
                    for (int r = 0; r < BENCH_REPS; r++) {
                        uint64_t t0 = now_ns();
                        for (int i = 0; i < iters; i++) {
                            y264_sub_dct4_blocks(gb, 4, 4, a, STRIDE, b, STRIDE);
                            sink += gb[0][0];
                        }
                        uint64_t t1 = now_ns();
                        for (int i = 0; i < iters; i++) {
                            y264_sub_dct4_blocks_c(gb, 4, 4, a, STRIDE, b, STRIDE);
                            sink += gb[0][0];
                        }
                        uint64_t t2 = now_ns();
                        for (int i = 0; i < iters; i++) {
                            for (int by = 0; by < 4; by++)
                                for (int bx = 0; bx < 4; bx++)
                                    y264_sub4x4_dct(gb[by * 4 + bx],
                                                    a + (by * 4) * STRIDE + bx * 4, STRIDE,
                                                    b + (by * 4) * STRIDE + bx * 4, STRIDE);
                            sink += gb[0][0];
                        }
                        uint64_t t3 = now_ns();
                        double o = (double)(t1 - t0) / iters, c = (double)(t2 - t1) / iters;
                        double pv = (double)(t3 - t2) / iters;
                        if (o < best_o) best_o = o;
                        if (c < best_r) best_r = c;
                        if (pv < best_p) best_p = pv;
                    }
                    (void)sink;
                    printf("  %-14s opt %6.2f  ref %6.2f  (%.2fx)  prev %6.2f  (%.2fx)\n",
                           "sub_dct4_16x16", best_o, best_r,
                           best_o > 0 ? best_r / best_o : 0.0,
                           best_p, best_o > 0 ? best_p / best_o : 0.0);
                }
            }
            uint8_t w16[64]; memset(w16, 16, sizeof(w16));
            BENCH_TR("quant_8x8", y264_quant_8x8(in8, out8, 26, 0, NULL),
                                  y264_quant_8x8(in8, out8, 26, 0, w16));
            BENCH_TR("quant_4x4", y264_quant_4x4(in4, out4, 26, 0, NULL),
                                  y264_quant_4x4(in4, out4, 26, 0, w16));
            {
                pixel ip[256];
                const pixel *rc = a + 8 * STRIDE + 16;
                BENCH_TR("intra16_vert", y264_intra16x16(ip, rc, STRIDE, Y264_I16_VERT, 1, 1),
                                         y264_intra16x16_c(ip, rc, STRIDE, Y264_I16_VERT, 1, 1));
                BENCH_TR("intra16_plane", y264_intra16x16(ip, rc, STRIDE, Y264_I16_PLANE, 1, 1),
                                          y264_intra16x16_c(ip, rc, STRIDE, Y264_I16_PLANE, 1, 1));
                BENCH_TR("intra8x8_vr", y264_intra8x8(ip, rc, STRIDE, Y264_I4_VR, 1, 1, 1, 1),
                                        y264_intra8x8_c(ip, rc, STRIDE, Y264_I4_VR, 1, 1, 1, 1));
                BENCH_TR("intra8x8_hd", y264_intra8x8(ip, rc, STRIDE, Y264_I4_HD, 1, 1, 1, 1),
                                        y264_intra8x8_c(ip, rc, STRIDE, Y264_I4_HD, 1, 1, 1, 1));
                BENCH_TR("intra_ch_plane", y264_intra_chroma(ip, rc, STRIDE, Y264_IC_PLANE, 1, 1, 8, 8),
                                           y264_intra_chroma_c(ip, rc, STRIDE, Y264_IC_PLANE, 1, 1, 8, 8));
            }
            {   /* chroma MC tails: dispatched (NEON) vs forced-C */
                pixel md[16 * 16];
                const pixel *rf = a + 4 * STRIDE + 12;
                BENCH_TR("mc_chroma_8x4", y264_mc_chroma(md, 16, rf, STRIDE, 40, 24, 8, 8, 5, 3, 8, 4, 2, 2),
                                          y264_mc_chroma_c(md, 16, rf, STRIDE, 40, 24, 8, 8, 5, 3, 8, 4, 2, 2));
                BENCH_TR("mc_chroma_4x4", y264_mc_chroma(md, 16, rf, STRIDE, 40, 24, 8, 8, 5, 3, 4, 4, 2, 2),
                                          y264_mc_chroma_c(md, 16, rf, STRIDE, 40, 24, 8, 8, 5, 3, 4, 4, 2, 2));
            }
#if defined(__aarch64__) && Y264_BIT_DEPTH == 8
            {   /* deblock 4-line luma edge kernels vs 4 scalar-formula lines:
 * bench the kernels directly (the scalar path lives in
 * deblock.c; the relative number vs the old per-line C is the
 * encoder-level wall change, tracked in the campaign doc). */
                double bo;
                for (int ori = 0; ori < 2; ori++)
                    for (int bs = 3; bs <= 4; bs++) {
                        bo = 1e30;
                        for (int r = 0; r < BENCH_REPS; r++) {
                            uint64_t t0 = now_ns();
                            for (int i = 0; i < BENCH_ITERS; i++) {
                                if (ori) y264_deblock_luma_h4_neon(a + 8 * STRIDE + 8, STRIDE, bs, 40, 10, 4);
                                else     y264_deblock_luma_v4_neon(a + 8 * STRIDE + 8, STRIDE, bs, 40, 10, 4);
                            }
                            uint64_t t1 = now_ns();
                            double o = (double)(t1 - t0) / BENCH_ITERS;
                            if (o < bo) bo = o;
                        }
                        printf("  deblock_%s_bs%d  opt %6.2f\n", ori ? "h4" : "v4", bs, bo);
                    }
                {   /* Whole-edge chroma: one pass vs eight scalar lines. The
 * plane is a smooth random walk, not the uniform noise the
 * rest of the bench uses: on noise the scalar filter fails
 * its alpha/beta gate on nearly every line and returns
 * without working, which would flatter it by a factor the
 * real recon path never sees. */
                    static const uint8_t cbs[4] = { 2, 1, 3, 2 };
                    static const uint8_t ctc[3] = { 1, 2, 3 };
                    for (int i = 0; i < STRIDE * PLANE_H; i++)
                        a[i] = (pixel)(128 + ((int)rnd8() % 9) - 4);
                    pixel *e = a + 8 * STRIDE + 8;
                    BENCH_TR("deblock_chroma_h",
                             y264_deblock_chroma8_h_neon(e, STRIDE, 40, 10, cbs, ctc, 2, 0),
                             chroma_edge_lines_c(e, STRIDE, 1, 40, 10, cbs, ctc, 2));
                }
            }
#endif
#undef BENCH_TR
        }
    }

    /* --- trellis lattice bench (goal-3 G2) -------------------------------
 * Not a correctness group: y264_cabac_trellis_4x4 has no second
 * implementation to diff against. This exists because the encoder-level
 * census says the lattice does ~7.8 coefficient steps and ~17 node
 * updates per call yet costs ~160 ns, i.e. ~33 cycles per node update,
 * and in-situ timers cannot resolve a function that small. Inputs are
 * synthesised to the MEASURED sparsity, so the cost model matches the
 * real workload rather than a dense worst case. */
    if (do_bench) {
        bench_prep();
        y264_cabac_t cb;
        static uint8_t ctxbuf[Y264_CABAC_CTX];
        memset(&cb, 0, sizeof cb);
        cb.ctx = ctxbuf;                      /* ctx is a POINTER in y264_cabac_t */
        for (int i = 0; i < Y264_CABAC_CTX; i++) ctxbuf[i] = (uint8_t)(2 * (i % 62) + 1);

        enum { NB = 256 };                 /* a pool of blocks, cycled */
        static int qn[NB][16], absc[NB][16], w2[NB][16], out[16];
        static long unmf[NB][16];
        for (int b = 0; b < NB; b++) {
            /* measured shape: last significant coefficient low (mean ~7.8
 * positions stepped), magnitudes mostly 1, occasional 2-3 */
            int last = 2 + (int)(rnd8() % 12);
            for (int i = 0; i < 16; i++) {
                int live = i <= last && (rnd8() % 100) < 55;
                int mag = !live ? 0 : ((rnd8() % 100) < 78 ? 1 : 1 + (int)(rnd8() % 3));
                qn[b][i] = mag;
                absc[b][i] = mag ? mag * 16 + (int)(rnd8() % 16) : (int)(rnd8() % 6);
                unmf[b][i] = 16 * 16;
                w2[b][i] = 16;
            }
        }
        double best = 1e30;
        for (int r = 0; r < BENCH_REPS; r++) {
            uint64_t t0 = now_ns();
            for (int i = 0; i < BENCH_ITERS; i++)
                y264_cabac_trellis_4x4(&cb, 2, 0, 0, 400, 16,
                                       qn[i & (NB - 1)], absc[i & (NB - 1)],
                                       unmf[i & (NB - 1)], w2[i & (NB - 1)],
                                       0, NULL, 0, out);
            uint64_t t1 = now_ns();
            double ns = (double)(t1 - t0) / BENCH_ITERS;
            if (ns < best) best = ns;
        }
        printf("  %-28s %8.1f ns/call   (encoder-level census: ~160 ns)\n",
               "trellis_4x4 lattice", best);

        /* est_decision throughput (G3 per-bin half). Our scheme is ONE packed
 * lookup (bits<<8 | next_state); the comparison variant below is the
 * conventional two-table form (a transition table plus an entropy
 * table), written from the algorithm to test whether the packing is
 * actually the faster shape on this core. Same bin stream both ways. */
        enum { NBIN = 4096 };
        static uint8_t bins[NBIN], ctxid[NBIN];
        for (int i = 0; i < NBIN; i++) {
            bins[i] = (uint8_t)(rnd8() & 1);
            ctxid[i] = (uint8_t)(rnd8() % 64);   /* small working set, as in a block */
        }
        /* packed: one uint32 load -> bits and next state */
        static uint32_t packed[128][2];
        static uint8_t  trans2[128][2];
        static uint16_t ent2[128][2];
        for (int st = 0; st < 128; st++)
            for (int b = 0; b < 2; b++) {
                uint32_t nxt = (uint32_t)((st + 3 * b) & 127);
                uint32_t bits = (uint32_t)(16 + ((st * 7 + b) & 255));
                packed[st][b] = (bits << 8) | nxt;
                trans2[st][b] = (uint8_t)nxt;
                ent2[st][b]   = (uint16_t)bits;
            }
        uint8_t stt[64];
        double bp = 1e30, bt = 1e30;
        for (int r = 0; r < BENCH_REPS; r++) {
            memset(stt, 3, sizeof stt);
            volatile long sinkp = 0;
            uint64_t t0 = now_ns();
            for (int it = 0; it < BENCH_ITERS; it++) {
                long acc = 0;
                for (int i = 0; i < NBIN; i++) {
                    uint8_t *st = &stt[ctxid[i] & 63];
                    uint32_t e = packed[*st][bins[i]];
                    *st = (uint8_t)e; acc += (long)(e >> 8);
                }
                sinkp += acc;
            }
            uint64_t t1 = now_ns();
            double ns = (double)(t1 - t0) / ((double)BENCH_ITERS * NBIN);
            if (ns < bp) bp = ns;
        }
        for (int r = 0; r < BENCH_REPS; r++) {
            memset(stt, 3, sizeof stt);
            volatile long sinkt = 0;
            uint64_t t0 = now_ns();
            for (int it = 0; it < BENCH_ITERS; it++) {
                long acc = 0;
                for (int i = 0; i < NBIN; i++) {
                    uint8_t *st = &stt[ctxid[i] & 63];
                    int is = *st, b = bins[i];
                    *st = trans2[is][b]; acc += ent2[is][b];
                }
                sinkt += acc;
            }
            uint64_t t1 = now_ns();
            double ns = (double)(t1 - t0) / ((double)BENCH_ITERS * NBIN);
            if (ns < bt) bt = ns;
        }
        printf("  %-28s %8.3f ns/bin  (packed, ours)\n", "est_decision packed", bp);
        printf("  %-28s %8.3f ns/bin  (two-table form)\n", "est_decision two-table", bt);

        /* Feasibility floor for the est-path design brief: a hand-rolled
 * STRAIGHT-LINE whole-MB sizer on our packed primitive, coding the
 * measured typical estimate (skip=0, mbtype, ref, 2 mvd pairs as ue-ish
 * bypass runs, cbp, 2.55 residual blocks of ~9 coefficients with
 * sig/last/lvl walks + fused gt1) against a 460-entry ctx working set.
 * If this lands near x264's 46 ns/estimate, the 125-ns gap is proven
 * to be the est walk's branchy control flow, not arithmetic. */
        {
            static uint8_t mctx[512];
            double bs2 = 1e30;
            volatile long sink3 = 0;
            for (int r = 0; r < BENCH_REPS; r++) {
                for (int i = 0; i < 512; i++) mctx[i] = (uint8_t)(2 * (i % 62) + 1);
                uint64_t t0 = now_ns();
                for (int it = 0; it < BENCH_ITERS; it++) {
                    long bits = 0;
                    uint8_t *cx = mctx;
                    /* header: skip, mbtype (3 bins), ref (2), dqp (1) */
                    for (int b = 0; b < 7; b++) {
                        uint32_t e = packed[cx[11 + b]][(it >> b) & 1];
                        cx[11 + b] = (uint8_t)e; bits += (long)(e >> 8);
                    }
                    bits += 256 * 12;                    /* mvd suffix bypass bins */
                    /* cbp: 4 luma + 2 chroma bins */
                    for (int b = 0; b < 6; b++) {
                        uint32_t e = packed[cx[70 + b]][(it >> b) & 1];
                        cx[70 + b] = (uint8_t)e; bits += (long)(e >> 8);
                    }
                    /* 2.55 residual blocks ~ alternate 2 and 3 per call */
                    int nblk = 2 + (it & 1);
                    for (int blk = 0; blk < nblk; blk++) {
                        uint8_t *sig = cx + 105 + 16 * blk, *lst = cx + 170 + 16 * blk,
                                *lvl = cx + 230 + 10 * blk;
                        /* cbf */
                        { uint32_t e = packed[cx[99 + blk]][1];
                          cx[99 + blk] = (uint8_t)e; bits += (long)(e >> 8); }
                        /* 9 coefficients over 13 positions: sig map + last */
                        for (int i = 0; i < 13; i++) {
                            int sbin = (0x1BD5 >> i) & 1;   /* 9 ones */
                            uint32_t e = packed[sig[i]][sbin];
                            sig[i] = (uint8_t)e; bits += (long)(e >> 8);
                            if (sbin) { uint32_t e2 = packed[lst[i]][i == 12];
                                        lst[i] = (uint8_t)e2; bits += (long)(e2 >> 8); }
                        }
                        /* levels: 9 lvl1 bins, 2 of them big -> fused unary via
 * the two-table pair (an upper bound on the fused cost) */
                        int node = 0;
                        for (int k = 8; k >= 0; k--) {
                            int big = (k == 2 || k == 6);
                            uint32_t e = packed[lvl[node]][big];
                            lvl[node] = (uint8_t)e; bits += (long)(e >> 8);
                            if (big) { int st = lvl[5 + (node & 3)];
                                       lvl[5 + (node & 3)] = trans2[st][1];
                                       bits += ent2[st][1] + 256; }
                            node = big ? 4 + (node & 3) : (node < 3 ? node + 1 : node);
                            bits += 256;                 /* sign bypass */
                        }
                    }
                    sink3 += bits;
                }
                uint64_t t1 = now_ns();
                double ns = (double)(t1 - t0) / BENCH_ITERS;
                if (ns < bs2) bs2 = ns;
            }
            printf("  %-28s %8.1f ns/estimate  (x264 measures 46; ours 125)\n",
                   "straight-line MB sizer", bs2);
        }
    }

    free(a);
    free(b);

    if (failures) {
        printf("checkasm: %d kernel group(s) FAILED\n", failures);
        return 1;
    }
    printf("checkasm: all kernels match reference\n");
    return 0;
}
