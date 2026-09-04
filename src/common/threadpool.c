/*
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Persistent worker pool + deterministic MB-row wavefront. See threadpool.h.
 *
 * Multi-frame: ONE worker set serves a small table of concurrent JOBS
 * (wavefronts or parallel-fors). Each job keeps its own in-job wavefront
 * dependency state (`progress`) plus an optional external per-row claim gate
 * (`row_ready`, e.g. the staircase's published-anchor-rows watermark). An idle
 * worker claims the next READY row from the OLDEST job that has one -- a row
 * is ready when its external gate is open and the row above has produced its
 * first cells -- so frames drain oldest-first and a gate-blocked frame never
 * parks a worker that another frame could use.
 *
 * PARKING (what makes the overlap real): when a claimed row stalls mid-row on
 * the row above and other work is runnable, the worker parks the row as a
 * resumable continuation (row, column, needed progress) and serves the other
 * frame; the wavefront's lag-chain bubbles become cross-frame throughput
 * instead of sleep. A parked row is WORKER-BOUND: the analyze run functions
 * keep row-carried per-worker state (the CABAC est_ctx advances across a row
 * in the worker's private engine copy), so only the owning worker may resume
 * it, and while it holds a parked row of job J it takes no other unit of J
 * (which would clobber that state). Re-entering a job after running another
 * one triggers `thread_attach` (re-install cheap thread-locals, e.g. the ME
 * hpel registry) -- never `thread_init`, which would reset row-carried state;
 * jobs providing thread_init but no thread_attach are simply never parked.
 *
 * Within a job nothing changes: rows are claimed in increasing order and
 * processed left-to-right; before cell (r,c) the worker waits until row r-1
 * has completed c+2 cells, tracked in the job's atomic `progress` array with
 * the per-row wake-slot + seq_cst-fence discipline that fixed the 9b6251f
 * lost wakeup (see run_row_from). With a single registered job there is never
 * an alternative unit, so nothing parks and this reduces exactly to the v1
 * wavefront -- same claim order, same cell schedule, byte-identical output
 * (worker identity never reaches the bitstream -- the determinism invariant).
 */
#include "common/threadpool.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <time.h>

/* Debug counters (Y264_NTP_STATS=1): printed at pool destroy. Relaxed atomics,
 * negligible cost, no behavioural effect. */
static _Atomic long st_parks, st_resumes, st_hint_bc, st_start_sig, st_inplace,
                    st_claims, st_rowfast, st_idle_sleeps,
                    st_spin_row, st_spin_row_hit, st_spin_idle, st_spin_idle_hit;

/* Mid-row parking (Y264_NTP_PARK=1, default OFF): measured a NET LOSS on both
 * live multi-frame topologies (fpipe pair t18: 4.29 -> 4.47 s; staircase t18:
 * 4.31 -> 4.76 s park_joy 500f) -- catch-up stalls are ~one cell long, and a
 * park/switch/resume round-trip (two lock+scan passes, cold row state, and on
 * the staircase a delayed producer row on the trailer's critical path) costs
 * more than the bubble it fills. Kept env-gated for future topologies where
 * stalls are long; scheduling-only, cannot change bits. */
static int park_on(void)
{
    static _Atomic int v = -1;
    int x = atomic_load_explicit(&v, memory_order_relaxed);
    if (x < 0) {
        const char *e = getenv("Y264_NTP_PARK");
        x = e ? (atoi(e) ? 1 : 0) : 0;
        atomic_store_explicit(&v, x, memory_order_relaxed);
    }
    return x;
}

/* Y264_NTP_STATS=1: debug counters at pool destroy. Same atomic lazy-static
 * pattern as park_on -- two GOP workers' destroys race a plain-int version
 * (same-value init; the TSan floor is 0). Resolved at create (main thread). */
static int ntp_stats_on(void)
{
    static _Atomic int v = -1;
    int x = atomic_load_explicit(&v, memory_order_relaxed);
    if (x < 0) {
        const char *e = getenv("Y264_NTP_STATS");
        x = e ? (atoi(e) ? 1 : 0) : 0;
        atomic_store_explicit(&v, x, memory_order_relaxed);
    }
    return x;
}

/* --- Y264_NTP_PROF=1: per-worker wait accounting (zero cost when off: every
 * instrumented path is behind one `p->prof` int test; when on, the cost is two
 * clock_gettime calls per ROW / per idle sleep / per actual stall -- never per
 * cell). The point is ATTRIBUTION: at 18 threads the wall is wait-dominated,
 * and "idle" must be split by WHY nothing was claimable at sleep entry:
 * gate -- a live job's next row exists but its external row_ready gate is
 * closed (the staircase's published-anchor watermark),
 * ramp -- next row exists, gate open, but the row above hasn't produced the
 * top-right dependency yet (wavefront ramp / dependency slope),
 * tail -- live jobs exist but every row is claimed (wavefront tail: the
 * last rows are still running on other workers),
 * nojob -- the table is empty or all-done: the pool is starved by the
 * SERIAL phase between submissions.
 * Plus the mid-row in-place stall (the wake-slot wait inside a claimed row)
 * and, per job TAG (ntp_prof_tag at the submission site), busy time, rows,
 * stall time, submitter join time and job span. Same lazy-static env pattern
 * as park_on; resolved at pool/bg create (main thread) per the warm rule. */
static int ntp_prof_env(void)
{
    static _Atomic int v = -1;
    int x = atomic_load_explicit(&v, memory_order_relaxed);
    if (x < 0) {
        const char *e = getenv("Y264_NTP_PROF");
        x = e ? atoi(e) : 0;             /* 2 = also histogram every cell */
        if (x < 0) x = 0;
        atomic_store_explicit(&v, x, memory_order_relaxed);
    }
    return x;
}

static uint64_t prof_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

enum { NPW_GATE, NPW_RAMP, NPW_TAIL, NPW_NOJOB, NPW_NCLASS };

/* --- mid-row sub-attribution -------------------------------------------
 * "midrow" as a single number covers three different problems, and the fix for
 * each is different: an ENTRY stall (the row was claimed while the row above
 * was barely ahead, so it blocks on its first cells -- a claim-order question)
 * vs a CATCH-UP stall later in the row (the producer fell behind -- a
 * dependency-slope question) vs the WAKE ROUND-TRIP itself (the sleeper's
 * condvar latency AFTER the dependency was already satisfied -- the only part
 * a faster wait primitive can recover). The split is per stall event, with
 * duration histograms, the resolution path (spun / raced / slept / parked) and
 * -- on the sleep path -- the producer's signal timestamp, which decomposes a
 * slept stall into dep-wait (before the signal) + wake latency (after it). */
enum { NSS_ENTRY, NSS_MID, NSS_NSITE };

#define NTP_HB 16                 /* log2 histogram, bucket 0 = [0, 250 ns) */
static inline int hbucket(uint64_t ns)
{
    int b = 0;
    for (uint64_t v = ns / 250; v && b < NTP_HB - 1; v >>= 1)
        b++;
    return b;
}
static inline double hb_upper_us(int b)
{
    return b >= NTP_HB - 1 ? 1e9 : 0.25 * (double)(1u << b);
}
/* cells the row above was short by, bucketed 1,2,3,4,5-8,9-16,17-32,>32 */
static inline int defbucket(int d)
{
    if (d < 1) d = 1;
    return d <= 4 ? d - 1 : d <= 8 ? 4 : d <= 16 ? 5 : d <= 32 ? 6 : 7;
}
/* cells the row above was AHEAD at claim, bucketed 2,3,4,5-8,...,>64 */
static inline int leadbucket(int l)
{
    if (l < 2) return 0;
    return l <= 4 ? l - 2 : l <= 8 ? 3 : l <= 16 ? 4 : l <= 32 ? 5 : l <= 64 ? 6 : 7;
}

struct ntp_sitestat {
    uint64_t spin_ns, sleep_ns, park_ns, dep_ns, wake_ns;
    long     spin_hit, raced, slept, parked, wake_n;
    long     hspin[NTP_HB], hsleep[NTP_HB], hwake[NTP_HB];
    long     deficit[8];
};

/* Written only by the owning worker (read at destroy, after join). Padded so
 * two workers' counters never share a line. */
struct ntp_wprof {
    uint64_t busy_ns, midrow_ns, spin_ns;
    uint64_t idle_ns[NPW_NCLASS];
    long     midrow_cnt, idle_cnt[NPW_NCLASS];
    long     rows, claims;
    uint64_t lock_ns, pick_ns, init_ns;  /* mutex acquire; claim scan; init */
    long     init_n;
    long     hcell[NTP_HB];         /* prof >= 2: per-cell duration histogram */
    struct ntp_sitestat site[NSS_NSITE];
    long     lead[8];
    char     pad[64];
};

#define NTP_PROF_TAGS 24
struct ntp_tagstat {                 /* counters are atomics: workers add from
 * outside the mutex at row completion */
    const char      *name;
    _Atomic uint64_t busy_ns, midrow_ns, join_ns, span_ns;
    _Atomic long     jobs, rows;
};

/* One-shot label for the next job registered by this thread. */
static _Thread_local const char *ntp_tls_tag;
void ntp_prof_tag(const char *tag) { ntp_tls_tag = tag; }

/* One-shot latency-critical mark for the next job (see threadpool.h). */
static _Thread_local int ntp_tls_prio;
void ntp_prio_hint(void) { ntp_tls_prio = 1; }

/* ntp_bg wait accounting (global across all bg threads; printed with the pool
 * dump, labeled process-wide). */
static _Atomic uint64_t st_bg_wait_ns;
static _Atomic long     st_bg_syncs;

/* --- Spin-then-sleep (Y264_NTP_SPIN=<usec>, default 25, 0 = off) ------------
 * At 18 threads the wait budget measures mid-row stalls averaging ~47 us
 * against ~9 us analyze cells, and row-start (ramp) waits in the same shape:
 * the condvar wake ROUND-TRIP (~30 us on this machine), not the dependency,
 * is the binding cost of every short wait -- and on small-cell grids
 * (lookahead fme, deblock chunks) it degenerates the wavefront to
 * serial-plus-overhead. So both wait sites spin briefly before committing to
 * the condvar:
 * - a stalled row polls its progress atomic (no lock, no wake needed);
 * - an idle worker polls `work_epoch`, bumped by every producer event that
 * can open a claim (job registration, kick, a row crossing its start
 * need, the cascade), then re-scans under the mutex.
 * The budget is ~the wake round-trip: spinning longer than a sleep+wake
 * costs can never pay. Scheduling-only: claim rules, wake protocol and the
 * lost-wakeup proof are untouched (a fruitless spin falls into the same
 * register+fence+re-scan+sleep path). Workers holding parked
 * rows never spin (their sleep must stay reachable by the owner-poke).
 * Bits cannot change: worker identity and timing never reach the bitstream
 * (the pool's determinism invariant, gated as always). */
static int spin_budget_ns(void)
{
    static _Atomic int v = -1;
    int x = atomic_load_explicit(&v, memory_order_relaxed);
    if (x < 0) {
        const char *e = getenv("Y264_NTP_SPIN");
        x = (e ? atoi(e) : 25) * 1000;
        if (x < 0) x = 0;
        atomic_store_explicit(&v, x, memory_order_relaxed);
    }
    return x;
}

/* The three spin sites are priced separately because they are not the same
 * trade. A stalled ROW's spin is on the critical path -- the cell it is waiting
 * for is the next thing this worker runs, so a hit converts directly into
 * wall-clock. An IDLE worker's spin is speculative: it burns a core polling for
 * work that may never arrive, and when the grid is narrower than the pool it
 * never does. A JOIN spin burns the submitting thread while the pool it is
 * waiting on is already saturated, so it competes with its own workers.
 * Y264_NTP_SPIN sets all three; the per-site vars override it. */
#define NTP_SPIN_SITE(fn, env, dflt)                                           \
    static int fn(void)                                                        \
    {                                                                          \
        static _Atomic int v = -1;                                             \
        int x = atomic_load_explicit(&v, memory_order_relaxed);                \
        if (x < 0) {                                                           \
            const char *e = getenv(env);                                       \
            x = e ? atoi(e) * 1000 : (dflt);                                   \
            if (x < 0) x = 0;                                                  \
            atomic_store_explicit(&v, x, memory_order_relaxed);                \
        }                                                                      \
        return x;                                                              \
    }
NTP_SPIN_SITE(spin_row_ns,  "Y264_NTP_SPIN_ROW",  spin_budget_ns())
NTP_SPIN_SITE(spin_join_ns, "Y264_NTP_SPIN_JOIN", spin_budget_ns())
/* Idle defaults to 0, unlike its two siblings, on the CPU-vs-wall measurement
 * that split them: across twelve shapes a zero idle spin never costs wall --
 * four shapes come out ahead -- while CPU drops 13-28% on CIF and 3-5% on
 * 720p/1080p, and aggregate throughput under real oversubscription gains 7.1%
 * at four concurrent encodes. The row and join spins earn their budget
 * (zeroing either costs 13-15% of wall and RAISES CPU as absorbed stalls turn
 * back into condvar traffic); this one is speculative, polling work_epoch for
 * a claim that never opens when the grid is narrower than the pool.
 * Y264_NTP_SPIN_IDLE=25 gives this site a budget; plain Y264_NTP_SPIN does
 * not reach it, which is the point of the split. */
NTP_SPIN_SITE(spin_idle_ns, "Y264_NTP_SPIN_IDLE", 0)

#if defined(__aarch64__)
# define NTP_PAUSE() __asm__ __volatile__("isb" ::: "memory")
#elif defined(__x86_64__) || defined(__i386__)
# define NTP_PAUSE() __asm__ __volatile__("pause" ::: "memory")
#else
# define NTP_PAUSE() ((void)0)
#endif

/* Spin until the condition holds or the budget expires; sets _sat = 1 when
 * satisfied. Clock checked every 32 pauses (~1 us) to bound the burn. */
#define NTP_SPIN_WAIT(budget, satisfied_expr)                                  \
    do {                                                                       \
        uint64_t _t0 = 0; int _it = 0, _b = (budget);                          \
        for (;;) {                                                             \
            if (satisfied_expr) { _sat = 1; break; }                           \
            NTP_PAUSE();                                                       \
            if ((++_it & 31) == 0) {                                           \
                uint64_t _n = prof_now();                                      \
                if (!_t0) _t0 = _n;                                            \
                else if (_n - _t0 > (uint64_t)_b) break;                       \
            }                                                                  \
        }                                                                      \
    } while (0)

/* Wavefront wake slots: a power of two >= any sane thread count, so consecutive
 * in-flight rows never collide. A collision would only cost a spurious wakeup,
 * not correctness. Concurrent jobs are offset by 16 slots each so two frames'
 * equal-numbered rows rarely share one (a collision is likewise benign).
 * The 16-slot stride means job indices themselves alias every SLOTS/16 jobs:
 * 4 at 64 slots, 8 at 128. Aliasing-free at NTP_MAX_JOBS would want 512, which
 * buys nothing but memory -- the cost of a hit is one spurious wake. */
#define NTP_CV_SLOTS 128
#define NTP_CV_OF(job_idx, row) ((((job_idx) << 4) + (row)) & (NTP_CV_SLOTS - 1))

/* Concurrent jobs the pool can hold. Registration blocks when full (jobs always
 * complete, so this bounds memory, not correctness). Live worst case:
 * staircase anchor + its reference B + both leaves (the deep B staircase has
 * a whole mini-GOP in flight at once) + their deblocks + a lookahead-fme
 * leg BATCH (up to 2*bframes small wavefronts riding alongside). At 8 the
 * table measured 707 ms of registration stall on a 2 s single-GOP 720p
 * encode with the B staircase engaged. A second concurrent chain adds
 * roughly its whole burst-side set, ~5-6 jobs, so the table is sized for it
 * here rather than at the point where a full table turns into a hang. */
#define NTP_MAX_JOBS 24

/* Per-submitting-thread scratch lanes (see ntp_pool_slot). One lane per thread
 * that carves per-frame scratch, and lanes are claimed for the pool's lifetime.
 * The count is three fixed threads (encoder main, lookahead, fpipe bg) plus TWO
 * PER BURST SLOT (runner + trailer) plus FOUR PER CHAIN (driver, bemit, and
 * BDEPTH's brunner + btrailer) -- the last group is what the per-chain
 * decomposition multiplies:
 *
 * K=3, BDEPTH off 3 + 6 + 6 = 15
 * K=3, BDEPTH on 3 + 6 + 12 = 21 (over 16)
 * K=4, BDEPTH on 3 + 8 + 16 = 27
 *
 * That ceiling is not what a run claims in practice: a thread takes a lane only
 * when it actually carves scratch, and while the chains are serialized only
 * one chain's four threads ever do. Instrumented, the high-water mark is 10
 * with BDEPTH off and 13 with it on -- which is the ceiling with the 4K term
 * collapsed to 4, exactly as serialization predicts. So the table is sized for
 * the arithmetic, not for the measurement: the measurement rises to meet it the
 * moment chains run concurrently.
 *
 * Running out returns NULL and drops a caller onto its serial path SILENTLY --
 * a scheduling cliff, not a wrong bitstream -- so this leads the decomposition
 * rather than following it, and it covers the K=4 rebuild the ring is re-gated
 * at, not just the shipped K. A lane is an owner id plus eight grow-only
 * pointers; the headroom costs nothing worth counting. */
#define NTP_LANES 32

struct ntp_job {
    int   used;                 /* slot holds a live (or awaiting-wait) job */
    _Atomic int done;           /* all rows complete; waiter may release.
 * Atomic so job_wait can SPIN on it outside
 * the mutex (set under it, as always). */
    unsigned seq;               /* registration order; lowest = oldest */
    int   kind;                 /* 0 = wavefront, 1 = parallel-for */
    int   nrows, ncols;
    void (*thread_init)(void *, int);
    void (*thread_attach)(void *, int);
    void (*cell_fn)(void *, int, int, int);
    void (*for_fn)(void *, int, int);
    void *ctx;
    /* External row gate (NULL = ungated). Called under the pool mutex: must be
 * cheap, non-blocking, MONOTONIC (once true for a row, stays true) and must
 * not touch the pool. */
    int  (*row_ready)(void *, int);
    void  *gate_ctx;
    int    prio;                /* latency-critical (ntp_prio_hint) */
    int      tag_idx;           /* prof: tag-table slot, -1 = untagged */
    uint64_t t_reg;             /* prof: registration timestamp */
    _Atomic int next_row;       /* next unclaimed row (kind 0; kind!=0 claims
 * through claimw below) */
    /* kind!=0 claim word: (seq32 << 32) | next_row, CAS-claimed with or
 * without the pool mutex (the worker fast path). The seq half
 * makes a stale CAS on a re-registered slot FAIL instead of stealing the
 * new job's rows -- slot reuse requires done, done requires every claimed
 * unit complete, so the only reachable hazard is the claim word itself,
 * and the seq tag closes it. */
    _Atomic uint64_t claimw;
    _Atomic int rows_done;      /* completed rows; == lowest incomplete row
 * for kind 0 (a row's last cell needs the row
 * above fully done, so kind-0 rows COMPLETE in
 * increasing order); a plain counter for
 * parallel-fors, advanced lock-free */
    pthread_cond_t done_cv;     /* the registering thread waits here */
    _Atomic int *progress;      /* progress[r] = cells done in row r (kind 0) */
    _Atomic uint8_t *counted;   /* per-row completion mark (kind 0, marked by an
 * atomic exchange, under the pool mutex on the
 * locked path and lock-free on the row fast path)
 * -- was: written
 * under the pool mutex) -- a row counted twice
 * is a rows_done overcount, which the
 * incarnation traps catch downstream */
    /* Producer-visible ONE-SHOT poke hint: the progress row r-1 must reach for
 * parked row r to resume (INT_MAX = not parked / already poked), plus the
 * owning worker to poke. The resume itself is owner-only; this only steers
 * the single targeted wakeup at the crossing moment. */
    _Atomic int *parked_need;
    int         *parked_owner;
    int    progress_cap;
};

struct ntp_lane {
    _Atomic int used;
    pthread_t   owner;
    void       *sslot[8];
    size_t      scap[8];
};

/* One worker's parked continuations: at most one per job (holding a row of J
 * forbids taking further units of J). Worker-local, no locking. */
struct ntp_wctx {
    int nheld;
    struct { struct ntp_job *j; int row, col, need; unsigned seq; } held[NTP_MAX_JOBS];
    unsigned cur_seq;           /* job seq of the last unit run (thread-local
 * state -- ME hpel -- currently installed) */
};

struct ntp_pool {
    int              nthreads;
    pthread_t       *threads;

    pthread_mutex_t  mtx;
    pthread_cond_t   slot_cv;                 /* submitters wait for a job slot */
    /* Idle workers park on their OWN condvar, registered in a LIFO stack; every
 * wake names its target explicitly (pop one / poke a parked row's owner /
 * drain all on registration+kick) and removes it from the stack BEFORE
 * signalling, so `!in_stack[me]` is the wait predicate and no wake is ever
 * broadcast to workers that cannot use it (the thundering-herd scar). */
    pthread_cond_t  *worker_cv;               /* [nthreads] */
    int             *idle_stack;              /* [nthreads] worker ids */
    int             *in_stack;                /* [nthreads] membership flag */
    int              nidle_stack;
    /* One condvar per in-flight ROW (slot-hashed), not one for the whole pool.
 * Only row r ever waits on row r-1's progress, and exactly one worker owns
 * row r, so the producer knows precisely which single thread to wake. The
 * old single prog_cv had to BROADCAST, so every completed cell woke every
 * blocked worker to re-check and go back to sleep -- a thundering herd that
 * cost 8.2 ms per launch at 18 threads on an empty 45x80 grid (pure
 * overhead: no cell work at all). Rows in flight per job are consecutive
 * and at most nthreads, so with 64 slots and a 16-slot per-job offset no
 * two live rows share one in practice. */
    pthread_cond_t   prog_cv[NTP_CV_SLOTS];
    _Atomic int      wslot[NTP_CV_SLOTS];     /* waiters parked on each slot */
    /* prof only: when the producer broadcasts a slot it stamps the moment, so a
 * woken sleeper can split its stall into dep-wait + wake round-trip. */
    _Atomic uint64_t wsig[NTP_CV_SLOTS];
    _Atomic int      idlers;                  /* workers registered as idle */
    _Atomic int      nparked;                 /* parked rows, pool-wide */
    /* Bumped (relaxed) by every producer event that can open a fresh claim:
 * job registration, external kick, a row crossing its start need, the
 * cascade. Idle workers SPIN on it briefly before sleeping (see
 * spin_budget_ns); correctness never depends on it -- a missed bump just
 * means the pre-existing register+re-scan+sleep path. */
    _Atomic unsigned work_epoch;

    int              shutdown;
    unsigned         seq_next;                /* job registration counter */
    struct ntp_job   job[NTP_MAX_JOBS];

    /* Persistent per-lane wavefront scratch (see ntp_pool_slot): each thread
 * that submits work to this pool gets its own bank of 8 grow-only slots,
 * so two concurrent frames' run functions never carve the same buffer. */
    struct ntp_lane  lane[NTP_LANES];

    /* Y264_NTP_PROF (all zero-cost when prof == 0) */
    int               prof;
    uint64_t          t_create;
    struct ntp_wprof *wprof;                  /* [nthreads] */
    struct ntp_tagstat tag[NTP_PROF_TAGS];
    int               ntags;
    int               live_jobs;              /* registered, not yet done */
    uint64_t          empty_ns, t_empty0;     /* time with zero live jobs */
    _Atomic uint64_t  empty_pub, empty_since; /* the same clock, lock-free read */
    uint64_t          slot_wait_ns;           /* job_register table-full waits */
    long              njobs;
    long              wakes_one, wakes_worker, wakes_all, kicks;
};

/* --- idle-stack wake primitives (pool mutex held for all of them) --- */

/* Remove worker `id` from the idle stack if present; returns 1 if it was. */
static int stack_remove(struct ntp_pool *p, int id)
{
    if (!p->in_stack[id])
        return 0;
    for (int i = p->nidle_stack - 1; i >= 0; i--)
        if (p->idle_stack[i] == id) {
            p->idle_stack[i] = p->idle_stack[--p->nidle_stack];
            break;
        }
    p->in_stack[id] = 0;
    return 1;
}

/* Wake one idle worker (LIFO -- warmest cache first). */
static void wake_one(struct ntp_pool *p)
{
    if (p->nidle_stack == 0)
        return;
    int id = p->idle_stack[--p->nidle_stack];
    p->in_stack[id] = 0;
    if (p->prof) p->wakes_one++;
    pthread_cond_signal(&p->worker_cv[id]);
}

/* Wake a SPECIFIC worker (the owner of a parked row) if it is idle. */
static void wake_worker(struct ntp_pool *p, int id)
{
    if (stack_remove(p, id)) {
        if (p->prof) p->wakes_worker++;
        pthread_cond_signal(&p->worker_cv[id]);
    }
}

/* Wake every idle worker (job registration / external kick: several rows may
 * have become claimable and each waker names its targets, so this is the only
 * multi-wake -- and it is per-event, not per-cell). */
static void wake_all(struct ntp_pool *p)
{
    if (p->prof && p->nidle_stack > 0) p->wakes_all++;
    while (p->nidle_stack > 0)
        wake_one(p);
}

/* The first cell of row r needs progress[r-1] >= min(2, ncols) -- the same
 * `need` run_row_from computes for c == 0. That is the row's startable
 * condition. */
static inline int job_start_need(const struct ntp_job *j)
{
    return (2 < j->ncols) ? 2 : j->ncols;
}

/* Escapes: Y264_NTP_FASTCLAIM=0 selects the locked per-unit claim/complete
 * for parallel-fors; Y264_NTP_WAKE1=0 selects wake_all at job registration.
 * Both are on unless escaped. */
/* Both lazy statics below are WARMED in ntp_pool_create before any worker
 * exists -- the tsan-lazy-static class: an unlocked first-touch from two pool
 * workers is a (benign, idempotent) data race TSan rightly flags. */
/* Y264_NTP_ROWFAST=1 (plan item E3): after a wavefront row completes, claim
 * the SAME job's next row lock-free when it is already runnable (its row
 * above is far enough along and the external row gate passes), no worker is
 * idle (so no cascade wake is owed) and no latency-critical job is ready
 * (which the locked picker would have preferred). Row completion is marked
 * with the same atomic exchange the locked path uses; the job's LAST row and
 * every other case take the locked path unchanged, so claim ORDER (next_row
 * is a monotonic CAS counter either way) and the done broadcast are
 * untouched -- only which worker runs a row can differ, which the wavefront
 * is deterministic under by design. Measured target: 1.0-1.8% of t12 CPU in
 * pool-mutex acquires (foreman 9.2k units, sunflower 44.7k). DEFAULT OFF
 * pending identity + walls; =0 escapes. */
static int ntp_rowfast_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_NTP_ROWFAST"); v = s ? (atoi(s) ? 1 : 0) : 0; }
    return v;
}
static int ntp_fastclaim_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_NTP_FASTCLAIM"); v = e ? (atoi(e) ? 1 : 0) : 1; }
    return v;
}
static int ntp_wake1_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_NTP_WAKE1"); v = e ? (atoi(e) ? 1 : 0) : 1; }
    return v;
}

/* Claim one parallel-for row. Safe with OR without the pool mutex: the CAS
 * carries the job's seq in the high half, so a claim racing a slot reuse
 * fails on the seq tag rather than bumping the new job's counter. -1 = no
 * row (exhausted, or the slot moved on). */
static int job_claim_pfor(struct ntp_job *j, unsigned seq32)
{
    for (;;) {
        uint64_t w = atomic_load_explicit(&j->claimw, memory_order_acquire);
        if ((unsigned)(w >> 32) != seq32)
            return -1;
        uint32_t r = (uint32_t)w;
        if (r >= (uint32_t)j->nrows)
            return -1;
        if (atomic_compare_exchange_weak_explicit(&j->claimw, &w, w + 1,
                memory_order_acq_rel, memory_order_relaxed))
            return (int)r;
    }
}

/* INCARNATION TRAP: a NULL j->cell_fn call from the kind-0 path means a job
 * slot changed identity under a running worker, which every hold invariant
 * individually forbids. Every claim and park stamps the incarnation (seq);
 * run entry, every cell, and resume verify it
 * and abort with the full state, so the violated invariant names itself
 * instead of leaving a corpse. Integer compares against already-hot fields:
 * cheap enough to keep armed permanently. */
static void ntp_trap(struct ntp_pool *p, struct ntp_job *j, const char *site,
                     int idx, unsigned want_seq, int r, int c)
{
    fprintf(stderr,
        "NTP INCARNATION TRAP @ %s: worker=%d job=%d want_seq=%u\n"
        "  j: used=%d done=%d seq=%u kind=%d nrows=%d ncols=%d\n"
        "  rows_done=%d claimw=0x%llx next_row=%d cell_fn=%p for_fn=%p r=%d c=%d\n",
        site, idx, (int)(j - p->job), want_seq,
        j->used, (int)j->done, j->seq, j->kind, j->nrows, j->ncols,
        (int)atomic_load_explicit(&j->rows_done, memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&j->claimw, memory_order_relaxed),
        atomic_load_explicit(&j->next_row, memory_order_relaxed), (void *)j->cell_fn, (void *)j->for_fn, r, c);
    abort();
}
#define NTP_CHECK(p, j, want, site, idx, r, c) do {                         \
        if ((j)->seq != (want) || !(j)->used ||                             \
            ((j)->kind == 0 ? !(j)->cell_fn : !(j)->for_fn))                \
            ntp_trap((p), (j), (site), (idx), (want), (r), (c));            \
    } while (0)

/* kind!=0's unclaimed-row count lives in claimw; kind 0 keeps next_row. */
static inline int job_rows_claimed(const struct ntp_job *j)
{
    return j->kind != 0
         ? (int)(uint32_t)atomic_load_explicit(
               (_Atomic uint64_t *)&j->claimw, memory_order_relaxed)
         : atomic_load_explicit(&j->next_row, memory_order_relaxed);
}

/* Is job j's next unclaimed row ready to start right now? (pool mutex held) */
static int job_next_ready(struct ntp_job *j)
{
    if (!j->used || j->done || job_rows_claimed(j) >= j->nrows)
        return 0;
    if (j->kind == 1)
        return 1;                             /* parallel-for: always ready */
    int r = atomic_load_explicit(&j->next_row, memory_order_relaxed);
    if (j->row_ready && !j->row_ready(j->gate_ctx, r))
        return 0;
    if (r == 0)
        return 1;
    return atomic_load_explicit(&j->progress[r - 1], memory_order_acquire)
           >= job_start_need(j);
}

/* prof: WHY is nothing claimable right now? Scanned at idle-sleep entry under
 * the pool mutex (same context and cost class as job_next_ready). Priority
 * gate > ramp > tail: when several live jobs block for different reasons the
 * most actionable one is reported. */
static int idle_class(struct ntp_pool *p)
{
    int gate = 0, ramp = 0, tail = 0;
    for (int i = 0; i < NTP_MAX_JOBS; i++) {
        struct ntp_job *j = &p->job[i];
        if (!j->used || j->done)
            continue;
        if (job_rows_claimed(j) >= j->nrows) {
            tail = 1;                    /* rows all claimed, still running */
        } else if (j->kind == 0) {
            int r = atomic_load_explicit(&j->next_row, memory_order_relaxed);
            if (j->row_ready && !j->row_ready(j->gate_ctx, r))
                gate = 1;
            else if (r > 0 &&
                     atomic_load_explicit(&j->progress[r - 1],
                                          memory_order_acquire) < job_start_need(j))
                ramp = 1;
        }
    }
    return gate ? NPW_GATE : ramp ? NPW_RAMP : tail ? NPW_TAIL : NPW_NOJOB;
}

/* Pool-empty time accumulated so far, including the interval in progress.
 * Called under p->mtx, which is where both live_jobs and t_empty0 are written. */
static uint64_t empty_acc(const struct ntp_pool *p, uint64_t now)
{
    return p->empty_ns + (p->live_jobs == 0 ? now - p->t_empty0 : 0);
}

/* A claimable fresh row for this worker, skipping `jcur` and every job `w`
 * holds a parked row of (their row-carried per-worker state must not be
 * touched by this worker until the parked row resumes). Job choice: the job
 * the worker LAST ran wins when it has a ready row (AFFINITY -- keeps a
 * worker's consecutive rows on one frame: warm caches, no per-row
 * init/attach churn; without it a two-frame pool ping-pongs workers between
 * frames on every row), else the OLDEST ready job (drain order). w/jcur may
 * be NULL (exclusion-free peek, no affinity).
 * (pool mutex held; does not claim -- doubles as a peek.) */
static struct ntp_job *pick_fresh(struct ntp_pool *p, const struct ntp_wctx *w,
                                  const struct ntp_job *jcur)
{
    struct ntp_job *best = NULL, *aff = NULL, *hi = NULL;
    unsigned aseq = w ? w->cur_seq : 0;         /* seq counter starts at 1 */
    for (int i = 0; i < NTP_MAX_JOBS; i++) {
        struct ntp_job *j = &p->job[i];
        if (j == jcur)
            continue;
        if (w) {
            int held = 0;
            for (int k = 0; k < w->nheld; k++)
                if (w->held[k].j == j) { held = 1; break; }
            if (held)
                continue;
        }
        if (!job_next_ready(j))
            continue;
        if (aseq && j->seq == aseq)
            aff = j;
        if (j->prio && (!hi || j->seq < hi->seq))
            hi = j;
        if (!best || j->seq < best->seq)
            best = j;
    }
    /* Oldest ready LATENCY-CRITICAL job first (a blocked submitter is
 * waiting on it, and it is short by contract; without this both the
 * oldest-first drain order AND affinity let a long analyze wavefront
 * starve the small lookahead grids -- the budget's 0.7x lookahead_lr),
 * then affinity (warm caches, no init churn), then oldest (drain
 * order). The switch-back cost is one thread_init (~a frame-struct
 * copy), paid once per excursion, microseconds against the milliseconds
 * the blocked submitter recovers. */
    return hi ? hi : aff ? aff : best;
}

/* Index into w->held of a parked row that can resume NOW (oldest job first),
 * or -1. Owner-only by construction: w is this worker's. */
static int held_resumable(const struct ntp_wctx *w)
{
    int best = -1;
    for (int k = 0; k < w->nheld; k++) {
        const struct ntp_job *j = w->held[k].j;
        if (atomic_load_explicit(&j->progress[w->held[k].row - 1],
                                 memory_order_acquire) >= w->held[k].need &&
            (best < 0 || j->seq < w->held[best].j->seq))
            best = k;
    }
    return best;
}

/* Run row r of job j from column c0 (0 = fresh claim, >0 = parked resume).
 * Returns -1 when the row completed, or the column it PARKED at: when the row
 * stalls on the row above and this worker has something else to run -- another
 * frame's row, a parallel-for index, or one of its own parked rows that became
 * resumable -- the row is recorded in w->held and the worker goes to serve
 * that instead of sleeping inside this job. A worker already holding parked
 * rows never commits to a single-row sleep here (a targeted sleep while
 * holding a resumable-later row of another job can cross-deadlock: two
 * sleepers each owning the continuation the other's chain needs); it parks
 * unconditionally and does its waiting in the worker loop's idle stack, where
 * every wake source can reach it. With a single registered job neither branch
 * ever fires, so the stall path is byte-for-byte the classic in-place
 * wake-slot wait. */
static int run_row_from(struct ntp_pool *p, struct ntp_job *j,
                        struct ntp_wctx *w, int idx, int r, int c0)
{
    int ncols = j->ncols;
    int ji = (int)(j - p->job);
    unsigned seq0 = j->seq;                 /* incarnation at entry (trap) */
    NTP_CHECK(p, j, seq0, "row-entry", idx, r, c0);
    int start_need = job_start_need(j);
    struct ntp_wprof *wp = p->prof ? &p->wprof[idx] : NULL;
    int deep = p->prof >= 2;               /* per-cell timing (diagnostic runs) */
    uint64_t t_row = wp ? prof_now() : 0, stall_ns = 0;
    /* Parking needs thread_attach on re-entry unless the job installs no
 * thread-locals at all (thread_init NULL). */
    int parkable = (j->thread_attach || !j->thread_init) && park_on();
    int nstall = 0;             /* prof: stalls so far in this row segment */
    for (int c = c0; c < ncols; c++) {
        NTP_CHECK(p, j, seq0, "cell", idx, r, c);
        if (r > 0) {
            /* need the top-right neighbour (r-1, c+1) done, i.e. progress[r-1] >=
 * c+2 -- but clamp to ncols: on the last column c+1 is out of bounds,
 * so the requirement is just "row r-1 fully done" (progress == ncols),
 * never the unreachable ncols+1. */
            int need = (c + 2 < ncols) ? c + 2 : ncols;
            if (atomic_load_explicit(&j->progress[r - 1], memory_order_acquire) < need) {
                int slot = NTP_CV_OF(ji, r);
                uint64_t t_st = wp ? prof_now() : 0;
                struct ntp_sitestat *ss = wp ? &wp->site[nstall ? NSS_MID
                                                                : NSS_ENTRY] : NULL;
                nstall++;
                if (ss)
                    ss->deficit[defbucket(need -
                        atomic_load_explicit(&j->progress[r - 1],
                                             memory_order_relaxed))]++;
                int sb = spin_row_ns();
                if (sb) {                   /* poll the dependency before sleeping */
                    int _sat = 0;
                    atomic_fetch_add_explicit(&st_spin_row, 1, memory_order_relaxed);
                    NTP_SPIN_WAIT(sb,
                        atomic_load_explicit(&j->progress[r - 1],
                                             memory_order_acquire) >= need);
                    if (_sat) {
                        atomic_fetch_add_explicit(&st_spin_row_hit, 1,
                                                  memory_order_relaxed);
                        if (wp) {
                            uint64_t d = prof_now() - t_st;
                            stall_ns += d;
                            ss->spin_ns += d; ss->spin_hit++;
                            ss->hspin[hbucket(d)]++;
                        }
                        goto satisfied;
                    }
                    /* spin burn stays inside the midrow stall accounting */
                }
                pthread_mutex_lock(&p->mtx);
                if (atomic_load_explicit(&j->progress[r - 1],
                                         memory_order_acquire) >= need) {
                    pthread_mutex_unlock(&p->mtx);   /* raced: satisfied already */
                    if (wp) {
                        uint64_t d = prof_now() - t_st;
                        stall_ns += d;
                        ss->spin_ns += d; ss->raced++;
                        ss->hspin[hbucket(d)]++;
                    }
                    goto satisfied;
                }
                if (parkable && (w->nheld > 0 || pick_fresh(p, w, j))) {
                    atomic_fetch_add_explicit(&st_parks, 1, memory_order_relaxed);
                    w->held[w->nheld].j = j;
                    w->held[w->nheld].row = r;
                    w->held[w->nheld].col = c;
                    w->held[w->nheld].need = need;
                    w->held[w->nheld].seq = j->seq;   /* incarnation stamp */
                    w->nheld++;
                    j->parked_owner[r] = idx;
                    atomic_store_explicit(&j->parked_need[r], need,
                                          memory_order_release);
                    atomic_fetch_add_explicit(&p->nparked, 1, memory_order_relaxed);
                    pthread_mutex_unlock(&p->mtx);
                    if (wp) {
                        uint64_t now = prof_now();
                        ss->park_ns += now - t_st; ss->parked++;
                        stall_ns += now - t_st;
                        wp->busy_ns += now - t_row - stall_ns;
                        wp->midrow_ns += stall_ns;
                        if (j->tag_idx >= 0) {
                            atomic_fetch_add_explicit(&p->tag[j->tag_idx].busy_ns,
                                now - t_row - stall_ns, memory_order_relaxed);
                            atomic_fetch_add_explicit(&p->tag[j->tag_idx].midrow_ns,
                                stall_ns, memory_order_relaxed);
                        }
                    }
                    return c;
                }
                /* Register as a waiter (once) BEFORE the re-check, and fence, so the
 * producer -- which stores progress then loads the slot's waiter
 * count WITHOUT the lock -- can never read 0 while we are about to
 * sleep on an already-satisfied condition. The seq_cst fence here
 * pairs with the producer's fence (below) to give the StoreLoad
 * ordering between our {wslot++, load progress} and its {store
 * progress, load wslot}: either it sees our wslot++ (and wakes us) or
 * we see its progress store (and don't wait). Doing the increment
 * INSIDE the loop after the check with only relaxed ordering gives a
 * rare lost wakeup that hangs the whole encode (one worker stuck here
 * => the job never completes => the join never returns). */
                atomic_fetch_add_explicit(&st_inplace, 1, memory_order_relaxed);
                atomic_fetch_add_explicit(&p->wslot[slot], 1, memory_order_relaxed);
                atomic_thread_fence(memory_order_seq_cst);
                while (atomic_load_explicit(&j->progress[r - 1],
                                            memory_order_acquire) < need)
                    pthread_cond_wait(&p->prog_cv[slot], &p->mtx);
                atomic_fetch_sub_explicit(&p->wslot[slot], 1, memory_order_relaxed);
                /* read the producer's signal stamp while still under the mutex
 * it stamped it under */
                uint64_t t_sig = wp ? atomic_load_explicit(&p->wsig[slot],
                                                   memory_order_relaxed) : 0;
                pthread_mutex_unlock(&p->mtx);
                if (wp) {
                    uint64_t now = prof_now(), d = now - t_st;
                    stall_ns += d;
                    wp->midrow_cnt++;
                    ss->sleep_ns += d; ss->slept++;
                    ss->hsleep[hbucket(d)]++;
                    if (t_sig > t_st && t_sig <= now) {   /* our waker's stamp */
                        ss->dep_ns  += t_sig - t_st;
                        ss->wake_ns += now - t_sig;
                        ss->wake_n++;
                        ss->hwake[hbucket(now - t_sig)]++;
                    }
                }
            }
        }
satisfied:
        if (!deep) {
            j->cell_fn(j->ctx, idx, r, c);
        } else {                        /* deep mode: is a stalled dependency a
 * slow CELL or an off-CPU producer? */
            uint64_t t0 = prof_now();
            j->cell_fn(j->ctx, idx, r, c);
            wp->hcell[hbucket(prof_now() - t0)]++;
        }
        atomic_store_explicit(&j->progress[r], c + 1, memory_order_release);
        /* Wake ONLY the row below, the single row that can be gated on this
 * progress -- and only pay the mutex when it is actually blocked (the
 * common case, once the wavefront is flowing, is a pure atomic store and
 * no lock). The seq_cst fence orders this store before the waiter-count
 * load, pairing with the waiter's fence so we never miss a waiter that
 * just registered (see the wait section above). */
        atomic_thread_fence(memory_order_seq_cst);
        int wake = NTP_CV_OF(ji, r + 1);
        if (atomic_load_explicit(&p->wslot[wake], memory_order_relaxed) > 0) {
            pthread_mutex_lock(&p->mtx);
            if (wp)                       /* prof: stamp for the sleeper's split */
                atomic_store_explicit(&p->wsig[wake], prof_now(),
                                      memory_order_relaxed);
            pthread_cond_broadcast(&p->prog_cv[wake]);
            pthread_mutex_unlock(&p->mtx);
        }
        /* Idle-worker wakes (only when someone is sleeping, i.e. the pool is
 * starved -- a saturated single-job run never pays this):
 * - c+1 hit the start need: row r+1 became CLAIMABLE -> wake ONE;
 * - c+1 crossed parked row r+1's recorded need: poke exactly its
 * OWNER, one-shot (the hint is consumed under the lock, so later
 * cells of this row do not re-fire; a busy owner re-scans its held
 * rows at its next loop anyway). Fence pairing as above: an idler
 * registers, fences, then re-scans before sleeping, so either we
 * see it here or its re-scan sees our progress store. */
        if (atomic_load_explicit(&p->idlers, memory_order_relaxed) > 0) {
            int hint = r + 1 < j->nrows &&
                       atomic_load_explicit(&j->parked_need[r + 1],
                                            memory_order_relaxed) <= c + 1;
            if (hint || c + 1 == start_need) {
                atomic_fetch_add_explicit(hint ? &st_hint_bc : &st_start_sig, 1,
                                          memory_order_relaxed);
                if (!hint)          /* row r+1 became claimable: notify spinners */
                    atomic_fetch_add_explicit(&p->work_epoch, 1,
                                              memory_order_relaxed);
                pthread_mutex_lock(&p->mtx);
                if (hint &&
                    atomic_load_explicit(&j->parked_need[r + 1],
                                         memory_order_relaxed) <= c + 1) {
                    atomic_store_explicit(&j->parked_need[r + 1], INT_MAX,
                                          memory_order_relaxed);
                    wake_worker(p, j->parked_owner[r + 1]);
                } else if (!hint) {
                    wake_one(p);
                }
                pthread_mutex_unlock(&p->mtx);
            }
        }
    }
    if (wp) {
        uint64_t now = prof_now();
        wp->busy_ns += now - t_row - stall_ns;
        wp->midrow_ns += stall_ns;
        wp->rows++;
        if (j->tag_idx >= 0) {
            atomic_fetch_add_explicit(&p->tag[j->tag_idx].busy_ns,
                                      now - t_row - stall_ns, memory_order_relaxed);
            atomic_fetch_add_explicit(&p->tag[j->tag_idx].midrow_ns, stall_ns,
                                      memory_order_relaxed);
            atomic_fetch_add_explicit(&p->tag[j->tag_idx].rows, 1,
                                      memory_order_relaxed);
        }
    }
    return -1;
}


/* Name the calling thread so a profile attributes by role (E4: unnamed
 * threads made the per-thread counter split useless). Best effort, no
 * error path: a name is a diagnostic, never a dependency. */
static void y264_thread_name(const char *name)
{
#if defined(__APPLE__)
    pthread_setname_np(name);
#elif defined(__linux__)
    pthread_setname_np(pthread_self(), name);
#else
    (void)name;
#endif
}
static void *worker_main(void *arg)
{
    struct ntp_pool *p = ((void **)arg)[0];
    int idx = (int)(intptr_t)((void **)arg)[1];
    free(arg);
    { char nm[24]; snprintf(nm, sizeof nm, "y264-w%d", idx); y264_thread_name(nm); }
    struct ntp_wctx w = {0};
    int spun = 0;               /* last idle attempt already burned its spin */

    pthread_mutex_lock(&p->mtx);
    for (;;) {
        struct ntp_job *j = NULL;
        int r, c0;
        /* Iterations that reach the run below never slept (every idle path
 * `continue`s), so this measures the pure claim-scan hold. */
        uint64_t t_it = p->prof ? prof_now() : 0;
        int hi = held_resumable(&w);
        if (hi < 0) {
            j = pick_fresh(p, &w, NULL);
            if (!j) {
                if (p->shutdown)
                    break;
                /* Spin on the work epoch before committing to the condvar --
 * but only once per miss (a fruitless spin falls through to
 * the real sleep next time around), and never while holding
 * parked rows (their sleep must stay owner-poke reachable).
 * The idlers count makes producers publish epoch bumps while
 * we spin; correctness never depends on catching one (the
 * sleep path re-scans under the mutex as always). */
                int sb;
                if (!spun && w.nheld == 0 && (sb = spin_idle_ns()) > 0) {
                    unsigned e0 = atomic_load_explicit(&p->work_epoch,
                                                       memory_order_relaxed);
                    atomic_fetch_add_explicit(&p->idlers, 1, memory_order_relaxed);
                    pthread_mutex_unlock(&p->mtx);
                    atomic_fetch_add_explicit(&st_spin_idle, 1, memory_order_relaxed);
                    int _sat = 0;
                    uint64_t sp0 = p->prof ? prof_now() : 0;
                    NTP_SPIN_WAIT(sb,
                        atomic_load_explicit(&p->work_epoch,
                                             memory_order_relaxed) != e0);
                    if (_sat)
                        atomic_fetch_add_explicit(&st_spin_idle_hit, 1,
                                                  memory_order_relaxed);
                    if (p->prof)
                        p->wprof[idx].spin_ns += prof_now() - sp0;
                    pthread_mutex_lock(&p->mtx);
                    atomic_fetch_sub_explicit(&p->idlers, 1, memory_order_relaxed);
                    spun = !_sat;
                    continue;           /* re-scan under the mutex either way */
                }
                /* Register as an idler (idlers counter + own slot on the idle
 * stack) BEFORE the final re-scan; the fence pairs with the
 * producers': either they see idlers > 0 and name us (or some
 * idler) as a wake target, or the re-scan sees their progress/
 * publish. Registration, re-scan and the wait all happen under
 * the pool mutex, and every waker removes its target from the
 * stack under it too: no lost-wakeup window. Workers holding
 * parked rows sleep HERE too (never on a single row's slot) so
 * the owner-poke can always reach them. */
                atomic_fetch_add_explicit(&p->idlers, 1, memory_order_relaxed);
                p->idle_stack[p->nidle_stack++] = idx;
                p->in_stack[idx] = 1;
                atomic_thread_fence(memory_order_seq_cst);
                hi = held_resumable(&w);
                if (hi < 0 && !(j = pick_fresh(p, &w, NULL))) {
                    atomic_fetch_add_explicit(&st_idle_sleeps, 1, memory_order_relaxed);
                    int cls = 0;
                    uint64_t t_idle = 0, e0 = 0;
                    if (p->prof) {
                        cls = idle_class(p); t_idle = prof_now();
                        e0 = empty_acc(p, t_idle);
                    }
                    while (p->in_stack[idx] && !p->shutdown)
                        pthread_cond_wait(&p->worker_cv[idx], &p->mtx);
                    if (p->prof) {
                        /* The class is what was blocking at ENTRY; a sleep that
 * outlives the last live job spends its remainder with
 * the pool empty, and billing all of it to `tail`
 * inflated that bucket by the whole serial term (the
 * 08-15 audit read 31% of tail against 2% of nojob on a
 * cell whose pool-empty was 28% of the wall). The
 * pool's own empty clock splits the interval exactly. */
                        uint64_t now = prof_now(), d = now - t_idle;
                        uint64_t emp = empty_acc(p, now) - e0;
                        if (emp > d) emp = d;
                        p->wprof[idx].idle_ns[cls] += d - emp;
                        p->wprof[idx].idle_cnt[cls]++;
                        if (emp) {
                            p->wprof[idx].idle_ns[NPW_NOJOB] += emp;
                            if (cls != NPW_NOJOB) p->wprof[idx].idle_cnt[NPW_NOJOB]++;
                        }
                    }
                    atomic_fetch_sub_explicit(&p->idlers, 1, memory_order_relaxed);
                    spun = 0;           /* a wake is an event: spin again next miss */
                    continue;
                }
                stack_remove(p, idx);          /* found work: withdraw */
                atomic_fetch_sub_explicit(&p->idlers, 1, memory_order_relaxed);
            }
        }
        if (hi >= 0) {                          /* resume own parked row */
            atomic_fetch_add_explicit(&st_resumes, 1, memory_order_relaxed);
            j = w.held[hi].j;
            r = w.held[hi].row;
            c0 = w.held[hi].col;
            NTP_CHECK(p, j, w.held[hi].seq, "resume", idx, r, c0);
            atomic_store_explicit(&j->parked_need[r], INT_MAX,
                                  memory_order_relaxed);
            atomic_fetch_sub_explicit(&p->nparked, 1, memory_order_relaxed);
            w.held[hi] = w.held[--w.nheld];
        } else {                                /* fresh claim */
            atomic_fetch_add_explicit(&st_claims, 1, memory_order_relaxed);
            if (j->kind != 0) {
                r = job_claim_pfor(j, j->seq);
                if (r < 0)
                    continue;   /* lost the row to a lock-free claimer: rescan */
            } else {
                /* E3 made next_row a shared CAS counter: a row-fast claimer can
 * advance it between pick_fresh's readiness check and this claim,
 * so re-validate and claim atomically; on any change, rescan (the
 * same "lost the row to a lock-free claimer" rule as kind!=0). */
                int r0 = atomic_load_explicit(&j->next_row, memory_order_relaxed);
                if (!job_next_ready(j) ||
                    !atomic_compare_exchange_strong_explicit(&j->next_row, &r0, r0 + 1,
                                                             memory_order_acq_rel, memory_order_relaxed))
                    continue;
                r = r0;
            }
            c0 = 0;
            if (p->prof) {
                p->wprof[idx].claims++;
                if (j->kind == 0 && r > 0)   /* how much lead did we claim with? */
                    p->wprof[idx].lead[leadbucket(
                        atomic_load_explicit(&j->progress[r - 1],
                                             memory_order_relaxed))]++;
            }
        }
        /* Cascade: if claimable work remains beyond the unit we just took and
 * another worker is idle, wake one -- keeps the invariant "runnable
 * work + idle worker => a wakeup is in flight" through multi-row
 * events (job registration, a kick opening several rows). */
        if (atomic_load_explicit(&p->idlers, memory_order_relaxed) > 0 &&
            pick_fresh(p, NULL, NULL)) {
            atomic_fetch_add_explicit(&p->work_epoch, 1, memory_order_relaxed);
            wake_one(p);
        }
        if (p->prof) p->wprof[idx].pick_ns += prof_now() - t_it;
        pthread_mutex_unlock(&p->mtx);
        spun = 0;

        int parked_at = -1;
        unsigned run_seq = j->seq;          /* incarnation at dispatch (trap) */
        NTP_CHECK(p, j, run_seq, "dispatch", idx, r, c0);
        /* THE PHANTOM-COUNT BUG: branching the bottom accounting on j->kind
 * read AFTER the run lets an EXHAUSTED parallel-for worker whose job
 * completed and was re-registered as a kind-0 wavefront before it took
 * the lock read kind==0 and INCREMENT THE NEW JOB'S rows_done. One
 * phantom row => done fires with workers still mid-cell => the la_lr
 * psum race, NULL cell_fn calls, double-count traps, and 2-in-40
 * wide-ref3 crashes. Branch on the CLAIMED incarnation's kind, and
 * seq-guard the parallel-for done-check so a reused slot is never
 * touched. */
        int was_pfor = (j->kind != 0);
        if (j->kind == 0) {
            /* Fresh row after a job switch: (re)run thread_init -- safe, all
 * row-carried state re-seeds at the row start. Mid-row RESUME
 * after a switch: thread_attach only (re-install thread-locals);
 * thread_init would reset the row-carried per-worker state. */
            if (w.cur_seq != j->seq) {
                uint64_t t0 = p->prof ? prof_now() : 0;
                if (c0 == 0) {
                    if (j->thread_init) j->thread_init(j->ctx, idx);
                } else {
                    if (j->thread_attach) j->thread_attach(j->ctx, idx);
                }
                w.cur_seq = j->seq;
                if (p->prof) {
                    p->wprof[idx].init_ns += prof_now() - t0;
                    p->wprof[idx].init_n++;
                }
            }
            parked_at = run_row_from(p, j, &w, idx, r, c0);
            /* E3 row fast path. ORDER MATTERS: claim the next row BEFORE
 * counting this one's completion. While row r is uncounted the job
 * cannot reach rows_done == nrows, so its slot cannot be torn down
 * or re-registered under us; once r2 is claimed and unfinished, the
 * same holds. Counting first opened a window where another worker's
 * completion of the last row let the submitter free the job while
 * this loop still read it (a 1-in-12 segfault at 18 threads). Other
 * jobs are consulted by plain flags only, never through their
 * arrays. */
            while (parked_at < 0 && w.nheld == 0 && ntp_rowfast_on() &&
                   j->seq == run_seq &&
                   atomic_load_explicit(&p->idlers, memory_order_relaxed) == 0) {
                int prio_live = 0;
                for (int i = 0; i < NTP_MAX_JOBS; i++) {
                    const struct ntp_job *o = &p->job[i];
                    if (o != j && o->used && o->prio && !o->done) { prio_live = 1; break; }
                }
                if (prio_live) break;                 /* the locked picker prefers it */
                int r2 = atomic_load_explicit(&j->next_row, memory_order_relaxed);
                if (r2 >= j->nrows) break;
                if (j->row_ready && !j->row_ready(j->gate_ctx, r2)) break;
                if (atomic_load_explicit(&j->progress[r2 - 1], memory_order_acquire) < job_start_need(j)) break;
                if (!atomic_compare_exchange_strong_explicit(&j->next_row, &r2, r2 + 1,
                                                             memory_order_acq_rel, memory_order_relaxed))
                    break;                            /* lost the claim: locked path */
                /* r2 is ours and unfinished: the job stays alive. Now count r. */
                if (j->counted && atomic_exchange_explicit(&j->counted[r], 1, memory_order_relaxed)) {
                    fprintf(stderr, "NTP DOUBLE COUNT (fast): worker=%d job=%d seq=%u row=%d\n",
                            idx, (int)(j - p->job), run_seq, r);
                    abort();
                }
                atomic_fetch_add_explicit(&j->rows_done, 1, memory_order_acq_rel);   /* never the last: r2 is open */
                atomic_fetch_add_explicit(&st_claims, 1, memory_order_relaxed);
                atomic_fetch_add_explicit(&st_rowfast, 1, memory_order_relaxed);
                if (p->prof) p->wprof[idx].claims++;
                r = r2; c0 = 0;
                NTP_CHECK(p, j, run_seq, "dispatch-fast", idx, r, c0);
                parked_at = run_row_from(p, j, &w, idx, r, c0);
            }
        } else {
            /* Parallel-for units: run, then complete AND claim the next unit
 * of the SAME job lock-free. The locked shape takes the pool mutex
 * once per unit for completion accounting plus once for the next
 * claim scan -- 35k acquisitions per encode, with the acquire
 * measured at 8us under t18 contention. The fast loop touches the
 * mutex only to signal the job's completion or when the job runs
 * out. */
            unsigned s32 = j->seq;      /* stable: we hold an incomplete unit */
            int fast = ntp_fastclaim_on();
            /* Cache the job's immutable-for-this-incarnation fields ONCE: a
 * seq-verified claim licenses them for the whole loop, and going
 * back to the struct each iteration turns a slot-reuse window into
 * an indirect call through NULL (crash reports name the for_fn call
 * site with fn == 0). With locals, even a reused slot cannot null
 * the pointer under us; the seq-tagged CAS remains the
 * claim-correctness gate. */
            void (*ffn)(void *, int, int) = j->for_fn;
            void *fctx = j->ctx;
            int fnrows = j->nrows;
            for (;;) {
                if (!p->prof) {
                    ffn(fctx, idx, r);
                } else {
                    uint64_t t0 = prof_now();
                    ffn(fctx, idx, r);
                    uint64_t dt = prof_now() - t0;
                    p->wprof[idx].busy_ns += dt;
                    p->wprof[idx].rows++;
                    if (j->tag_idx >= 0) {
                        atomic_fetch_add_explicit(&p->tag[j->tag_idx].busy_ns, dt,
                                                  memory_order_relaxed);
                        atomic_fetch_add_explicit(&p->tag[j->tag_idx].rows, 1,
                                                  memory_order_relaxed);
                    }
                }
                int fin = atomic_fetch_add_explicit(&j->rows_done, 1,
                                                    memory_order_acq_rel) + 1;
                if (fin >= fnrows)
                    break;              /* we completed the job: locked path */
                int r2 = fast ? job_claim_pfor(j, s32) : -1;
                if (r2 < 0)
                    break;              /* exhausted (or escape): locked path */
                r = r2;
            }
        }

        if (!p->prof) {
            pthread_mutex_lock(&p->mtx);
        } else {
            uint64_t t0 = prof_now();
            pthread_mutex_lock(&p->mtx);
            p->wprof[idx].lock_ns += prof_now() - t0;
        }
        /* Double-count trap: rows_done reaching nrows with workers still
 * mid-cell means some row completed twice. Mark each kind-0 row's
 * completion under the mutex and abort loudly on a duplicate, naming
 * the row. */
        if (!was_pfor && parked_at < 0) {
            if (j->counted && atomic_exchange_explicit(&j->counted[r], 1, memory_order_relaxed)) {
                fprintf(stderr, "NTP DOUBLE COUNT: worker=%d job=%d seq=%u row=%d"
                        " (progress=%d/%d rows_done=%d/%d)" "\n",
                        idx, (int)(j - p->job), run_seq, r,
                        (int)atomic_load_explicit(&j->progress[r],
                                                  memory_order_relaxed),
                        j->ncols,
                        (int)atomic_load_explicit(&j->rows_done,
                                                  memory_order_relaxed),
                        j->nrows);
                abort();
            }
        }
        if (was_pfor
            /* ACQUIRE, not relaxed: an EXHAUSTED worker can observe the last
 * completer's count and become the done-setter -- its load must
 * synchronize with that completer's release-add, or the waiter
 * wakes before the final unit's data is visible. The seq guard is
 * the phantom-count fix's second half: a reused slot (seq moved)
 * is not ours to touch at all. */
            ? (j->seq == run_seq
               && atomic_load_explicit(&j->rows_done, memory_order_acquire)
                  == j->nrows
               && !j->done)
            : (parked_at < 0 && atomic_fetch_add_explicit(&j->rows_done, 1,
                                    memory_order_relaxed) + 1 == j->nrows)) {
            j->done = 1;
            if (p->prof) {
                uint64_t now = prof_now();
                if (j->tag_idx >= 0) {
                    atomic_fetch_add_explicit(&p->tag[j->tag_idx].span_ns,
                                              now - j->t_reg, memory_order_relaxed);
                    atomic_fetch_add_explicit(&p->tag[j->tag_idx].jobs, 1,
                                              memory_order_relaxed);
                }
                if (--p->live_jobs == 0) {
                    p->t_empty0 = now;
                    atomic_store_explicit(&p->empty_since, now, memory_order_release);
                }
            }
            pthread_cond_broadcast(&j->done_cv);
        }
    }
    pthread_mutex_unlock(&p->mtx);
    return NULL;
}

ntp_pool_t *ntp_pool_create(int nthreads)
{
    if (nthreads < 1)
        return NULL;
    (void)park_on(); (void)ntp_stats_on();   /* resolve env statics up front */
    (void)ntp_prof_env(); (void)spin_budget_ns(); (void)spin_row_ns();
    (void)ntp_rowfast_on();
    (void)spin_idle_ns(); (void)spin_join_ns();
    (void)ntp_fastclaim_on(); (void)ntp_wake1_on();
    struct ntp_pool *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->prof = ntp_prof_env();
    if (p->prof) {
        p->wprof = calloc((size_t)nthreads, sizeof *p->wprof);
        if (!p->wprof)
            p->prof = 0;                     /* profiling is best-effort */
        p->t_create = p->t_empty0 = prof_now();
    }
    p->nthreads = nthreads;
    p->threads = calloc((size_t)nthreads, sizeof *p->threads);
    p->worker_cv = calloc((size_t)nthreads, sizeof *p->worker_cv);
    p->idle_stack = calloc((size_t)nthreads, sizeof *p->idle_stack);
    p->in_stack = calloc((size_t)nthreads, sizeof *p->in_stack);
    if (!p->threads || !p->worker_cv || !p->idle_stack || !p->in_stack) {
        free(p->threads); free(p->worker_cv); free(p->idle_stack);
        free(p->in_stack); free(p);
        return NULL;
    }
    pthread_mutex_init(&p->mtx, NULL);
    for (int i = 0; i < nthreads; i++) pthread_cond_init(&p->worker_cv[i], NULL);
    pthread_cond_init(&p->slot_cv, NULL);
    for (int i = 0; i < NTP_CV_SLOTS; i++) pthread_cond_init(&p->prog_cv[i], NULL);
    for (int i = 0; i < NTP_MAX_JOBS; i++) pthread_cond_init(&p->job[i].done_cv, NULL);

    for (int i = 0; i < nthreads; i++) {
        void **a = malloc(2 * sizeof(void *));
        a[0] = p; a[1] = (void *)(intptr_t)i;
        if (pthread_create(&p->threads[i], NULL, worker_main, a) != 0) {
            free(a);
            /* tear down the workers already started */
            pthread_mutex_lock(&p->mtx);
            p->shutdown = 1;
            wake_all(p);
            pthread_mutex_unlock(&p->mtx);
            for (int k = 0; k < i; k++) pthread_join(p->threads[k], NULL);
            pthread_mutex_destroy(&p->mtx);
            for (int k = 0; k < nthreads; k++) pthread_cond_destroy(&p->worker_cv[k]);
            pthread_cond_destroy(&p->slot_cv);
            for (int k = 0; k < NTP_CV_SLOTS; k++) pthread_cond_destroy(&p->prog_cv[k]);
            for (int k = 0; k < NTP_MAX_JOBS; k++) pthread_cond_destroy(&p->job[k].done_cv);
            free(p->threads); free(p->worker_cv); free(p->idle_stack);
            free(p->in_stack); free(p->wprof); free(p);
            return NULL;
        }
    }
    return p;
}

/* Grow-only scratch slot `i` of the CALLING THREAD's lane, returning it. Each
 * thread that submits work to the pool (encoder main thread, stair runner,
 * fpipe bg) gets its own bank on first use, so concurrent frames never carve
 * the same buffer; within a thread the memory persists across frames,
 * replacing the per-frame malloc/free churn in the wavefront run functions.
 * Contents are undefined (the caller overwrites). Returns NULL when the lanes
 * are exhausted (more than NTP_LANES submitting threads) -- callers fall back
 * to their serial path. */
void *ntp_pool_slot(ntp_pool_t *p, int i, size_t bytes)
{
    if (!p || i < 0 || i >= 8)
        return NULL;
    pthread_t self = pthread_self();
    struct ntp_lane *ln = NULL;
    for (int k = 0; k < NTP_LANES; k++)
        if (atomic_load_explicit(&p->lane[k].used, memory_order_acquire) &&
            pthread_equal(p->lane[k].owner, self)) { ln = &p->lane[k]; break; }
    if (!ln) {
        pthread_mutex_lock(&p->mtx);
        for (int k = 0; k < NTP_LANES; k++) {
            if (!atomic_load_explicit(&p->lane[k].used, memory_order_relaxed)) {
                p->lane[k].owner = self;
                atomic_store_explicit(&p->lane[k].used, 1, memory_order_release);
                ln = &p->lane[k];
                break;
            }
        }
        pthread_mutex_unlock(&p->mtx);
        if (!ln)
            return NULL;
    }
    if (bytes > ln->scap[i]) {
        void *n = realloc(ln->sslot[i], bytes);
        if (!n)
            return NULL;
        ln->sslot[i] = n;
        ln->scap[i] = bytes;
    }
    return ln->sslot[i];
}

/* Bucket upper bound (us) containing the p-th percentile of a log2 histogram. */
static double hist_pct(const long *h, double p)
{
    long n = 0;
    for (int b = 0; b < NTP_HB; b++) n += h[b];
    if (!n) return 0;
    long target = (long)(p * (double)n), acc = 0;
    for (int b = 0; b < NTP_HB; b++) {
        acc += h[b];
        if (acc >= target) return hb_upper_us(b);
    }
    return hb_upper_us(NTP_HB - 1);
}

/* prof report: per-worker busy vs waits by site, tag table, pool-empty time.
 * Called after every worker joined, so the per-worker structs are quiescent. */
static void ntp_prof_dump(struct ntp_pool *p, uint64_t t_end)
{
    double MS = 1e-6;
    double life = (double)(t_end - p->t_create) * MS;
    uint64_t empty = p->empty_ns +
                     (p->live_jobs == 0 ? t_end - p->t_empty0 : 0);
    fprintf(stderr, "\n=== Y264_NTP_PROF (%d workers, lifetime %.1f ms, "
            "%ld jobs) ===\n", p->nthreads, life, p->njobs);
    fprintf(stderr, "  pool-empty (no live jobs; serial phases): %.1f ms "
            "(%.1f%% of lifetime)\n", (double)empty * MS,
            100.0 * (double)empty * MS / (life > 0 ? life : 1));
    fprintf(stderr, "  %3s %9s %9s(%7s) %8s %9s(%6s) %9s(%6s) %9s(%6s) %9s(%6s) %7s %7s\n",
            "wkr", "busy-ms", "midrow", "n", "spin", "gate", "n", "ramp", "n",
            "tail", "n", "nojob", "n", "claims", "rows");
    struct ntp_wprof tot = {0};
    for (int i = 0; i < p->nthreads; i++) {
        struct ntp_wprof *w = &p->wprof[i];
        fprintf(stderr, "  %3d %9.1f %9.1f(%7ld) %8.1f %9.1f(%6ld) %9.1f(%6ld) "
                "%9.1f(%6ld) %9.1f(%6ld) %7ld %7ld\n", i,
                (double)w->busy_ns * MS, (double)w->midrow_ns * MS, w->midrow_cnt,
                (double)w->spin_ns * MS,
                (double)w->idle_ns[NPW_GATE] * MS, w->idle_cnt[NPW_GATE],
                (double)w->idle_ns[NPW_RAMP] * MS, w->idle_cnt[NPW_RAMP],
                (double)w->idle_ns[NPW_TAIL] * MS, w->idle_cnt[NPW_TAIL],
                (double)w->idle_ns[NPW_NOJOB] * MS, w->idle_cnt[NPW_NOJOB],
                w->claims, w->rows);
        tot.busy_ns += w->busy_ns;
        tot.midrow_ns += w->midrow_ns;
        tot.midrow_cnt += w->midrow_cnt;
        tot.spin_ns += w->spin_ns;
        tot.rows += w->rows;
        tot.claims += w->claims;
        for (int k = 0; k < NPW_NCLASS; k++) {
            tot.idle_ns[k] += w->idle_ns[k];
            tot.idle_cnt[k] += w->idle_cnt[k];
        }
    }
    double cap = life * p->nthreads;
    fprintf(stderr, "  SUM %9.1f %9.1f(%7ld) %8.1f %9.1f(%6ld) %9.1f(%6ld) %9.1f(%6ld) "
            "%9.1f(%6ld) %7ld %7ld\n", (double)tot.busy_ns * MS,
            (double)tot.midrow_ns * MS, tot.midrow_cnt,
            (double)tot.spin_ns * MS,
            (double)tot.idle_ns[NPW_GATE] * MS, tot.idle_cnt[NPW_GATE],
            (double)tot.idle_ns[NPW_RAMP] * MS, tot.idle_cnt[NPW_RAMP],
            (double)tot.idle_ns[NPW_TAIL] * MS, tot.idle_cnt[NPW_TAIL],
            (double)tot.idle_ns[NPW_NOJOB] * MS, tot.idle_cnt[NPW_NOJOB],
            tot.claims, tot.rows);
    fprintf(stderr, "  %% of %d x lifetime: busy %.1f%%  midrow %.1f%%  "
            "idle-spin %.1f%%  gate %.1f%%  ramp %.1f%%  tail %.1f%%  nojob %.1f%%  "
            "(unaccounted %.1f%%)\n", p->nthreads,
            100.0 * (double)tot.busy_ns * MS / cap,
            100.0 * (double)tot.midrow_ns * MS / cap,
            100.0 * (double)tot.spin_ns * MS / cap,
            100.0 * (double)tot.idle_ns[NPW_GATE] * MS / cap,
            100.0 * (double)tot.idle_ns[NPW_RAMP] * MS / cap,
            100.0 * (double)tot.idle_ns[NPW_TAIL] * MS / cap,
            100.0 * (double)tot.idle_ns[NPW_NOJOB] * MS / cap,
            100.0 * (cap - (double)(tot.busy_ns + tot.midrow_ns + tot.spin_ns +
                                    tot.idle_ns[0] + tot.idle_ns[1] +
                                    tot.idle_ns[2] + tot.idle_ns[3]) * MS) / cap);
    /* mid-row sub-attribution: entry vs catch-up, and how each resolved */
    {
        struct ntp_sitestat s[NSS_NSITE];
        memset(s, 0, sizeof s);
        long lead[8] = {0};
        for (int i = 0; i < p->nthreads; i++) {
            for (int k = 0; k < NSS_NSITE; k++) {
                struct ntp_sitestat *d = &s[k], *o = &p->wprof[i].site[k];
                d->spin_ns += o->spin_ns; d->sleep_ns += o->sleep_ns;
                d->park_ns += o->park_ns; d->dep_ns += o->dep_ns;
                d->wake_ns += o->wake_ns;
                d->spin_hit += o->spin_hit; d->raced += o->raced;
                d->slept += o->slept; d->parked += o->parked;
                d->wake_n += o->wake_n;
                for (int b = 0; b < NTP_HB; b++) {
                    d->hspin[b] += o->hspin[b];
                    d->hsleep[b] += o->hsleep[b];
                    d->hwake[b] += o->hwake[b];
                }
                for (int b = 0; b < 8; b++) d->deficit[b] += o->deficit[b];
            }
            for (int b = 0; b < 8; b++) lead[b] += p->wprof[i].lead[b];
        }
        fprintf(stderr, "  --- midrow sub-attribution (entry = first stall after a"
                " row claim, mid = catch-up) ---\n");
        fprintf(stderr, "  %-5s %9s %8s %7s %7s %8s %8s %8s   %s\n", "site",
                "events", "spin-hit", "spin-ms", "slp-ms", "dep-ms", "wake-ms",
                "park-ms", "p50/p90/p99 us: spin | sleep | wake-latency");
        for (int k = 0; k < NSS_NSITE; k++) {
            struct ntp_sitestat *d = &s[k];
            long n = d->spin_hit + d->raced + d->slept + d->parked;
            fprintf(stderr, "  %-5s %9ld %7.1f%% %7.1f %7.1f %8.1f %8.1f %8.1f   "
                    "%.2f/%.2f/%.2f | %.1f/%.1f/%.1f | %.1f/%.1f/%.1f\n",
                    k == NSS_ENTRY ? "entry" : "mid", n,
                    n ? 100.0 * (double)d->spin_hit / (double)n : 0.0,
                    (double)d->spin_ns * MS, (double)d->sleep_ns * MS,
                    (double)d->dep_ns * MS, (double)d->wake_ns * MS,
                    (double)d->park_ns * MS,
                    hist_pct(d->hspin, 0.5), hist_pct(d->hspin, 0.9),
                    hist_pct(d->hspin, 0.99),
                    hist_pct(d->hsleep, 0.5), hist_pct(d->hsleep, 0.9),
                    hist_pct(d->hsleep, 0.99),
                    hist_pct(d->hwake, 0.5), hist_pct(d->hwake, 0.9),
                    hist_pct(d->hwake, 0.99));
            fprintf(stderr, "        raced=%ld slept=%ld (wake-stamped %ld) "
                    "parked=%ld  deficit cells 1:%ld 2:%ld 3:%ld 4:%ld 5-8:%ld "
                    "9-16:%ld 17-32:%ld >32:%ld\n", d->raced, d->slept,
                    d->wake_n, d->parked, d->deficit[0], d->deficit[1],
                    d->deficit[2], d->deficit[3], d->deficit[4], d->deficit[5],
                    d->deficit[6], d->deficit[7]);
        }
        uint64_t lk = 0, pk = 0, ik = 0; long in = 0;
        for (int i = 0; i < p->nthreads; i++) {
            lk += p->wprof[i].lock_ns;
            pk += p->wprof[i].pick_ns;
            ik += p->wprof[i].init_ns;
            in += p->wprof[i].init_n;
        }
        fprintf(stderr, "  per-unit pool-mutex: acquire %.1f ms (%.1f%% of cap)"
                "  claim scan %.1f ms (%.1f%%)  over %ld units\n",
                (double)lk * MS, 100.0 * (double)lk * MS / cap,
                (double)pk * MS, 100.0 * (double)pk * MS / cap, tot.rows);
        fprintf(stderr, "  job-switch thread_init/attach: %.1f ms (%.1f%% of cap)"
                " over %ld switches\n", (double)ik * MS,
                100.0 * (double)ik * MS / cap, in);
        if (p->prof >= 2) {
            long hc[NTP_HB] = {0}, n = 0;
            for (int i = 0; i < p->nthreads; i++)
                for (int b = 0; b < NTP_HB; b++) { hc[b] += p->wprof[i].hcell[b]; }
            for (int b = 0; b < NTP_HB; b++) n += hc[b];
            fprintf(stderr, "  cell duration over %ld cells: p50 %.1f p90 %.1f "
                    "p99 %.1f p99.9 %.1f us\n", n, hist_pct(hc, 0.5),
                    hist_pct(hc, 0.9), hist_pct(hc, 0.99), hist_pct(hc, 0.999));
        }
        fprintf(stderr, "  lead at claim (cells row r-1 was ahead) 2:%ld 3:%ld "
                "4:%ld 5-8:%ld 9-16:%ld 17-32:%ld 33-64:%ld >64:%ld\n",
                lead[0], lead[1], lead[2], lead[3], lead[4], lead[5], lead[6],
                lead[7]);
    }
    if (p->ntags) {
        fprintf(stderr, "  %-16s %6s %9s %10s %10s %10s %10s\n", "tag", "jobs",
                "rows", "busy-ms", "midrow-ms", "join-ms", "span-ms");
        for (int t = 0; t < p->ntags; t++) {
            struct ntp_tagstat *ts = &p->tag[t];
            fprintf(stderr, "  %-16s %6ld %9ld %10.1f %10.1f %10.1f %10.1f\n",
                    ts->name, atomic_load(&ts->jobs), atomic_load(&ts->rows),
                    (double)atomic_load(&ts->busy_ns) * MS,
                    (double)atomic_load(&ts->midrow_ns) * MS,
                    (double)atomic_load(&ts->join_ns) * MS,
                    (double)atomic_load(&ts->span_ns) * MS);
        }
    }
    fprintf(stderr, "  wakes: one=%ld worker=%ld all-events=%ld kicks=%ld  "
            "slot-wait %.1f ms\n", p->wakes_one, p->wakes_worker, p->wakes_all,
            p->kicks, (double)p->slot_wait_ns * MS);
    fprintf(stderr, "  bg (process-wide): syncs=%ld wait %.1f ms\n",
            atomic_load(&st_bg_syncs),
            (double)atomic_load(&st_bg_wait_ns) * MS);
}

void ntp_pool_destroy(ntp_pool_t *p)
{
    if (!p)
        return;
    {
        if (ntp_stats_on())
            fprintf(stderr, "NTP claims=%ld inplace=%ld parks=%ld resumes=%ld "
                    "start_sig=%ld hint_bc=%ld idle_sleeps=%ld "
                    "spin_row=%ld/%ld spin_idle=%ld/%ld\n",
                    atomic_load(&st_claims), atomic_load(&st_inplace),
                    atomic_load(&st_parks), atomic_load(&st_resumes),
                    atomic_load(&st_start_sig), atomic_load(&st_hint_bc),
                    atomic_load(&st_idle_sleeps),
                    atomic_load(&st_spin_row_hit), atomic_load(&st_spin_row),
                    atomic_load(&st_spin_idle_hit), atomic_load(&st_spin_idle));
    }
    uint64_t t_end = p->prof ? prof_now() : 0;  /* pre-shutdown: exclude teardown */
    pthread_mutex_lock(&p->mtx);
    p->shutdown = 1;
    wake_all(p);
    pthread_mutex_unlock(&p->mtx);
    for (int i = 0; i < p->nthreads; i++)
        pthread_join(p->threads[i], NULL);
    if (p->prof)
        ntp_prof_dump(p, t_end);
    free(p->wprof);
    pthread_mutex_destroy(&p->mtx);
    for (int i = 0; i < p->nthreads; i++) pthread_cond_destroy(&p->worker_cv[i]);
    pthread_cond_destroy(&p->slot_cv);
    for (int i = 0; i < NTP_CV_SLOTS; i++) pthread_cond_destroy(&p->prog_cv[i]);
    for (int i = 0; i < NTP_MAX_JOBS; i++) {
        pthread_cond_destroy(&p->job[i].done_cv);
        free(p->job[i].progress);
        free(p->job[i].parked_need);
        free(p->job[i].parked_owner);
        free(p->job[i].counted);
    }
    for (int k = 0; k < NTP_LANES; k++)
        for (int i = 0; i < 8; i++)
            free(p->lane[k].sslot[i]);
    free(p->threads);
    free(p->worker_cv);
    free(p->idle_stack);
    free(p->in_stack);
    free(p);
}

int ntp_pool_nthreads(const ntp_pool_t *p)
{
    return p ? p->nthreads : 0;
}

/* Pool-empty milliseconds so far, including an interval in progress. Read
 * without the mutex (two relaxed atomics written at the live_jobs 0-crossings),
 * so a caller sampling it either side of a driver stage can charge that stage
 * for the idleness it caused. Profiling only: it reads 0 unless Y264_NTP_PROF
 * is on, because live_jobs is only tracked there. */
double ntp_pool_empty_ms(const ntp_pool_t *p)
{
    if (!p)
        return 0.0;
    uint64_t since = atomic_load_explicit(&p->empty_since, memory_order_acquire);
    uint64_t tot = atomic_load_explicit(&p->empty_pub, memory_order_relaxed);
    if (since) {
        uint64_t now = prof_now();
        if (now > since) tot += now - since;
    }
    return (double)tot / 1e6;
}

/* External-progress kick: re-evaluate every gated claim. Called by a producer
 * outside the pool (e.g. the staircase trailer after publishing a consumable
 * anchor row) so parked workers re-check row_ready gates. Broadcast is fine
 * here: kicks are per-published-row (~one per MB row per frame), not per cell. */
void ntp_pool_kick(ntp_pool_t *p)
{
    if (!p)
        return;
    atomic_fetch_add_explicit(&p->work_epoch, 1, memory_order_relaxed);
    pthread_mutex_lock(&p->mtx);
    if (p->prof) p->kicks++;
    wake_all(p);
    pthread_mutex_unlock(&p->mtx);
}

/* Register a job and return its slot. Blocks while the table is full unless
 * `nonblock` -- then it returns NULL with *full = 1 and registers nothing. A
 * CALLER THAT HOLDS UNJOINED JOBS MUST PASS nonblock: a job's slot is released
 * in job_wait, so a submitter that blocks for a slot while holding jobs it has
 * not waited on can never be unblocked by its own work (the batch-registration
 * deadlock -- see ntp_wavefront_batch). With nothing held, blocking is safe:
 * every job in the table belongs to some other thread that will join it.
 * Pool mutex taken inside. */
static struct ntp_job *job_register_ex(struct ntp_pool *p, int kind,
                                    int nrows, int ncols,
                                    void (*thread_init)(void *, int),
                                    void (*thread_attach)(void *, int),
                                    void (*cell_fn)(void *, int, int, int),
                                    void (*for_fn)(void *, int, int), void *ctx,
                                    int (*row_ready)(void *, int), void *gate_ctx,
                                    int nonblock, int *full)
{
    const char *tag = ntp_tls_tag;
    ntp_tls_tag = NULL;                      /* one-shot */
    if (full) *full = 0;
    pthread_mutex_lock(&p->mtx);
    struct ntp_job *j = NULL;
    for (;;) {
        for (int i = 0; i < NTP_MAX_JOBS; i++)
            if (!p->job[i].used) { j = &p->job[i]; break; }
        if (j)
            break;
        if (nonblock) {
            if (full) *full = 1;
            ntp_tls_prio = 0;                /* one-shot, consumed either way */
            pthread_mutex_unlock(&p->mtx);
            return NULL;
        }
        if (!p->prof) {
            pthread_cond_wait(&p->slot_cv, &p->mtx);
        } else {
            uint64_t t0 = prof_now();
            pthread_cond_wait(&p->slot_cv, &p->mtx);
            p->slot_wait_ns += prof_now() - t0;
        }
    }
    if (kind == 0) {
        if (nrows > j->progress_cap) {
            _Atomic int *np = realloc(j->progress, (size_t)nrows * sizeof *np);
            _Atomic int *pn = realloc(j->parked_need, (size_t)nrows * sizeof *pn);
            int         *po = realloc(j->parked_owner, (size_t)nrows * sizeof *po);
            _Atomic uint8_t *ct = realloc(j->counted, (size_t)nrows);
            if (np) j->progress = np;
            if (pn) j->parked_need = pn;
            if (po) j->parked_owner = po;
            if (ct) j->counted = ct;
            if (!np || !pn || !po || !ct) { /* leave old buffers; caller must retry serial */
                pthread_mutex_unlock(&p->mtx);
                return NULL;
            }
            j->progress_cap = nrows;
        }
        for (int r = 0; r < nrows; r++) {
            atomic_store_explicit(&j->progress[r], 0, memory_order_relaxed);
            atomic_store_explicit(&j->parked_need[r], INT_MAX, memory_order_relaxed);
            j->counted[r] = 0;
        }
    }
    j->prio = ntp_tls_prio;
    ntp_tls_prio = 0;                        /* one-shot */
    j->tag_idx = -1;
    if (p->prof) {
        uint64_t now = prof_now();
        j->t_reg = now;
        if (p->live_jobs++ == 0) {
            p->empty_ns += now - p->t_empty0;
            atomic_store_explicit(&p->empty_pub, p->empty_ns, memory_order_relaxed);
            atomic_store_explicit(&p->empty_since, 0, memory_order_release);
        }
        p->njobs++;
        if (tag) {
            int t;
            for (t = 0; t < p->ntags; t++)
                if (strcmp(p->tag[t].name, tag) == 0)
                    break;
            if (t == p->ntags && t < NTP_PROF_TAGS)
                p->tag[p->ntags++].name = tag;
            if (t < p->ntags)
                j->tag_idx = t;
        }
    }
    j->used = 1;
    j->done = 0;
    j->seq = ++p->seq_next;
    j->kind = kind;
    j->nrows = nrows;
    j->ncols = ncols;
    j->thread_init = thread_init;
    j->thread_attach = thread_attach;
    j->cell_fn = cell_fn;
    j->for_fn = for_fn;
    j->ctx = ctx;
    j->row_ready = row_ready;
    j->gate_ctx = gate_ctx;
    atomic_store_explicit(&j->next_row, 0, memory_order_relaxed);
    atomic_store_explicit(&j->claimw,
                          (uint64_t)(uint32_t)j->seq << 32,
                          memory_order_release);
    atomic_store_explicit(&j->rows_done, 0, memory_order_relaxed);
    /* Row 0 (and for a parallel-for, every index) is claimable now.
 * Wake ONE worker, not all (Y264_NTP_WAKE1=0 escapes): only row 0 is
 * claimable on a kind-0 job anyway, and for parallel-fors the claim-time
 * cascade (runnable work + idler => wake in flight) fans out one wake per
 * claim. wake_all at every registration is the largest single wake source
 * -- it DOUBLES the wakes from t8 to t12 in width_prof. */
    atomic_fetch_add_explicit(&p->work_epoch, 1, memory_order_relaxed);
    if (ntp_wake1_on())
        wake_one(p);
    else
        wake_all(p);
    pthread_mutex_unlock(&p->mtx);
    return j;
}

static struct ntp_job *job_register(struct ntp_pool *p, int kind,
                                    int nrows, int ncols,
                                    void (*thread_init)(void *, int),
                                    void (*thread_attach)(void *, int),
                                    void (*cell_fn)(void *, int, int, int),
                                    void (*for_fn)(void *, int, int), void *ctx,
                                    int (*row_ready)(void *, int), void *gate_ctx)
{
    return job_register_ex(p, kind, nrows, ncols, thread_init, thread_attach,
                           cell_fn, for_fn, ctx, row_ready, gate_ctx, 0, NULL);
}

/* Block until the job completes, then release its slot. The submitter (often
 * the API thread -- the encode's critical path) spins briefly on the atomic
 * `done` before committing to the condvar: a short job's join then costs no
 * wake round-trip. `done` is still SET under the mutex; the spin only reads. */
static void job_wait(struct ntp_pool *p, struct ntp_job *j)
{
    uint64_t t0 = p->prof ? prof_now() : 0;
    int sb = spin_join_ns();
    if (sb && !atomic_load_explicit(&j->done, memory_order_acquire)) {
        int _sat = 0;
        NTP_SPIN_WAIT(sb, atomic_load_explicit(&j->done, memory_order_acquire));
        (void)_sat;
    }
    pthread_mutex_lock(&p->mtx);
    while (!atomic_load_explicit(&j->done, memory_order_relaxed))
        pthread_cond_wait(&j->done_cv, &p->mtx);
    if (p->prof && j->tag_idx >= 0)
        atomic_fetch_add_explicit(&p->tag[j->tag_idx].join_ns,
                                  prof_now() - t0, memory_order_relaxed);
    j->used = 0;
    pthread_cond_signal(&p->slot_cv);
    pthread_mutex_unlock(&p->mtx);
}

void ntp_wavefront_gated(ntp_pool_t *p, int nrows, int ncols,
                         void (*thread_init)(void *, int),
                         void (*thread_attach)(void *, int),
                         void (*cell_fn)(void *, int, int, int), void *ctx,
                         int (*row_ready)(void *, int), void *gate_ctx)
{
    if (nrows <= 0 || ncols <= 0)
        return;
    struct ntp_job *j = job_register(p, 0, nrows, ncols, thread_init,
                                     thread_attach, cell_fn, NULL, ctx,
                                     row_ready, gate_ctx);
    if (!j) {
        /* Progress-array OOM. The caller cannot retry (this returns void and
 * every analyze caller treats a return as a frame done), so run the
 * job here in raster order, honouring the row gate by waiting on it:
 * the same fallback ntp_wavefront_batch already takes. Found by the
 * 2026-09-04 review: before this, an OOM here committed a frame from
 * uninitialised macroblock decisions. */
        if (thread_init) thread_init(ctx, 0);
        if (thread_attach) thread_attach(ctx, 0);
        for (int r = 0; r < nrows; r++) {
            while (row_ready && !row_ready(gate_ctx, r))
                sched_yield();
            for (int c = 0; c < ncols; c++)
                cell_fn(ctx, 0, r, c);
        }
        return;
    }
    job_wait(p, j);
}

void ntp_wavefront(ntp_pool_t *p, int nrows, int ncols,
                   void (*thread_init)(void *, int),
                   void (*cell_fn)(void *, int, int, int), void *ctx)
{
    ntp_wavefront_gated(p, nrows, ncols, thread_init, NULL, cell_fn, ctx,
                        NULL, NULL);
}

/* Independent wavefronts as concurrent jobs (see threadpool.h). Registration
 * can block on a full table; that is safe (jobs complete without their
 * submitter) and just serializes the overflow. The one-shot prof tag applies
 * to every job of the batch. */
void ntp_wavefront_batch(ntp_pool_t *p, int n, const ntp_wf_spec_t *spec)
{
    const char *tag = ntp_tls_tag;
    int prio = ntp_tls_prio;
    while (n > 0) {
        int m = n < 16 ? n : 16;
        struct ntp_job *jobs[16];
        int held = 0, i;
        /* Register as many of the chunk as the table has room for, then wait on
 * THOSE and come back for the rest. Once we hold a job, registration
 * must not block: our own slots are released in job_wait below, so
 * blocking here would deadlock against ourselves (2*bframes legs vs a
 * table of NTP_MAX_JOBS -- the bframes-7 hang). With nothing held,
 * blocking is safe: the table's jobs belong to other threads. */
        for (i = 0; i < m; i++) {
            if (spec[i].nrows <= 0 || spec[i].ncols <= 0) {
                jobs[i] = NULL;
                continue;
            }
            ntp_tls_tag = tag;
            ntp_tls_prio = prio;
            int table_full = 0;
            jobs[i] = job_register_ex(p, 0, spec[i].nrows, spec[i].ncols,
                                      spec[i].thread_init, NULL, spec[i].cell_fn,
                                      NULL, spec[i].ctx, NULL, NULL,
                                      held > 0, &table_full);
            if (!jobs[i] && table_full)
                break;                  /* drain what we hold, then retry from i */
            if (!jobs[i]) {             /* progress OOM: run this one serially
 * (raster order satisfies the deps) */
                if (spec[i].thread_init)
                    spec[i].thread_init(spec[i].ctx, 0);
                for (int r = 0; r < spec[i].nrows; r++)
                    for (int c = 0; c < spec[i].ncols; c++)
                        spec[i].cell_fn(spec[i].ctx, 0, r, c);
            } else {
                held++;
            }
        }
        for (int k = 0; k < i; k++)
            if (jobs[k])
                job_wait(p, jobs[k]);
        spec += i;                      /* i = specs consumed this pass (>= 1) */
        n -= i;
    }
    ntp_tls_tag = NULL;
    ntp_tls_prio = 0;
}

void ntp_parallel_for(ntp_pool_t *p, int n,
                      void (*for_fn)(void *, int, int), void *ctx)
{
    if (n <= 0)
        return;
    struct ntp_job *j = job_register(p, 1, n, 1, NULL, NULL, NULL, for_fn, ctx,
                                     NULL, NULL);
    if (!j) {                            /* progress OOM: run it here */
        for (int i = 0; i < n; i++)
            for_fn(ctx, 0, i);
        return;
    }
    job_wait(p, j);
}

/* ------------------------------------------------------------------ */
/* Single background worker (one outstanding task, submit/sync). */
/* ------------------------------------------------------------------ */
struct ntp_bg {
    pthread_t       thread;
    char            name[16];       /* profile name, set before the thread starts */
    pthread_mutex_t mtx;
    pthread_cond_t  cv;
    void          (*fn)(void *);
    void           *arg;
    _Atomic int     have_task;      /* a task is queued or running; atomic so
 * ntp_bg_sync can spin outside the mutex */
    int             shutdown;
};
static void *ntp_bg_main(void *arg)
{
    struct ntp_bg *b = arg;
    y264_thread_name(b->name);
    for (;;) {
        pthread_mutex_lock(&b->mtx);
        while (!b->shutdown && !b->have_task)
            pthread_cond_wait(&b->cv, &b->mtx);
        if (b->shutdown && !b->have_task) { pthread_mutex_unlock(&b->mtx); return NULL; }
        void (*fn)(void *) = b->fn; void *a = b->arg;
        pthread_mutex_unlock(&b->mtx);
        fn(a);
        pthread_mutex_lock(&b->mtx);
        b->have_task = 0;
        pthread_cond_broadcast(&b->cv);     /* wake ntp_bg_sync */
        pthread_mutex_unlock(&b->mtx);
    }
}
ntp_bg_t *ntp_bg_create_named(const char *name)
{
    (void)ntp_prof_env();               /* warm the env static (main thread) */
    struct ntp_bg *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    snprintf(b->name, sizeof b->name, "%s", name && *name ? name : "y264-bg");
    pthread_mutex_init(&b->mtx, NULL);
    pthread_cond_init(&b->cv, NULL);
    if (pthread_create(&b->thread, NULL, ntp_bg_main, b) != 0) {
        pthread_mutex_destroy(&b->mtx); pthread_cond_destroy(&b->cv); free(b); return NULL;
    }
    return b;
}

ntp_bg_t *ntp_bg_create(void) { return ntp_bg_create_named("y264-bg"); }
void ntp_bg_submit(ntp_bg_t *b, void (*fn)(void *), void *arg)
{
    pthread_mutex_lock(&b->mtx);
    b->fn = fn; b->arg = arg; b->have_task = 1;
    pthread_cond_broadcast(&b->cv);
    pthread_mutex_unlock(&b->mtx);
}
void ntp_bg_sync(ntp_bg_t *b)
{
    int sb = spin_join_ns();
    if (sb && atomic_load_explicit(&b->have_task, memory_order_acquire)) {
        int _sat = 0;
        NTP_SPIN_WAIT(sb, !atomic_load_explicit(&b->have_task,
                                                memory_order_acquire));
        (void)_sat;
    }
    pthread_mutex_lock(&b->mtx);
    if (!ntp_prof_env()) {
        while (atomic_load_explicit(&b->have_task, memory_order_relaxed))
            pthread_cond_wait(&b->cv, &b->mtx);
    } else if (atomic_load_explicit(&b->have_task, memory_order_relaxed)) {
        uint64_t t0 = prof_now();
        while (atomic_load_explicit(&b->have_task, memory_order_relaxed))
            pthread_cond_wait(&b->cv, &b->mtx);
        atomic_fetch_add_explicit(&st_bg_wait_ns, prof_now() - t0,
                                  memory_order_relaxed);
        atomic_fetch_add_explicit(&st_bg_syncs, 1, memory_order_relaxed);
    }
    pthread_mutex_unlock(&b->mtx);
}
void ntp_bg_destroy(ntp_bg_t *b)
{
    if (!b) return;
    pthread_mutex_lock(&b->mtx);
    b->shutdown = 1;
    pthread_cond_broadcast(&b->cv);
    pthread_mutex_unlock(&b->mtx);
    pthread_join(b->thread, NULL);
    pthread_mutex_destroy(&b->mtx);
    pthread_cond_destroy(&b->cv);
    free(b);
}
