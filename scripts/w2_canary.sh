#!/usr/bin/env bash
# Copyright (c) 2026, the next264 authors
# SPDX-License-Identifier: BSD-2-Clause
#
# w2_canary.sh - byte-identity canary comparing a candidate encoder to a
# reference binary across a config matrix, on BOTH the serial (--threads 1) and
# wavefront (--threads 4) paths. Used to gate each W2 refactor step: the default
# path must stay byte-identical to a fresh HEAD build (memory: byte-identity
# tests must compare against a FRESH HEAD build, not an old baseline).
#
# Usage: scripts/w2_canary.sh <candidate-bin> <reference-bin> [extra-env-for-candidate]
set -uo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
CAND="${1:?candidate binary}"
REF="${2:?reference binary}"
CENV="${3:-}"                          # e.g. "N264_W2=1" applied to the candidate only
CORPUS="$root/tests/corpus"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
md5f() { md5 -q "$1" 2>/dev/null || md5sum "$1" | awk '{print $1}'; }

configs=(
  "foreman_cif   30 --cabac --bframes 2"
  "foreman_cif   30 --cabac --bframes 0"
  "foreman_cif   30 --bframes 2"
  "foreman_cif   30 --bframes 0"
  "mobile_cif    24 --cabac --bframes 2"
  "stefan_cif    24 --cabac --bframes 3"
  "bus_cif       24 --bframes 3"
  "akiyo_cif     20 --cabac --bframes 1"
  "park_joy_720p 12 --cabac --bframes 2"
  "park_joy_720p 12 --bframes 2"
  "foreman_cif   30 --cabac --bframes 2 --transform-8x8"
  "foreman_cif   30 --cabac --bframes 2 --crf 26"
  "foreman_cif   30 --cabac --bframes 2 --bitrate 800 --aq-strength 0.5"
)

fail=0; total=0
for th in 1 4; do
  for cfg in "${configs[@]}"; do
    read -r clip frames extra <<<"$cfg"
    cp="$CORPUS/$clip.y4m"; [ -f "$cp" ] || continue
    args="--input-y4m $cp --qp 26 --keyint 1000 --frames $frames --threads $th $extra"
    env $CENV "$CAND" $args -o "$work/c.264" >/dev/null 2>&1
    "$REF"                  $args -o "$work/r.264" >/dev/null 2>&1
    mc=$(md5f "$work/c.264"); mr=$(md5f "$work/r.264")
    total=$((total+1))
    if [ -n "$mc" ] && [ "$mc" = "$mr" ]; then :; else
      echo "DIFF  t$th  $clip.f$frames $extra   cand=$mc ref=$mr"; fail=$((fail+1))
    fi
  done
done
echo "$((total-fail))/$total byte-identical  (cand-env='${CENV:-none}')"
[ "$fail" = 0 ] && echo "CANARY OK" || echo "CANARY FAIL ($fail)"
exit "$fail"
