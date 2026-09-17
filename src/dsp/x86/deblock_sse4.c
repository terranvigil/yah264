/*
 * deblock_sse4.c - SSE4.2 kernels for the deblock family
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Empty by design. The file exists so that every x86-64 build compiles this
 * tier's translation unit with this tier's -m flags, links it into the 8-bit
 * library through its own static library, and sees the same depth, tier and
 * family macros the dispatcher sees -- so the build shape is exercised now
 * and not written blind on the day the first kernel lands. The kernels
 * themselves are waves 1 to 3 of docs/x86-plan.md; their names are already
 * declared in src/dsp/arch.h.
 *
 * C11 intrinsics only. There is no assembly in this tree, and
 * hygiene_check refuses any inline form under this directory.
 */
#include "dsp/arch.h"

/* A translation unit needs one declaration to be a translation unit. */
typedef int y264_x86_deblock_sse4_placeholder;
