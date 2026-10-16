/* Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * alloc_census.c -- which call sites hold the memory, and which churn it.
 *
 * A dyld-interposed malloc/calloc/realloc/posix_memalign/aligned_alloc/free
 * that tracks every allocation of 256 KiB or more by its two innermost return
 * addresses and size class: count, bytes allocated over the run (churn), and
 * peak bytes live at once. Printed at exit to $MSHIM_OUT (or stderr) as image
 * offsets; scripts/alloc_census.sh symbolises them with atos. macOS only;
 * the binary must not be hardened (ours is not). Peak "live" is VIRTUAL:
 * a worst-case buffer that is never written counts in full, so read it beside
 * the process's peak footprint from `/usr/bin/time -l`.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <dlfcn.h>
#include <stdint.h>
#include <malloc/malloc.h>
#define MIN_SZ (256*1024)
#define NSITE 4096
#define NPTR (1<<20)
typedef struct { void *ra, *rb; size_t sz; long n; double churn; double live, peak; } site_t;
static site_t sites[NSITE];
static struct { void *p; int site; size_t sz; } ptrs[NPTR];
static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
static double tot_live, tot_peak;
static int site_of(void *ra, void *rb, size_t sz) {
    size_t szc = (sz + 65535) & ~65535UL;
    uintptr_t h = (((uintptr_t)ra ^ ((uintptr_t)rb<<7)) * 2654435761u ^ szc) % NSITE;
    for (int i = 0; i < NSITE; i++) { site_t *s = &sites[(h + i) % NSITE];
        if (!s->ra) { s->ra = ra; s->rb = rb; s->sz = szc; return (h + i) % NSITE; }
        if (s->ra == ra && s->rb == rb && s->sz == szc) return (h + i) % NSITE; }
    return 0;
}
static void track(void *p, size_t sz, void *ra, void *rb) {
    if (!p || sz < MIN_SZ) return;
    pthread_mutex_lock(&mx);
    int si = site_of(ra, rb, sz); site_t *s = &sites[si];
    s->n++; s->churn += sz; s->live += sz; if (s->live > s->peak) s->peak = s->live;
    tot_live += sz; if (tot_live > tot_peak) tot_peak = tot_live;
    uintptr_t h = ((uintptr_t)p >> 4) % NPTR;
    for (int i = 0; i < NPTR; i++) { int k = (h + i) % NPTR; if (!ptrs[k].p || ptrs[k].p == (void*)1) { ptrs[k].p = p; ptrs[k].site = si; ptrs[k].sz = sz; break; } }
    pthread_mutex_unlock(&mx);
}
static void untrack(void *p) {
    if (!p) return;
    pthread_mutex_lock(&mx);
    uintptr_t h = ((uintptr_t)p >> 4) % NPTR;
    for (int i = 0; i < NPTR; i++) { int k = (h + i) % NPTR;
        if (!ptrs[k].p) break;
        if (ptrs[k].p == p) { sites[ptrs[k].site].live -= ptrs[k].sz; tot_live -= ptrs[k].sz; ptrs[k].p = (void*)1; break; } }
    pthread_mutex_unlock(&mx);
}
static void *my_malloc(size_t n) { void *p = malloc(n); if (n >= MIN_SZ) track(p, n, __builtin_return_address(0), __builtin_return_address(1)); return p; }
static void *my_calloc(size_t a, size_t b) { void *p = calloc(a, b); if (a*b >= MIN_SZ) track(p, a*b, __builtin_return_address(0), __builtin_return_address(1)); return p; }
static void *my_realloc(void *o, size_t n) { if (o) untrack(o); void *p = realloc(o, n); if (n >= MIN_SZ) track(p, n, __builtin_return_address(0), __builtin_return_address(1)); return p; }
static int my_pm(void **pp, size_t al, size_t n) { int r = posix_memalign(pp, al, n); if (!r && n >= MIN_SZ) track(*pp, n, __builtin_return_address(0), __builtin_return_address(1)); return r; }
static void *my_aa(size_t al, size_t n) { void *p = aligned_alloc(al, n); if (n >= MIN_SZ) track(p, n, __builtin_return_address(0), __builtin_return_address(1)); return p; }
static void my_free(void *p) { if (p && malloc_size(p) >= MIN_SZ) untrack(p); free(p); }
static int cmp(const void *a, const void *b) { const site_t *x = a, *y = b; double kx = x->peak, ky = y->peak; return kx < ky ? 1 : kx > ky ? -1 : 0; }
__attribute__((destructor)) static void dump(void) {
    const char *path = getenv("MSHIM_OUT"); FILE *f = path ? fopen(path, "w") : stderr; if (!f) f = stderr;
    qsort(sites, NSITE, sizeof(site_t), cmp);
    fprintf(f, "total peak live %.2f GiB\n%-50s %10s %8s %10s %10s\n", tot_peak/1073741824.0, "site", "sizeKB", "count", "peakGiB", "churnGiB");
    for (int i = 0; i < NSITE && sites[i].ra; i++) {
        if (sites[i].peak < 16e6 && sites[i].churn < 1e9) continue;
        Dl_info d; char nm[256]; if (dladdr(sites[i].ra, &d)) { const char *b = strrchr(d.dli_fname, '/'); snprintf(nm, sizeof nm, "0x%lx 0x%lx", (long)((char*)sites[i].ra - (char*)d.dli_fbase), (long)((char*)sites[i].rb - (char*)d.dli_fbase)); (void)b; } else snprintf(nm, sizeof nm, "%p", sites[i].ra);
        fprintf(f, "%-50s %10zu %8ld %10.2f %10.2f\n", nm, sites[i].sz/1024, sites[i].n, sites[i].peak/1073741824.0, sites[i].churn/1073741824.0);
    }
    if (f != stderr) fclose(f);
}
typedef struct { const void *r, *o; } ip_t;
__attribute__((used)) static const ip_t ip[] __attribute__((section("__DATA,__interpose"))) = {
    {my_malloc, malloc}, {my_calloc, calloc}, {my_realloc, realloc}, {my_free, free}, {my_pm, posix_memalign}, {my_aa, aligned_alloc} };
