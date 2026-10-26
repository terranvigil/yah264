#!/usr/bin/env bash
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# alloc_census.sh OUT -- CMD ARGS...
#
# Run CMD under tools/alloc_census (large allocations by call site) and write
# the symbolised table to OUT: two innermost frames, size class in KiB, count,
# peak GiB live, GiB churned. CMD's own binary is what gets symbolised, so pass
# the encoder itself (or `env VAR=.. BIN ...`; the first argument after env's
# assignments is taken as the binary). Found the per-frame decision records
# (426 GB churned in 3000 frames of 2160p) and the input store's 15 GiB.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
out=$1; shift; [ "${1:-}" = "--" ] && shift
lib="${TMPDIR:-/tmp}/alloc_census.$$.dylib"
raw="$out.raw"
cc -O2 -dynamiclib -o "$lib" "$root/tools/alloc_census/alloc_census.c"
bin=""
for a in "$@"; do
  case "$a" in env|*=*) continue;; *) bin=$a; break;; esac
done
[ "$1" = env ] && shift
set -- env DYLD_INSERT_LIBRARIES="$lib" MSHIM_OUT="$raw" "$@"
rc=0; "$@" || rc=$?
rm -f "$lib"
[ "$rc" -eq 0 ] || { echo "alloc_census: the command exited $rc" >&2; rm -f "$raw"; exit "$rc"; }
{
  head -2 "$raw"
  tail -n +3 "$raw" | while read -r a b sz n pk ch; do
    s=$(atos -o "$bin" -l 0x100000000 "$(printf '0x%x' $((0x100000000 + a)))" \
                                     "$(printf '0x%x' $((0x100000000 + b)))" |
        sed 's/ (in [^)]*)//' | paste -sd'<' -)
    printf '%-80s %8s %7s %8s %9s\n' "$s" "$sz" "$n" "$pk" "$ch"
  done
} > "$out"
rm -f "$raw"
echo "alloc_census: $out"
