#!/usr/bin/env bash
# Copyright (c) 2026, the next264 authors
# SPDX-License-Identifier: BSD-2-Clause
#
# perf-comp-purec.sh -- pure-C speed+quality comparison: next264 (NEXT264_NO_ASM=1)
# vs x264 with NO hand-asm. IMPORTANT: this is NOT "both truly scalar." next264's
# NEXT264_NO_ASM=1 is a runtime dispatch switch -- its C is still -O3 AUTO-VECTORIZED.
# So the fair x264 side is x264-noasm-AUTOVEC (perf-comp.sh default), not plain
# x264-noasm (which x264's configure builds -fno-tree-vectorize = genuinely scalar).
# Comparing next264-autovec vs x264-scalar flattered the gap to ~1.5x
# (docs/archive/purec-harness-bias-handoff.md). Isolates the no-hand-asm (algorithm +
# compiler-autovec) gap from the hand-SIMD gap.
#
# There is no single "the" pure-C gap: it runs ~2.5x on CIF and 2.7-3.6x on 720p
# at the same config, so ONE clip is not a campaign number. Use
# `make perf-comp-purec-set` for the clip set; this script measures one clip.
#
# Quality is unchanged vs the optimized run (asm/NEON kernels are bit-exact with
# their C), so VMAF/size match; only the speed numbers differ. Thin wrapper --
# same args and env as perf-comp.sh.
exec env PURE_C=1 "$(cd "$(dirname "$0")" && pwd)/perf-comp.sh" "$@"
