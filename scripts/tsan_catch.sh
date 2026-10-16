#!/usr/bin/env bash
# macOS only: symbolises with `atos`; BIN defaults to a TSan build under /tmp (build one with -Db_sanitize=thread).
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
# Run one shape until TSan reports, then symbolize the addresses with atos.
# TSan runs with symbolize=0 on purpose: its own symbolizer stalls for minutes
# on a report and looks exactly like a deadlock.
set -o pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${BIN:-/tmp/y264tsan/cli/yah264}"
CLIP="${CLIP:-park_joy_720p}"
OUT="${OUT:-/tmp/tsan_catch.txt}"
# The default BIN is a path under /tmp that nothing in this tree creates, so the
# ordinary way to get here is with no binary at all -- and every rep then fails
# silently, no rep writes "WARNING: ThreadSanitizer" into $OUT, and the script
# ends by printing "no report in N reps". That reads exactly like a clean gate.
# It read that way to one session on the day this line was added, on an item
# whose race TSan went on to catch the moment the binary existed. Refuse instead.
if [ ! -x "$BIN" ]; then
  echo "tsan_catch: no TSan binary at $BIN" >&2
  echo "tsan_catch:   meson setup /tmp/y264tsan -Db_sanitize=thread && ninja -C /tmp/y264tsan" >&2
  echo "tsan_catch:   (or point BIN at one). Refusing: a missing binary reports clean." >&2
  exit 2
fi
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
