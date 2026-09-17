/*
 * cpu.h - runtime CPU feature detection for kernel dispatch
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef YAH264_CPU_H
#define YAH264_CPU_H

#include <stdatomic.h>
#include <stdint.h>

#include "yah264.h"   /* Y264_BIT_DEPTH, which defaults to 8 in there */

/* Feature flags, one bit per instruction-set tier we dispatch on. The set is
 * deliberately wider than what Phase 0 uses so the dispatch tables and checkasm
 * are ready as kernels land. */
enum {
    /* x86-64 */
    Y264_CPU_SSE2     = 1u << 0,
    Y264_CPU_SSSE3    = 1u << 1,
    Y264_CPU_SSE4     = 1u << 2,   /* SSE4.1 + SSE4.2 */
    Y264_CPU_AVX      = 1u << 3,
    Y264_CPU_AVX2     = 1u << 4,
    Y264_CPU_FMA3     = 1u << 5,
    Y264_CPU_BMI2     = 1u << 6,
    Y264_CPU_AVX512F  = 1u << 7,
    Y264_CPU_AVX512BW = 1u << 8,
    Y264_CPU_AVX512VL = 1u << 9,
    Y264_CPU_AVXVNNI  = 1u << 10,

    /* aarch64 */
    Y264_CPU_NEON     = 1u << 16,
    Y264_CPU_DOTPROD  = 1u << 17,  /* FEAT_DotProd (udot/sdot) */
    Y264_CPU_I8MM     = 1u << 18,  /* FEAT_I8MM */
};

/* What a tier's translation unit was COMPILED with, and so what the running
 * CPU must have before a kernel out of it may be called. `-mavx2 -mfma -mbmi2`
 * is one tier and not three, so the dispatch test is one mask. */
#define Y264_CPU_SSE4_ALL   (Y264_CPU_SSE2 | Y264_CPU_SSSE3 | Y264_CPU_SSE4)
#define Y264_CPU_AVX2_ALL   (Y264_CPU_SSE4_ALL | Y264_CPU_AVX | Y264_CPU_AVX2 | \
                             Y264_CPU_FMA3 | Y264_CPU_BMI2)
#define Y264_CPU_AVX512_ALL (Y264_CPU_AVX2_ALL | Y264_CPU_AVX512F | \
                             Y264_CPU_AVX512BW | Y264_CPU_AVX512VL)

/* ---- the tiers this build compiled in -------------------------------------
 *
 * meson's `simd` option is the build-time cap and defines these per library;
 * the fallbacks keep a bare compiler invocation -- a scratch test, an editor's
 * index -- building. A tier at 0 contributes no translation unit and no
 * dispatch, which is the whole of `-Dsimd=none`.
 *
 * NEON carries the depth in its condition because its kernels are uint8/int16
 * lanes; the x86 tiers carry it in meson, which links them into the 8-bit
 * library only. */
#ifndef Y264_HAVE_NEON
#  if (defined(__aarch64__) || defined(_M_ARM64)) && Y264_BIT_DEPTH == 8
#    define Y264_HAVE_NEON 1
#  else
#    define Y264_HAVE_NEON 0
#  endif
#endif
#ifndef Y264_HAVE_SSE4
#  define Y264_HAVE_SSE4 0
#endif
#ifndef Y264_HAVE_AVX2
#  define Y264_HAVE_AVX2 0
#endif
#ifndef Y264_HAVE_AVX512
#  define Y264_HAVE_AVX512 0
#endif

/* One bit per compiled-in tier, saying "this build holds kernels a CPU with
 * that bit may run". ANDed with detection it answers the only question the
 * generic gate asks -- is there any usable SIMD here at all -- and it is a
 * compile-time constant, so the gate stays an inlined load and two tests on
 * every architecture. WHICH tier runs is y264_cpu_tier()'s question, asked
 * once per dispatch site rather than once per call. */
#define Y264_CPU_SIMD_ANY \
    ((Y264_HAVE_NEON   ? (uint32_t)Y264_CPU_NEON    : 0u) | \
     (Y264_HAVE_SSE4   ? (uint32_t)Y264_CPU_SSE4    : 0u) | \
     (Y264_HAVE_AVX2   ? (uint32_t)Y264_CPU_AVX2    : 0u) | \
     (Y264_HAVE_AVX512 ? (uint32_t)Y264_CPU_AVX512F : 0u))

/* The tier a dispatch site selects, best usable first. */
enum {
    Y264_TIER_C = 0,
    Y264_TIER_NEON,
    Y264_TIER_SSE4,
    Y264_TIER_AVX2,
    Y264_TIER_AVX512,
};

/* Detect the running CPU's features. The result is cached after the first call.
 * The fast path inlines to an acquire load + plain read: dispatch sites call
 * this per kernel invocation, and the previous out-of-line pthread_once walk
 * measured ~1% of encode self-time. The acquire pairs with the release store
 * cpu.c makes after filling the cache, so the read is race-free (TSan-clean)
 * even for a first call from a worker thread. */
extern _Atomic int y264_cpu_ready_;
extern uint32_t y264_cpu_cached_;
uint32_t y264_cpu_detect_slow(void);
static inline uint32_t y264_cpu_detect(void)
{
    if (atomic_load_explicit(&y264_cpu_ready_, memory_order_acquire))
        return y264_cpu_cached_;
    return y264_cpu_detect_slow();
}

/* Per-class SIMD ablation, a measurement hook only: Y264_ASM_OFF is a
 * comma-separated class list (`pixel,dct,quant,mc,hpel,pred,deblock,ssd,scan`, or
 * `all`) that forces those dispatch sites onto their C fallback while the rest
 * of the encoder stays as-shipped. Output is unchanged by construction -- every
 * kernel is bit-exact with its C reference -- so a class's wall delta is what
 * that class's SIMD currently buys. Parsed in cpu_detect_once and published by
 * the same release store as the feature word, so reading it after
 * y264_cpu_detect returns is race-free. */
enum {
    Y264_ASM_PIXEL   = 1u << 0,  /* sad/sad_x4/satd/sa8d/hadamard/var/texture */
    Y264_ASM_DCT     = 1u << 1,  /* fdct/idct 4x4+8x8, sub-dct, add-idct */
    Y264_ASM_QUANT   = 1u << 2,  /* quant/dequant 4x4+8x8 */
    Y264_ASM_MC      = 1u << 3,  /* mc luma + chroma */
    Y264_ASM_HPEL    = 1u << 4,  /* half-pel plane build */
    Y264_ASM_PRED    = 1u << 5,  /* intra prediction */
    Y264_ASM_DEBLOCK = 1u << 6,
    Y264_ASM_SSD     = 1u << 7,
    Y264_ASM_SCAN    = 1u << 8,  /* zig-zag scan / RDOQ coefficient marshalling */
    Y264_ASM_ALL     = 0x1ffu,
};
/* The machine's thread budget: every online core, resolved once and cached.
 * Answers "how much machine is there", never "how much can this picture use" --
 * the caller clamps to yah264_frame_thread_cap. Y264_AUTO_THREADS pins it. */
int y264_machine_threads(void);

extern uint32_t y264_asm_off_;

/* Is class `cls` allowed to leave the C path? Two questions, and neither of
 * them is architectural any more: does this build hold SIMD the running CPU
 * can execute, and has the ablation hook turned this class off. The test used
 * to name Y264_CPU_NEON, which is what made every dispatch site aarch64-only
 * by construction. */
static inline int y264_asm_on(uint32_t cls)
{
    return (y264_cpu_detect() & Y264_CPU_SIMD_ANY) && !(y264_asm_off_ & cls);
}

/* The best tier this build compiled in AND this CPU can run, after
 * Y264_SIMD_FORCE has had its say (cpu.c clears the bits above the forced
 * tier, so the answer here needs no second knob). A tier is selected only if
 * every feature its translation unit was compiled with is present: an AVX2
 * file built with -mfma may emit an FMA anywhere, so AVX2 without FMA3 is a
 * SIGILL and not a slow path. */
static inline int y264_cpu_tier(void)
{
    uint32_t f = y264_cpu_detect();
    if (!(f & Y264_CPU_SIMD_ANY))
        return Y264_TIER_C;
#if Y264_HAVE_AVX512
    if ((f & Y264_CPU_AVX512_ALL) == Y264_CPU_AVX512_ALL)
        return Y264_TIER_AVX512;
#endif
#if Y264_HAVE_AVX2
    if ((f & Y264_CPU_AVX2_ALL) == Y264_CPU_AVX2_ALL)
        return Y264_TIER_AVX2;
#endif
#if Y264_HAVE_SSE4
    if ((f & Y264_CPU_SSE4_ALL) == Y264_CPU_SSE4_ALL)
        return Y264_TIER_SSE4;
#endif
#if Y264_HAVE_NEON
    if (f & Y264_CPU_NEON)
        return Y264_TIER_NEON;
#endif
    return Y264_TIER_C;
}

/* Human-readable list of the active features, into `buf` (NUL-terminated). */
void y264_cpu_name(uint32_t flags, char *buf, int size);

#endif /* YAH264_CPU_H */
