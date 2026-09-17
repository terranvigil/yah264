/*
 * checkasm.h - the shared harness: the registration table, the rng, the bench
 * line, and the guard-buffer / page-guard helpers every group runs its kernel
 * through.
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Layout (split out of the single-file checkasm.c for the x86 programme,
 * docs/x86-plan.md W0a):
 *
 *   checkasm.h   this file: the table type, the rng, the bench macros, the
 *                guard buffers and the page guard
 *   main.c       the harness implementation and the CLI
 *   pixel.c      SAD / SATD / SA8D / variance / psy texture / intra costs / SSD
 *   transform.c  forward and inverse DCT, quant, dequant, scan
 *   mc.c         luma and chroma interpolation, the half-pel plane, averaging
 *   predict.c    intra prediction builders
 *   deblock.c    boundary strengths and the edge filters
 *
 * Adding a kernel is one row in its family's table; adding a FAMILY is one
 * file, one `extern` and one line in main.c's `families`, plus the source in
 * tools/checkasm/meson.build. Three kernel worktrees can add three families
 * without touching one another's files.
 *
 * THE RULE THIS HARNESS EXISTS TO KEEP: a group compares the C reference
 * against the kernel SYMBOL, never against the dispatching entry point. The
 * entry point can be ablated by Y264_ASM_OFF and steered by CPU detection, so
 * a harness that reached through it could be turned into a C-against-C
 * tautology by an environment variable -- and two of the groups here were
 * exactly that before the split. Where a kernel takes a table the dispatcher
 * owns (the quant multiplier rows), the group builds the table itself from the
 * specification and the reference derives its own independently: if the two
 * disagree the group FAILS, which is the point.
 */
#ifndef YAH264_CHECKASM_H
#define YAH264_CHECKASM_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsp/pixel.h"      /* pixel, dctcoef, PIXEL_MAX, the PU enum */
#include "common/cpu.h"

/* The harness plane: every group draws its blocks out of one of these. */
#define STRIDE      64
#define PLANE_H     32
#define TRIALS      64
#define BENCH_ITERS 200000
#define BENCH_REPS  4       /* best-of: absorbs DVFS ramp + scheduler noise */

/* ---- the registration table ----------------------------------------------
 *
 * `cpu_mask` is the set of feature bits the kernel this row tests NEEDS. Zero
 * means the row is portable -- a C fast path checked against a C reference, or
 * a definitional oracle over whatever the build dispatches. The row runs when
 * the detected CPU has every bit, and when the selected --isa admits it; a row
 * whose bits are missing is reported `skip`, never `ok`, because a group that
 * checked nothing must not read as a group that agreed.
 *
 * The x86 slots (Y264_CPU_SSE4, Y264_CPU_AVX2, Y264_CPU_AVX512BW) are live in
 * the CLI and in this type from day one with no rows behind them, so the kernel
 * items of docs/x86-plan.md waves 1-3 only ever ADD rows.
 */
typedef struct ca_test {
    const char *name;       /* the kernel group, as printed and as --list shows */
    const char *family;     /* the file it lives in; also the CLI's [family] */
    uint32_t    cpu_mask;   /* feature bits the kernel requires; 0 = portable */
    int       (*fn)(void);  /* 0 = pass, nonzero = at least one mismatch */
} ca_test;

/* One table per family file, NULL-terminated. */
extern const ca_test ca_pixel_tests[];
extern const ca_test ca_transform_tests[];
extern const ca_test ca_mc_tests[];
extern const ca_test ca_predict_tests[];
extern const ca_test ca_deblock_tests[];

/* Set by main() from --bench: groups check unconditionally and bench only when
 * this is set, so `make test` stays fast and the tables come from --bench. */
extern int ca_bench;

/* ---- randomness ----------------------------------------------------------
 *
 * One stream for the whole run, so a failure reproduces exactly. */
extern uint64_t ca_rng;
pixel ca_rnd(void);                  /* one sample, full range for the depth */
void  ca_fill(pixel *p, int n);
unsigned ca_below(unsigned n);       /* a value in [0, n) */
/* A sample at one end of the domain, for the adversarial fills. */
static inline pixel ca_rnd_edge(void)
{
    return (ca_rnd() & 1) ? (pixel)PIXEL_MAX : (pixel)0;
}

double ca_now_ns(void);
/* Pin to a performance core at a ramped clock before timing anything. */
void ca_bench_prep(void);

/* ---- the bench line ------------------------------------------------------
 *
 * Two forms: `opt` against `ref`, and the three-column form for a kernel that
 * displaced a path which was itself already optimized (`prev`), because the
 * two baselines answer different questions -- what the pure-C board would pay,
 * and what the as-shipped encoder actually stopped paying.
 */
#define CA_BENCH2(label, opt_call, ref_call) do {                            \
    if (!ca_bench) break;                                                    \
    double bo_ = 1e30, br_ = 1e30;                                           \
    for (int r_ = 0; r_ < BENCH_REPS; r_++) {                                \
        double t0_ = ca_now_ns();                                            \
        for (int i_ = 0; i_ < BENCH_ITERS; i_++) { opt_call; }               \
        double t1_ = ca_now_ns();                                            \
        for (int i_ = 0; i_ < BENCH_ITERS; i_++) { ref_call; }               \
        double t2_ = ca_now_ns();                                            \
        double o_ = (t1_ - t0_) / BENCH_ITERS, c_ = (t2_ - t1_) / BENCH_ITERS;\
        if (o_ < bo_) bo_ = o_;                                              \
        if (c_ < br_) br_ = c_;                                              \
    }                                                                        \
    printf("    %-18s opt %7.2f  ref %7.2f  (%.2fx)\n", label, bo_, br_,     \
           bo_ > 0 ? br_ / bo_ : 0.0);                                       \
} while (0)

#define CA_BENCH3(label, opt_call, ref_call, prev_call) do {                 \
    if (!ca_bench) break;                                                    \
    double bo_ = 1e30, br_ = 1e30, bp_ = 1e30;                               \
    for (int r_ = 0; r_ < BENCH_REPS; r_++) {                                \
        double t0_ = ca_now_ns();                                            \
        for (int i_ = 0; i_ < BENCH_ITERS; i_++) { opt_call; }               \
        double t1_ = ca_now_ns();                                            \
        for (int i_ = 0; i_ < BENCH_ITERS; i_++) { ref_call; }               \
        double t2_ = ca_now_ns();                                            \
        for (int i_ = 0; i_ < BENCH_ITERS; i_++) { prev_call; }              \
        double t3_ = ca_now_ns();                                            \
        double o_ = (t1_ - t0_) / BENCH_ITERS, c_ = (t2_ - t1_) / BENCH_ITERS;\
        double p_ = (t3_ - t2_) / BENCH_ITERS;                               \
        if (o_ < bo_) bo_ = o_;                                              \
        if (c_ < br_) br_ = c_;                                              \
        if (p_ < bp_) bp_ = p_;                                              \
    }                                                                        \
    printf("    %-18s opt %7.2f  ref %7.2f  (%.2fx)  prev %7.2f  (%.2fx)\n", \
           label, bo_, br_, bo_ > 0 ? br_ / bo_ : 0.0,                       \
           bp_, bo_ > 0 ? bp_ / bo_ : 0.0);                                  \
} while (0)

/* ---- reporting -----------------------------------------------------------
 *
 * A group counts mismatches and prints the FIRST one with enough detail to
 * reproduce it; the rest are a count. `ca_fail` is the one-liner for that. */
#define ca_fail(fmt, ...) printf("    FAIL " fmt "\n", __VA_ARGS__)

/* ---- guard buffers -------------------------------------------------------
 *
 * Writing or reading past a block edge is the classic vector bug: the tail of
 * a width the vector loop does not cover, or a load that grabs sixteen samples
 * where eight were declared. The sanitiser finds those only when the block
 * happens to end an allocation; these find them on every call.
 *
 * A guarded block is a malloc with CA_GUARD bytes of border on each side:
 *
 *   OVERWRITE  ca_guard_poison() before the call, ca_guard_check() after: any
 *              byte written outside the block is named.
 *   OVERREAD   ca_guard_scramble() the border, run, keep the output, scramble
 *              with different bytes and run again. Differing outputs mean the
 *              kernel read outside its declared window.
 */
#define CA_GUARD  64
#define CA_POISON 0xa5

void *ca_guard_alloc(size_t bytes);
void  ca_guard_free(void *blk);
void  ca_guard_poison(void *blk, size_t bytes);
void  ca_guard_scramble(void *blk, size_t bytes);
int   ca_guard_check(const void *blk, size_t bytes, const char *what);

/* A destination written at `stride` leaves padding to the right of every row
 * that no kernel may touch. Fill the block with CA_POISON, run, then call
 * this: it reports a write into the padding, which the border check cannot see
 * because the padding is interior. Works in BYTES; the _pix form converts. A
 * poisoned sample is 0xa5a5 at Main10 and 0xa5 at 8 bits, so every byte of it
 * is CA_POISON and the byte-wise check is exact at both depths. */
int ca_check_pad(const uint8_t *blk, int stride, int w, int h, const char *what);

static inline int ca_check_pad_pix(const pixel *blk, int stride, int w, int h,
                                   const char *what)
{
    return ca_check_pad((const uint8_t *)blk, (int)(stride * sizeof(pixel)),
                        (int)(w * sizeof(pixel)), h, what);
}

/* ---- the page guard ------------------------------------------------------
 *
 * The two nets above catch a read whose VALUE reaches the output. They do not
 * catch one whose value is thrown away, and every family here can produce
 * exactly that: a four-sample tail written as "load 8, run the 8-lane op,
 * store 4" compiles to a plain 16-byte load, so the kernel reads four samples
 * past the row, discards them, and passes the scramble check because the
 * output cannot move. The sanitiser is quiet too: the bytes are inside the
 * allocation.
 *
 * The page guard does not need the value. It is a mapping sized to EXACTLY the
 * declared window with an unmapped page hard against it, so a read one byte too
 * far faults whether or not anything is done with it. Each pass runs twice:
 * `tail = 1` puts the window's last byte against the far page (an overread
 * faults) and `tail = 0` puts its first byte against the near page (an
 * underread faults).
 *
 * The window has to be the one the kernel DECLARES, sized per call: give it a
 * spare row and the check is worth nothing. For a block read at a stride that
 * window is ca_pg_window(), which stops at the last sample of the last row
 * rather than at the end of that row -- a read into an INTERIOR row's padding
 * stays the scramble check's job, since that padding is mapped either way.
 *
 * A fault is a hard stop rather than a counted failure, so arm the handler with
 * the shape's name and it is printed on the way out.
 */
typedef struct ca_pg {
    uint8_t *map;
    size_t   maplen;
    void    *user;      /* the buffer the kernel is handed */
} ca_pg;

void *ca_pg_alloc(ca_pg *g, size_t bytes, int tail);
void  ca_pg_free(ca_pg *g);
void  ca_pg_fill(ca_pg *g, size_t bytes);
static inline void ca_pg_fill_pix(ca_pg *g, size_t n)
{
    ca_fill((pixel *)g->user, (int)n);
}
void ca_pg_arm(const char *what);
void ca_pg_disarm(void);

/* The bytes a w x h block read at `stride` declares: through the last sample of
 * the last row, and not one byte of that row's padding. */
static inline size_t ca_pg_window(int stride, int w, int h)
{
    return (size_t)(h - 1) * (size_t)stride + (size_t)w;
}

static inline size_t ca_pg_window_pix(int stride, int w, int h)
{
    return ca_pg_window(stride, w, h) * sizeof(pixel);
}

#endif /* YAH264_CHECKASM_H */
