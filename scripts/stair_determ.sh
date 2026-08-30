#!/usr/bin/env bash
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: BSD-2-Clause
# Repeated-run determinism for Y264_STAIR_WIDE: N runs at a fixed thread count,
# each compared to the SERIALIZED (gate-off) output, not merely to each other.
# Usage: scripts/stair_determ.sh <bin> <threads> <reps> [extra-env...]
set -o pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${1:?bin}"; TH="${2:-18}"; REPS="${3:-12}"; shift 3 2>/dev/null || shift $#
EXTRA=("$@")
C="$root/tests/corpus"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
md5f() { md5 -q "$1" 2>/dev/null || md5sum "$1" | awk '{print $1}'; }

shapes=(
  "foreman_cif   120 30  --cabac --bframes 3"
  "foreman_cif   120 60  --cabac --bframes 7"
  "foreman_cif   120 250 --cabac --bframes 2"
  "foreman_cif   120 60  --cavlc --bframes 3"
  "bus_cif       120 30  --cabac --bframes 7 --crf 26"
  "stefan_cif    120 60  --cabac --bframes 3"
  "park_joy_720p  48 30  --cabac --bframes 3"
  "ducks_720p     48 60  --cavlc --bframes 3"
)
pass=0; tot=0
for s in "${shapes[@]}"; do
  read -r clip frames keyint extra <<<"$s"
  [ -f "$C/$clip.y4m" ] || continue
  # shellcheck disable=SC2206
  args=(--input-y4m "$C/$clip.y4m" --frames "$frames" --keyint "$keyint" --ref 1 $extra --threads "$TH")
  env "${EXTRA[@]}" "$BIN" "${args[@]}" --output "$work/ser.264" >/dev/null 2>&1
  ref=$(md5f "$work/ser.264")
  for r in $(seq 1 "$REPS"); do
    env Y264_STAIR_WIDE=1 "${EXTRA[@]}" "$BIN" "${args[@]}" --output "$work/w.264" >/dev/null 2>&1
    tot=$((tot+1))
    if [ "$(md5f "$work/w.264")" = "$ref" ]; then pass=$((pass+1))
    else echo "DIFF $clip k$keyint $extra rep$r"; fi
  done
done
echo "wide == serialized at t$TH: $pass/$tot"
[ "$pass" = "$tot" ]
