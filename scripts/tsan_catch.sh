#!/usr/bin/env bash
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: BSD-2-Clause
# Run one shape until TSan reports, then symbolize the addresses with atos.
# TSan runs with symbolize=0 on purpose: its own symbolizer stalls for minutes
# on a report and looks exactly like a deadlock.
set -o pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${BIN:-/tmp/y264tsan/cli/yah264}"
CLIP="${CLIP:-park_joy_720p}"
OUT="${OUT:-/tmp/tsan_catch.txt}"
export TSAN_OPTIONS="halt_on_error=0 symbolize=0"
for r in $(seq 1 "${REPS:-12}"); do
  Y264_STAIR_WIDE=1 Y264_STAIR_BDEPTH="${BD:-1}" "$BIN" \
    --input-y4m "$root/tests/corpus/$CLIP.y4m" --frames "${FRAMES:-24}" \
    --keyint 30 --cabac --bframes 3 --ref 1 --qp 26 --threads 18 \
    ${ARGS:-} --output /dev/null 2>"$OUT" >/dev/null
  if grep -q "WARNING: ThreadSanitizer" "$OUT"; then
    echo "caught on rep $r -> $OUT"
    grep -oE 'yah264:arm64\+0x[0-9a-f]+' "$OUT" | sort -u | \
      sed 's/.*+//' | while read -r a; do
        printf '%s  ' "$a"; atos -o "$BIN" -l 0x100000000 "$a" 2>/dev/null || echo
      done
    exit 0
  fi
done
echo "no report in ${REPS:-12} reps"
