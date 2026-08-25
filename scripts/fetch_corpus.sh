#!/usr/bin/env bash
# Copyright (c) 2026, the next264 authors
# SPDX-License-Identifier: BSD-2-Clause
#
# fetch_corpus.sh - download a small set of standard test clips into
# tests/corpus/ for local benchmarking and conformance. Clips are checked
# against known SHA-256 hashes so a corrupted or swapped download fails loudly.
#
# The clips are classic Xiph "derf" test sequences in raw Y4M. They are not
# committed to the repo (see .gitignore); CI conformance uses synthetic clips
# and does not need this. Run it once locally for realistic quality numbers.
#
# Usage: fetch_corpus.sh [--full]
#   (default)  the 7 CIF clips below -- fast, small, the day-to-day gate.
#   --full     ALSO the large native-res grain/motion/HD clips (class: tier2),
#              which unlock the built-but-unmeasurable features (psy-trellis,
#              b-adapt) and a stable broad-content parity number. GBs of raw Y4M;
#              the harness truncates at encode. See docs/corpus-broadening-plan.md.
set -euo pipefail

full=0
for a in "$@"; do
    case "$a" in
        --full) full=1 ;;
        *) echo "unknown arg: $a (usage: fetch_corpus.sh [--full])" >&2; exit 2 ;;
    esac
done

root="$(cd "$(dirname "$0")/.." && pwd)"
dest="$root/tests/corpus"
mkdir -p "$dest"

base="https://media.xiph.org/video/derf"

# name  url  sha256(-=skip)  class -- the CIF gate set (always fetched).
clips=(
    "akiyo_cif       $base/y4m/akiyo_cif.y4m        -   static"
    "foreman_cif     $base/y4m/foreman_cif.y4m      -   motion"
    "mobile_cif      $base/y4m/mobile_cif.y4m       -   detail"
    # Wider set that exposes gaps the original three cannot measure:
    # stefan = erratic fast motion (b-adapt / variable cadence), bus = fast pan,
    # tempete = zoom + fine detail (psy), coastguard = water texture/detail.
    "stefan_cif      $base/y4m/stefan_cif.y4m       -   motion"
    "bus_cif         $base/y4m/bus_cif.y4m          -   motion"
    "tempete_cif     $base/y4m/tempete_cif.y4m      -   detail"
    "coastguard_cif  $base/y4m/coastguard_cif.y4m   -   detail"
)

# Tier 2 (--full): large native-res clips per content class. Grain clips stay at
# native res on purpose -- a CIF downscale removes the grain. sha256 "-" until a
# first verified download pins them. See docs/corpus-broadening-plan.md.
clips_full=(
    "park_joy_720p        $base/y4m/park_joy_420_720p50.y4m         b1e3784a6c74e07d53dd7077737b5a6edcd53e5ea07e4718c9affbe0ec36536f   grain"
    "old_town_720p        $base/y4m/old_town_cross_420_720p50.y4m   0fbc2ec0c552ad693528b2e49409c7449880b9230055a7696346473d1aaf4123   grain"
    "ducks_720p           $base/y4m/ducks_take_off_420_720p50.y4m   0e09aa923ce72e2f6d1c978c3c1cc8dd9588d2ede819081f040213e361970efc   grain"
    "in_to_tree_720p      $base/y4m/in_to_tree_420_720p50.y4m       81166517a3054c789923535b5da23fbbb9bbd7fd4c28dac83918ac351e121e74   detail"
    "crowd_run_1080p      $base/y4m/crowd_run_1080p50.y4m           e056e01fc13906dd9a84f5cc686a36888436379476577965a0866821caaf55c8   crowd"
    "touchdown_1080p      $base/y4m/touchdown_pass_1080p.y4m        4cf0d19e332fa021660d450fd0da1201c7e153d70e5e5606c8497a1835477382   cadence"
    # sintel: animated hard-cut/flash content for b-adapt. The 720p variant is
    # 1.6 GB vs the 1080p's 3.6 GB and identical in cadence structure -- prefer it
    # for the b-adapt A/B (truncated to ~10 s at encode anyway).
    "sintel_720p          $base/y4m/sintel_trailer_2k_720p24.y4m    -   cadence"
    # Screen content: not auto-fetched (host/licensing) -- drop a 720p desktop
    # capture into tests/corpus/ manually and add a "name screen" CLASSES line.
)

# tests/corpus/CLASSES: "name class" per line, so bdcompare/bench can select a
# content subset without hardcoding clip names. Rewritten fresh each run.
manifest="$dest/CLASSES"
: > "$manifest"

fetch() {
    local name="$1" url="$2" want="$3" class="$4"
    local out="$dest/$name.y4m"
    echo "$name $class" >> "$manifest"
    if [ -f "$out" ]; then
        echo "have  $name  [$class]"
        return
    fi
    echo "fetch $name  [$class]"
    if command -v curl >/dev/null; then
        curl -fSL "$url" -o "$out"
    else
        wget -O "$out" "$url"
    fi
    if [ "$want" != "-" ]; then
        local got
        got="$(shasum -a 256 "$out" | awk '{print $1}')"
        if [ "$got" != "$want" ]; then
            echo "  hash mismatch for $name: got $got" >&2
            rm -f "$out"
            exit 1
        fi
    fi
}

for row in "${clips[@]}"; do
    read -r c_name c_url c_want c_class <<< "$row"
    fetch "$c_name" "$c_url" "$c_want" "$c_class"
done

if [ "$full" = 1 ]; then
    echo "-- tier 2 (--full): large native-res class clips --"
    for row in "${clips_full[@]}"; do
        read -r c_name c_url c_want c_class <<< "$row"
        fetch "$c_name" "$c_url" "$c_want" "$c_class"
    done
fi

echo "corpus ready in $dest (classes in $manifest)"
