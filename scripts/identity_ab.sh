#!/usr/bin/env bash
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# identity_ab.sh - do two encoders produce the same bitstreams?
#
#   scripts/identity_ab.sh OLD_BIN NEW_BIN
#   MATRIX=quick scripts/identity_ab.sh build-old/cli/yah264 build/cli/yah264
#   ARM_B='Y264_FOO=0' scripts/identity_ab.sh build/cli/yah264 build/cli/yah264
#   ONLY='cavlc|422' scripts/identity_ab.sh A B
#
# Encodes every cell of a config matrix with both binaries and compares the
# md5 of the outputs. It is the gate for changes that must not move a bit: a
# refactor, a layout change, the removal of a knob whose default branch is
# kept. Arguing that a change is "byte-identical by construction" is not a
# gate (docs/instruments.md, section 7); this is.
#
# Statuses, one row per cell:
#   SAME    the md5s match
#   DIFF    they don't, and at one thread that is final. Threaded cells are
#           re-run (REPEAT times per side) before a DIFF is called: both
#           sides repeated themselves and still disagree
#   VARIES  threaded only: a side gave more than one bitstream across its
#           re-runs and no run of OLD matched a run of NEW. Threaded output
#           need not repeat (owner rule, 2026-09-25), so this warns and does
#           not fail; read the cell at one thread instead
#   FAIL    an encode exited non-zero or wrote nothing
# A control cell runs the two sides with different flags and must come out
# DIFF. If it reads SAME the comparison itself is broken (an encode that never
# ran hashes the same on both sides) and the script exits 2.
#
# Exit: 0 when every cell is SAME or VARIES, 1 on any DIFF or FAIL, 2 when
# the control fails.
#
# Environment:
#   MATRIX     full (default) or quick
#   ONLY       extended regex on cell names; only matching cells run
#   FRAMES     frames for the CIF cells (default 40); HD cells use fewer
#   THREADED   thread count for the threaded cells (default 4; 0 = auto)
#   BUDGET     total threads the script may keep busy (default 4)
#   REPEAT     re-runs per side before a threaded cell is called (default 3)
#   ARM_A, ARM_B   NAME=VALUE env settings for the OLD / NEW side only
#   CORPUS     clip directory (default tests/corpus)
#   CLIP_4K    optional path to a 2160p y4m; adds 4K cells when set
#   FFMPEG     used to derive the 4:2:2, 4:4:4 and 10-bit inputs
#   KEEP_SCRATCH=1 keeps the working directory (bitstreams, logs)
#
# Every Y264_* variable in the caller's environment is cleared first, so both
# sides start from the shipped defaults; ARM_A / ARM_B are the only way to set
# one.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SELF="$ROOT/scripts/identity_ab.sh"

hash_of() {
    if command -v md5 >/dev/null 2>&1; then md5 -q "$1"
    else md5sum "$1" | cut -d' ' -f1; fi
}

# ---------------------------------------------------------------- one cell
# Called back through xargs: identity_ab.sh --cell WORK LINE
if [ "${1:-}" = "--cell" ]; then
    work="$2"; line="$3"
    IFS='|' read -r name threads clip frames args_a args_b <<EOF
$line
EOF
    [ -n "$args_b" ] || args_b="$args_a"
    [ "$args_b" = "=" ] && args_b="$args_a"
    d="$work/cells/$name"; mkdir -p "$d"

    # enc SIDE RUN -> prints the md5 of the stream (plus stats for 2-pass),
    # or FAIL
    enc() {
        local side="$1" run="$2" bin arm args out log
        if [ "$side" = a ]; then bin="$OLD"; arm="$ARM_A"; args="$args_a"
        else bin="$NEW"; arm="$ARM_B"; args="$args_b"; fi
        out="$d/$side$run.264"; log="$d/$side$run.log"
        rm -f "$out" "$d/$side$run.stats"*
        local pass2=0
        case " $args " in *" PASS2 "*) pass2=1; args="${args/PASS2/}";; esac
        # shellcheck disable=SC2086 # args and arm are word lists on purpose
        if [ "$pass2" = 1 ]; then
            env $arm "$bin" --input-y4m "$clip" --frames "$frames" \
                --threads "$threads" --no-progress $args --pass 1 \
                --stats "$d/$side$run.stats" -o /dev/null >"$log" 2>&1 \
                || { echo FAIL; return; }
            env $arm "$bin" --input-y4m "$clip" --frames "$frames" \
                --threads "$threads" --no-progress $args --pass 2 \
                --stats "$d/$side$run.stats" -o "$out" >>"$log" 2>&1 \
                || { echo FAIL; return; }
        else
            env $arm "$bin" --input-y4m "$clip" --frames "$frames" \
                --threads "$threads" --no-progress $args -o "$out" \
                >"$log" 2>&1 || { echo FAIL; return; }
        fi
        [ -s "$out" ] || { echo FAIL; return; }
        hash_of "$out"
    }

    a=$(enc a 1); b=$(enc b 1); status=SAME
    if [ "$a" = FAIL ] || [ "$b" = FAIL ]; then
        status=FAIL
    elif [ "$a" != "$b" ]; then
        status=DIFF
        if [ "$threads" != 1 ]; then
            as="$a"; bs="$b"; i=2
            while [ "$i" -le "$REPEAT" ]; do
                as="$as $(enc a "$i")"; bs="$bs $(enc b "$i")"; i=$((i + 1))
            done
            case " $as $bs " in *" FAIL "*) status=FAIL;; esac
            if [ "$status" != FAIL ]; then
                hit=0
                for x in $as; do for y in $bs; do
                    [ "$x" = "$y" ] && hit=1
                done; done
                na=$(printf '%s\n' $as | sort -u | wc -l | tr -d ' ')
                nb=$(printf '%s\n' $bs | sort -u | wc -l | tr -d ' ')
                if [ "$hit" = 1 ]; then status=SAME
                elif [ "$na" -gt 1 ] || [ "$nb" -gt 1 ]; then status=VARIES
                fi
                a="$a(x$na)"; b="$b(x$nb)"
            fi
        fi
    fi
    printf '%s|%s|%s|%s|%s\n' "$name" "$threads" "$status" "$a" "$b" \
        > "$d/result"
    [ "${KEEP_SCRATCH:-0}" = 1 ] || rm -f "$d"/*.264 "$d"/*.stats*
    exit 0
fi

# ---------------------------------------------------------------- driver
[ $# -eq 2 ] || { sed -n '5,10p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
OLD="$1"; NEW="$2"
for b in "$OLD" "$NEW"; do
    [ -x "$b" ] || { echo "identity_ab: not an executable: $b" >&2; exit 2; }
done
OLD="$(cd "$(dirname "$OLD")" && pwd)/$(basename "$OLD")"
NEW="$(cd "$(dirname "$NEW")" && pwd)/$(basename "$NEW")"

MATRIX="${MATRIX:-full}"; ONLY="${ONLY:-}"
FRAMES="${FRAMES:-40}"; THREADED="${THREADED:-4}"; BUDGET="${BUDGET:-4}"
REPEAT="${REPEAT:-3}"; ARM_A="${ARM_A:-}"; ARM_B="${ARM_B:-}"
CORPUS="${CORPUS:-$ROOT/tests/corpus}"; CLIP_4K="${CLIP_4K:-}"
FFMPEG="${FFMPEG:-ffmpeg}"

# An ARM word that is not NAME=VALUE makes env(1) run it as the command, so
# every encode would fail the same way on both sides. Refuse it up front.
for w in $ARM_A $ARM_B; do
    case "$w" in
        [A-Za-z_]*=*) ;;
        *) echo "identity_ab: ARM word '$w' is not NAME=VALUE" >&2; exit 2;;
    esac
done
for v in $(env | sed -n 's/^\(Y264_[A-Za-z0-9_]*\)=.*/\1/p'); do unset "$v"; done
export OLD NEW ARM_A ARM_B REPEAT

. "$ROOT/scripts/scratch.sh"
y264_scratch_dir identityab WORK

# Derived inputs: other chroma formats and 10-bit from one 4:2:0 clip.
base="$CORPUS/foreman_cif.y4m"
[ -f "$base" ] || { echo "identity_ab: no $base (set CORPUS)" >&2; exit 2; }
derive() {   # derive PIX_FMT NAME
    "$FFMPEG" -nostdin -v error -i "$base" -frames:v "$FRAMES" -pix_fmt "$1" \
        -strict -1 -y "$WORK/$2.y4m" || {
        echo "identity_ab: ffmpeg could not make $2" >&2; exit 2; }
}
derive yuv422p f422; derive yuv444p f444
derive yuv420p10le f10; derive yuv422p10le f42210

c() { echo "$CORPUS/$1.y4m"; }
F="$FRAMES"; HD=$(( F / 2 )); FHD=$(( F / 4 ))
[ "$HD" -ge 8 ] || HD=8; [ "$FHD" -ge 6 ] || FHD=6
T="$THREADED"

# name|threads|clip|frames|args_old|args_new (args_new empty = same)
cells() {
    local fm sh pd st ak
    fm=$(c foreman_cif); st=$(c stefan_cif); ak=$(c akiyo_cif)
    sh=$(c shields_720p); pd=$(c pedestrian_1080p)
    # the quick set: one cell per rate mode and format, plus a threaded pair
    echo "crf|1|$fm|$F|--crf 23"
    echo "cqp|1|$fm|$F|--qp 30"
    echo "abr|1|$fm|$F|--bitrate 600"
    echo "abr_vbv|1|$fm|$F|--bitrate 600 --vbv-maxrate 900 --vbv-bufsize 900"
    echo "cbr|1|$st|$F|--bitrate 800 --vbv-maxrate 800 --vbv-bufsize 800 --nal-hrd cbr"
    echo "crf_vbv|1|$st|$F|--crf 23 --vbv-maxrate 700 --vbv-bufsize 700"
    echo "twopass|1|$fm|$F|--bitrate 500 PASS2"
    echo "cavlc|1|$fm|$F|--crf 26 --cavlc"
    echo "yuv422|1|$WORK/f422.y4m|$F|--crf 26"
    echo "yuv444|1|$WORK/f444.y4m|$F|--crf 26"
    echo "10bit|1|$WORK/f10.y4m|$F|--crf 26"
    echo "1080p|1|$pd|$FHD|--crf 23"
    echo "crf_t$T|$T|$fm|$F|--crf 23"
    echo "abr_t$T|$T|$st|$F|--bitrate 800"
    [ "$MATRIX" = quick ] && return
    # presets and tunes
    for p in ultrafast superfast veryfast faster fast slow slower veryslow; do
        echo "preset_$p|1|$fm|$F|--preset $p --crf 24"
    done
    for t in grain animation film psnr ssim zerolatency; do
        echo "tune_$t|1|$st|$F|--tune $t --crf 24"
    done
    # rate control corners
    echo "crf_hi|1|$ak|$F|--crf 40"
    echo "crf_lo|1|$st|$F|--crf 14"
    echo "cqp_hi|1|$st|$F|--qp 45"
    echo "abr_low|1|$st|$F|--bitrate 150"
    echo "abr_qpstep|1|$fm|$F|--bitrate 600 --qpstep 2 --qpmin 20 --qpmax 40"
    echo "twopass_vbv|1|$st|$F|--bitrate 700 --vbv-maxrate 1000 --vbv-bufsize 1000 PASS2"
    echo "crf_max|1|$st|$F|--crf 20 --vbv-maxrate 500 --vbv-bufsize 500 --crf-max 40"
    echo "vbr_hrd|1|$fm|$F|--bitrate 600 --vbv-maxrate 900 --vbv-bufsize 1800 --nal-hrd vbr --vbv-init 0.5"
    echo "no_mbtree|1|$fm|$F|--crf 23 --no-mbtree"
    echo "lookahead10|1|$fm|$F|--crf 23 --rc-lookahead 10"
    echo "aq_off|1|$fm|$F|--crf 23 --aq-strength 0"
    echo "aq_mode2|1|$fm|$F|--crf 23 --aq-mode 2"
    # coding tools
    echo "bframes0|1|$fm|$F|--crf 23 --bframes 0"
    echo "bframes1|1|$fm|$F|--crf 23 --bframes 1"
    echo "no_pyramid|1|$fm|$F|--crf 23 --b-pyramid none"
    echo "badapt0|1|$fm|$F|--crf 23 --b-adapt 0"
    echo "ref1|1|$st|$F|--crf 23 --ref 1"
    echo "ref5|1|$st|$F|--crf 23 --ref 5"
    echo "direct_temporal|1|$st|$F|--crf 23 --direct temporal"
    echo "direct_spatial|1|$st|$F|--crf 23 --direct spatial"
    echo "weightp2|1|$fm|$F|--crf 23 --weightp 2"
    echo "no_weightb|1|$fm|$F|--crf 23 --no-weightb"
    echo "no_8x8|1|$fm|$F|--crf 23 --no-transform-8x8"
    echo "trellis0|1|$fm|$F|--crf 23 --trellis 0"
    echo "trellis2|1|$fm|$F|--crf 23 --trellis 2"
    echo "no_psy|1|$fm|$F|--crf 23 --no-psy"
    echo "psy_trellis|1|$st|$F|--crf 23 --psy-trellis 1.0"
    echo "me_dia|1|$st|$F|--crf 23 --me dia"
    echo "me_umh|1|$st|$F|--crf 23 --me umh --merange 24"
    echo "subme5|1|$st|$F|--crf 23 --subme 5"
    echo "subme9|1|$st|$F|--crf 23 --subme 9"
    echo "subme11|1|$fm|$F|--crf 23 --subme 11"
    echo "partitions_all|1|$fm|$F|--crf 23 --partitions all"
    echo "cqm_jvt|1|$fm|$F|--crf 23 --cqm jvt"
    echo "constrained_intra|1|$fm|$F|--crf 23 --constrained-intra"
    echo "deblock|1|$fm|$F|--crf 23 --deblock -2:1"
    echo "no_deblock|1|$fm|$F|--crf 23 --no-deblock"
    echo "deadzone|1|$fm|$F|--crf 23 --deadzone-inter 25 --deadzone-intra 15"
    echo "no_dct_decimate|1|$fm|$F|--crf 23 --no-dct-decimate"
    echo "no_fast_pskip|1|$fm|$F|--crf 23 --no-fast-pskip"
    echo "chroma_qp|1|$fm|$F|--crf 23 --chroma-qp-offset 2"
    echo "nr|1|$st|$F|--crf 23 --nr 200"
    echo "mcstf|1|$st|$F|--crf 23 --mcstf"
    echo "slices4|1|$fm|$F|--crf 23 --slices 4"
    echo "keyint12_open|1|$fm|$F|--crf 23 --keyint 12 --open-gop"
    echo "no_scenecut|1|$fm|$F|--crf 23 --no-scenecut --keyint 20"
    echo "paff_tff|1|$fm|$F|--crf 23 --tff"
    echo "baseline|1|$fm|$F|--profile baseline --crf 23"
    echo "main_profile|1|$fm|$F|--profile main --crf 23"
    echo "no_asm|1|$fm|$F|--crf 23 --no-asm"
    # formats
    echo "yuv422_cavlc|1|$WORK/f422.y4m|$F|--crf 26 --cavlc"
    echo "yuv444_abr|1|$WORK/f444.y4m|$F|--bitrate 900"
    echo "10bit_cavlc|1|$WORK/f10.y4m|$F|--crf 26 --cavlc"
    echo "10bit_abr_vbv|1|$WORK/f10.y4m|$F|--bitrate 700 --vbv-maxrate 1000 --vbv-bufsize 1000"
    echo "10bit_422|1|$WORK/f42210.y4m|$F|--crf 26"
    echo "out_depth10|1|$fm|$F|--crf 26 --output-depth 10"
    # sizes
    echo "720p|1|$sh|$HD|--crf 23"
    echo "720p_veryslow|1|$sh|$FHD|--preset veryslow --crf 23"
    echo "1080p_abr_vbv|1|$pd|$FHD|--bitrate 6000 --vbv-maxrate 9000 --vbv-bufsize 9000"
    # threaded: the wavefront, the staircase, GOP workers
    echo "cqp_t$T|$T|$fm|$F|--qp 28"
    echo "abr_vbv_t$T|$T|$st|$F|--bitrate 800 --vbv-maxrate 1200 --vbv-bufsize 1200"
    echo "cavlc_t$T|$T|$fm|$F|--crf 26 --cavlc"
    echo "slices_t$T|$T|$fm|$F|--crf 23 --slices 4"
    echo "direct_temporal_t$T|$T|$st|$F|--crf 23 --direct temporal"
    echo "yuv422_t$T|$T|$WORK/f422.y4m|$F|--crf 26"
    echo "10bit_t$T|$T|$WORK/f10.y4m|$F|--crf 26"
    echo "720p_t$T|$T|$sh|$HD|--crf 23"
    echo "1080p_t$T|$T|$pd|$HD|--crf 23"
    echo "keyint10_t$T|$T|$fm|$(( F * 2 ))|--crf 23 --keyint 10"
    if [ -n "$CLIP_4K" ]; then
        echo "2160p|1|$CLIP_4K|6|--crf 23"
        echo "2160p_t$T|$T|$CLIP_4K|8|--crf 23"
    fi
}

{
    cells
    # the control: NEW is asked for a different stream; it must read DIFF
    echo "control_must_differ|1|$(c foreman_cif)|8|--crf 23|--crf 30"
} | { if [ -n "$ONLY" ]; then grep -E "^[^|]*(${ONLY})[^|]*\||^control_must_differ\|"; else cat; fi; } \
  > "$WORK/cells.txt"

missing=$(cut -d'|' -f3 "$WORK/cells.txt" | sort -u | while read -r f; do
    [ -f "$f" ] || echo "$f"; done)
[ -z "$missing" ] || { echo "identity_ab: missing clips:" $missing >&2; exit 2; }

ncell=$(wc -l < "$WORK/cells.txt" | tr -d ' ')
echo "identity_ab: $ncell cells, matrix $MATRIX, threaded=$THREADED, budget $BUDGET"
echo "  OLD $OLD  md5 $(hash_of "$OLD")  ${ARM_A:+env $ARM_A}"
echo "  NEW $NEW  md5 $(hash_of "$NEW")  ${ARM_B:+env $ARM_B}"

# one-thread cells BUDGET at a time, then threaded cells as many as fit
run_set() {   # run_set PARALLEL < lines
    tr '\n' '\0' | xargs -0 -n 1 -P "$1" "$SELF" --cell "$WORK"
}
awk -F'|' '$2 == 1' "$WORK/cells.txt" | run_set "$BUDGET"
tp=1; [ "$THREADED" -gt 0 ] 2>/dev/null && tp=$(( BUDGET / THREADED ))
[ "$tp" -ge 1 ] || tp=1
awk -F'|' '$2 != 1' "$WORK/cells.txt" | run_set "$tp"

same=0; diff=0; vary=0; fail=0; ctrl=BAD
printf '\n%-24s %3s  %-6s  %-38s %s\n' cell thr status "old md5" "new md5"
while IFS='|' read -r name _; do
    r="$WORK/cells/$name/result"
    if [ ! -f "$r" ]; then
        printf '%-24s %3s  %-6s\n' "$name" "?" FAIL; fail=$((fail + 1)); continue
    fi
    IFS='|' read -r n t s a b < "$r"
    if [ "$n" = control_must_differ ]; then
        [ "$s" = DIFF ] && ctrl=OK
        printf '%-24s %3s  %-6s  (control: must be DIFF -> %s)\n' "$n" "$t" "$s" "$ctrl"
        continue
    fi
    printf '%-24s %3s  %-6s  %-38s %s\n' "$n" "$t" "$s" "$a" "$b"
    case "$s" in
        SAME) same=$((same + 1));; DIFF) diff=$((diff + 1));;
        VARIES) vary=$((vary + 1));; *) fail=$((fail + 1));;
    esac
done < "$WORK/cells.txt"

echo
if [ "$ctrl" != OK ]; then
    echo "identity_ab: CONTROL FAILED (the comparison cannot see a difference)"
    exit 2
fi
verdict=PASS; [ $((diff + fail)) -eq 0 ] || verdict=DIFF
echo "identity_ab: $verdict  same $same  diff $diff  varies $vary  fail $fail"
[ "$vary" -eq 0 ] || echo "  (VARIES = threaded output not repeatable here; informational)"
[ "$verdict" = PASS ]
