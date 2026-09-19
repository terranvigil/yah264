/*
 * checkasm: pixel metrics - SAD, SATD, SA8D, variance, psy texture, the fused
 * intra costs, and SSD.
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The reference side is y264_pixel_init_c, which fills a table with the
 * portable kernels and nothing else; the optimized side is the kernel SYMBOL,
 * never y264_pixel_init. That distinction is load-bearing twice over. It keeps
 * Y264_ASM_OFF from turning a group into a C-against-C tautology, and it
 * reaches the kernels the dispatcher does NOT select: on a FEAT_DotProd box
 * y264_pixel_init overwrites sad_16x16 and var16x16 with their UDOT twins, so
 * every run of the old harness on this machine left the plain-NEON 16x16 SAD
 * and variance kernels -- which still ship, and still run on a box without
 * dotprod -- entirely untested. Both forms are rows here.
 *
 * Since the x86 wave (docs/x86-plan.md wave 1) every group's BODY is written
 * once and takes the kernel it checks as an argument, with one thin row
 * function per tier on top. A tier is then a list of names and symbols rather
 * than a copy of the checks, which is what puts the SSE4.2 and AVX2 twins
 * under exactly the same adversarial fills, the same page guards and the same
 * reference as the NEON ones -- including the fills a later tier would not have
 * known to write for itself.
 */

#include "checkasm.h"
#include "dsp/arch.h"
#include "dsp/predict.h"

static y264_pixel_fn_t ref;
static int ref_ready;

static void refs(void)
{
    if (!ref_ready) {
        y264_pixel_init_c(&ref);
        ref_ready = 1;
    }
}

/* ---- the read-window checks ----------------------------------------------
 *
 * A metric kernel declares that it reads w x h samples at the stride it is
 * given, and nothing else. These put that claim on a page boundary: the window
 * is mapped to exactly ca_pg_window_pix() bytes with PROT_NONE hard against it,
 * at both tails, so a load one sample too far or one sample too early faults
 * instead of quietly returning a value that happens to be right.
 *
 * Both operands are guarded at once, and at the same tail, because a kernel
 * that over-reads the source and the reference by the same amount would still
 * produce the correct answer from a heap buffer.
 */
typedef int (*pixfn2)(const pixel *, int, const pixel *, int);

static int pg_check2(const char *what, pixfn2 fn, int w, int h, int stride)
{
    size_t win = ca_pg_window_pix(stride, w, h);
    for (int tail = 0; tail <= 1; tail++) {
        ca_pg ga, gb;
        pixel *a = ca_pg_alloc(&ga, win, tail);
        pixel *b = ca_pg_alloc(&gb, win, tail);
        volatile int sink;
        ca_pg_fill_pix(&ga, win / sizeof(pixel));
        ca_pg_fill_pix(&gb, win / sizeof(pixel));
        ca_pg_arm(what);
        sink = fn(a, stride, b, stride);
        ca_pg_disarm();
        (void)sink;
        ca_pg_free(&ga);
        ca_pg_free(&gb);
    }
    return 0;
}

typedef long (*pixfn1)(const pixel *, int);

static int pg_check1(const char *what, pixfn1 fn, int w, int h, int stride)
{
    size_t win = ca_pg_window_pix(stride, w, h);
    for (int tail = 0; tail <= 1; tail++) {
        ca_pg g;
        pixel *p = ca_pg_alloc(&g, win, tail);
        volatile long sink;
        ca_pg_fill_pix(&g, win / sizeof(pixel));
        ca_pg_arm(what);
        sink = fn(p, stride);
        ca_pg_disarm();
        (void)sink;
        ca_pg_free(&g);
    }
    return 0;
}

/* ---- the shared plane ---------------------------------------------------- */

static pixel *pa, *pb;

static void plane(void)
{
    if (!pa) {
        pa = ca_guard_alloc(STRIDE * PLANE_H * sizeof(pixel));
        pb = ca_guard_alloc(STRIDE * PLANE_H * sizeof(pixel));
    }
    ca_fill(pa, STRIDE * PLANE_H);
    ca_fill(pb, STRIDE * PLANE_H);
}

/* ---- SAD ----------------------------------------------------------------- */

struct sadrow { const char *n; int pu, w, h; pixfn2 k; };

static int run_sad(const struct sadrow *rows, int n)
{
    int bad = 0;
    refs();
    for (int t = 0; t < TRIALS; t++) {
        plane();
        if (t == 0) memcpy(pb, pa, STRIDE * PLANE_H * sizeof(pixel));
        if (t == 1) { memset(pa, 0, STRIDE * PLANE_H * sizeof(pixel));
                      memset(pb, 0xff, STRIDE * PLANE_H * sizeof(pixel)); }
        for (int k = 0; k < n; k++) {
            int r = ref.sad[rows[k].pu](pa, STRIDE, pb, STRIDE);
            int o = rows[k].k(pa, STRIDE, pb, STRIDE);
            if (r != o) {
                if (!bad) ca_fail("%s: ref=%d opt=%d", rows[k].n, r, o);
                bad++;
            }
        }
    }
    for (int k = 0; k < n && !bad; k++)
        pg_check2(rows[k].n, rows[k].k, rows[k].w, rows[k].h, STRIDE);
    if (ca_bench)
        for (int k = 0; k < n; k++) {
            volatile int s = 0;
            CA_BENCH2(rows[k].n, s += rows[k].k(pa, STRIDE, pb, STRIDE),
                                 s += ref.sad[rows[k].pu](pa, STRIDE, pb, STRIDE));
            (void)s;
        }
    return bad;
}

/* The four candidate SADs the batch replaces, through whichever kernel the
 * dispatcher picked. Bench baseline only. */
static int four_singles(y264_satd_fn f)
{
    return f(pa, STRIDE, pb, STRIDE) + f(pa, STRIDE, pb + 1, STRIDE) +
           f(pa, STRIDE, pb + STRIDE, STRIDE) +
           f(pa, STRIDE, pb + STRIDE + 3, STRIDE);
}

/* Batched SAD against four single C SADs at four distinct candidate offsets,
 * and the four candidates are guarded together at both tails: the x4 form's
 * risk is a load that spans two candidates. */
typedef void (*sadx4fn)(const pixel *, int, const pixel *, const pixel *,
                        const pixel *, const pixel *, int, int[4]);

struct sadx4row { const char *n; int pu, w, h; sadx4fn k; };

static int run_sad_x4(const struct sadx4row *rows, int n)
{
    int bad = 0;
    refs();
    for (int t = 0; t < TRIALS; t++) {
        plane();
        if (t == 0) memcpy(pb, pa, STRIDE * PLANE_H * sizeof(pixel));
        for (int k = 0; k < n; k++) {
            const pixel *r0 = pb, *r1 = pb + 1, *r2 = pb + STRIDE,
                        *r3 = pb + STRIDE + 3;
            int s[4];
            rows[k].k(pa, STRIDE, r0, r1, r2, r3, STRIDE, s);
            int e[4] = { ref.sad[rows[k].pu](pa, STRIDE, r0, STRIDE),
                         ref.sad[rows[k].pu](pa, STRIDE, r1, STRIDE),
                         ref.sad[rows[k].pu](pa, STRIDE, r2, STRIDE),
                         ref.sad[rows[k].pu](pa, STRIDE, r3, STRIDE) };
            if (memcmp(s, e, sizeof(s))) {
                if (!bad)
                    ca_fail("%s: {%d,%d,%d,%d} vs {%d,%d,%d,%d}", rows[k].n,
                            s[0], s[1], s[2], s[3], e[0], e[1], e[2], e[3]);
                bad++;
            }
        }
    }
    /* The four candidate windows sit at +0, +1, +stride, +stride+3 of one
     * plane, so the union they may read is a (w+3) x (h+1) block: guard that,
     * which still catches a kernel reading a whole row past the tallest one. */
    for (int k = 0; k < n && !bad; k++) {
        int w = rows[k].w, h = rows[k].h;
        size_t win = ca_pg_window_pix(STRIDE, w + 3, h + 1);
        for (int tail = 0; tail <= 1; tail++) {
            ca_pg gs, gr;
            pixel *src = ca_pg_alloc(&gs, ca_pg_window_pix(STRIDE, w, h), tail);
            pixel *r = ca_pg_alloc(&gr, win, tail);
            int s[4];
            ca_pg_fill_pix(&gs, ca_pg_window_pix(STRIDE, w, h) / sizeof(pixel));
            ca_pg_fill_pix(&gr, win / sizeof(pixel));
            ca_pg_arm(rows[k].n);
            rows[k].k(src, STRIDE, r, r + 1, r + STRIDE, r + STRIDE + 3,
                      STRIDE, s);
            ca_pg_disarm();
            ca_pg_free(&gs);
            ca_pg_free(&gr);
        }
    }
    /* The bench baseline for a BATCHED kernel is four DISPATCHED singles, not
     * four C singles: the question this row answers -- does fusing the calls
     * pay -- is about the path the encoder would otherwise take, and
     * docs/instruments.md section 4 quotes it that way. Reading y264_dsp here
     * is a timing baseline and never a correctness claim; the check above
     * names its kernels. */
    if (ca_bench)
        for (int k = 0; k < n; k++) {
            int s[4];
            volatile int sink = 0;
            CA_BENCH2(rows[k].n,
                      (rows[k].k(pa, STRIDE, pb, pb + 1, pb + STRIDE,
                                 pb + STRIDE + 3, STRIDE, s), sink += s[0]),
                      sink += four_singles(y264_dsp.sad[rows[k].pu]));
            (void)sink;
        }
    return bad;
}

/* ---- SATD / SA8D --------------------------------------------------------- */

static int run_metric2(const char *name, pixfn2 kern, y264_satd_fn cref,
                       int w, int h)
{
    int bad = 0;
    refs();
    for (int t = 0; t < TRIALS; t++) {
        plane();
        if (t == 0) memcpy(pb, pa, STRIDE * PLANE_H * sizeof(pixel));
        if (t == 1) { memset(pa, 0, STRIDE * PLANE_H * sizeof(pixel));
                      memset(pb, 0xff, STRIDE * PLANE_H * sizeof(pixel)); }
        if (t == 2) for (int i = 0; i < STRIDE * PLANE_H; i++) {
                        pa[i] = ca_rnd_edge(); pb[i] = ca_rnd_edge(); }
        int r = cref(pa, STRIDE, pb, STRIDE);
        int o = kern(pa, STRIDE, pb, STRIDE);
        if (r != o) {
            if (!bad) ca_fail("%s: ref=%d opt=%d", name, r, o);
            bad++;
        }
    }
    if (!bad)
        pg_check2(name, kern, w, h, STRIDE);
    {
        volatile int s = 0;
        CA_BENCH2(name, s += kern(pa, STRIDE, pb, STRIDE),
                        s += cref(pa, STRIDE, pb, STRIDE));
        (void)s;
    }
    return bad;
}

/* Batched SATD gated against ref.satd8x8, not against the tier's own single:
 * the claim the encoder rests on is that batching is byte-identical to the
 * scalar metric, and comparing two kernels to each other would not test that. */
typedef void (*satdx4fn)(const pixel *, int, const pixel *, const pixel *,
                         const pixel *, const pixel *, int, int[4]);

static int run_satd_x4(const char *name, satdx4fn kern)
{
    int bad = 0;
    refs();
    for (int t = 0; t < TRIALS; t++) {
        plane();
        if (t == 0) memcpy(pb, pa, STRIDE * PLANE_H * sizeof(pixel));
        const pixel *r0 = pb, *r1 = pb + 1, *r2 = pb + STRIDE,
                    *r3 = pb + STRIDE + 3;
        int s[4];
        kern(pa, STRIDE, r0, r1, r2, r3, STRIDE, s);
        int e[4] = { ref.satd8x8(pa, STRIDE, r0, STRIDE),
                     ref.satd8x8(pa, STRIDE, r1, STRIDE),
                     ref.satd8x8(pa, STRIDE, r2, STRIDE),
                     ref.satd8x8(pa, STRIDE, r3, STRIDE) };
        if (memcmp(s, e, sizeof(s))) {
            if (!bad)
                ca_fail("%s: {%d,%d,%d,%d} vs {%d,%d,%d,%d}", name,
                        s[0], s[1], s[2], s[3], e[0], e[1], e[2], e[3]);
            bad++;
        }
    }
    if (ca_bench) {
        int s[4];
        volatile int sink = 0;
        CA_BENCH2(name,
                  (kern(pa, STRIDE, pb, pb + 1, pb + STRIDE,
                        pb + STRIDE + 3, STRIDE, s), sink += s[0]),
                  sink += four_singles(y264_dsp.satd8x8));
        (void)sink;
    }
    return bad;
}

/* ---- single-plane energies ----------------------------------------------- */

static int run_hadamard_ac(const char *name, pixfn1 kern)
{
    int bad = 0;
    refs();
    for (int t = 0; t < TRIALS; t++) {
        plane();
        if (t == 0) memset(pa, 0, STRIDE * PLANE_H * sizeof(pixel));
        if (t == 1) memset(pa, 0xff, STRIDE * PLANE_H * sizeof(pixel));
        long r = ref.hadamard_ac8x8(pa, STRIDE);
        long o = kern(pa, STRIDE);
        if (r != o) {
            if (!bad) ca_fail("%s: ref=%ld opt=%ld", name, r, o);
            bad++;
        }
    }
    if (!bad)
        pg_check1(name, kern, 8, 8, STRIDE);
    {
        volatile long s = 0;
        CA_BENCH2(name, s += kern(pa, STRIDE),
                        s += ref.hadamard_ac8x8(pa, STRIDE));
        (void)s;
    }
    return bad;
}

/* Flat and saturated planes exercise the rounded-mean DC correction at both
 * extremes; the alternating 7/8 plane pins the rounding itself. */
static int run_texture_ac4(const char *name, pixfn1 kern)
{
    int bad = 0;
    refs();
    for (int t = 0; t < TRIALS; t++) {
        plane();
        if (t == 0) memset(pa, 0, STRIDE * PLANE_H * sizeof(pixel));
        if (t == 1) memset(pa, 0xff, STRIDE * PLANE_H * sizeof(pixel));
        if (t == 2) for (int i = 0; i < STRIDE * PLANE_H; i++)
                        pa[i] = (pixel)(i & 1 ? 8 : 7);
        long r = ref.texture_ac4_16x16(pa, STRIDE);
        long o = kern(pa, STRIDE);
        if (r != o) {
            if (!bad) ca_fail("%s: ref=%ld opt=%ld", name, r, o);
            bad++;
        }
    }
    if (!bad)
        pg_check1(name, kern, 16, 16, STRIDE);
    {
        volatile long s = 0;
        CA_BENCH2(name, s += kern(pa, STRIDE),
                        s += ref.texture_ac4_16x16(pa, STRIDE));
        (void)s;
    }
    return bad;
}

typedef void (*varfn)(const pixel *, int, uint32_t[2]);

static int run_var16x16(const char *name, varfn kern)
{
    int bad = 0;
    refs();
    for (int t = 0; t < TRIALS; t++) {
        plane();
        if (t == 0) memset(pa, 0, STRIDE * PLANE_H * sizeof(pixel));
        if (t == 1) memset(pa, 0xff, STRIDE * PLANE_H * sizeof(pixel));
        uint32_t r[2], o[2];
        ref.var16x16(pa, STRIDE, r);
        kern(pa, STRIDE, o);
        if (r[0] != o[0] || r[1] != o[1]) {
            if (!bad) ca_fail("%s: ref=%u/%u opt=%u/%u", name,
                              r[0], r[1], o[0], o[1]);
            bad++;
        }
    }
    if (!bad) {
        size_t win = ca_pg_window_pix(STRIDE, 16, 16);
        for (int tail = 0; tail <= 1; tail++) {
            ca_pg g;
            pixel *p = ca_pg_alloc(&g, win, tail);
            uint32_t o[2];
            ca_pg_fill_pix(&g, win / sizeof(pixel));
            ca_pg_arm(name);
            kern(p, STRIDE, o);
            ca_pg_disarm();
            ca_pg_free(&g);
        }
    }
    if (ca_bench) {
        uint32_t v[2];
        volatile long s = 0;
        CA_BENCH2(name, (kern(pa, STRIDE, v), s += v[0]),
                        (ref.var16x16(pa, STRIDE, v), s += v[0]));
        (void)s;
    }
    return bad;
}

/* Both psy terms in one pass. Checked three ways: against the C fused
 * reference, against the kernel, and against the two SEPARATE C kernels,
 * because the fused form derives the 8x8 coefficients from the 4x4 tiles and a
 * shared error in fused-C and fused-kernel would agree with itself. Binary
 * noise is the input class that stresses its packed-lane bounds hardest. */
typedef void (*tex48fn)(const pixel *, int, long[2]);

static int run_texture_ac48(const char *name, tex48fn kern)
{
    int bad = 0;
    const int trials = TRIALS * 64;         /* cheap kernel, wide net */
    refs();
    plane();
    for (int t = 0; t < trials; t++) {
        if (t & 1) for (int i = 0; i < STRIDE * PLANE_H; i++) pa[i] = ca_rnd_edge();
        else plane();
        if (t == 0) memset(pa, 0, STRIDE * PLANE_H * sizeof(pixel));
        if (t == 2) for (int i = 0; i < STRIDE * PLANE_H; i++)
                        pa[i] = (pixel)(i & 1 ? 8 : 7);
        if (t == 4) for (int i = 0; i < STRIDE * PLANE_H; i++) pa[i] = PIXEL_MAX;
        long ro[2], oo[2];
        ref.texture_ac48_16x16(pa, STRIDE, ro);
        kern(pa, STRIDE, oo);
        long sep4 = ref.texture_ac4_16x16(pa, STRIDE), sep8 = 0;
        for (int by = 0; by < 16; by += 8)
            for (int bx = 0; bx < 16; bx += 8)
                sep8 += ref.hadamard_ac8x8(pa + by * STRIDE + bx, STRIDE);
        if (ro[0] != oo[0] || ro[1] != oo[1] || ro[0] != sep4 || ro[1] != sep8) {
            if (!bad)
                ca_fail("%s: ref=%ld/%ld opt=%ld/%ld split=%ld/%ld", name,
                        ro[0], ro[1], oo[0], oo[1], sep4, sep8);
            bad++;
        }
    }
    if (!bad) {
        size_t win = ca_pg_window_pix(STRIDE, 16, 16);
        for (int tail = 0; tail <= 1; tail++) {
            ca_pg g;
            pixel *p = ca_pg_alloc(&g, win, tail);
            long o[2];
            ca_pg_fill_pix(&g, win / sizeof(pixel));
            ca_pg_arm(name);
            kern(p, STRIDE, o);
            ca_pg_disarm();
            ca_pg_free(&g);
        }
    }
    if (ca_bench) {
        long v[2];
        volatile long s = 0;
        CA_BENCH2(name, (kern(pa, STRIDE, v), s += v[0]),
                        (ref.texture_ac48_16x16(pa, STRIDE, v), s += v[0]));
        (void)s;
    }
    return bad;
}

/* ---- the fused intra costs ----------------------------------------------- */

/* The path the fused kernels displaced: a C mode builder per mode feeding the
 * dispatched metric of the same tier. It is the `prev` column of the bench, and
 * it is what the as-shipped encoder actually stopped paying. */
static long prev_intra4x4_x9(const pixel *src, const pixel *rc, pixfn2 satd4)
{
    pixel pr[16];
    long s = 0;
    for (int m = 0; m < 9; m++) {
        y264_intra4x4(pr, rc, STRIDE, m, 1, 1, 1, 1);
        s += satd4(src, STRIDE, pr, 4);
    }
    return s;
}

static long prev_intra_satd_x3_16(const pixel *src, const pixel *rc, pixfn2 satd16)
{
    static const int md[3] = { Y264_I16_VERT, Y264_I16_HORIZ, Y264_I16_DC };
    pixel pr[256];
    long s = 0;
    for (int m = 0; m < 3; m++) {
        y264_intra16x16(pr, rc, STRIDE, md[m], 1, 1);
        s += satd16(src, STRIDE, pr, 16);
    }
    return s;
}

/* Every mode under every availability combination, including the ones the
 * encoder's gate forbids: the kernel computes them anyway and a wrong value
 * there is a latent trap for any future caller. */
typedef void (*i4x9fn)(const pixel *, int, const pixel *, int,
                       int, int, int, int, int[9]);

static int run_intra4x4_x9(const char *name, i4x9fn kern, pixfn2 satd4)
{
    enum { PORG = 8 * STRIDE + 16 };
    int bad = 0;
    refs();
    for (int t = 0; t < TRIALS; t++) {
        plane();
        if (t == 0) memset(pa, 0, STRIDE * PLANE_H * sizeof(pixel));
        if (t == 1) { memset(pa, 0, STRIDE * PLANE_H * sizeof(pixel));
                      memset(pb, 0xff, STRIDE * PLANE_H * sizeof(pixel)); }
        const pixel *rc = pa + PORG;
        for (int ht = 0; ht <= 1; ht++)
            for (int hl = 0; hl <= 1; hl++)
                for (int htl = 0; htl <= (ht && hl); htl++)
                    for (int htr = 0; htr <= ht; htr++) {
                        int rc9[9], oc9[9];
                        ref.intra4x4_x9(pb, STRIDE, rc, STRIDE, ht, hl, htl, htr, rc9);
                        kern(pb, STRIDE, rc, STRIDE, ht, hl, htl, htr, oc9);
                        for (int m = 0; m < 9; m++)
                            if (rc9[m] != oc9[m]) {
                                if (!bad)
                                    ca_fail("%s: mode %d avail %d%d%d%d "
                                            "ref=%d opt=%d", name, m,
                                            ht, hl, htl, htr, rc9[m], oc9[m]);
                                bad++;
                            }
                    }
    }
    if (ca_bench) {
        const pixel *rc = pa + PORG;
        int c9[9];
        volatile long s = 0;
        CA_BENCH3(name,
                  (kern(pb, STRIDE, rc, STRIDE, 1, 1, 1, 1, c9), s += c9[0]),
                  (ref.intra4x4_x9(pb, STRIDE, rc, STRIDE, 1, 1, 1, 1, c9),
                   s += c9[0]),
                  s += prev_intra4x4_x9(pb, rc, satd4));
        (void)s;
    }
    return bad;
}

/* The DC value is swept over the whole sample range as well as the derivations
 * the encoder passes, since the kernel takes it as an input. */
typedef void (*x3fn)(const pixel *, int, const pixel *, const pixel *,
                     int, int[3]);

static int run_intra_satd_x3_16(const char *name, x3fn kern, pixfn2 satd16)
{
    int bad = 0;
    refs();
    for (int t = 0; t < TRIALS; t++) {
        plane();
        if (t == 0) { memset(pa, 0, STRIDE * PLANE_H * sizeof(pixel));
                      memset(pb, 0xff, STRIDE * PLANE_H * sizeof(pixel)); }
        if (t == 1) { memset(pa, 0xff, STRIDE * PLANE_H * sizeof(pixel));
                      memset(pb, 0, STRIDE * PLANE_H * sizeof(pixel)); }
        const pixel *top = pa + 3 * STRIDE + 5, *left = pa + 9 * STRIDE + 1;
        static const int dcs[] = { 0, 1, 128, PIXEL_MAX };
        for (unsigned k = 0; k < sizeof(dcs) / sizeof(dcs[0]); k++) {
            int rc3[3], oc3[3];
            ref.intra_satd_x3_16(pb, STRIDE, top, left, dcs[k], rc3);
            kern(pb, STRIDE, top, left, dcs[k], oc3);
            for (int m = 0; m < 3; m++)
                if (rc3[m] != oc3[m]) {
                    if (!bad)
                        ca_fail("%s: mode %d dc %d ref=%d opt=%d", name,
                                m, dcs[k], rc3[m], oc3[m]);
                    bad++;
                }
        }
    }
    if (ca_bench) {
        int c3[3];
        volatile long s = 0;
        const pixel *rc = pa + 8 * STRIDE + 16;
        CA_BENCH3(name,
                  (kern(pb, STRIDE, pa, pa + STRIDE, 128, c3), s += c3[0]),
                  (ref.intra_satd_x3_16(pb, STRIDE, pa, pa + STRIDE, 128, c3),
                   s += c3[0]),
                  s += prev_intra_satd_x3_16(pb, rc, satd16));
        (void)s;
    }
    return bad;
}

/* ---- SSD -----------------------------------------------------------------
 *
 * Dispatched from the macroblock code rather than the pixel table, and one of
 * the four kernels the single-file harness never tested at all: a grep for
 * `ssd` in it returned only the dispatcher's own name. */
static int ssd_ref(const pixel *a, const pixel *b, int w, int h)
{
    int r = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int d = a[y * STRIDE + x] - b[y * STRIDE + x];
            r += d * d;
        }
    return r;
}

static int ssd_group(int (*k16)(const uint8_t *, int, const uint8_t *, int, int),
                     int (*k8)(const uint8_t *, int, const uint8_t *, int, int),
                     const char *tag)
{
    int bad = 0;
    for (int t = 0; t < TRIALS; t++) {
        plane();
        for (int w = 8; w <= 16; w += 8)
            for (int h = 4; h <= 16; h += 4) {
                int r = 0;
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++) {
                        int d = pa[y * STRIDE + x] - pb[y * STRIDE + x];
                        r += d * d;
                    }
                int o = w == 16
                    ? k16((const uint8_t *)pa, STRIDE,
                          (const uint8_t *)pb, STRIDE, h)
                    : k8((const uint8_t *)pa, STRIDE,
                         (const uint8_t *)pb, STRIDE, h);
                if (r != o) {
                    if (!bad) ca_fail("ssd%s_%dx%d: ref=%d opt=%d", tag, w, h, r, o);
                    bad++;
                }
            }
    }
    for (int w = 8; w <= 16 && !bad; w += 8)
        for (int h = 4; h <= 16; h += 4) {
            size_t win = ca_pg_window_pix(STRIDE, w, h);
            for (int tail = 0; tail <= 1; tail++) {
                ca_pg ga, gb;
                pixel *a = ca_pg_alloc(&ga, win, tail);
                pixel *b = ca_pg_alloc(&gb, win, tail);
                volatile int sink;
                char what[32];
                ca_pg_fill_pix(&ga, win / sizeof(pixel));
                ca_pg_fill_pix(&gb, win / sizeof(pixel));
                snprintf(what, sizeof(what), "ssd%s_%dx%d", tag, w, h);
                ca_pg_arm(what);
                sink = w == 16
                    ? k16((const uint8_t *)a, STRIDE, (const uint8_t *)b, STRIDE, h)
                    : k8((const uint8_t *)a, STRIDE, (const uint8_t *)b, STRIDE, h);
                ca_pg_disarm();
                (void)sink;
                ca_pg_free(&ga);
                ca_pg_free(&gb);
            }
        }
    if (ca_bench) {
        volatile int s_ = 0;
        char lab[32];
        plane();
        snprintf(lab, sizeof(lab), "ssd_16x16%s", tag);
        CA_BENCH2(lab, s_ += k16((const uint8_t *)pa, STRIDE,
                                 (const uint8_t *)pb, STRIDE, 16),
                  s_ += ssd_ref(pa, pb, 16, 16));
        snprintf(lab, sizeof(lab), "ssd_8x8%s", tag);
        CA_BENCH2(lab, s_ += k8((const uint8_t *)pa, STRIDE,
                                (const uint8_t *)pb, STRIDE, 8),
                  s_ += ssd_ref(pa, pb, 8, 8));
    }
    return bad;
}

/* ---- the rows: one thin function per kernel per tier ---------------------- */

#if Y264_HAVE_NEON

static int t_sad_neon(void)
{
    static const struct sadrow rows[] = {
        { "sad_16x16", Y264_PU_16x16, 16, 16, y264_sad_16x16_neon },
        { "sad_16x8",  Y264_PU_16x8,  16,  8, y264_sad_16x8_neon  },
        { "sad_8x16",  Y264_PU_8x16,   8, 16, y264_sad_8x16_neon  },
        { "sad_8x8",   Y264_PU_8x8,    8,  8, y264_sad_8x8_neon   },
    };
    return run_sad(rows, 4);
}

static int t_sad_dotprod(void)
{
    static const struct sadrow rows[] = {
        { "sad_16x16_dotprod", Y264_PU_16x16, 16, 16, y264_sad_16x16_neon_dotprod },
        { "sad_8x16_dotprod",  Y264_PU_8x16,   8, 16, y264_sad_8x16_neon_dotprod  },
    };
    return run_sad(rows, 2);
}

static int t_sad_x4_neon(void)
{
    static const struct sadx4row rows[] = {
        { "sad_x4_16x16", Y264_PU_16x16, 16, 16, y264_sad_x4_16x16_neon },
        { "sad_x4_16x8",  Y264_PU_16x8,  16,  8, y264_sad_x4_16x8_neon  },
        { "sad_x4_8x16",  Y264_PU_8x16,   8, 16, y264_sad_x4_8x16_neon  },
        { "sad_x4_8x8",   Y264_PU_8x8,    8,  8, y264_sad_x4_8x8_neon   },
        { "sad_x4_8x4",   Y264_PU_8x4,    8,  4, y264_sad_x4_8x4_neon   },
    };
    return run_sad_x4(rows, 5);
}

static int t_satd4x4(void)
{ refs(); return run_metric2("satd4x4", y264_satd_4x4_neon, ref.satd4x4, 4, 4); }
static int t_satd8x8(void)
{ refs(); return run_metric2("satd8x8", y264_satd_8x8_neon, ref.satd8x8, 8, 8); }
static int t_satd16x16(void)
{ refs(); return run_metric2("satd16x16", y264_satd_16x16_neon_ded, ref.satd16x16, 16, 16); }
static int t_sa8d8x8(void)
{ refs(); return run_metric2("sa8d8x8", y264_sa8d_8x8_neon, ref.sa8d8x8, 8, 8); }
static int t_sa8d16x16(void)
{ refs(); return run_metric2("sa8d16x16", y264_sa8d_16x16_neon, ref.sa8d16x16, 16, 16); }
static int t_satd_x4_8x8(void)
{ return run_satd_x4("satd_x4_8x8", y264_satd_x4_8x8_neon); }
static int t_hadamard_ac(void)
{ return run_hadamard_ac("hadamard_ac8x8", y264_hadamard_ac_8x8_neon); }
static int t_texture_ac4(void)
{ return run_texture_ac4("texture_ac4", y264_texture_ac4_16x16_neon); }
static int t_texture_ac48(void)
{ return run_texture_ac48("texture_ac48", y264_texture_ac48_16x16_neon); }
static int t_var16x16(void)
{ return run_var16x16("var16x16", y264_var_16x16_neon); }
static int t_var16x16_dotprod(void)
{ return run_var16x16("var16x16_dotprod", y264_var_16x16_neon_dotprod); }
static int t_intra4x4_x9(void)
{ return run_intra4x4_x9("intra4x4_x9", y264_intra4x4_x9_neon, y264_satd_4x4_neon); }
static int t_intra_satd_x3_16(void)
{ return run_intra_satd_x3_16("intra_satd_x3_16", y264_intra_satd_x3_16x16_neon,
                              y264_satd_16x16_neon_ded); }
static int t_ssd(void)
{ return ssd_group(y264_ssd_16xh_neon, y264_ssd_8xh_neon, ""); }
static int t_ssd_dotprod(void)
{ return ssd_group(y264_ssd_16xh_neon_dotprod, y264_ssd_8xh_neon_dotprod,
                   "_dotprod"); }

#endif /* Y264_HAVE_NEON */

#if Y264_HAVE_SSE4

static int t_sad_sse4(void)
{
    static const struct sadrow rows[] = {
        { "sad_16x16_sse4", Y264_PU_16x16, 16, 16, y264_sad_16x16_sse4 },
        { "sad_16x8_sse4",  Y264_PU_16x8,  16,  8, y264_sad_16x8_sse4  },
        { "sad_8x16_sse4",  Y264_PU_8x16,   8, 16, y264_sad_8x16_sse4  },
        { "sad_8x8_sse4",   Y264_PU_8x8,    8,  8, y264_sad_8x8_sse4   },
    };
    return run_sad(rows, 4);
}

static int t_sad_x4_sse4(void)
{
    static const struct sadx4row rows[] = {
        { "sad_x4_16x16_sse4", Y264_PU_16x16, 16, 16, y264_sad_x4_16x16_sse4 },
        { "sad_x4_16x8_sse4",  Y264_PU_16x8,  16,  8, y264_sad_x4_16x8_sse4  },
        { "sad_x4_8x16_sse4",  Y264_PU_8x16,   8, 16, y264_sad_x4_8x16_sse4  },
        { "sad_x4_8x8_sse4",   Y264_PU_8x8,    8,  8, y264_sad_x4_8x8_sse4   },
        { "sad_x4_8x4_sse4",   Y264_PU_8x4,    8,  4, y264_sad_x4_8x4_sse4   },
    };
    return run_sad_x4(rows, 5);
}

static int t_satd4x4_sse4(void)
{ refs(); return run_metric2("satd4x4_sse4", y264_satd_4x4_sse4, ref.satd4x4, 4, 4); }
static int t_satd8x8_sse4(void)
{ refs(); return run_metric2("satd8x8_sse4", y264_satd_8x8_sse4, ref.satd8x8, 8, 8); }
static int t_satd16x16_sse4(void)
{ refs(); return run_metric2("satd16x16_sse4", y264_satd_16x16_sse4, ref.satd16x16, 16, 16); }
static int t_sa8d8x8_sse4(void)
{ refs(); return run_metric2("sa8d8x8_sse4", y264_sa8d_8x8_sse4, ref.sa8d8x8, 8, 8); }
static int t_sa8d16x16_sse4(void)
{ refs(); return run_metric2("sa8d16x16_sse4", y264_sa8d_16x16_sse4, ref.sa8d16x16, 16, 16); }
static int t_satd_x4_8x8_sse4(void)
{ return run_satd_x4("satd_x4_8x8_sse4", y264_satd_x4_8x8_sse4); }
static int t_hadamard_ac_sse4(void)
{ return run_hadamard_ac("hadamard_ac8x8_sse4", y264_hadamard_ac_8x8_sse4); }
static int t_texture_ac4_sse4(void)
{ return run_texture_ac4("texture_ac4_sse4", y264_texture_ac4_16x16_sse4); }
static int t_texture_ac48_sse4(void)
{ return run_texture_ac48("texture_ac48_sse4", y264_texture_ac48_16x16_sse4); }
static int t_var16x16_sse4(void)
{ return run_var16x16("var16x16_sse4", y264_var_16x16_sse4); }
static int t_intra4x4_x9_sse4(void)
{ return run_intra4x4_x9("intra4x4_x9_sse4", y264_intra4x4_x9_sse4,
                         y264_satd_4x4_sse4); }
static int t_intra_satd_x3_16_sse4(void)
{ return run_intra_satd_x3_16("intra_satd_x3_16_sse4",
                              y264_intra_satd_x3_16x16_sse4,
                              y264_satd_16x16_sse4); }
static int t_ssd_sse4(void)
{ return ssd_group(y264_ssd_16xh_sse4, y264_ssd_8xh_sse4, "_sse4"); }

#endif /* Y264_HAVE_SSE4 */

#if Y264_HAVE_AVX2

static int t_sad_avx2(void)
{
    static const struct sadrow rows[] = {
        { "sad_16x16_avx2", Y264_PU_16x16, 16, 16, y264_sad_16x16_avx2 },
        { "sad_16x8_avx2",  Y264_PU_16x8,  16,  8, y264_sad_16x8_avx2  },
        { "sad_8x16_avx2",  Y264_PU_8x16,   8, 16, y264_sad_8x16_avx2  },
        { "sad_8x8_avx2",   Y264_PU_8x8,    8,  8, y264_sad_8x8_avx2   },
    };
    return run_sad(rows, 4);
}

static int t_sad_x4_avx2(void)
{
    static const struct sadx4row rows[] = {
        { "sad_x4_16x16_avx2", Y264_PU_16x16, 16, 16, y264_sad_x4_16x16_avx2 },
        { "sad_x4_16x8_avx2",  Y264_PU_16x8,  16,  8, y264_sad_x4_16x8_avx2  },
        { "sad_x4_8x16_avx2",  Y264_PU_8x16,   8, 16, y264_sad_x4_8x16_avx2  },
        { "sad_x4_8x8_avx2",   Y264_PU_8x8,    8,  8, y264_sad_x4_8x8_avx2   },
        { "sad_x4_8x4_avx2",   Y264_PU_8x4,    8,  4, y264_sad_x4_8x4_avx2   },
    };
    return run_sad_x4(rows, 5);
}

static int t_satd4x4_avx2(void)
{ refs(); return run_metric2("satd4x4_avx2", y264_satd_4x4_avx2, ref.satd4x4, 4, 4); }
static int t_satd8x8_avx2(void)
{ refs(); return run_metric2("satd8x8_avx2", y264_satd_8x8_avx2, ref.satd8x8, 8, 8); }
static int t_satd16x16_avx2(void)
{ refs(); return run_metric2("satd16x16_avx2", y264_satd_16x16_avx2, ref.satd16x16, 16, 16); }
static int t_sa8d8x8_avx2(void)
{ refs(); return run_metric2("sa8d8x8_avx2", y264_sa8d_8x8_avx2, ref.sa8d8x8, 8, 8); }
static int t_sa8d16x16_avx2(void)
{ refs(); return run_metric2("sa8d16x16_avx2", y264_sa8d_16x16_avx2, ref.sa8d16x16, 16, 16); }
static int t_satd_x4_8x8_avx2(void)
{ return run_satd_x4("satd_x4_8x8_avx2", y264_satd_x4_8x8_avx2); }
static int t_hadamard_ac_avx2(void)
{ return run_hadamard_ac("hadamard_ac8x8_avx2", y264_hadamard_ac_8x8_avx2); }
static int t_texture_ac4_avx2(void)
{ return run_texture_ac4("texture_ac4_avx2", y264_texture_ac4_16x16_avx2); }
static int t_texture_ac48_avx2(void)
{ return run_texture_ac48("texture_ac48_avx2", y264_texture_ac48_16x16_avx2); }
static int t_var16x16_avx2(void)
{ return run_var16x16("var16x16_avx2", y264_var_16x16_avx2); }
static int t_intra4x4_x9_avx2(void)
{ return run_intra4x4_x9("intra4x4_x9_avx2", y264_intra4x4_x9_avx2,
                         y264_satd_4x4_avx2); }
static int t_intra_satd_x3_16_avx2(void)
{ return run_intra_satd_x3_16("intra_satd_x3_16_avx2",
                              y264_intra_satd_x3_16x16_avx2,
                              y264_satd_16x16_avx2); }
static int t_ssd_avx2(void)
{ return ssd_group(y264_ssd_16xh_avx2, y264_ssd_8xh_avx2, "_avx2"); }

#endif /* Y264_HAVE_AVX2 */

/* ---- the fused psy pass, portable ---------------------------------------
 *
 * One row that is not about SIMD at all: the fused C texture_ac48 replaced two
 * C kernels, so it is a fast path with a reference and belongs here on every
 * architecture. It is the row an x86-only box still runs from this family. */
static int t_texture_ac48_c(void)
{
    static pixel p[STRIDE * PLANE_H];
    int bad = 0;
    refs();
    for (int t = 0; t < TRIALS * 8; t++) {
        if (t & 1) for (int i = 0; i < STRIDE * PLANE_H; i++) p[i] = ca_rnd_edge();
        else ca_fill(p, STRIDE * PLANE_H);
        if (t == 0) memset(p, 0, sizeof(p));
        if (t == 2) for (int i = 0; i < STRIDE * PLANE_H; i++)
                        p[i] = (pixel)(i & 1 ? 8 : 7);
        long fused[2], sep4 = ref.texture_ac4_16x16(p, STRIDE), sep8 = 0;
        ref.texture_ac48_16x16(p, STRIDE, fused);
        for (int by = 0; by < 16; by += 8)
            for (int bx = 0; bx < 16; bx += 8)
                sep8 += ref.hadamard_ac8x8(p + by * STRIDE + bx, STRIDE);
        if (fused[0] != sep4 || fused[1] != sep8) {
            if (!bad)
                ca_fail("texture_ac48_c: fused=%ld/%ld split=%ld/%ld",
                        fused[0], fused[1], sep4, sep8);
            bad++;
        }
    }
    return bad;
}

const ca_test ca_pixel_tests[] = {
#if Y264_HAVE_NEON
    { "sad",              "pixel", Y264_CPU_NEON,                    t_sad_neon },
    { "sad_dotprod",      "pixel", Y264_CPU_NEON | Y264_CPU_DOTPROD, t_sad_dotprod },
    { "sad_x4",           "pixel", Y264_CPU_NEON,                    t_sad_x4_neon },
    { "satd4x4",          "pixel", Y264_CPU_NEON,                    t_satd4x4 },
    { "satd8x8",          "pixel", Y264_CPU_NEON,                    t_satd8x8 },
    { "satd16x16",        "pixel", Y264_CPU_NEON,                    t_satd16x16 },
    { "satd_x4_8x8",      "pixel", Y264_CPU_NEON,                    t_satd_x4_8x8 },
    { "sa8d8x8",          "pixel", Y264_CPU_NEON,                    t_sa8d8x8 },
    { "sa8d16x16",        "pixel", Y264_CPU_NEON,                    t_sa8d16x16 },
    { "hadamard_ac8x8",   "pixel", Y264_CPU_NEON,                    t_hadamard_ac },
    { "texture_ac4",      "pixel", Y264_CPU_NEON,                    t_texture_ac4 },
    { "texture_ac48",     "pixel", Y264_CPU_NEON,                    t_texture_ac48 },
    { "var16x16",         "pixel", Y264_CPU_NEON,                    t_var16x16 },
    { "var16x16_dotprod", "pixel", Y264_CPU_NEON | Y264_CPU_DOTPROD, t_var16x16_dotprod },
    { "intra4x4_x9",      "pixel", Y264_CPU_NEON,                    t_intra4x4_x9 },
    { "intra_satd_x3_16", "pixel", Y264_CPU_NEON,                    t_intra_satd_x3_16 },
    { "ssd",              "pixel", Y264_CPU_NEON,                    t_ssd },
    { "ssd_dotprod",      "pixel", Y264_CPU_NEON | Y264_CPU_DOTPROD, t_ssd_dotprod },
#endif
#if Y264_HAVE_SSE4
    { "sad_sse4",              "pixel", Y264_CPU_SSE4_ALL, t_sad_sse4 },
    { "sad_x4_sse4",           "pixel", Y264_CPU_SSE4_ALL, t_sad_x4_sse4 },
    { "satd4x4_sse4",          "pixel", Y264_CPU_SSE4_ALL, t_satd4x4_sse4 },
    { "satd8x8_sse4",          "pixel", Y264_CPU_SSE4_ALL, t_satd8x8_sse4 },
    { "satd16x16_sse4",        "pixel", Y264_CPU_SSE4_ALL, t_satd16x16_sse4 },
    { "satd_x4_8x8_sse4",      "pixel", Y264_CPU_SSE4_ALL, t_satd_x4_8x8_sse4 },
    { "sa8d8x8_sse4",          "pixel", Y264_CPU_SSE4_ALL, t_sa8d8x8_sse4 },
    { "sa8d16x16_sse4",        "pixel", Y264_CPU_SSE4_ALL, t_sa8d16x16_sse4 },
    { "hadamard_ac8x8_sse4",   "pixel", Y264_CPU_SSE4_ALL, t_hadamard_ac_sse4 },
    { "texture_ac4_sse4",      "pixel", Y264_CPU_SSE4_ALL, t_texture_ac4_sse4 },
    { "texture_ac48_sse4",     "pixel", Y264_CPU_SSE4_ALL, t_texture_ac48_sse4 },
    { "var16x16_sse4",         "pixel", Y264_CPU_SSE4_ALL, t_var16x16_sse4 },
    { "intra4x4_x9_sse4",      "pixel", Y264_CPU_SSE4_ALL, t_intra4x4_x9_sse4 },
    { "intra_satd_x3_16_sse4", "pixel", Y264_CPU_SSE4_ALL, t_intra_satd_x3_16_sse4 },
    { "ssd_sse4",              "pixel", Y264_CPU_SSE4_ALL, t_ssd_sse4 },
#endif
#if Y264_HAVE_AVX2
    { "sad_avx2",              "pixel", Y264_CPU_AVX2_ALL, t_sad_avx2 },
    { "sad_x4_avx2",           "pixel", Y264_CPU_AVX2_ALL, t_sad_x4_avx2 },
    { "satd4x4_avx2",          "pixel", Y264_CPU_AVX2_ALL, t_satd4x4_avx2 },
    { "satd8x8_avx2",          "pixel", Y264_CPU_AVX2_ALL, t_satd8x8_avx2 },
    { "satd16x16_avx2",        "pixel", Y264_CPU_AVX2_ALL, t_satd16x16_avx2 },
    { "satd_x4_8x8_avx2",      "pixel", Y264_CPU_AVX2_ALL, t_satd_x4_8x8_avx2 },
    { "sa8d8x8_avx2",          "pixel", Y264_CPU_AVX2_ALL, t_sa8d8x8_avx2 },
    { "sa8d16x16_avx2",        "pixel", Y264_CPU_AVX2_ALL, t_sa8d16x16_avx2 },
    { "hadamard_ac8x8_avx2",   "pixel", Y264_CPU_AVX2_ALL, t_hadamard_ac_avx2 },
    { "texture_ac4_avx2",      "pixel", Y264_CPU_AVX2_ALL, t_texture_ac4_avx2 },
    { "texture_ac48_avx2",     "pixel", Y264_CPU_AVX2_ALL, t_texture_ac48_avx2 },
    { "var16x16_avx2",         "pixel", Y264_CPU_AVX2_ALL, t_var16x16_avx2 },
    { "intra4x4_x9_avx2",      "pixel", Y264_CPU_AVX2_ALL, t_intra4x4_x9_avx2 },
    { "intra_satd_x3_16_avx2", "pixel", Y264_CPU_AVX2_ALL, t_intra_satd_x3_16_avx2 },
    { "ssd_avx2",              "pixel", Y264_CPU_AVX2_ALL, t_ssd_avx2 },
#endif
    { "texture_ac48_c",   "pixel", 0,                                t_texture_ac48_c },
    { NULL, "pixel", 0, NULL },
};
