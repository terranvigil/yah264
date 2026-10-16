/*
 * nzmask.h - nonzero masks of coefficient rows, for the entropy coders and the
 * rate estimators: bit i set where coefficient i is nonzero.
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The plain loop is what every caller used to write inline. On x86-64 the
 * compiler can only vectorise it at the SSE2 baseline with a float-exponent
 * shift trick that costs more than the scalar loop, so the x86 form is a
 * compare, a pack and a byte movemask; aarch64 is a test, a narrow and a
 * weighted add. Both are baseline for their architecture, so neither needs
 * dispatch. tests/test_cavlc.c checks them against the loop.
 */
#ifndef YAH264_NZMASK_H
#define YAH264_NZMASK_H

#include "../common/bitdepth.h"
#include <stdint.h>

#if defined(__x86_64__) || defined(__SSE2__)
#include <emmintrin.h>
#define Y264_NZMASK_SSE2 1
#elif defined(__aarch64__) && Y264_BIT_DEPTH <= 8
#include <arm_neon.h>
#define Y264_NZMASK_NEON 1
#endif

/* coefficients p[0..7] */
static inline uint32_t y264_nzmask8(const dctcoef *p)
{
#ifdef Y264_NZMASK_SSE2
    const __m128i z = _mm_setzero_si128();
#if Y264_BIT_DEPTH <= 8
    __m128i e = _mm_cmpeq_epi16(_mm_loadu_si128((const __m128i *)p), z);
#else
    __m128i e = _mm_packs_epi32(_mm_cmpeq_epi32(_mm_loadu_si128((const __m128i *)p), z),
                                _mm_cmpeq_epi32(_mm_loadu_si128((const __m128i *)(p + 4)), z));
#endif
    return ~(uint32_t)_mm_movemask_epi8(_mm_packs_epi16(e, e)) & 0xffu;
#elif defined(Y264_NZMASK_NEON)
    static const uint8_t bw[8] = { 1, 2, 4, 8, 16, 32, 64, 128 };
    int16x8_t v = vld1q_s16(p);
    return vaddv_u8(vand_u8(vmovn_u16(vtstq_s16(v, v)), vld1_u8(bw)));
#else
    uint32_t m = 0;
    for (int i = 0; i < 8; i++)
        m |= (uint32_t)(p[i] != 0) << i;
    return m;
#endif
}

/* coefficients p[0..n-1], n <= 16; reads nothing past p[n-1] */
static inline uint32_t y264_nzmask_n(const dctcoef *p, int n)
{
#if defined(Y264_NZMASK_SSE2) || defined(Y264_NZMASK_NEON)
    if (n >= 8)                 /* two rows of eight, the second ending at n-1 */
        return y264_nzmask8(p) | (y264_nzmask8(p + n - 8) << (n - 8));
#endif
    uint32_t m = 0;
    if (n == 16) {              /* a fixed trip count the compiler vectorises */
        for (int i = 0; i < 16; i++)
            m |= (uint32_t)(p[i] != 0) << i;
        return m;
    }
    for (int i = 0; i < n; i++)
        m |= (uint32_t)(p[i] != 0) << i;
    return m;
}

#endif
