/*
 * cpu.h - runtime CPU feature detection for kernel dispatch
 * Copyright (c) 2026, the next264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef NEXT264_CPU_H
#define NEXT264_CPU_H

#include <stdatomic.h>
#include <stdint.h>

/* Feature flags, one bit per instruction-set tier we dispatch on. The set is
 * deliberately wider than what Phase 0 uses so the dispatch tables and checkasm
 * are ready as kernels land. */
enum {
    /* x86-64 */
    N264_CPU_SSE2     = 1u << 0,
    N264_CPU_SSSE3    = 1u << 1,
    N264_CPU_SSE4     = 1u << 2,   /* SSE4.1 + SSE4.2 */
    N264_CPU_AVX      = 1u << 3,
    N264_CPU_AVX2     = 1u << 4,
    N264_CPU_FMA3     = 1u << 5,
    N264_CPU_BMI2     = 1u << 6,
    N264_CPU_AVX512F  = 1u << 7,
    N264_CPU_AVX512BW = 1u << 8,
    N264_CPU_AVX512VL = 1u << 9,
    N264_CPU_AVXVNNI  = 1u << 10,

    /* aarch64 */
    N264_CPU_NEON     = 1u << 16,
    N264_CPU_DOTPROD  = 1u << 17,  /* FEAT_DotProd (udot/sdot) */
    N264_CPU_I8MM     = 1u << 18,  /* FEAT_I8MM */
};

/* Detect the running CPU's features. The result is cached after the first call.
 * The fast path inlines to an acquire load + plain read: dispatch sites call
 * this per kernel invocation, and the previous out-of-line pthread_once walk
 * measured ~1% of encode self-time. The acquire pairs with the release store
 * cpu.c makes after filling the cache, so the read is race-free (TSan-clean)
 * even for a first call from a worker thread. */
extern _Atomic int n264_cpu_ready_;
extern uint32_t n264_cpu_cached_;
uint32_t n264_cpu_detect_slow(void);
static inline uint32_t n264_cpu_detect(void)
{
    if (atomic_load_explicit(&n264_cpu_ready_, memory_order_acquire))
        return n264_cpu_cached_;
    return n264_cpu_detect_slow();
}

/* Per-class SIMD ablation, a measurement hook only: N264_ASM_OFF is a
 * comma-separated class list (`pixel,dct,quant,mc,hpel,pred,deblock,ssd,scan`, or
 * `all`) that forces those dispatch sites onto their C fallback while the rest
 * of the encoder stays as-shipped. Output is unchanged by construction -- every
 * kernel is bit-exact with its C reference -- so a class's wall delta is what
 * that class's SIMD currently buys. Parsed in cpu_detect_once and published by
 * the same release store as the feature word, so reading it after
 * n264_cpu_detect returns is race-free. */
enum {
    N264_ASM_PIXEL   = 1u << 0,  /* sad/sad_x4/satd/sa8d/hadamard/var/texture */
    N264_ASM_DCT     = 1u << 1,  /* fdct/idct 4x4+8x8, sub-dct, add-idct */
    N264_ASM_QUANT   = 1u << 2,  /* quant/dequant 4x4+8x8 */
    N264_ASM_MC      = 1u << 3,  /* mc luma + chroma */
    N264_ASM_HPEL    = 1u << 4,  /* half-pel plane build */
    N264_ASM_PRED    = 1u << 5,  /* intra prediction */
    N264_ASM_DEBLOCK = 1u << 6,
    N264_ASM_SSD     = 1u << 7,
    N264_ASM_SCAN    = 1u << 8,  /* zig-zag scan / RDOQ coefficient marshalling */
    N264_ASM_ALL     = 0x1ffu,
};
extern uint32_t n264_asm_off_;
static inline int n264_asm_on(uint32_t cls)
{
    return (n264_cpu_detect() & N264_CPU_NEON) && !(n264_asm_off_ & cls);
}

/* Human-readable list of the active features, into `buf` (NUL-terminated). */
void n264_cpu_name(uint32_t flags, char *buf, int size);

#endif /* NEXT264_CPU_H */
