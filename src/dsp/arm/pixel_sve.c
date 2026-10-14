/*
 * pixel_sve.c - SVE pixel metrics (docs/x86-plan.md, "The ARM SVE tier")
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Every kernel here is vector-length agnostic: it is exact at any SVE vector
 * length, 128 bits through 2048, and checkasm runs it at whatever length the
 * CPU (or QEMU) has. What depends on the length is the DISPATCH. These kernels
 * spend one 16-bit lane per pixel column, so at 256 bits a 16-wide block row
 * is one register where NEON needs two, and the transform work halves; at 128
 * bits they do NEON's work with more loads, so y264_pixel_init installs them
 * only on a part whose vectors are at least 256 bits (Y264_CPU_SVE256).
 *
 * The Hadamard shapes need no permute that crosses a 128-bit segment. A
 * horizontal butterfly between neighbouring columns is TRN1/TRN2 at 16 bits
 * between two rows, the next one TRN at 32 bits, the one after that TRN at 64
 * bits; each pairs elements 2k and 2k+1 of its width and nothing else, so the
 * columns of one 4x4 or 8x8 block never leave their group of lanes whatever
 * the vector length. The last butterfly of each transform is folded into a
 * max: |a + b| + |a - b| == 2 * max(|a|, |b|).
 *
 * Columns past the block are loaded as zero (an inactive lane of a predicated
 * load reads nothing and holds zero) and transform to zero, so a register
 * wider than the block costs lanes and never correctness.
 *
 * The feature is selected for this file only, with a target pragma, so no SVE
 * instruction can reach a baseline translation unit.
 */
#include <stdint.h>

#include "../arch.h"

#if defined(__clang__)
#pragma clang attribute push(__attribute__((target("sve"))), apply_to = function)
#else
#pragma GCC target("+sve")
#endif
#include <arm_sve.h>

/* The vector length in bytes, for detection (src/common/cpu.c). Called only
 * once the hwcaps have said SVE exists. */
int y264_sve_cntb(void)
{
    return (int)svcntb();
}

#if defined(__clang__)
#pragma clang attribute pop
#endif
