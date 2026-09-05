#!/usr/bin/env bash
# Copyright (c) 2026, the yah264 authors
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
# Usage: fetch_corpus.sh [--full] [--res]
#   (default)  the 7 CIF clips below -- fast, small, the day-to-day gate.
#   --full     ALSO the large native-res grain/motion/HD clips (class: tier2),
#              which unlock the built-but-unmeasurable features (psy-trellis,
#              b-adapt) and a stable broad-content parity number. GBs of raw Y4M;
#              the harness truncates at encode. See docs/corpus-sources.md.
#   --res      ALSO the resolution-balance set: 6 more at 720p and 6 at 1080p,
#              because the band corpus was 7 CIF / 4x720p / 1x1080p and its
#              median was therefore a CIF decision. ~12 GB. --full implies it.
set -euo pipefail

full=0
res=0
review=0
for a in "$@"; do
    case "$a" in
        --full) full=1; res=1 ;;
        --res)  res=1 ;;
        --review) review=1 ;;
        *) echo "unknown arg: $a (usage: fetch_corpus.sh [--full] [--res] [--review])" >&2; exit 2 ;;
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
# first verified download pins them. See docs/corpus-sources.md.
clips_full=(
    "park_joy_720p        $base/y4m/park_joy_420_720p50.y4m         b1e3784a6c74e07d53dd7077737b5a6edcd53e5ea07e4718c9affbe0ec36536f   grain"
    "ducks_720p           $base/y4m/ducks_take_off_420_720p50.y4m   0e09aa923ce72e2f6d1c978c3c1cc8dd9588d2ede819081f040213e361970efc   grain"
    "touchdown_1080p      $base/y4m/touchdown_pass_1080p.y4m        4cf0d19e332fa021660d450fd0da1201c7e153d70e5e5606c8497a1835477382   cadence"
    # sintel: animated hard-cut/flash content for b-adapt. The 720p variant is
    # 1.6 GB vs the 1080p's 3.6 GB and identical in cadence structure -- prefer it
    # for the b-adapt A/B (truncated to ~10 s at encode anyway).
    "sintel_720p          $base/y4m/sintel_trailer_2k_720p24.y4m    -   cadence"
    # Screen content: not auto-fetched (host/licensing) -- drop a 720p desktop
    # capture into tests/corpus/ manually and add a "name screen" CLASSES line.
)

# Tier 3 (--res, also pulled by --full): the RESOLUTION-BALANCE set.
#
# WHY IT EXISTS. The twelve-clip band corpus was 7 CIF, 4x720p and ONE 1080p
# clip, so every "corpus median" was in effect a CIF decision and the whole
# 1080p class rested on touchdown alone -- with no way to tell whether 1080p
# genuinely dislikes an arm or touchdown is peculiar. Six more at each size
# closes that.
#
# WHAT IS DELIBERATELY ABSENT. Nothing here may appear in the ML training
# corpus (BVI-AOM) or the train/test split breaks silently. BVI-AOM draws on
# BVI-Texture, IRIS, Harmonics, Videvo, SJTU, MCL-JCV, LIVE-Netflix and Yonsei
# material, so the Netflix 4K set on this same host is OFF LIMITS for gate use
# and so is anything Harmonics-derived. The SVT, TUM and JCT-VC class-E sets
# below appear nowhere in it, checked name by name on 2026-08-30.
#
# Also absent: the aspen / red_kayak / speed_bag / snow_mnt / west_wind_easy /
# rush_field_cuts / controlled_burn group. touchdown_pass comes from that set
# and is the 4:2:2 clip already in the tree, so treat the whole group as
# suspect until a header says otherwise.
clips_res=(
    # 720p. shields/parkrun/stockholm are the SVT "ter" pans, the classic
    # detail-under-motion cases; in_to_tree is a slow zoom into foliage;
    # old_town is an aerial pan. fourpeople is the videoconference class, which
    # the corpus otherwise only has at CIF (akiyo).
    "shields_720p         $base/y4m/720p50_shields_ter.y4m          -   detail"
    "parkrun_720p         $base/y4m/720p50_parkrun_ter.y4m          -   detail"
    "stockholm_720p       $base/y4m/720p5994_stockholm_ter.y4m      -   detail"
    "in_to_tree_720p      $base/y4m/in_to_tree_420_720p50.y4m       -   detail"
    "old_town_720p        $base/y4m/old_town_cross_420_720p50.y4m   -   grain"
    "fourpeople_720p      $base/y4m/FourPeople_1280x720_60.y4m      -   static"
    # 1080p, the class that had one member. blue_sky is a slow rotation over
    # low texture, pedestrian a static camera over walking people, riverbed is
    # water at the edge of noise (the hardest thing here), station2 a pan over
    # fine rail detail, sunflower a smooth close-up, crowd_run dense motion.
    "blue_sky_1080p       $base/y4m/blue_sky_1080p25.y4m            -   motion"
    "pedestrian_1080p     $base/y4m/pedestrian_area_1080p25.y4m     -   static"
    "riverbed_1080p       $base/y4m/riverbed_1080p25.y4m            -   detail"
    "station2_1080p       $base/y4m/station2_1080p25.y4m            -   detail"
    "sunflower_1080p      $base/y4m/sunflower_1080p25.y4m           -   static"
    "crowd_run_1080p      $base/y4m/crowd_run_1080p50.y4m           e056e01fc13906dd9a84f5cc686a36888436379476577965a0866821caaf55c8   crowd"
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
        got="$( (shasum -a 256 "$out" 2>/dev/null || sha256sum "$out") | awk '{print $1}')"
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

if [ "$res" = 1 ]; then
    echo "-- tier 3 (--res): the resolution-balance set, 6 at 720p and 6 at 1080p --"
    for row in "${clips_res[@]}"; do
        read -r c_name c_url c_want c_class <<< "$row"
        fetch "$c_name" "$c_url" "$c_want" "$c_class"
    done
fi

# --review: the windows `make review` times (scripts/parity-clips.sh REVIEW_CLIPS).
# These are cut from two long public sources rather than downloaded whole:
# Big Buck Bunny (Blender Foundation, CC-BY 3.0; the 2013 30 fps 1080p
# remaster, a 350 MB zip) and NASA's Perseverance landing (JPL-Caltech, public
# domain, SVS item 31250, 265 MB). The source files are kept in
# tests/corpus/.src/ so the cuts are reproducible; ffmpeg does the cutting.
cut_from() {      # cut_from <name> <source-file> <start-seconds> <frames> <scale or ->
    local name="$1" src="$2" ss="$3" frames="$4" scale="$5"
    local out="$dest/$name.y4m"
    echo "$name timed-only" >> "$manifest"
    if [ -f "$out" ]; then echo "have  $name  [timed-only]"; return; fi
    echo "cut   $name  [timed-only] from ${src##*/} at ${ss}s, $frames frames"
    local vf="format=yuv420p"
    [ "$scale" != "-" ] && vf="scale=$scale:flags=lanczos,format=yuv420p"
    ffmpeg -v error -y -ss "$ss" -i "$src" -frames:v "$frames" -vf "$vf" -f yuv4mpegpipe "$out.tmp.$$"
    mv "$out.tmp.$$" "$out"
}
get_source() {    # get_source <file-name> <url>  -> tests/corpus/.src/<file-name>, unzipped if a .zip
    local name="$1" url="$2" dir="$dest/.src"
    mkdir -p "$dir"
    if [ -f "$dir/$name" ]; then return; fi
    echo "source $name"
    case "$url" in
        *.zip) curl -fSL "$url" -o "$dir/$name.zip"; (cd "$dir" && unzip -o -q "$name.zip" && rm -f "$name.zip") ;;
        *)     curl -fSL "$url" -o "$dir/$name" ;;
    esac
    [ -f "$dir/$name" ] || { echo "  $name did not appear after download" >&2; exit 1; }
}
if [ "$review" = 1 ]; then
    echo "-- tier 4 (--review): the make-review windows, cut from two public sources --"
    command -v ffmpeg >/dev/null || { echo "ffmpeg is needed for --review" >&2; exit 1; }
    get_source bbb_sunflower_1080p_30fps_normal.mp4 https://download.blender.org/demo/movies/BBB/bbb_sunflower_1080p_30fps_normal.mp4.zip
    get_source Perseverance-landing-1080p.mp4 https://svs.gsfc.nasa.gov/vis/a030000/a031200/a031250/Perseverance-landing-1080p.mp4
    bbb="$dest/.src/bbb_sunflower_1080p_30fps_normal.mp4"; per="$dest/.src/Perseverance-landing-1080p.mp4"
    cut_from bbb_720p           "$bbb" 585 450 1280:720
    cut_from bbb10s_1080p_o120  "$bbb" 120 300 -
    cut_from bbb15s_1080p_o120  "$bbb" 120 450 -
    cut_from bbb30s_1080p_o120  "$bbb" 120 900 -
    cut_from perseverance_1080p "$per" 168 450 -
    cut_from perseverance_720p  "$per" 168 450 1280:720
fi

echo "corpus ready in $dest (classes in $manifest)"
