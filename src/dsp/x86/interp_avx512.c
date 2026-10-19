/*
 * interp_avx512.c - AVX-512 kernels for the mc family
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Empty so far. The AVX-512 tier is built by default (-Davx512=false leaves it
 * out) and covers the pixel metrics today. An MC kernel goes here once its
 * AVX2 twin has shipped and the wider form measures faster on both an Intel
 * and an AMD part. See docs/simd.md.
 *
 * C11 intrinsics only. There is no assembly in this tree, and
 * hygiene_check refuses any inline form under this directory.
 */
#include "dsp/arch.h"

/* A translation unit needs one declaration to be a translation unit. */
typedef int y264_x86_mc_avx512_placeholder;
