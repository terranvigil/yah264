/*
 * checkasm - kernel correctness and benchmark harness: the driver and the CLI
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Every dispatched DSP kernel is validated against its portable C reference on
 * randomized and adversarial input. A kernel that disagrees fails the run
 * (non-zero exit), which is what gates a merge. `--bench` adds the timing
 * tables; the multiple a kernel prints there is a coverage reading and has
 * never been a ship criterion on its own.
 *
 *   checkasm                        every group the CPU can run
 *   checkasm --list                 the table: group, family, required ISA
 *   checkasm pixel                  one family
 *   checkasm --isa neon             only the rows a given tier's kernels own
 *   checkasm --bench [family]       correctness, then the timing tables
 *
 * The ISA slots for SSE4.2, AVX2 and AVX-512 exist here with no rows behind
 * them yet; docs/x86-plan.md waves 1-3 fill them in.
 */

#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#include <pthread.h>
#include <sys/qos.h>
#endif
#ifdef __linux__
#include <sched.h>
#endif

#include "checkasm.h"
#include "dsp/transform.h"

int ca_bench;

/* ---- randomness ---------------------------------------------------------- */

uint64_t ca_rng = 0x2545F4914F6CDD1DULL;

static inline uint8_t ca_byte(void)
{
    ca_rng ^= ca_rng << 13;
    ca_rng ^= ca_rng >> 7;
    ca_rng ^= ca_rng << 17;
    return (uint8_t)(ca_rng >> 24);
}

/* Full sample range for the build's depth. A byte alone left a 10-bit build
 * exercising a quarter of the range, which is where a clip bug hides. */
pixel ca_rnd(void)
{
#if Y264_BIT_DEPTH > 8
    return (pixel)((((unsigned)ca_byte() << 8) | ca_byte()) & PIXEL_MAX);
#else
    return (pixel)ca_byte();
#endif
}

void ca_fill(pixel *p, int n)
{
    for (int i = 0; i < n; i++)
        p[i] = ca_rnd();
}

unsigned ca_below(unsigned n)
{
    unsigned v = (unsigned)(ca_rng % n);
    ca_byte();
    return v;
}

double ca_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

/* Pin the bench to a performance core at a ramped frequency. Without the QoS
 * boost macOS may run the process on an efficiency core in a low DVFS state,
 * which measured our SAD kernels 4x slower than their true P-core speed and
 * inverted before/after comparisons. On Linux there is no QoS class to raise,
 * so the pin is an affinity mask: CHECKASM_CPU names the core, and a bench run
 * that does not set it is as noisy as the scheduler is. The warm-up spin ramps
 * the clock either way. */
void ca_bench_prep(void)
{
    static int done = 0;
    if (done)
        return;
    done = 1;
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
#ifdef __linux__
    {
        const char *e = getenv("CHECKASM_CPU");
        if (e) {
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(atoi(e), &set);
            if (sched_setaffinity(0, sizeof(set), &set))
                printf("checkasm: could not pin to CPU %s\n", e);
        }
    }
#endif
    {
        volatile uint64_t x = 1;
        double t0 = ca_now_ns();
        while (ca_now_ns() - t0 < 300e6)        /* ~300 ms spin */
            x = x * 6364136223846793005ull + 1442695040888963407ull;
        (void)x;
    }
}

/* ---- guard buffers ------------------------------------------------------- */

void *ca_guard_alloc(size_t bytes)
{
    uint8_t *base = malloc(bytes + 2 * CA_GUARD);
    if (!base) {
        fprintf(stderr, "checkasm: out of memory\n");
        exit(2);
    }
    memset(base, CA_POISON, bytes + 2 * CA_GUARD);
    return base + CA_GUARD;
}

void ca_guard_free(void *blk)
{
    if (blk)
        free((uint8_t *)blk - CA_GUARD);
}

void ca_guard_poison(void *blk, size_t bytes)
{
    uint8_t *p = blk;
    memset(p - CA_GUARD, CA_POISON, CA_GUARD);
    memset(p + bytes, CA_POISON, CA_GUARD);
}

void ca_guard_scramble(void *blk, size_t bytes)
{
    uint8_t *p = blk;
    for (int i = 0; i < CA_GUARD; i++) {
        p[-CA_GUARD + i] = ca_byte();
        p[bytes + i] = ca_byte();
    }
}

int ca_guard_check(const void *blk, size_t bytes, const char *what)
{
    const uint8_t *p = blk;
    for (int i = 0; i < CA_GUARD; i++) {
        if (p[-CA_GUARD + i] != CA_POISON) {
            printf("    %s: wrote %d byte(s) BEFORE the block\n", what,
                   CA_GUARD - i);
            return 1;
        }
        if (p[bytes + i] != CA_POISON) {
            printf("    %s: wrote %d byte(s) PAST the block\n", what, i + 1);
            return 1;
        }
    }
    return 0;
}

int ca_check_pad(const uint8_t *blk, int stride, int w, int h, const char *what)
{
    for (int y = 0; y < h; y++)
        for (int x = w; x < stride; x++)
            if (blk[(ptrdiff_t)y * stride + x] != CA_POISON) {
                printf("    %s: wrote into the row padding at byte (%d,%d)\n",
                       what, x, y);
                return 1;
            }
    return 0;
}

/* ---- the page guard ------------------------------------------------------ */

static char ca_pg_what[96];

static void ca_pg_fault(int sig)
{
    (void)sig;
    /* async-signal-safe: write(2) and _exit(2) only */
    static const char msg[] =
        "    checkasm: ACCESS OUTSIDE THE DECLARED WINDOW: ";
    ssize_t r = write(2, msg, sizeof(msg) - 1);
    r += write(2, ca_pg_what, strlen(ca_pg_what));
    r += write(2, "\n", 1);
    (void)r;
    _exit(1);
}

void *ca_pg_alloc(ca_pg *g, size_t bytes, int tail)
{
    size_t pg = (size_t)sysconf(_SC_PAGESIZE);
    size_t body = (bytes + pg - 1) / pg * pg;

    g->maplen = body + 2 * pg;
    g->map = mmap(NULL, g->maplen, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANON, -1, 0);
    if (g->map == MAP_FAILED) {
        printf("    checkasm: mmap failed\n");
        exit(2);
    }
    if (mprotect(g->map, pg, PROT_NONE) ||
        mprotect(g->map + pg + body, pg, PROT_NONE)) {
        printf("    checkasm: mprotect failed\n");
        exit(2);
    }
    g->user = g->map + pg + (tail ? body - bytes : 0);
    return g->user;
}

void ca_pg_free(ca_pg *g)
{
    munmap(g->map, g->maplen);
}

void ca_pg_fill(ca_pg *g, size_t bytes)
{
    uint8_t *p = g->user;
    for (size_t i = 0; i < bytes; i++)
        p[i] = ca_byte();
}

void ca_pg_arm(const char *what)
{
    snprintf(ca_pg_what, sizeof(ca_pg_what), "%s", what);
    signal(SIGSEGV, ca_pg_fault);
    signal(SIGBUS, ca_pg_fault);
}

void ca_pg_disarm(void)
{
    signal(SIGSEGV, SIG_DFL);
    signal(SIGBUS, SIG_DFL);
}

/* ---- the ISA tiers ------------------------------------------------------- */

typedef struct ca_isa {
    const char *name;
    uint32_t    bits;       /* every feature bit this tier and below owns */
} ca_isa;

/* A tier's mask INCLUDES the tiers under it, so `--isa avx2` runs the SSE4
 * rows too: that is the question a box with AVX2 is actually asked, since an
 * AVX2 build still dispatches the SSE4 kernels for the shapes it has no wide
 * twin for. `all` admits everything, portable rows included. */
static const ca_isa isas[] = {
    { "neon",   Y264_CPU_NEON | Y264_CPU_DOTPROD | Y264_CPU_I8MM },
    { "sse4",   Y264_CPU_SSE2 | Y264_CPU_SSSE3 | Y264_CPU_SSE4 },
    { "avx2",   Y264_CPU_SSE2 | Y264_CPU_SSSE3 | Y264_CPU_SSE4 | Y264_CPU_AVX |
                Y264_CPU_AVX2 | Y264_CPU_FMA3 | Y264_CPU_BMI2 },
    { "avx512", Y264_CPU_SSE2 | Y264_CPU_SSSE3 | Y264_CPU_SSE4 | Y264_CPU_AVX |
                Y264_CPU_AVX2 | Y264_CPU_FMA3 | Y264_CPU_BMI2 |
                Y264_CPU_AVX512F | Y264_CPU_AVX512BW | Y264_CPU_AVX512VL |
                Y264_CPU_AVXVNNI },
    { "all",    0xffffffffu },
    { NULL, 0 },
};

/* ---- the families -------------------------------------------------------- */

static const ca_test *const families[] = {
    ca_pixel_tests,
    ca_transform_tests,
    ca_mc_tests,
    ca_predict_tests,
    ca_deblock_tests,
    NULL,
};

static void usage(void)
{
    printf("usage: checkasm [--bench] [--isa neon|sse4|avx2|avx512|all] "
           "[--list] [family]\n"
           "  families:");
    for (const ca_test *const *f = families; *f; f++)
        if ((*f)->name)
            printf(" %s", (*f)->family);
    printf("\n");
}

static void list_tests(uint32_t cpu, uint32_t isa_bits)
{
    printf("%-22s %-10s %-24s %s\n", "group", "family", "needs", "here");
    for (const ca_test *const *f = families; *f; f++)
        for (const ca_test *t = *f; t->name; t++) {
            char need[64];
            if (t->cpu_mask)
                y264_cpu_name(t->cpu_mask, need, sizeof(need));
            else
                snprintf(need, sizeof(need), "-");
            printf("%-22s %-10s %-24s %s\n", t->name, t->family, need,
                   (t->cpu_mask & ~cpu) ? "no"
                   : (t->cpu_mask & ~isa_bits) ? "not in --isa" : "yes");
        }
}

int main(int argc, char **argv)
{
    const char *only = NULL, *isa_name = NULL;
    int do_list = 0, fails = 0, ran = 0, skipped = 0;
    uint32_t isa_bits = 0xffffffffu;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bench")) {
            ca_bench = 1;
        } else if (!strcmp(argv[i], "--list")) {
            do_list = 1;
        } else if (!strcmp(argv[i], "--isa")) {
            if (++i >= argc) { usage(); return 2; }
            isa_name = argv[i];
        } else if (!strncmp(argv[i], "--isa=", 6)) {
            isa_name = argv[i] + 6;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            return 0;
        } else if (argv[i][0] == '-') {
            usage();
            return 2;
        } else {
            only = argv[i];
        }
    }

    if (isa_name) {
        const ca_isa *s = isas;
        for (; s->name && strcmp(s->name, isa_name); s++)
            ;
        if (!s->name) {
            printf("checkasm: no such isa '%s'\n", isa_name);
            usage();
            return 2;
        }
        isa_bits = s->bits;
    }
    if (only) {
        int known = 0;
        for (const ca_test *const *f = families; *f; f++)
            for (const ca_test *t = *f; t->name; t++)
                known |= !strcmp(t->family, only);
        if (!known) {
            printf("checkasm: no such family '%s'\n", only);
            usage();
            return 2;
        }
    }

    uint32_t cpu = y264_cpu_detect();
    {
        char name[128];
        y264_cpu_name(cpu, name, sizeof(name));
        printf("checkasm: cpu %s, %d-bit samples%s%s\n", name, Y264_BIT_DEPTH,
               isa_name ? ", isa " : "", isa_name ? isa_name : "");
    }
    /* The tables the transform kernels are fed from are built lazily by the
     * library; resolve them on this thread before any group runs, exactly as
     * the encoder's warm-up does. */
    y264_transform_warm_statics();
    /* Some composed NEON metrics (the 16x16 SA8D) reach the 8x8 kernel through
     * the process-wide dispatch table, so it has to be filled before any group
     * calls one. It is never the thing under test: a group always names the
     * kernel symbol it means. */
    y264_dsp_init();

    if (do_list) {
        list_tests(cpu, isa_bits);
        return 0;
    }
    if (ca_bench)
        ca_bench_prep();

    for (const ca_test *const *f = families; *f; f++)
        for (const ca_test *t = *f; t->name; t++) {
            int r;
            if (only && strcmp(t->family, only))
                continue;
            if (t->cpu_mask & ~isa_bits)
                continue;
            if (t->cpu_mask & ~cpu) {
                printf("  skip %-22s (needs a feature this CPU lacks)\n", t->name);
                skipped++;
                continue;
            }
            r = t->fn();
            printf("  %-4s %s\n", r ? "FAIL" : "ok", t->name);
            fails += !!r;
            ran++;
        }

    printf("checkasm: %d group(s) run, %d skipped, %d failure(s)\n",
           ran, skipped, fails);
    return fails ? 1 : 0;
}
