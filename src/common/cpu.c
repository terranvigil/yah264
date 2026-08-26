/*
 * cpu.c - runtime CPU feature detection
 * Copyright (c) 2026, the next264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "cpu.h"
#include <string.h>
#include <unistd.h>              /* sysconf, the portable core count */
#if defined(__APPLE__)
#  include <sys/sysctl.h>        /* hw.nperflevels, for the asymmetric budget */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#if defined(__x86_64__) || defined(_M_X64)
#include <cpuid.h>

static uint32_t detect_x86(void)
{
    uint32_t flags = 0;
    unsigned a, b, c, d;

    if (!__get_cpuid(1, &a, &b, &c, &d))
        return 0;
    if (d & (1u << 26)) flags |= N264_CPU_SSE2;
    if (c & (1u << 9))  flags |= N264_CPU_SSSE3;
    if ((c & (1u << 19)) && (c & (1u << 20))) flags |= N264_CPU_SSE4;   /* 4.1 & 4.2 */
    int have_osxsave = (c & (1u << 27)) != 0;
    int have_avx     = (c & (1u << 28)) != 0;
    if (c & (1u << 12)) flags |= N264_CPU_FMA3;

    /* AVX and beyond need OS support for the wider register state (XCR0). */
    int ymm_ok = 0, zmm_ok = 0;
    if (have_osxsave) {
        uint32_t xcr0_lo, xcr0_hi;
        __asm__ volatile("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
        ymm_ok = (xcr0_lo & 0x6) == 0x6;            /* XMM + YMM */
        zmm_ok = (xcr0_lo & 0xe6) == 0xe6;          /* + opmask, ZMM hi16, ZMM */
    }
    if (have_avx && ymm_ok)
        flags |= N264_CPU_AVX;

    if (__get_cpuid_count(7, 0, &a, &b, &c, &d)) {
        if ((flags & N264_CPU_AVX) && (b & (1u << 5)))
            flags |= N264_CPU_AVX2;
        if (b & (1u << 8))
            flags |= N264_CPU_BMI2;
        if (zmm_ok) {
            if (b & (1u << 16)) flags |= N264_CPU_AVX512F;
            if (b & (1u << 30)) flags |= N264_CPU_AVX512BW;
            if (b & (1u << 31)) flags |= N264_CPU_AVX512VL;
        }
        /* AVX-VNNI (the AVX-encoded VNNI, distinct from AVX-512 VNNI) sits in
 * CPUID.(EAX=7,ECX=1):EAX bit 4. */
        unsigned a1, b1, c1, d1;
        if (__get_cpuid_count(7, 1, &a1, &b1, &c1, &d1)) {
            if (a1 & (1u << 4))
                flags |= N264_CPU_AVXVNNI;
        }
    }
    return flags;
}
#endif /* x86 */

#if defined(__aarch64__) || defined(_M_ARM64)
#if defined(__APPLE__)
#include <sys/sysctl.h>

static int sysctl_flag(const char *name)
{
    int value = 0;
    size_t len = sizeof(value);
    if (sysctlbyname(name, &value, &len, NULL, 0) != 0)
        return 0;
    return value != 0;
}

static uint32_t detect_arm(void)
{
    uint32_t flags = N264_CPU_NEON;   /* mandatory on aarch64 */
    if (sysctl_flag("hw.optional.arm.FEAT_DotProd"))
        flags |= N264_CPU_DOTPROD;
    if (sysctl_flag("hw.optional.arm.FEAT_I8MM"))
        flags |= N264_CPU_I8MM;
    return flags;
}
#else /* Linux / other aarch64 */
#include <sys/auxv.h>
#ifndef HWCAP_ASIMDDP
#define HWCAP_ASIMDDP (1 << 20)
#endif
#ifndef HWCAP2_I8MM
#define HWCAP2_I8MM (1 << 13)
#endif

static uint32_t detect_arm(void)
{
    uint32_t flags = N264_CPU_NEON;
    unsigned long hw = getauxval(AT_HWCAP);
    unsigned long hw2 = getauxval(AT_HWCAP2);
    if (hw & HWCAP_ASIMDDP)
        flags |= N264_CPU_DOTPROD;
    if (hw2 & HWCAP2_I8MM)
        flags |= N264_CPU_I8MM;
    return flags;
}
#endif
#endif /* aarch64 */

/* Detected once, under pthread_once; published through the release store on
 * n264_cpu_ready_ that the inline fast path in cpu.h acquires. The old
 * `if (done) return cached;` guard was a data race on first use; this keeps
 * pthread_once as the slow-path arbiter while the settled fast path is one
 * inlined load + branch. */
uint32_t n264_cpu_cached_;
uint32_t n264_asm_off_;
_Atomic int n264_cpu_ready_;
static pthread_once_t g_cpu_once = PTHREAD_ONCE_INIT;

static uint32_t parse_asm_off(const char *s)
{
    static const struct { const char *name; uint32_t bit; } tbl[] = {
        { "pixel", N264_ASM_PIXEL }, { "dct", N264_ASM_DCT },
        { "quant", N264_ASM_QUANT }, { "mc", N264_ASM_MC },
        { "hpel", N264_ASM_HPEL },   { "pred", N264_ASM_PRED },
        { "deblock", N264_ASM_DEBLOCK }, { "ssd", N264_ASM_SSD },
        { "scan", N264_ASM_SCAN },
        { "all", N264_ASM_ALL },
    };
    uint32_t off = 0;
    while (*s) {
        while (*s == ',' || *s == ' ')
            s++;
        size_t n = 0;
        while (s[n] && s[n] != ',' && s[n] != ' ')
            n++;
        for (size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++)
            if (n == strlen(tbl[i].name) && !strncmp(s, tbl[i].name, n))
                off |= tbl[i].bit;
        s += n;
    }
    return off;
}

static void cpu_detect_once(void)
{
#if defined(__x86_64__) || defined(_M_X64)
    n264_cpu_cached_ = detect_x86();
#elif defined(__aarch64__) || defined(_M_ARM64)
    n264_cpu_cached_ = detect_arm();
#else
    n264_cpu_cached_ = 0;
#endif
    if (getenv("NEXT264_NO_ASM"))       /* measurement hook: force scalar C */
        n264_cpu_cached_ = 0;
    const char *off = getenv("N264_ASM_OFF");
    n264_asm_off_ = off ? parse_asm_off(off) : 0;
    atomic_store_explicit(&n264_cpu_ready_, 1, memory_order_release);
}
uint32_t n264_cpu_detect_slow(void)
{
    pthread_once(&g_cpu_once, cpu_detect_once);
    return n264_cpu_cached_;
}

void n264_cpu_name(uint32_t flags, char *buf, int size)
{
    static const struct { uint32_t bit; const char *name; } tbl[] = {
        { N264_CPU_SSE2, "sse2" }, { N264_CPU_SSSE3, "ssse3" },
        { N264_CPU_SSE4, "sse4" }, { N264_CPU_AVX, "avx" },
        { N264_CPU_AVX2, "avx2" }, { N264_CPU_FMA3, "fma3" },
        { N264_CPU_BMI2, "bmi2" }, { N264_CPU_AVX512F, "avx512f" },
        { N264_CPU_AVX512BW, "avx512bw" }, { N264_CPU_AVX512VL, "avx512vl" },
        { N264_CPU_AVXVNNI, "avxvnni" }, { N264_CPU_NEON, "neon" },
        { N264_CPU_DOTPROD, "dotprod" }, { N264_CPU_I8MM, "i8mm" },
    };
    buf[0] = '\0';
    int off = 0;
    for (size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
        if (!(flags & tbl[i].bit))
            continue;
        int n = snprintf(buf + off, size - off, "%s%s",
                         off ? " " : "", tbl[i].name);
        if (n < 0 || n >= size - off)
            break;
        off += n;
    }
    if (off == 0)
        snprintf(buf, size, "scalar");
}

/* The machine's thread budget, resolved once.
 *
 * Apple Silicon is asymmetric, so "how many cores" has more than one answer.
 * hw.nperflevels enumerates the performance levels and hw.perflevelN.logicalcpu
 * gives each one's width; on the M5 Max that reads two levels of 6 and 12. The
 * levels are enumerated rather than interpreted because the informal "N
 * performance plus M efficiency" description and sysctl's ordering do not agree
 * about which tier is level 0, and a policy that hardcoded an interpretation
 * would silently invert on another part.
 *
 * TODAY'S POLICY IS THE SUM, i.e. every online core. That is a measurement, not
 * an assumption: at 450 frames of 1080p the full count beat two thirds of it on
 * both input shapes, with and without a decoder competing in the same process
 * (docs/threading-ownership-plan.md, S0b). An earlier reading said the opposite
 * and was an artefact of streaming the source off an external disk, where I/O
 * and not the thread count was the constraint.
 *
 * The caller still clamps to next264_frame_thread_cap: this answers "how much
 * machine is there", never "how much of it can this picture use". */
static int machine_threads_(void)
{
    long n = 0;
#if defined(__APPLE__)
    unsigned int levels = 0;
    size_t sz = sizeof(levels);
    if (sysctlbyname("hw.nperflevels", &levels, &sz, NULL, 0) == 0 && levels > 0) {
        for (unsigned int i = 0; i < levels; i++) {
            char key[64];
            int cpus = 0;
            sz = sizeof(cpus);
            snprintf(key, sizeof(key), "hw.perflevel%u.logicalcpu", i);
            if (sysctlbyname(key, &cpus, &sz, NULL, 0) == 0 && cpus > 0)
                n += cpus;
        }
    }
#endif
    if (n <= 0) {
#if defined(_SC_NPROCESSORS_ONLN)
        n = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    }
    if (n < 1)
        n = 1;
    if (n > 256)
        n = 256;
    return (int)n;
}

int n264_machine_threads(void)
{
    static int cached;                  /* 0 until resolved; benign if raced */
    int v = cached;
    if (!v) {
        v = machine_threads_();
        if (v < 1)
            v = 1;
        cached = v;
    }
    return v;
}
