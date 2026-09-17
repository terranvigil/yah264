/*
 * mc_avx512.c - AVX-512 kernels for the mc family
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Empty by design, and behind a gate on top of that: the AVX-512 tier is
 * compiled only at -Davx512=true, it covers the pixel metrics and MC and
 * nothing else, and no kernel lands here until its AVX2 twin has shipped and
 * been measured on both an Intel and an AMD part. See docs/x86-plan.md.
 *
 * C11 intrinsics only. There is no assembly in this tree, and
 * hygiene_check refuses any inline form under this directory.
 */
#include "dsp/arch.h"

/* A translation unit needs one declaration to be a translation unit. */
typedef int y264_x86_mc_avx512_placeholder;
