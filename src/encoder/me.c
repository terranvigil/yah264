/*
 * me.c - motion estimation: diamond integer search + subpel refinement
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "me.h"
#include "yah264.h"            /* YAH264_ME_* -- the --me values we gate on */
#include "../dsp/mc.h"
#include "../dsp/pixel.h"
#include "../common/ledger.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>

/* Analysis effort (x264-style subme). Atomic (relaxed) so concurrent GOP-parallel
 * encoder_open calls setting it don't race the ME reads under TSan; a relaxed
 * load/store is a plain load/store on the target archs (no cost). subme<8 (medium
 * and faster) uses the improved hex; >=8 (slow+) adds the wider UMH grid. The
 * threshold is >=8 rather than >=7 so that medium (subme 7) runs the parity hex
 * path (rich seeds + lowres field + square refine), measured -0.48% vs x264
 * medium. --me / Y264_NO_UMH override. */
static _Atomic int s_me_subme = 10;

/* ME method (x264-style --me), decoupled from the preset. Values are
 * YAH264_ME_*, which carry X264_ME_*'s numbering: _AUTO follows the subme gate
 * above, _DIA is a bare hex (no wide grid, no hex-parity features), _HEX is the
 * improved hex-parity path, _UMH forces the wide grid on. Set once per encode
 * before any worker runs ME. Do NOT write a bare number here -- _AUTO is not 0,
 * and 0 is _DIA. */
static _Atomic int s_me_method = YAH264_ME_AUTO;
void y264_me_set_method(int m)
{
    atomic_store_explicit(&s_me_method, m, memory_order_relaxed);
}

/* Y264_NO_UMH env override, read once: -2 = unread, -1 = absent, 0/1 = set.
 * When present it is the highest-precedence switch (1 forces hex, 0 forces the
 * wide grid on), ahead of both --me and the subme ladder. */
static int no_umh_env(void)
{
    static int v = -2;
    if (v == -2) { const char *e = getenv("Y264_NO_UMH"); v = e ? (atoi(e) ? 1 : 0) : -1; }
    return v;
}

/* Whether the UMH wide grid is eligible this frame (before the per-search
 * small-block / cheap gates). Precedence: Y264_NO_UMH env > --me method >
 * subme ladder (>=8 = slow+). */
static int umh_allowed(void)
{
    int env = no_umh_env();
    if (env >= 0) return env ? 0 : 1;
    int m = atomic_load_explicit(&s_me_method, memory_order_relaxed);
    if (m == YAH264_ME_UMH) return 1;
    if (m == YAH264_ME_DIA || m == YAH264_ME_HEX) return 0;
    return atomic_load_explicit(&s_me_subme, memory_order_relaxed) >= 8;
}

/* Whether the hex-parity compensation features are active: the rich MV seed
 * set (rich_seeds / b_seeds), the behaviour-matched lowres MV field, the terminal
 * 8-point square refine, and round-to-nearest fpel seed alignment. On exactly
 * when hex is the primary search AND it needs to make up for a missing wide grid
 * -- i.e. --me hex or the auto medium+fast tiers; off for --me umh (has the
 * grid) and --me dia (deliberately bare/cheap). Consumed here and, via the same
 * predicate, in macroblock.c (seeds) and encoder.c (lowres field). */
int y264_me_hex_features(void)
{
    int env = no_umh_env();
    if (env >= 0) return env ? 1 : 0;
    int m = atomic_load_explicit(&s_me_method, memory_order_relaxed);
    if (m == YAH264_ME_UMH || m == YAH264_ME_DIA) return 0;  /* no compensation */
    if (m == YAH264_ME_HEX) return 1;
    return atomic_load_explicit(&s_me_subme, memory_order_relaxed) < 8;
}

/* Precomputed MV-component bit costs: bits(d) = 1 for d==0, else 1 + 2*bitlen(|d|)
 * (the same value the loop below produces). Indexed by |difference| in quarter-
 * pel units; anything beyond the table falls through to the loop. Filled once in
 * y264_me_set_subme (before any worker thread runs ME). */
#define MVBITS_N 4096
static uint8_t s_mvbits[MVBITS_N];

static pthread_once_t s_mvbits_once = PTHREAD_ONCE_INIT;
static void mvbits_init(void)
{
    for (int d = 0; d < MVBITS_N; d++) {
        int t = d, bits = 1;
        while (t) { bits += 2; t >>= 1; }
        s_mvbits[d] = (uint8_t)bits;
    }
}

void y264_me_set_subme(int subme)
{
    atomic_store_explicit(&s_me_subme, subme > 0 ? subme : 10, memory_order_relaxed);
    /* the table is subme-independent -> fill exactly once (race-free, no rewrite) */
    pthread_once(&s_mvbits_once, mvbits_init);
}

/* Lambda-premultiplied MV-component cost tables : per
 * distinct ME lambda, cost[d] = lambda * mv_bits(d) for |d| < MVBITS_N, signed-
 * indexed around the centre so a probe's rate is two loads and an add (no abs,
 * no multiply), offset by the predictor once per search. Identical integers to
 * lambda * (mv_bits(dx) + mv_bits(dy)): the table IS that product (lambda <=
 * ME_LAMBDA_MAX = 127, bits <= 25 -> fits uint16), and out-of-table |d| falls
 * back to the multiply path. Primed from y264_mb_warm_statics for every value
 * lambda_me can produce; CAS-published so a concurrent prime is a benign
 * lose-and-free, and unprimed lambdas just keep the multiply (still exact). */
#define ME_LAMBDA_MAX 127
static _Atomic(const uint16_t *) s_cmv_tab[ME_LAMBDA_MAX + 1];

void y264_me_prime_lambda(int lambda)
{
    if (lambda <= 0 || lambda > ME_LAMBDA_MAX)
        return;
    if (atomic_load_explicit(&s_cmv_tab[lambda], memory_order_acquire))
        return;
    pthread_once(&s_mvbits_once, mvbits_init);
    uint16_t *t = malloc((2 * MVBITS_N - 1) * sizeof *t);
    if (!t) return;
    for (int d = 0; d < MVBITS_N; d++) {
        t[MVBITS_N - 1 + d] = (uint16_t)(lambda * s_mvbits[d]);
        t[MVBITS_N - 1 - d] = t[MVBITS_N - 1 + d];
    }
    const uint16_t *expected = NULL;
    if (!atomic_compare_exchange_strong_explicit(&s_cmv_tab[lambda], &expected,
                                                 t, memory_order_release,
                                                 memory_order_acquire))
        free(t);
}

/* Preset-selected subpel pattern: -1 = auto (square-to-convergence). The
 * Y264_SUBPEL env still overrides this (see subpel_mode). Set once before any
 * worker runs ME, like y264_me_set_subme. */
static _Atomic int s_me_subpel = -1;
void y264_me_set_subpel(int subpel)
{
    atomic_store_explicit(&s_me_subpel, subpel, memory_order_relaxed);
}

/* All of ME's per-thread search state in ONE thread-local struct: on macOS
 * every distinct _Thread_local object costs a _tlv_get_addr walk per function
 * that touches it; one struct means one walk. Same TLS semantics per field. */
static _Thread_local struct me_tls {
    const y264_hpel_ref_t *hpel;     /* half-pel plane registry (see me.h) */
    int hpel_n, hpel_stride;
    int orc_valid;                   /* lowres oracle (single-shot) */
    long orc_cost;
    int orc_mvx, orc_mvy;
    int me_cheap;                    /* frame-level cheap-search flag */
    int hpel_thresh[2];              /* x264's half-pel threshold, PER LIST */
    int me_list;                     /* which list the current search is on */
    int me_isb;                      /* oracle attribution: B-frame search */
    int me_ymax;                     /* staircase vertical qpel cap */
    int me_et_off;                   /* importance rescue: suppress the ME_ET
 * early-out for this MB's searches (set
 * per MB by the analysis entry points,
 * from the mb-tree offset -- an MB the
 * future leans on keeps its full integer
 * search). Y264_ME_ET_IMP gates it. */
    int stq;                         /* single-thread quality mode: ME_ET family
 * disengages (the Y264_ME_ET=0 escape,
 * keyed on width instead of env) */
    int et_class;                    /* frame-class of the current MB:
 * 1=P, 2=ref B, 4=nonref B, 0=unstamped.
 * Y264_ME_ET_FT masks which classes may
 * take the ME_ET early-out (default 7 =
 * all = byte-identical). */
} s_met = { NULL, 0, 0, 0, 0, 0, 0, 0, { 0x7fffffff, 0x7fffffff }, 0, 0, INT_MAX, 0, 0, 0 };

void y264_me_set_hpel(const y264_hpel_ref_t *refs, int n, int stride)
{
    s_met.hpel = refs;
    s_met.hpel_n = refs ? n : 0;
    s_met.hpel_stride = stride;
}

void y264_me_set_et_off(int off) { s_met.me_et_off = off; }
void y264_me_set_et_class(int c) { s_met.et_class = c; }
void y264_me_set_stq(int q) { s_met.stq = q; }

/* Half-pel plane operands per quarter-pel position (fy*4+fx). 0 = integer,
 * 1 = H (horizontal half), 2 = V (vertical half), 3 = C (centre).
 *
 * Not a tuning choice -- 8.4.2.2.1 fully determines it. Each quarter-pel sample
 * is the average of the two nearest half- or integer-pel samples, so the pair
 * of source planes follows from the position alone and there is exactly one
 * correct table. Derived from the standard's diagram and then verified
 * position-by-position against y264_mc_luma_c's own switch. */
static const uint8_t qpel_plane_a[16] = { 0,1,1,1, 0,1,1,1, 2,3,3,3, 0,1,1,1 };
static const uint8_t qpel_plane_b[16] = { 0,0,0,0, 2,2,3,2, 2,2,3,2, 2,2,3,2 };

/* --- lazy-hpel census -------------------------------------------------------
 * Which row bands of a built half-pel plane does anything ever read? A band is
 * the unit an on-touch build would have to fill, so the untouched share is that
 * build's whole ceiling. Off unless Y264_HPEL_CENSUS=<band rows>; the marking
 * is not atomic, so run it at --threads 1. Bands are indexed in plane rows
 * offset by HPC_OFF so the border rows (negative) index non-negatively. */
#define HPC_MAX   72
#define HPC_ROWS  2048
#define HPC_OFF   64
#define HPC_COLS  16            /* column tiles of HPC_CW px, for the 2D bound */
#define HPC_CW    128
struct hpc_ent {
    const void *h;              /* the H plane pointer identifies the triple */
    int lo, hi;                 /* rows built since the last flush (index space) */
    int ncol;                   /* column tiles this plane actually spans */
    unsigned char row[HPC_ROWS];
    unsigned char cell[HPC_ROWS][HPC_COLS];
};
static struct hpc_ent s_hpc[HPC_MAX];
static int s_hpc_n, s_hpc_band = -1;
static long long s_hpc_built, s_hpc_used, s_hpc_rows, s_hpc_rows_used;
static long long s_hpc_cells, s_hpc_cells_used;
static long long s_hpc_builds, s_hpc_marks;

int y264_hpel_census_on(void)
{
    if (s_hpc_band < 0) {
        const char *e = getenv("Y264_HPEL_CENSUS");
        s_hpc_band = e ? atoi(e) : 0;
        if (s_hpc_band < 0) s_hpc_band = 0;
    }
    return s_hpc_band;
}

/* Fold one entry's marks into the run totals and clear it. */
static void hpc_flush(struct hpc_ent *e)
{
    if (e->hi <= e->lo) return;
    int b = s_hpc_band;
    int b0 = e->lo / b, b1 = (e->hi + b - 1) / b;
    for (int k = b0; k < b1; k++) {
        int y0 = k * b, y1 = y0 + b;
        if (y0 < e->lo) y0 = e->lo;
        if (y1 > e->hi) y1 = e->hi;
        int hit = 0;
        for (int y = y0; y < y1; y++) hit |= e->row[y];
        s_hpc_built++;
        s_hpc_used += hit != 0;
    }
    s_hpc_rows += e->hi - e->lo;
    for (int y = e->lo; y < e->hi; y++) {
        s_hpc_rows_used += e->row[y] != 0;
        for (int x = 0; x < e->ncol; x++) s_hpc_cells_used += e->cell[y][x] != 0;
    }
    s_hpc_cells += (long long)(e->hi - e->lo) * e->ncol;
    memset(e->row + e->lo, 0, (size_t)(e->hi - e->lo));
    memset(e->cell[e->lo], 0, (size_t)(e->hi - e->lo) * HPC_COLS);
    e->lo = e->hi = 0;
}

static struct hpc_ent *hpc_find(const void *h)
{
    for (int i = 0; i < s_hpc_n; i++)
        if (s_hpc[i].h == h) return &s_hpc[i];
    if (s_hpc_n >= HPC_MAX) return NULL;
    s_hpc[s_hpc_n].h = h;
    return &s_hpc[s_hpc_n++];
}

void y264_hpel_census_built(const void *h, int y0, int y1, int stride)
{
    if (!y264_hpel_census_on() || !h) return;
    struct hpc_ent *e = hpc_find(h);
    if (!e) return;
    e->ncol = (stride + HPC_OFF + HPC_CW - 1) / HPC_CW;   /* real tiles only */
    if (e->ncol > HPC_COLS) e->ncol = HPC_COLS;
    y0 += HPC_OFF; y1 += HPC_OFF;
    if (y0 < 0) y0 = 0;
    if (y1 > HPC_ROWS) y1 = HPC_ROWS;
    if (y1 <= y0) return;
    if (y0 < e->hi) hpc_flush(e);              /* overlaps -> this is a rebuild */
    if (e->hi == e->lo) e->lo = y0;
    e->hi = y1;
    s_hpc_builds++;
}

/* Mark the w x n window at (x, y) of the plane triple whose H plane is `h` as
 * read. The column tiles are the 2D bound only -- the item builds row bands. */
static void hpc_mark(const void *h, int x, int y, int w, int n)
{
    if (!h) return;
    struct hpc_ent *e = hpc_find(h);
    if (!e) return;
    int y0 = y + HPC_OFF, y1 = y0 + n + 1;     /* +1: the fy==3 / 2-tap row */
    if (y0 < 0) y0 = 0;
    if (y1 > HPC_ROWS) y1 = HPC_ROWS;
    int x0 = (x + HPC_OFF) / HPC_CW, x1 = (x + HPC_OFF + w) / HPC_CW;
    if (x0 < 0) x0 = 0;
    if (x1 > HPC_COLS - 1) x1 = HPC_COLS - 1;
    for (int i = y0; i < y1; i++) {
        e->row[i] = 1;
        for (int j = x0; j <= x1; j++) e->cell[i][j] = 1;
    }
    s_hpc_marks++;
}

/* Same, but only when the qpel phase actually reads an H/V/C plane -- a pure
 * integer position reads the reference plane and needs no build. */
static void hpc_mark_q(const void *h, int qi, int x, int y, int w, int n)
{
    if (qpel_plane_a[qi] || ((qi & 5) && qpel_plane_b[qi])) hpc_mark(h, x, y, w, n);
}

__attribute__((destructor)) static void hpc_dump(void)
{
    if (!s_hpc_band) return;
    for (int i = 0; i < s_hpc_n; i++) hpc_flush(&s_hpc[i]);
    if (!s_hpc_built) return;
    fprintf(stderr, "=== Y264_HPEL_CENSUS band=%d ===\n", s_hpc_band);
    fprintf(stderr, "  builds %lld  planes %d  marks %lld\n",
            s_hpc_builds, s_hpc_n, s_hpc_marks);
    fprintf(stderr, "  bands built %lld  touched %lld  (%.1f%%)  untouched %.1f%%\n",
            s_hpc_built, s_hpc_used, 100.0 * (double)s_hpc_used / (double)s_hpc_built,
            100.0 - 100.0 * (double)s_hpc_used / (double)s_hpc_built);
    fprintf(stderr, "  rows  built %lld  touched %lld  (%.1f%%)\n",
            s_hpc_rows, s_hpc_rows_used,
            100.0 * (double)s_hpc_rows_used / (double)s_hpc_rows);
    fprintf(stderr, "  cells built %lld  touched %lld  (%.1f%%)  [%dpx tiles, the "
            "2D bound]\n", s_hpc_cells, s_hpc_cells_used,
            100.0 * (double)s_hpc_cells_used / (double)s_hpc_cells, HPC_CW);
}

/* --- lowres oracle: the per-block lookahead prior the escalation gates
 * condition on. The caller sets it before a 16x16 ref0 search (where lr_seed
 * maps cleanly) and clears it otherwise. The search only READS it to record
 * distributions -- no gate acts, so the default is byte-identical.
 * Thread-local like s_met.hpel (safe under the wavefront).
 * docs/adaptive-me-design.md. */
void y264_me_set_oracle(int valid, long cost, int mvx, int mvy)
{
    s_met.orc_valid = valid;
    s_met.orc_cost = cost; s_met.orc_mvx = mvx; s_met.orc_mvy = mvy;
}

/* Content-adaptive ME: per-frame cheap mode set by the encoder
 * from the lookahead lowres motion field (frame-level gate). Cheap = drop the
 * UMH wide scan and cap subpel at the x264 subme-7 diamond (the exact config
 * that WINS BD on static clips but blows up on motion). Thread-local like
 * s_met.hpel: the analysis entry points stamp it per MB from f->me_cheap, so the
 * wavefront and GOP-parallel paths each see their own frame's flag. */
void y264_me_set_cheap(int on)
{
    s_met.me_cheap = on;
}

/* The half-pel threshold (x264's subpel-refinement rule): after half-pel refinement,
 * skip quarter-pel refinement on any candidate whose SATD-scored cost*7/8 exceeds
 * the best cost seen so far in this MB's inter analysis -- only near-winners pay
 * for qpel. Without it every partition x ref is qpel-refined (SATD ~6x x264);
 * this brings SATD toward x264's count. TLS, reset per MB by the analyze
 * entry (deterministic under the wavefront -- each MB is one worker, same search
 * order => same threshold evolution). Gated to subme<=7 (medium) so slower presets
 * keep exhaustive qpel. Y264_HPEL_THRESH forces on(1)/off(0). */
void y264_me_reset_hpel_thresh(void)
{
    s_met.hpel_thresh[0] = s_met.hpel_thresh[1] = 0x7fffffff;
    s_met.me_list = 0;
}

/* Which reference list the next search belongs to. x264 keeps
 * a half-pel threshold per list (passed into
 * the B 16x16 analysis per list), and so do we. With ONE accumulator shared
 * by both, list 0 -- searched first -- sets the bar and list 1's quarter-pel
 * refinement is gated by a threshold list 0 earned. Measured on mobile: that
 * costs list 1 SEVEN TIMES what it costs list 0 (mean 16x16 SATD distortion
 * -7.3% for L1 with the gate off against -0.7% for L0), and it picks L1-alone
 * 20.9% of the time where x264 picks it 38.8%. P frames only ever use list 0,
 * so they are unaffected either way. */
/* Y264_HPEL_LIST=0 selects the single shared accumulator, which is what an A/B
 * and the identity check need. */
static int hpel_list_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_HPEL_LIST"); v = e ? (atoi(e) ? 1 : 0) : 1; }
    return v;
}

void y264_me_set_list(int l) { s_met.me_list = hpel_list_on() ? (l == 1) : 0; }

static int hpel_thresh_on(void)
{
    static int env = -2;
    if (env == -2) { const char *e = getenv("Y264_HPEL_THRESH"); env = e ? atoi(e) : -1; }
    if (env >= 0) return env;
    return atomic_load_explicit(&s_me_subme, memory_order_relaxed) <= 7;
}

/* --- ME instrumentation (Y264_ME_STATS). Zero cost when off (one predicted
 * branch on a once-read flag). Run --threads 1 for a clean single-thread merge
 * (the accumulators are a plain global; the diagnostic doesn't need threading).
 * The 2D histogram over (hex_bcost/oracle_cost, |hex_mv - oracle_mv|) with the
 * UMH-improved rate per cell is what sets the escalation gate's (alpha, r);
 * the subpel-iteration histogram sets the subpel budget. --- */
#define ORC_RB 16      /* ratio buckets: floor(hex_bcost*4/oracle_cost), capped */
#define ORC_DB 6       /* mv-distance buckets (qpel): 0, <=4, <=8, <=16, <=32, >32 */
static struct me_stats {
    long searches, s16_oracle;                 /* all searches; 16x16-with-oracle */
    long umh_ran, umh_improved;                /* over all UMH-eligible searches */
    long cell_n[ORC_RB][ORC_DB];               /* joint histogram, 16x16-oracle */
    long cell_umh_imp[ORC_RB][ORC_DB];         /* of which UMH then improved */
    double cell_umh_gain[ORC_RB][ORC_DB];      /* sum (hex_bcost - umh_bcost) */
    long subpel_iters[3][9];                    /* [level 2=hpel,1=qpel][iters 0..8] */
    long probes_int, probes_sub;                /* total, to size current cost */
    /* Of the SATD-gain UMH buys, how far (integer pels) is the
 * improved MV from the hex result? NEAR = a better local search/seed would
 * have found it (the cheap search is under-powered); FAR = a separated
 * cost-surface minimum only a wide scan reaches
 * (UMH intrinsic). Buckets by |final-hex| in integer pels: 0, 1, 2, <=4,
 * <=8, >8. Gain-weighted (what matters is quality bought, not MB count). */
    long   umhdist_n[6];
    double umhdist_gain[6];
} g_ms;
static int umhdist_bucket(int px)
{
    if (px <= 0) return 0; if (px == 1) return 1; if (px == 2) return 2;
    if (px <= 4) return 3; if (px <= 8) return 4; return 5;
}
static int me_stats_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_ME_STATS"); v = e ? atoi(e) : 0; }
    return v;
}

/* --- Y264_ME_ETSTAT: what an integer-search EARLY-OUT would cost and save.
 *
 * The question this exists to answer: at the
 * high-QP end, x264's motion search on macroblocks that end SKIP costs them
 * ~0.47 us/MB against ~5.0 on the ones that end INTER -- a 10x collapse -- while
 * ours is FLAT (6.6 vs 7.1). We run the same search on a macroblock the
 * predictor already nails as on one that needs finding. So: bucketed by how good
 * the best SEED already was (SAD-and-rate per pixel), how much integer work runs
 * after it, and what that work actually moves.
 *
 * Bucket unit is one cost-per-pixel, so bucket b covers [b, b+1) and the
 * top bucket is the catch-all. Per bucket: searches, integer probes spent AFTER
 * the seeds, how often the post-seed stages moved the MV at all, and the mean
 * integer-pel distance they moved it.
 *
 * MV DISTANCE, NOT SATD GAIN, is the column to read. The note at the hex loop
 * records the same lesson: integer-SATD-gain VOLUME does not predict BD,
 * because the load-bearing finds are the rare distant ones. A bucket that moves
 * the MV nowhere is a bucket an early-out can have; a bucket with a small mean
 * gain but occasional multi-pel moves is not. Both columns are printed so the
 * cheap answer cannot be read without the expensive one beside it. --- */
#define ETB 32                                  /* cost/pixel buckets, 1 each */
static struct me_et_stats {
    long n[ETB];                                /* searches whose seed lands here */
    long probes[ETB];                           /* integer probes after the seeds */
    long moved[ETB];                            /* post-seed stages changed the MV */
    long dist[ETB];                             /* summed |mv - seed| in integer pels */
    long far[ETB];                              /* of those, moves > 2 integer pels */
    double gain[ETB];                           /* summed (seed_bcost - int_bcost) */
} g_et;
static int me_et_stat_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_ME_ETSTAT"); v = e ? atoi(e) : 0; }
    return v;
}

/* Y264_ME_ET=<K>: integer-search early-out. When the best SEED already costs
 * <= K per pixel, the block is predicted well enough that the hexagon, the wide
 * grid and the terminal diamond are unlikely to move it, so return the seed as
 * the integer result. SUBPEL STILL RUNS -- this deletes the integer refinement
 * only, which is the stage the ETSTAT histogram prices; half-pel and quarter-pel
 * are cheaper and carry more of the quality.
 *
 * K = 0 (default) makes the predicate dead and the encoder byte-identical.
 *
 * This is a search-level early-out, NOT a skip decision: the macroblock still
 * runs its whole tournament and can still come out inter. That distinction is
 * why it is worth trying at all -- committing to skip before ME is a
 * measured-refused route (docs/b-skip-decision-design.md), and this does not
 * do it. */
static int me_et_k(void)
{
    /* DEFAULT 48, paired with SHAPE=1 and ET16=4: K48 sub-16x16 plus a K=4
 * 16x16 exit reads the best board dVMAF of any swept arm (-0.51, bus
 * IMPROVED) for 2-3% of t12 wall; CRF band cost median +0.12% (foreman
 * +1.12 / bus +0.71 the exposed rows). Escape: Y264_ME_ET=0 kills the
 * whole family. */
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_ME_ET"); v = e ? atoi(e) : 48; }
    return v;
}

/* Y264_ME_ET_SHAPE: which block shapes the early-out is allowed on.
 * 0 = all (default when ET is armed)
 * 1 = sub-16x16 only 2 = 16x16 only
 *
 * The split is not arbitrary. The UMH note above records that the wide grid is
 * "valuable for 16x16 (no tight seed) but redundant for 8x8-and-smaller shapes,
 * whose predictor is the already-refined parent MV", and the note at the hex
 * loop records that the load-bearing integer finds are the rare DISTANT ones.
 * Both point the
 * same way: a partition whose seed is its parent's refined MV has little left to
 * find, while a 16x16 starting from a median predictor does. Mode 1 spends the
 * early-out only where that reasoning says it is cheap, and mode 2 is its
 * control -- if mode 2 costs the quality and mode 1 does not, the reasoning is
 * confirmed rather than assumed. */
static int me_et_shape(void)
{
    /* Default 1 (sub-16x16 only): the quality-safe half -- a partition seeded
 * from its refined parent MV has little left to find. Explicit env wins, so
 * an armed sweep can still ask for 0/2. */
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_ME_ET_SHAPE"); v = e ? atoi(e) : 1; }
    return v;
}

/* Y264_ME_ET16=<K>: a SEPARATE, independent early-out threshold for the 16x16
 * search, so the two shape classes can run at different strengths (the sub-16x16
 * exits are the quality-safe half -- their seed is the refined parent MV -- while
 * a 16x16 exit is only safe when the seed is a near-perfect match, i.e. at a much
 * lower K). 0 leaves 16x16 governed by Y264_ME_ET/Y264_ME_ET_SHAPE alone. */
static int me_et_k16(void)
{
    /* Default 4, paired with ME_ET=48 SHAPE=1: the near-perfect-seed 16x16
 * exit that makes K48s1+ET16=4 the best-dVMAF arm of the sweep.
 * Y264_ME_ET=0 kills the family; =0 here restores the 16x16 search
 * alone. */
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_ME_ET16"); v = e ? atoi(e) : 4; }
    return v;
}
/* Y264_ME_ET_FT: frame-class mask for the early-out (is the band cost
 * concentrated in the classes whose MVs propagate?). Bit 0 = P, bit 1 =
 * reference B, bit 2 = non-reference B. Default 7 = every class = the shipped
 * behaviour byte-exactly (the gate also ignores unstamped searches then). */
static int me_et_ft(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_ME_ET_FT"); v = e ? atoi(e) : 7; }
    return v;
}
/* Y264_ME_ET_CROSS=1: early-out MBs still probe the UMH cross (see the
 * eligibility computation at the search site). Default 0 = byte-identical. */
static int me_et_cross(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_ME_ET_CROSS"); v = e ? atoi(e) : 0; }
    return v;
}
static int dist_bucket(int d)
{
    if (d <= 0) return 0; if (d <= 4) return 1; if (d <= 8) return 2;
    if (d <= 16) return 3; if (d <= 32) return 4; return 5;
}

/* Build a w x h predicted block into `pred` (stride 16) from the precomputed
 * planes, reproducing y264_mc_luma exactly: half/integer positions are a strided
 * copy, quarter positions the 2-tap average of two planes (with the +1 row/col
 * offset for the fy==3 / fx==3 positions). */
static void build_pred_hpel(pixel *pred, const pixel *G, const pixel *H,
                            const pixel *V, const pixel *C, int rs,
                            int ix, int iy, int fx, int fy, int w, int h)
{
    const pixel *pl[4] = { G, H, V, C };
    int qi = fy * 4 + fx;
    NLED(getref_build, 1); NLED(getref_pix, (uint64_t)w*h);
    if (qi & 5) { NLED(avg_call, 1); NLED(avg_pix, (uint64_t)w*h); }
    const pixel *s1 = pl[qpel_plane_a[qi]] + (ptrdiff_t)iy * rs + ix + (fy == 3 ? rs : 0);
    if (qi & 5) {
        const pixel *s2 = pl[qpel_plane_b[qi]] + (ptrdiff_t)iy * rs + ix + (fx == 3 ? 1 : 0);
        y264_pred_avg2(pred, 16, s1, s2, rs, w, h);
    } else {
        y264_pred_copy(pred, 16, s1, rs, w, h);
    }
}

/* Public plane-read luma MC into a stride-16 pred block. Byte-identical to
 * y264_mc_luma: reads this reference's registered half-pel planes (y264_me_set_hpel)
 * when in bounds -- a strided copy or 2-tap average -- else interpolates on the fly.
 * Lets build_inter_pred's per-candidate MC skip the 6-tap convolution. Neutral where
 * y264_mc_luma has a SIMD kernel (the plane read is scalar here too), but a real win
 * on the scalar/no-SIMD path, where it replaces the 6-tap with a copy/average. */
void y264_me_mc_luma(pixel *pred, const pixel *ref, int rs, int pw, int ph,
                     int bx, int by, int mvx, int mvy, int w, int h)
{
    const pixel *hp_h = NULL, *hp_v = NULL, *hp_c = NULL;
    if (s_met.hpel && rs == s_met.hpel_stride) {
        for (int i = 0; i < s_met.hpel_n; i++)
            if (s_met.hpel[i].ref == ref) {
                hp_h = s_met.hpel[i].h; hp_v = s_met.hpel[i].v; hp_c = s_met.hpel[i].c;
                break;
            }
    }
    int ix = bx + (mvx >> 2), iy = by + (mvy >> 2);
    const int B = Y264_LUMA_BORDER;
    { static int dg = -1;   /* DIAG: force the non-hpel fallback everywhere */
      if (dg < 0) { const char *e = getenv("Y264_DIAG_NOHPEL"); dg = e ? atoi(e) : 0; }
      if (dg) hp_h = NULL; }
    if (hp_h && ix >= -B && iy >= -B &&
        ix + w + 1 <= pw + B && iy + h + 1 <= ph + B) {
        if (s_hpc_band > 0) hpc_mark_q(hp_h, (mvy & 3) * 4 + (mvx & 3), ix, iy, w, h);
        build_pred_hpel(pred, ref, hp_h, hp_v, hp_c, rs, ix, iy, mvx & 3, mvy & 3, w, h);
    }
    else
        y264_mc_luma(pred, 16, ref, rs, pw, ph, bx, by, mvx, mvy, w, h);
}

static int sad(const pixel *a, int as, const pixel *b, int bs, int w, int h)
{
    int s = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++)
            s += abs((int)a[y * as + x] - (int)b[y * bs + x]);
    }
    return s;
}

static inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static int f3_border(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_F3"); v = e ? atoi(e) : 1; }
    return v;
}

/* SAD for an integer-pel MV, read straight from the reference (no interpolation).
 * Fast path (dispatched kernels) when the block is in-bounds; clamped per-pixel at
 * the edges. F3: the reference carries a B-px replicated
 * border, so reading it IS the clamp -- widen the fast path to the border bounds
 * (ix>=-B .. ix+w<=pw+B) and only clamp beyond it. Byte-identical: border pixel ==
 * nearest edge == clampi. x264 never per-pixel-clamps (planes padded, MV clamped
 * once per search). */
/* Approximate bit cost of coding a component difference (unary-ish, cheap).
 * Table lookup for the common small range; identical value to the loop. */
static int mv_bits(int d)
{
    d = d < 0 ? -d : d;
    if (d < MVBITS_N) return s_mvbits[d];
    int bits = 1;
    while (d) { bits += 2; d >>= 1; }
    return bits;
}

/* Per-search integer-probe context: everything constant across the ~420 probes
 * of one y264_me_search call -- block position, dims, the SAD kernel resolved
 * ONCE from (w,h), the predictor/lambda rate state, and the two safe MV
 * rectangles (integer fast window, subpel plane window) computed once so the
 * per-probe bounds test is four compares on the candidate MV itself (x264
 * computes mv_min/mv_max per search the same way). Byte-identical to a
 * per-arg / per-probe-recompute path. */
typedef struct {
    const pixel *src; int ss;
    const pixel *ref; int rs;
    int pw, ph, bx, by, w, h;
    y264_sad_fn sad_fn;              /* NULL -> generic sad (unsupported shape) */
    y264_sad_x4_fn sadx4_fn;         /* NULL -> per-candidate probes */
    int pmvx, pmvy, lambda;          /* rate state, constant per search */
    const uint16_t *cmv;             /* premultiplied cost table, centred on d=0
 * (index by SIGNED d); NULL = multiply path */
    int ixlo, ixhi, iylo, iyhi;      /* integer-probe fast window (qpel, on mv) */
    const pixel *hp[4];              /* {ref, H, V, C}; hp[1]==NULL = no planes */
    int sxlo, sxhi, sylo, syhi;      /* subpel plane-read window (qpel, on mv);
 * empty (1..0) when no planes registered */
    int f1;                          /* f1_zerocopy, hoisted */
} me_ctx;

/* MV rate via the premultiplied table (two loads + add) when |d| is in range,
 * else the exact multiply fallback -- identical integers either way. */
static inline int mv_cost_ctx(const me_ctx *c, int mvx, int mvy)
{
    int dx = mvx - c->pmvx, dy = mvy - c->pmvy;
    if (c->cmv &&
        (unsigned)(dx + (MVBITS_N - 1)) <= 2u * (MVBITS_N - 1) &&
        (unsigned)(dy + (MVBITS_N - 1)) <= 2u * (MVBITS_N - 1))
        return c->cmv[dx] + c->cmv[dy];
    return c->lambda * (mv_bits(dx) + mv_bits(dy));
}

static int resolve_pu(int w, int h)
{
    int pu = -1;
    if (w == 16)     pu = h == 16 ? Y264_PU_16x16 : (h == 8 ? Y264_PU_16x8 : -1);
    else if (w == 8) pu = h == 16 ? Y264_PU_8x16 : (h == 8 ? Y264_PU_8x8
                         : (h == 4 ? Y264_PU_8x4 : -1));
    else if (w == 4) pu = h == 8 ? Y264_PU_4x8 : (h == 4 ? Y264_PU_4x4 : -1);
    return pu;
}

static int sad_int(const me_ctx *c, int mvx, int mvy)
{
    /* Window test == the per-probe in-bounds test, precomputed on the MV
 * domain (ix >= -B <=> mvx >= 4*(-B-bx) under the flooring shift, etc). */
    if (mvx >= c->ixlo && mvx <= c->ixhi && mvy >= c->iylo && mvy <= c->iyhi) {
        const pixel *r = c->ref + (c->by + (mvy >> 2)) * c->rs + c->bx + (mvx >> 2);
        if (c->sad_fn) return c->sad_fn(c->src, c->ss, r, c->rs);
        return sad(c->src, c->ss, r, c->rs, c->w, c->h);
    }
    int ix = c->bx + (mvx >> 2), iy = c->by + (mvy >> 2);
    int s = 0;
    for (int y = 0; y < c->h; y++)
        for (int x = 0; x < c->w; x++) {
            int rx = clampi(ix + x, 0, c->pw - 1), ry = clampi(iy + y, 0, c->ph - 1);
            s += abs((int)c->src[y * c->ss + x] - (int)c->ref[ry * c->rs + rx]);
        }
    return s;
}

/* Integer-pel candidate cost: direct reference SAD (no interpolation). */
static int probe_int(const me_ctx *c, int mvx, int mvy)
{
    NLED(probe_int, 1);
    return sad_int(c, mvx, mvy) + mv_cost_ctx(c, mvx, mvy);
}

/* Batched integer probes: cost[i] == probe_int(c, mv[i]) exactly -- a probe's
 * SAD never depends on the running best, so scoring 4 (or n) candidates
 * before the caller's ordered strict-< comparisons cannot change a decision
 * or a tie-break. Groups of 4 whose candidates all sit in the borderless
 * fast window go through the sad_x4 kernel (one source load feeds four
 * accumulator chains); everything else falls back per-candidate. */
/* `inw` = caller proved every candidate is inside the fast window (ring
 * bounding box vs the window, one test per ring) -> skip the per-candidate
 * window tests and read the reference directly. inw=0 is the general path;
 * both produce the identical costs (the window test only picks the identical
 * fast/edge implementation of the same SAD). */
static void probe_int_list(const me_ctx *c, int n, const int (*mv)[2], int *cost,
                           int inw)
{
    int i = 0;
    if (inw) {
        if (c->sadx4_fn) {
            for (; i + 4 <= n; i += 4) {
                const pixel *r[4];
                for (int k = 0; k < 4; k++)
                    r[k] = c->ref + (c->by + (mv[i + k][1] >> 2)) * c->rs
                                  + c->bx + (mv[i + k][0] >> 2);
                NLED(probe_int, 4);
                int s[4];
                c->sadx4_fn(c->src, c->ss, r[0], r[1], r[2], r[3], c->rs, s);
                for (int k = 0; k < 4; k++)
                    cost[i + k] = s[k] + mv_cost_ctx(c, mv[i + k][0], mv[i + k][1]);
            }
        }
        for (; i < n; i++) {
            const pixel *r = c->ref + (c->by + (mv[i][1] >> 2)) * c->rs
                                    + c->bx + (mv[i][0] >> 2);
            NLED(probe_int, 1);
            int s = c->sad_fn ? c->sad_fn(c->src, c->ss, r, c->rs)
                              : sad(c->src, c->ss, r, c->rs, c->w, c->h);
            cost[i] = s + mv_cost_ctx(c, mv[i][0], mv[i][1]);
        }
        return;
    }
    if (c->sadx4_fn) {
        for (; i + 4 <= n; i += 4) {
            const pixel *r[4];
            int ok = 1;
            for (int k = 0; k < 4; k++) {
                int mx = mv[i + k][0], my = mv[i + k][1];
                if (mx >= c->ixlo && mx <= c->ixhi &&
                    my >= c->iylo && my <= c->iyhi) {
                    r[k] = c->ref + (c->by + (my >> 2)) * c->rs
                                  + c->bx + (mx >> 2);
                } else { ok = 0; break; }
            }
            if (!ok) break;
            NLED(probe_int, 4);
            int s[4];
            c->sadx4_fn(c->src, c->ss, r[0], r[1], r[2], r[3], c->rs, s);
            for (int k = 0; k < 4; k++)
                cost[i + k] = s[k] + mv_cost_ctx(c, mv[i + k][0], mv[i + k][1]);
        }
    }
    for (; i < n; i++)
        cost[i] = probe_int(c, mv[i][0], mv[i][1]);
}

/* Ring bounding-box vs fast-window test: centre +- radius (qpel) entirely
 * inside the window means every ring candidate passes the per-probe test. */
static inline int ring_in_window(const me_ctx *c, int cx, int cy, int rad)
{
    return cx - rad >= c->ixlo && cx + rad <= c->ixhi &&
           cy - rad >= c->iylo && cy + rad <= c->iyhi;
}

/* SATD (Hadamard) over a w x h block, summed in 4x4 units. Tracks the actual
 * transform-coded cost better than SAD, so it makes better subpel decisions. */
static int satd_blk(const pixel *a, int as, const pixel *b, int bs, int w, int h)
{
    if (w == 16 && h == 16)
        return y264_dsp.satd16x16(a, as, b, bs);
    int s = 0, y = 0;
    for (; y + 8 <= h; y += 8) {
        int x = 0;
        for (; x + 8 <= w; x += 8)
            s += y264_dsp.satd8x8(a + y*as + x, as, b + y*bs + x, bs);
        for (; x < w; x += 4)
            for (int yy = 0; yy < 8; yy += 4)
                s += y264_dsp.satd4x4(a + (y+yy)*as + x, as, b + (y+yy)*bs + x, bs);
    }
    for (; y < h; y += 4)
        for (int x = 0; x < w; x += 4)
            s += y264_dsp.satd4x4(a + y*as + x, as, b + y*bs + x, bs);
    return s;
}

/* SAD over a w x h block via the dispatched (NEON) kernels, scalar fallback for
 * odd shapes. Used for half-pel scoring (x264 scores hpel in SAD, SATD only for
 * quarter-pel -- a satd4x4 is ~2-3x a SAD, and hpel decisions don't need it). */
static int sad_blk(const pixel *a, int as, const pixel *b, int bs, int w, int h)
{
    int pu = -1;
    if (w == 16)     pu = h == 16 ? Y264_PU_16x16 : (h == 8 ? Y264_PU_16x8 : -1);
    else if (w == 8) pu = h == 16 ? Y264_PU_8x16 : (h == 8 ? Y264_PU_8x8
                        : (h == 4 ? Y264_PU_8x4 : -1));
    else if (w == 4) pu = h == 8 ? Y264_PU_4x8 : (h == 4 ? Y264_PU_4x4 : -1);
    if (pu >= 0) return y264_dsp.sad[pu](a, as, b, bs);
    return sad(a, as, b, bs, w, h);
}

/* Sub-pel candidate cost: build the predicted block, then a distortion metric
 * (metric != 0 => SATD, else SAD) plus the MV rate. When the reference's half-pel
 * planes are registered (hpel_h != NULL) and the read window is inside the
 * bordered allocation, read from the planes (a strided copy or 2-tap average);
 * otherwise interpolate on the fly. Both paths are bit-identical. */
static int f1_zerocopy(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_F1"); v = e ? atoi(e) : 1; }
    return v;
}

static int probe_sub_m(const me_ctx *c, int mvx, int mvy, int metric)
{
    pixel pred[16 * 16];
    NLED(probe_sub, 1);
    int fx = mvx & 3, fy = mvy & 3;
    /* Plane window (precomputed, empty when no planes registered) == the old
 * per-probe hpel_h + in-bounds test. */
    int inb = mvx >= c->sxlo && mvx <= c->sxhi &&
              mvy >= c->sylo && mvy <= c->syhi;
    /* F1: a pure half/integer position (fx,fy both
 * even) is a strided COPY of one plane -- SAD/SATD it straight off the plane
 * pointer instead, no pred[] build. build_pred_hpel's else-branch (qi&5==0)
 * writes exactly these pixels, so the metric is byte-identical. Quarter-pel
 * (fx|fy odd) still needs the 2-tap average build. */
    if (inb && c->f1 && !(fx & 1) && !(fy & 1)) {
        if (s_hpc_band > 0)
            hpc_mark_q(c->hp[1], fy * 4 + fx, c->bx + (mvx >> 2),
                       c->by + (mvy >> 2), c->w, c->h);
        const pixel *s = c->hp[qpel_plane_a[fy * 4 + fx]]
                       + (ptrdiff_t)(c->by + (mvy >> 2)) * c->rs + c->bx + (mvx >> 2);
        NLED(getref_ptr, 1);
        int d = metric ? satd_blk(c->src, c->ss, s, c->rs, c->w, c->h)
                       : sad_blk(c->src, c->ss, s, c->rs, c->w, c->h);
        return d + mv_cost_ctx(c, mvx, mvy);
    }
    if (inb) {
        if (s_hpc_band > 0)
            hpc_mark_q(c->hp[1], fy * 4 + fx, c->bx + (mvx >> 2),
                       c->by + (mvy >> 2), c->w, c->h);
        build_pred_hpel(pred, c->hp[0], c->hp[1], c->hp[2], c->hp[3], c->rs,
                        c->bx + (mvx >> 2), c->by + (mvy >> 2), fx, fy, c->w, c->h);
    } else {
        y264_mc_luma(pred, 16, c->ref, c->rs, c->pw, c->ph, c->bx, c->by,
                     mvx, mvy, c->w, c->h);
    }
    int d = metric ? satd_blk(c->src, c->ss, pred, 16, c->w, c->h)
                   : sad_blk(c->src, c->ss, pred, 16, c->w, c->h);
    return d + mv_cost_ctx(c, mvx, mvy);
}

/* SATD-scored sub-pel probe (back-compat wrapper). */
static int probe_sub(const me_ctx *c, int mvx, int mvy)
{
    return probe_sub_m(c, mvx, mvy, 1);
}

/* Batched SAD-scored half-pel probes. Eligible only when every candidate is a
 * pure half/integer position (fx, fy even) inside the bordered plane window --
 * then each probe is probe_sub_m's F1 zero-copy branch, a strided read of one
 * plane, and the 4 SADs batch exactly like the integer probes (same kernel
 * family, same mv_cost). Returns 1 with cost[] filled, 0 = caller probes
 * singly (any interpolated/out-of-window candidate disables the whole group,
 * keeping the code one branch per group). */
static int probe_hpel_x4(const me_ctx *c, int n, const int (*mv)[2], int *cost)
{
    if (!c->sadx4_fn || !c->hp[1] || !c->f1)
        return 0;
    const pixel *r[8];
    if (n > 8) return 0;
    for (int k = 0; k < n; k++) {
        int mx = mv[k][0], my = mv[k][1];
        int fx = mx & 3, fy = my & 3;
        if ((fx & 1) || (fy & 1) || mx < c->sxlo || mx > c->sxhi ||
            my < c->sylo || my > c->syhi)
            return 0;
        r[k] = c->hp[qpel_plane_a[fy * 4 + fx]]
             + (ptrdiff_t)(c->by + (my >> 2)) * c->rs + c->bx + (mx >> 2);
        if (s_hpc_band > 0)
            hpc_mark_q(c->hp[1], fy * 4 + fx, c->bx + (mx >> 2),
                       c->by + (my >> 2), c->w, c->h);
    }
    int k = 0;
    for (; k + 4 <= n; k += 4) {
        NLED(probe_sub, 4); NLED(getref_ptr, 4);
        int s[4];
        c->sadx4_fn(c->src, c->ss, r[k], r[k + 1], r[k + 2], r[k + 3], c->rs, s);
        for (int t = 0; t < 4; t++)
            cost[k + t] = s[t] + mv_cost_ctx(c, mv[k + t][0], mv[k + t][1]);
    }
    for (; k < n; k++) {
        NLED(probe_sub, 1); NLED(getref_ptr, 1);
        cost[k] = c->sad_fn(c->src, c->ss, r[k], c->rs)
                + mv_cost_ctx(c, mv[k][0], mv[k][1]);
    }
    return 1;
}

/* On by default: skipping the UMH wide grid for 8x8-and-smaller shapes is
 * ~6-7% faster single-thread and BD-neutral-to-better on VMAF-NEG across the
 * motion corpus (stefan -0.75%, foreman -0.15%, bus +0.21%, mobile -0.18%,
 * coastguard -0.06%). Set Y264_ME_SMALL_NOUMH=0 to restore the full grid. */
static int me_small_noumh(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_ME_SMALL_NOUMH"); v = e ? atoi(e) : 1; }
    return v;
}

/* Legacy hex-only predicate for the oracle diagnostic below: true when the wide
 * grid is off, so the oracle can re-run it as a reference. (The shipping gates
 * use umh_allowed / y264_me_hex_features above.) */
static int no_umh(void)
{
    return !umh_allowed();
}

/* ORACLE-SEED DIAGNOSTIC (Y264_HEX_ORACLE, only meaningful with Y264_NO_UMH=1):
 * after hex converges, run the UMH cross+grid as an oracle, and if it finds a
 * better integer MV, re-run the hex loop seeded from it. Downstream (square
 * refine, subpel, mode/RD) stays the hex-only path. If this closes the bus gap,
 * hex's deficiency is SEARCH REACH/SEED; if not, it is downstream of the
 * integer search. Experiment-only; never on in any shipping config. */
static int hex_oracle(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_HEX_ORACLE"); v = e ? atoi(e) : 0; }
    return v;
}

/* Frame-type flag for the oracle attribution (1=all, 2=P-searches only,
 * 3=B-searches only). Stamped per MB by the analysis entry points. */
void y264_me_set_isb(int b) { s_met.me_isb = b; }

/* Staircase vertical qpel cap (see me.h). INT_MAX = uncapped: every guard below
 * compares `my > s_met.me_ymax`, which can never fire then, so the default path is
 * byte-identical. TLS, set/cleared around a B slice's list-1 searches. */
void y264_me_set_ymax(int ymax_qpel) { s_met.me_ymax = ymax_qpel; }

/* UMH search radius (integer pels). Default 16 = x264's --merange default. Reducing it with
 * good seeds in place is a cheaper wide search (A/B via Y264_UMH_RANGE). */
static int umh_range(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_UMH_RANGE"); v = e ? atoi(e) : 16; }
    return v > 0 ? v : 16;
}

/* Subpel refinement pattern (see the subpel loop in y264_me_search):
 * 0 = 8-neighbour square (default), 1 = 4-point diamond, 2 = capped diamond. */
static int subpel_mode(void)
{
    /* Precedence: explicit Y264_SUBPEL env (A/B override) > preset-set value >
 * default 0 (square). env is checked once (-2 = unchecked). */
    static int env = -2;
    if (env == -2) { const char *e = getenv("Y264_SUBPEL"); env = e ? atoi(e) : -1; }
    if (env >= 0) return env;
    int s = atomic_load_explicit(&s_me_subpel, memory_order_relaxed);
    return s >= 0 ? s : 0;
}

/* Score half-pel refinement in SAD instead of SATD (x264 uses SAD for hpel,
 * SATD only for qpel -- ~2-3x cheaper per hpel probe). On by default at the
 * medium tier (subme <= 8); subme >= 9 keeps SATD so the max-quality default is
 * byte-identical. Y264_HPEL_SAD forces on(1)/off(0) for A/B. */
static int hpel_sad(void)
{
    static int env = -2;
    if (env == -2) { const char *e = getenv("Y264_HPEL_SAD"); env = e ? atoi(e) : -1; }
    if (env >= 0) return env;
    return atomic_load_explicit(&s_me_subme, memory_order_relaxed) <= 8;
}

int y264_me_search(const pixel *src, int ss,
                   const pixel *ref, int rs, int pw, int ph,
                   int bx, int by, int w, int h,
                   int pmvx, int pmvy, int lambda,
                   const int *seeds, int nseeds,
                   int *out_mvx, int *out_mvy)
{
    /* Consume the oracle single-shot: it applies to exactly this search, so no
 * stale prior leaks into sub-partition / B / other-ref searches that never set it. */
    int  orc_v = s_met.orc_valid; long orc_c = s_met.orc_cost;
    int  orc_mx = s_met.orc_mvx, orc_my = s_met.orc_mvy;
    s_met.orc_valid = 0;

    /* Resolve this reference's precomputed half-pel planes (once per search). */
    const pixel *hp_h = NULL, *hp_v = NULL, *hp_c = NULL;
    if (s_met.hpel && rs == s_met.hpel_stride) {
        for (int i = 0; i < s_met.hpel_n; i++)
            if (s_met.hpel[i].ref == ref) {
                hp_h = s_met.hpel[i].h; hp_v = s_met.hpel[i].v; hp_c = s_met.hpel[i].c;
                break;
            }
    }

    /* Fullpel-align a qpel MV before probing it as an integer start. The default
 * (UMH) path truncates toward -inf (& ~3); x264's hex search rounds to nearest
 * (FPEL = (mv+2)>>2). Truncation places a negative-MV seed up to 1px too far
 * out -- systematic on radial/zoom motion (bus), where half the MVs are
 * negative -- so hex can land in the wrong basin. Match x264's rounding on the
 * hex-only path; keep truncation on the default path (byte-identity). */
    int align_round = y264_me_hex_features();
#define FPEL_ALIGN(mv) (align_round ? (((mv) + 2) >> 2) * 4 : ((mv) & ~3))   /* *4: mv<0 (UB on <<) */

    /* Hoist the constant search state: shape dispatch, rate state, and the two
 * safe MV rectangles, all resolved once per search instead of per probe. */
    int pu = resolve_pu(w, h);
    me_ctx mc = { src, ss, ref, rs, pw, ph, bx, by, w, h,
                  pu >= 0 ? y264_dsp.sad[pu] : NULL,
                  pu >= 0 ? y264_dsp.sad_x4[pu] : NULL,
                  pmvx, pmvy, lambda, NULL,
                  0, 0, 0, 0, { ref, hp_h, hp_v, hp_c }, 0, 0, 0, 0,
                  f1_zerocopy() };
    {
        const uint16_t *lt = (lambda > 0 && lambda <= ME_LAMBDA_MAX)
            ? atomic_load_explicit(&s_cmv_tab[lambda], memory_order_acquire)
            : NULL;
        mc.cmv = lt ? lt + (MVBITS_N - 1) : NULL;
        /* Integer fast window: mv (qpel) range where the direct-read SAD is
 * legal. == sad_int's old per-probe test (B only with the F3 border). */
        const int Bi = f3_border() ? Y264_LUMA_BORDER : 0;
        mc.ixlo = 4 * (-Bi - bx);          mc.iylo = 4 * (-Bi - by);
        mc.ixhi = 4 * (pw + Bi - w - bx) + 3;
        mc.iyhi = 4 * (ph + Bi - h - by) + 3;
        /* Subpel plane window: the +1-widened read (2-tap average) must stay
 * inside the bordered planes; empty when no planes are registered. */
        const int Bs = Y264_LUMA_BORDER;
        if (hp_h) {
            mc.sxlo = 4 * (-Bs - bx);          mc.sylo = 4 * (-Bs - by);
            mc.sxhi = 4 * (pw + Bs - w - 1 - bx) + 3;
            mc.syhi = 4 * (ph + Bs - h - 1 - by) + 3;
        } else {
            mc.sxlo = 1; mc.sxhi = 0; mc.sylo = 1; mc.syhi = 0;
        }
    }

    /* Staircase vertical cap: start candidates are CLAMPED to it, pattern
 * probes above it are SKIPPED, so bmy <= ymax is an invariant and the
 * returned MV never reads past the anchor's published rows. ymax is a
 * multiple of 4 (see me.h), so clamping keeps fpel alignment. INT_MAX
 * (default) makes every guard dead -> byte-identical. */
    const int ymax = s_met.me_ymax;

    /* Start from the two strongest guesses: zero and the predicted MV.
 *
 * Start-point dedup: an aligned start equal to one already probed returns
 * the identical cost, which cannot pass the strict-< (first-wins)
 * acceptance -- bcost has been <= that cost since its first probe -- so
 * skipping the exact duplicate changes neither the argmin nor any
 * tie-break. Duplicates are common: the zero/median/neighbour/temporal/
 * lowres seeds frequently coincide after fpel alignment. */
    int bmx = 0, bmy = 0;
    int bcost = probe_int(&mc, 0, 0);
    int psx = FPEL_ALIGN(pmvx), psy = FPEL_ALIGN(pmvy);
    if (psy > ymax) psy = ymax;
    int spt[10][2] = { { 0, 0 } }, nspt = 1;    /* probed starts; 2+nseeds <= 10 */
    if (psx | psy) {
        int pcost = probe_int(&mc, psx, psy);
        if (pcost < bcost) { bcost = pcost; bmx = psx; bmy = psy; }
        spt[nspt][0] = psx; spt[nspt][1] = psy; nspt++;
    }

    /* Extra seeds (spatial-neighbour MVs): the median predictor can be skewed
 * when the true motion matches one neighbour, so probe each as an integer
 * start too. Rate is still centred on pmv. */
    for (int s = 0; s < nseeds; s++) {
        int sx = FPEL_ALIGN(seeds[2 * s]), sy = FPEL_ALIGN(seeds[2 * s + 1]);
        if (sy > ymax) sy = ymax;
        int dup = 0;
        for (int k = 0; k < nspt; k++)
            if (spt[k][0] == sx && spt[k][1] == sy) { dup = 1; break; }
        if (dup) continue;
        if (nspt < 10) { spt[nspt][0] = sx; spt[nspt][1] = sy; nspt++; }
        int c = probe_int(&mc, sx, sy);
        if (c < bcost) { bcost = c; bmx = sx; bmy = sy; }
    }
#undef FPEL_ALIGN

    /* Integer-pel search: a hexagon pattern (x264-style) covers more ground per
 * step than a 4-point diamond and escapes the local minima it stalls in;
 * quarter-pel units, so one integer pel is 4. */
    static const int hx[6] = { -8, -4,  4,  8,  4, -4 };
    static const int hy[6] = {  0, -8, -8,  0,  8,  8 };
    /* Early-out accounting (Y264_ME_ETSTAT): the seed result is the state an
 * early-out would return, so everything from here to the end of the integer
 * stage is the work it would delete. */
    const int etst = me_et_stat_on();
    const int et_seed = bcost, et_sx = bmx, et_sy = bmy;
    long etp = 0;
    /* Y264_ME_ET: the seed is already good enough per pixel -- skip the integer
 * refinement (hex, wide grid, terminal diamond) and go straight to subpel.
 * K = 0 leaves this dead, so the default is byte-identical. */
    const int et_k = s_met.stq ? 0 : me_et_k();   /* stq = the family escape */
    const int et_shape = me_et_shape();
    const int et_small = (w < 16 && h < 16);
    const int et_ok = et_shape == 0 || (et_shape == 1 ? et_small : !et_small);
    /* Effective K for THIS shape: 16x16 takes its own threshold when armed
 * (Y264_ET16), else both classes run the shared Y264_ME_ET/SHAPE gate.
 * Y264_ME_ET=0 is the FAMILY escape: it kills the 16x16 exit too, so one
 * env disarms the whole family (a 16x16-only sweep is ME_ET=<K> SHAPE=2). */
    int et_keff = et_ok ? et_k : 0;
    if (et_k && !et_small) { int k16 = me_et_k16(); if (k16) et_keff = k16; }
    const int et_hit = et_keff && !s_met.me_et_off
                     && (me_et_ft() == 7 || (me_et_ft() & s_met.et_class))
                     && (long)bcost <= (long)et_keff * w * h;
    int improved = !et_hit, iters = 0;
    int hexdir = -1;              /* arrival direction: index of the accepted move */
    while (improved && iters++ < 32) {
        improved = 0;
        int cx = bmx, cy = bmy;
        /* Score the whole ring first (batched x4 where in-bounds), then run
 * the ordered strict-< comparisons -- identical decisions, since a
 * probe's cost never depends on the running best.
 *
 * Direction-tracked dedup : after a move along
 * hx[d], only vertices {d-1, d, d+1} are NEW positions; the other
 * three are ring points of an earlier iteration (hx[d]+hx[d+/-2] ==
 * hx[d+/-1], hx[d]+hx[d+3] == 0 -> the old centre), i.e. positions
 * whose cost was already evaluated. Any evaluated position has cost
 * >= bcost (bcost is the running min under strict-< acceptance), so
 * skipping the exact re-probe can change neither the argmin nor a
 * tie-break. Probe volume shrinks by re-probes only. */
        int mv[6][2], idx[6], cost[6], nc = 0;
        for (int i = 0; i < 6; i++) {
            if (hexdir >= 0) {
                int rel = (i - hexdir + 6) % 6;
                if (rel != 0 && rel != 1 && rel != 5) continue;
            }
            int mx = cx + hx[i], my = cy + hy[i];
            if (my > ymax) continue;
            mv[nc][0] = mx; mv[nc][1] = my; idx[nc] = i; nc++;
        }
        probe_int_list(&mc, nc, (const int (*)[2])mv, cost,
                       ring_in_window(&mc, cx, cy, 8));
        if (etst) etp += nc;
        for (int i = 0; i < nc; i++)
            if (cost[i] < bcost) {
                bcost = cost[i]; bmx = mv[i][0]; bmy = mv[i][1]; improved = 1;
                hexdir = idx[i];
            }
    }
    /* NOTE: a behaviour-matched 8-point square refine here does NOT replace
 * UMH. Built and BD-tested (Y264_SQ_REFINE + Y264_NO_UMH), it captures 95%
 * of UMH's near-hex SATD-gain (umh_improved 44%->3.3%) yet dropping UMH
 * still costs +15.9% BD on bus, +8.2% stefan (VMAF-NEG). Integer-SATD-gain
 * VOLUME (mostly tiny 1px near-hex moves) does NOT predict BD; UMH's few
 * RARE distant finds are the load-bearing part on motion. The wide scan is
 * not replaceable by cheap local refinement. */
    /* Capture the post-hex state (the gate would decide here). */
    int stats = me_stats_on();
    int hex_bcost = bcost, hex_bmx = bmx, hex_bmy = bmy;
    if (stats) g_ms.searches++;
    /* UMH: uneven cross + multi-hexagon grid, bounded by the search range, to
 * escape the wider local minima the local hex stalls in on high-motion
 * content. x264's UMH searches the cross fully in x but half in y (camera
 * motion is mostly horizontal); the grid is the hex pattern at each ring
 * radius, carried to the full range so erratic (diagonal, non-camera)
 * motion beyond radius 8 is still reached -- measured -0.2..-0.4% BD-PSNR on
 * the motion clips (stefan/foreman/bus/coastguard) for ~6% more ME work.
 * Integer pels in quarter-pel units (1 pel = 4). */
    const int ME_RANGE = umh_range();
    /* The UMH wide grid escapes local minima the local hex stalls in -- valuable
 * for 16x16 (no tight seed) but redundant for 8x8-and-smaller shapes, whose
 * predictor is the already-refined parent MV. Skipping UMH for small blocks
 * (env Y264_ME_SMALL_NOUMH) removes the bulk of the per-MB ME fan-out. */
    int umh = umh_allowed() && !s_met.me_cheap && !et_hit;
    if (umh && me_small_noumh() && w < 16 && h < 16) umh = 0;
    /* Thin compensation (Y264_ME_ET_CROSS=1, default 0 = byte-identical):
 * an early-out MB still runs the UMH CROSS arms -- the integer stage's
 * load-bearing part is the RARE DISTANT finds, which no local refinement
 * recovers, and the cross is the cheapest probe of that axis (a few dozen
 * line probes vs the grid's rings). Same eligibility the full UMH would
 * have had. */
    int et_cross = et_hit && me_et_cross() && umh_allowed() && !s_met.me_cheap;
    if (et_cross && me_small_noumh() && w < 16 && h < 16) et_cross = 0;
    /* Oracle diagnostic: same eligibility the real UMH would have had. */
    int orc_mode = hex_oracle();
    int umh_orc = no_umh() && orc_mode && !s_met.me_cheap && !et_hit
                  && !(me_small_noumh() && w < 16 && h < 16)
                  && !(orc_mode == 2 && s_met.me_isb) && !(orc_mode == 3 && !s_met.me_isb);
    if (umh || umh_orc || et_cross) {
        int cx = bmx, cy = bmy;
        int mv[8][2], cost[8], nc;
        /* Cross arms in the original probe order (+i, -i, +i+1, -i+1, ...),
 * batched two radii at a time; the center is fixed for the whole
 * cross, so pre-scoring cannot change any ordered comparison. */
        for (int i = 1; i <= ME_RANGE; i += 2) {
            nc = 0;
            for (int j = i; j <= ME_RANGE && j < i + 2; j++) {
                mv[nc][0] = cx + 4 * j; mv[nc][1] = cy; nc++;
                mv[nc][0] = cx - 4 * j; mv[nc][1] = cy; nc++;
            }
            probe_int_list(&mc, nc, (const int (*)[2])mv, cost, 0);
            if (etst) etp += nc;
            for (int k = 0; k < nc; k++)
                if (cost[k] < bcost) { bcost = cost[k]; bmx = mv[k][0]; bmy = mv[k][1]; }
        }
        for (int i = 1; i <= ME_RANGE / 2; i += 2) {
            nc = 0;
            for (int j = i; j <= ME_RANGE / 2 && j < i + 2; j++) {
                if (cy + 4 * j <= ymax) { mv[nc][0] = cx; mv[nc][1] = cy + 4 * j; nc++; }
                mv[nc][0] = cx; mv[nc][1] = cy - 4 * j; nc++;
            }
            probe_int_list(&mc, nc, (const int (*)[2])mv, cost, 0);
            if (etst) etp += nc;
            for (int k = 0; k < nc; k++)
                if (cost[k] < bcost) { bcost = cost[k]; bmx = mv[k][0]; bmy = mv[k][1]; }
        }
        cx = bmx; cy = bmy;
        for (int r = 1; !et_cross && r <= ME_RANGE; r++) {
            nc = 0;
            for (int i = 0; i < 6; i++) {
                int mx = cx + hx[i] * r, my = cy + hy[i] * r;
                if (my > ymax) continue;
                mv[nc][0] = mx; mv[nc][1] = my; nc++;
            }
            probe_int_list(&mc, nc, (const int (*)[2])mv, cost, 0);
            if (etst) etp += nc;
            for (int k = 0; k < nc; k++)
                if (cost[k] < bcost) { bcost = cost[k]; bmx = mv[k][0]; bmy = mv[k][1]; }
        }
    }
    /* Oracle diagnostic: if the grid found a better basin, RE-RUN the hex loop
 * from it (hex "given the right seed"), then continue as pure hex-only. */
    if (umh_orc && (bmx != hex_bmx || bmy != hex_bmy)) {
        improved = 1; iters = 0;
        while (improved && iters++ < 32) {
            improved = 0;
            int cx = bmx, cy = bmy;
            for (int i = 0; i < 6; i++) {
                int mx = cx + hx[i], my = cy + hy[i];
                if (my > ymax) continue;
                int c = probe_int(&mc, mx, my);
                if (c < bcost) { bcost = c; bmx = mx; bmy = my; improved = 1; }
            }
        }
    }
    /* Record what UMH bought and where -- the gate-setting distribution. */
    if (stats && umh) {
        int umh_imp = bcost < hex_bcost;
        g_ms.umh_ran++; g_ms.umh_improved += umh_imp;
        if (umh_imp) {
            int px = (abs(bmx - hex_bmx) + abs(bmy - hex_bmy)) >> 2;  /* qpel->pel */
            int b = umhdist_bucket(px);
            g_ms.umhdist_n[b]++;
            g_ms.umhdist_gain[b] += (double)(hex_bcost - bcost);
        }
        if (w == 16 && h == 16 && orc_v && orc_c > 0) {
            g_ms.s16_oracle++;
            int rb = (int)((long)hex_bcost * 4 / orc_c);
            if (rb < 0) rb = 0; if (rb >= ORC_RB) rb = ORC_RB - 1;
            int db = dist_bucket(abs(hex_bmx - orc_mx) + abs(hex_bmy - orc_my));
            g_ms.cell_n[rb][db]++;
            g_ms.cell_umh_imp[rb][db] += umh_imp;
            g_ms.cell_umh_gain[rb][db] += (double)(hex_bcost - bcost);
        }
    }

    /* Small-diamond finish: refine to the nearest integer-pel minimum. */
    static const int dx[4] = { 4, -4, 0, 0 };
    static const int dy[4] = { 0, 0, 4, -4 };
    /* Hex-only (Y264_NO_UMH) mirrors the terminal 8-point SQUARE of x264's hex search
 * refine: the axis diamond misses the 4 diagonal corners, so a MV whose true
 * minimum sits diagonally off the hex vertex is left coarse. Add the corners
 * on the no-UMH path only, so the default (UMH) finish stays byte-identical
 * (the UMH grid already covers the diagonals). */
    static const int sqx[4] = { 4, 4, -4, -4 };
    static const int sqy[4] = { 4, -4, 4, -4 };
    int sq_refine = y264_me_hex_features();
    /* et_cross: if the cross escaped the seed's basin, polish the landing with
 * the diamond; a seed the cross could not beat keeps the pure early-out. */
    improved = !et_hit || (et_cross && (bmx != et_sx || bmy != et_sy)); iters = 0;
    /* Dedup vs the previous iteration's ring: with the square on, every 4-grid
 * position within Chebyshev 4 of the previous centre was evaluated there
 * (or inherited evaluated by induction); diamond-only, the axis neighbours
 * + centre were. Evaluated positions have cost >= bcost (running min,
 * strict-< acceptance), so skipping the exact re-probe changes nothing.
 * ymax-skipped positions are skipped again by the same guard first. */
    int pfx = 0, pfy = 0, have_prev = 0;
#define FIN_PROBED(mx, my) (have_prev && (sq_refine \
        ? (abs((mx) - pfx) <= 4 && abs((my) - pfy) <= 4) \
        : (abs((mx) - pfx) + abs((my) - pfy) <= 4)))
    while (improved && iters++ < 8) {
        improved = 0;
        int cx = bmx, cy = bmy;
        int mv[4][2], cost[4], nc = 0;
        for (int i = 0; i < 4; i++) {
            int mx = cx + dx[i], my = cy + dy[i];
            if (my > ymax) continue;
            if (FIN_PROBED(mx, my)) continue;
            mv[nc][0] = mx; mv[nc][1] = my; nc++;
        }
        probe_int_list(&mc, nc, (const int (*)[2])mv, cost,
                       ring_in_window(&mc, cx, cy, 4));
        if (etst) etp += nc;
        for (int i = 0; i < nc; i++)
            if (cost[i] < bcost) {
                bcost = cost[i]; bmx = mv[i][0]; bmy = mv[i][1]; improved = 1;
            }
        if (sq_refine) {
            nc = 0;
            for (int i = 0; i < 4; i++) {
                int mx = cx + sqx[i], my = cy + sqy[i];
                if (my > ymax) continue;
                if (FIN_PROBED(mx, my)) continue;
                mv[nc][0] = mx; mv[nc][1] = my; nc++;
            }
            probe_int_list(&mc, nc, (const int (*)[2])mv, cost,
                           ring_in_window(&mc, cx, cy, 4));
            if (etst) etp += nc;
            for (int i = 0; i < nc; i++)
                if (cost[i] < bcost) {
                    bcost = cost[i]; bmx = mv[i][0]; bmy = mv[i][1]; improved = 1;
                }
        }
        pfx = cx; pfy = cy; have_prev = 1;
    }
#undef FIN_PROBED

    /* Early-out accounting: bucket this search by how good its best SEED was,
 * per pixel, and record what the integer stages after it bought. */
    if (etst) {
        int area = w * h;
        int b = area ? (int)((long)et_seed / area) : ETB - 1;
        if (b < 0) b = 0;
        if (b >= ETB) b = ETB - 1;
        int d = (abs(bmx - et_sx) + abs(bmy - et_sy)) >> 2;   /* qpel -> integer */
        g_et.n[b]++;
        g_et.probes[b] += etp;
        g_et.gain[b] += (double)(et_seed - bcost);
        if (bmx != et_sx || bmy != et_sy) g_et.moved[b]++;
        g_et.dist[b] += d;
        if (d > 2) g_et.far[b]++;
    }

    /* Half-pel then quarter-pel refinement around the integer best. The subpel
 * stage scores in SATD (probe_sub); re-evaluate the integer optimum in
 * SATD first so candidates compare on the same metric.
 *
 * Pattern is env-selectable (Y264_SUBPEL) for the speed/quality trade:
 * 0 = 8-neighbour square iterated to convergence (default, max quality);
 * 1 = 4-point diamond iterated to convergence (half the probes/iter, still
 * follows a moving minimum -- the diagonal is reached over two axis
 * steps unless the surface has a strict diagonal valley);
 * 2 = 4-point diamond capped at 2 iterations/level (x264 subme-7 subpel).
 * The 8-neighbour corners rarely beat the axis neighbours, so the diamond is
 * a near-neutral efficiency win; the cap is the x264-speed trade. */
    int sp = subpel_mode();
    if (s_met.me_cheap && sp < 2) sp = 2;   /* cheap frame: capped diamond */
    if (sp == 0 && !hpel_sad()) {
        /* Default max-quality path (verbatim original): 8-neighbour square,
 * iterated to convergence, SATD at both levels. Byte-identical. */
        bcost = probe_sub(&mc, bmx, bmy);
        for (int step = 2; step >= 1; step--) {
            int moved = 1, sit = 0;
            /* Per-level square dedup, same proof as the integer finish: every
 * step-grid position within Chebyshev `step` of the previous
 * centre was evaluated last iteration (or inherited evaluated),
 * and evaluated costs are >= bcost -- exact re-probe removal. */
            int psx2 = 0, psy2 = 0, havep = 0;
            while (moved && sit++ < 8) {
                moved = 0;
                int cbx = bmx, cby = bmy;
                for (int j = -1; j <= 1; j++)
                    for (int i = -1; i <= 1; i++) {
                        if (!i && !j) continue;
                        int mx = cbx + i * step, my = cby + j * step;
                        if (my > ymax) continue;
                        if (havep && abs(mx - psx2) <= step && abs(my - psy2) <= step)
                            continue;
                        int c = probe_sub(&mc, mx, my);
                        if (c < bcost) { bcost = c; bmx = mx; bmy = my; moved = 1; }
                    }
                psx2 = cbx; psy2 = cby; havep = 1;
            }
            if (stats) g_ms.subpel_iters[step == 2 ? 0 : 1][sit > 8 ? 8 : sit]++;
        }
    } else {
        /* Env-selected variants: hpel in SAD (Y264_HPEL_SAD), 4-point diamond
 * (Y264_SUBPEL=1) or capped diamond (=2). Each level re-scores the best
 * in its own metric before comparing. */
        int maxit = sp == 2 ? 2 : 8;
        int hp_metric = hpel_sad() ? 0 : 1;
        static const int diaX[4] = { 1, -1, 0, 0 };
        static const int diaY[4] = { 0, 0, 1, -1 };
        for (int step = 2; step >= 1; step--) {
            int metric = step == 2 ? hp_metric : 1;   /* qpel always SATD */
            bcost = probe_sub_m(&mc, bmx, bmy, metric);
            /* x264's half-pel threshold: entering the qpel
 * level, bcost is the hpel-best re-scored in SATD -- exactly what
 * x264 gates on. Skip qpel for candidates already 8/7 worse than the
 * MB's best hpel; otherwise lower the bar. The returned bcost stays
 * SATD-comparable either way (the re-score above already ran). */
            if (step == 1 && hpel_thresh_on()) {
                int *ht = &s_met.hpel_thresh[s_met.me_list];
                if ((((long)bcost * 7) >> 3) > *ht) break;
                if (bcost < *ht) *ht = bcost;
            }
            int moved = 1, sit = 0;
            /* Per-level dedup vs the previous centre's ring (square: Chebyshev
 * <= step; diamond: L1 <= step on the step grid == the axis
 * neighbours + centre). Same exact-re-probe proof as the integer
 * loops; metric is constant within a level, so evaluated costs
 * stay comparable and >= bcost. */
            int pdx = 0, pdy = 0, havep = 0;
            while (moved && sit++ < maxit) {
                moved = 0;
                int cbx = bmx, cby = bmy;
                /* Gather the pattern's candidates, score them all (batched
 * x4 on the SAD-scored half-pel level when every candidate is
 * a pure plane read), then run the ordered comparisons --
 * decisions identical to the one-by-one loop. */
                int mvs[8][2], costs[8], nc = 0;
                if (sp == 0) {
                    for (int j = -1; j <= 1; j++)
                        for (int i = -1; i <= 1; i++) {
                            if (!i && !j) continue;
                            int mx = cbx + i * step, my = cby + j * step;
                            if (my > ymax) continue;
                            if (havep && abs(mx - pdx) <= step && abs(my - pdy) <= step)
                                continue;
                            mvs[nc][0] = mx; mvs[nc][1] = my; nc++;
                        }
                } else {
                    for (int k = 0; k < 4; k++) {
                        int mx = cbx + diaX[k] * step, my = cby + diaY[k] * step;
                        if (my > ymax) continue;
                        if (havep && abs(mx - pdx) + abs(my - pdy) <= step)
                            continue;
                        mvs[nc][0] = mx; mvs[nc][1] = my; nc++;
                    }
                }
                if (metric != 0 ||
                    !probe_hpel_x4(&mc, nc, (const int (*)[2])mvs, costs))
                    for (int k = 0; k < nc; k++)
                        costs[k] = probe_sub_m(&mc, mvs[k][0], mvs[k][1], metric);
                for (int k = 0; k < nc; k++)
                    if (costs[k] < bcost) {
                        bcost = costs[k]; bmx = mvs[k][0]; bmy = mvs[k][1]; moved = 1;
                    }
                pdx = cbx; pdy = cby; havep = 1;
            }
            if (stats) g_ms.subpel_iters[step == 2 ? 0 : 1][sit > 8 ? 8 : sit]++;
        }
    }

    *out_mvx = bmx;
    *out_mvy = bmy;
    return bcost;
}

static void me_et_stats_dump(void)
{
    long tn = 0, tp = 0;
    for (int b = 0; b < ETB; b++) { tn += g_et.n[b]; tp += g_et.probes[b]; }
    if (!tn) return;
    fprintf(stderr, "\n=== Y264_ME_ETSTAT: integer-search work after the seeds ===\n");
    fprintf(stderr, "searches=%ld  post-seed integer probes=%ld (%.1f/search)\n",
            tn, tp, (double)tp / tn);
    fprintf(stderr, "  READ THE MV COLUMNS, NOT THE GAIN. The R5 note at the hex loop\n"
                    "  records that integer-SATD-gain volume does not predict BD.\n");
    fprintf(stderr, "seed cost/px | searches   cum%%  | probes  cum%% of all | moved  mean-px  >2px\n");
    long cn = 0, cp = 0;
    for (int b = 0; b < ETB; b++) {
        if (!g_et.n[b]) continue;
        cn += g_et.n[b]; cp += g_et.probes[b];
        fprintf(stderr, "%5d-%-5d  | %8ld %5.1f%% | %7ld %6.1f%%      | %4.1f%% %7.2f %5.1f%%\n",
                b, b + 1, g_et.n[b], 100.0 * cn / tn,
                g_et.probes[b], tp ? 100.0 * cp / tp : 0.0,
                100.0 * g_et.moved[b] / g_et.n[b],
                (double)g_et.dist[b] / g_et.n[b],
                100.0 * g_et.far[b] / g_et.n[b]);
    }
    fprintf(stderr, "=== end ME early-out stats ===\n\n");
}

void y264_me_stats_dump(void)
{
    me_et_stats_dump();
    if (!me_stats_on()) return;
    fprintf(stderr, "\n=== Y264_ME_STATS (E2, adaptive-me-design.md) ===\n");
    fprintf(stderr, "searches=%ld  umh_ran=%ld  umh_improved=%ld (%.1f%%)  16x16-oracle=%ld\n",
            g_ms.searches, g_ms.umh_ran, g_ms.umh_improved,
            g_ms.umh_ran ? 100.0 * g_ms.umh_improved / g_ms.umh_ran : 0.0, g_ms.s16_oracle);
    fprintf(stderr, "\nG1 gate table: 16x16-with-oracle searches, by (hex_bcost/oracle) x |hex_mv-oracle_mv|.\n");
    fprintf(stderr, "  cell = N / %%UMH-improved / mean-gain. Skip UMH where %%improved ~ 0.\n");
    fprintf(stderr, "  ratio\\dist  |  =0      <=4     <=8     <=16    <=32    >32\n");
    for (int rb = 0; rb < ORC_RB; rb++) {
        long rn = 0; for (int db = 0; db < ORC_DB; db++) rn += g_ms.cell_n[rb][db];
        if (!rn) continue;
        fprintf(stderr, "  %4.2f-%-4.2f  |", rb / 4.0, (rb + 1) / 4.0);
        for (int db = 0; db < ORC_DB; db++) {
            long n = g_ms.cell_n[rb][db];
            if (!n) { fprintf(stderr, "  .          "); continue; }
            fprintf(stderr, " %5ld/%2.0f%%/%-4.0f", n,
                    100.0 * g_ms.cell_umh_imp[rb][db] / n, g_ms.cell_umh_gain[rb][db] / n);
        }
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "\nG2 subpel iterations used (of budget): [hpel] then [qpel]\n");
    for (int lv = 0; lv < 2; lv++) {
        long tot = 0; for (int k = 0; k < 9; k++) tot += g_ms.subpel_iters[lv][k];
        if (!tot) continue;
        fprintf(stderr, "  %s:", lv == 0 ? "hpel" : "qpel");
        for (int k = 0; k < 9; k++)
            if (g_ms.subpel_iters[lv][k])
                fprintf(stderr, " %d:%.0f%%", k, 100.0 * g_ms.subpel_iters[lv][k] / tot);
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "\nR5: UMH gain by |final-hex| distance (integer pels). "
                    "NEAR=local-search-fixable, FAR=separated-minimum-intrinsic.\n");
    double gtot = 0; long ntot = 0;
    for (int b = 0; b < 6; b++) { gtot += g_ms.umhdist_gain[b]; ntot += g_ms.umhdist_n[b]; }
    static const char *bl[6] = { "0px", "1px", "2px", "<=4px", "<=8px", ">8px" };
    fprintf(stderr, "  dist    %%of-improved-MBs   %%of-total-SATD-gain\n");
    for (int b = 0; b < 6; b++)
        fprintf(stderr, "  %-6s  %6.1f%%            %6.1f%%\n", bl[b],
                ntot ? 100.0 * g_ms.umhdist_n[b] / ntot : 0.0,
                gtot ? 100.0 * g_ms.umhdist_gain[b] / gtot : 0.0);
    fprintf(stderr, "  (NEAR = 0..2px: %.1f%% of gain; FAR = >4px: %.1f%% of gain)\n",
            gtot ? 100.0 * (g_ms.umhdist_gain[0] + g_ms.umhdist_gain[1] + g_ms.umhdist_gain[2]) / gtot : 0.0,
            gtot ? 100.0 * (g_ms.umhdist_gain[4] + g_ms.umhdist_gain[5]) / gtot : 0.0);
    fprintf(stderr, "=== end ME stats ===\n\n");
}

/* Resolve every env-gated lazy static in the motion search ONCE, on the calling
 * (main) thread, before any worker runs -- same contract as
 * y264_mb_warm_statics, which calls this. Afterwards the search threads only
 * READ them, so there is no first-use race across GOP workers / the wavefront. */
void y264_me_warm_statics(void)
{
    (void)no_umh_env(); (void)hpel_thresh_on(); (void)me_stats_on();
    (void)me_et_stat_on(); (void)me_et_k(); (void)me_et_shape();
    (void)me_et_k16(); (void)me_et_ft(); (void)me_et_cross();
    (void)f3_border(); (void)f1_zerocopy(); (void)me_small_noumh();
    (void)hex_oracle(); (void)umh_range(); (void)subpel_mode(); (void)hpel_sad();
    (void)y264_hpel_census_on(); (void)hpel_list_on();
}
