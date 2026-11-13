#!/usr/bin/env bash
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# campaign.sh -- the measurement session on a rented x86-64 box
# (docs/simd.md).
#
# It runs after bootstrap.sh and reads everything it knows about the machine
# out of `bootstrap.ok`. It invents nothing: no CPU name, no thread count, no
# binary path. If bootstrap did not produce something, the leg that needs it
# skips by name.
#
# WHAT IT PRODUCES, per SKU per day, under results/<sku>/<date>/:
#   board-t1-{asm,purec}.txt   perf-comp over the clip set at one PINNED core
#   board-tN-{asm,purec}.txt   the same at the SKU's full vCPU count
#   ffboard-*.txt              the in-process control, both encoders as
#                              libraries in one ffmpeg
#   bd-at-rate.txt             BD-rate at matched achieved bitrate
#   checkasm-bench-<tier>.txt  per-tier kernel timings
#   instr-ratio.txt            the CPU-seconds column (perf stat)
#   identity.txt               md5 per arm and per tier
#   avx512-ab.txt              AVX-512 vs AVX2 on ONE binary, with a
#                              YAH264_NO_ASM=1 control column
#   mhz.log                    /proc/cpuinfo MHz every 30 s, beside every table
#   campaign-report.txt        the stage table: PASS, SKIP, FAIL, REHEARSED
# ...then one tarball, pushed to the bucket before the instance is terminated.
#
# THE RULE THIS SCRIPT EXISTS TO KEEP. A wall time read under an emulator is
# the EMULATOR's, and so is an instruction count. So a clock is read only when
# CLOUD=1 AND no emulator is detected (lib.sh, can_time). Everywhere else every
# timed leg still RUNS -- at a shape small enough to be cheap -- and is
# recorded REHEARSED. That is the rehearsal criterion: the kit runs unattended
# to a tarball. It is not, and must never be quoted as, a speed number.
#
# MHz DRIFT. A turbo curve that sags mid-table turns a speed ratio into a
# thermal reading, so the MHz log runs beside every leg and the plan's rule is
# that any run over 3% drift is repeated. This script logs and reports the
# drift; repeating is the operator's call, because the repeat costs money.
#
# Usage:
#   scripts/cloud/campaign.sh                 # after bootstrap.sh
#   CLOUD=1 WALL_BUDGET_H=7.5 scripts/cloud/campaign.sh
#
# Environment:
#   CLOUD=1           real silicon: clocks are read, AVX-512 and SDE legs run
#   BUCKET_URL        where the tarball is pushed (file:// for the rehearsal)
#   CLOUD_WORK        must match bootstrap.sh (default <tree>/scratch/cloud)
#   SKU               the instance name, e.g. c3-highcpu-22; derived if unset
#   WALL_BUDGET_H     hours; partial results are pushed before it runs out
#   TN                threads for the tN leg (default: the SKU's vCPU count)
#   SET_SECONDS       seconds of each clip per cell (default 6, 2 rehearsing)
#   LEGS              space-separated subset of the stage names below
set -uo pipefail

C_SELF="$(cd "$(dirname "$0")" && pwd)"
C_TREE="$(cd "$C_SELF/../.." && pwd)"

CLOUD="${CLOUD:-0}"
CLOUD_WORK="${CLOUD_WORK:-$C_TREE/scratch/cloud}"
C_OK="$CLOUD_WORK/bootstrap.ok"

# shellcheck source=scripts/cloud/lib.sh
. "$C_SELF/lib.sh"

[ -f "$C_OK" ] || {
    echo "campaign: no $C_OK -- run scripts/cloud/bootstrap.sh first." >&2
    exit 2
}
# shellcheck source=/dev/null
. "$C_OK"

# THEN CHECK IT ARRIVED. bootstrap.ok is sourced, so a badly quoted value in it
# does not fail loudly -- it closes a quote early and the lines beneath it are
# reparsed as something else, leaving variables simply undefined. Under `set -u`
# the first symptom is "CPU_MODEL: unbound variable" eighty lines away, which
# points at a variable that has nothing wrong with it. The rehearsal produced
# exactly that. So name the file and the missing keys instead.
_missing=""
for _k in Y264_CPU_NAME CPU_MODEL CPU_FLAGS NPROC KERNEL Y264_SRC \
          Y264_COMMIT Y264_BUILD8 X264_ASM X264_C CLOUD_WORK; do
    eval "[ -n \"\${$_k+set}\" ]" || _missing="$_missing $_k"
done
if [ -n "$_missing" ]; then
    echo "campaign: $C_OK is incomplete; these keys never got defined:" >&2
    echo "   $_missing" >&2
    echo "campaign: that usually means a value in it is badly quoted. Re-run" >&2
    echo "          scripts/cloud/bootstrap.sh with STAGES=bootstrap.ok" >&2
    exit 2
fi

C_SRC="${Y264_SRC:-$C_TREE}"
C_B8="${Y264_BUILD8:-$C_SRC/build-rel}"
CK="$C_B8/tools/checkasm/checkasm"
Y264="$C_B8/cli/yah264"

c_detect_emulator

# THE SKU IS THE RESULT DIRECTORY'S NAME, so it has to be the instance's and
# not a guess. The operator passes it (c3-highcpu-22); with nothing passed it
# is derived from the CPU model and the vCPU count, which is right often enough
# to be useful and is always visible in the path.
SKU="${SKU:-$(c_slug "$(c_cpu_model)")-$(c_nproc)vcpu}"
DATE="${DATE:-$(date -u +%Y-%m-%d)}"
RES="$CLOUD_WORK/results/$SKU/$DATE"
C_LOGS="$RES/logs"
mkdir -p "$RES" "$C_LOGS"

TN="${TN:-$(c_nproc)}"
# SMT AS SHIPPED. The tN leg runs at the SKU's full vCPU count with SMT on,
# because that is how a user runs x264 too. A physical-core reading wants a
# different SKU with SMT switched off; the plan's table prices both and the
# header below says which one this is.
SET_SECONDS="${SET_SECONDS:-$(c_shape 6 2)}"

# THE PINNED CORE for every t1 leg. `taskset -c 2` is the plan's choice: core 0
# takes the timer and most of the interrupt load on a GCP instance, and core 1
# is core 0's SMT sibling on the SKUs in the table. A box with fewer than three
# CPUs pins to the last one it has and says so.
#
# PIN_DESC is what every table header says, and it is derived from what
# actually happened rather than from the intent. The dry run of this kit on a
# box with no taskset printed "pinned to cpu 2" over an unpinned leg, which is
# the precise kind of caption that survives into a published table.
PIN_CPU=2
[ "$(c_nproc)" -ge 3 ] || PIN_CPU=$(( $(c_nproc) - 1 ))
PIN=()
if command -v taskset >/dev/null 2>&1; then
    PIN=(taskset -c "$PIN_CPU")
    PIN_DESC="pinned to cpu $PIN_CPU"
else
    PIN_DESC="UNPINNED (no taskset on this box)"
    echo "campaign: no taskset; the t1 legs run unpinned and every header says so"
fi

ALL_LEGS="identity board-t1-asm board-t1-purec board-tN-asm board-tN-purec
          ffboard bd-at-rate bench-sse4 bench-avx2 bench-avx512 instr-ratio
          avx512-ab checkasm10 identity10 board10-t1 board10-tN"
# Normalised to single spaces before matching: ALL_LEGS wraps across lines, so
# the token at each line end is followed by a newline and the `case` below
# never matches it. Unfixed this made board-tN-purec and instr-ratio skip as
# "not in LEGS" on a run that named no LEGS. See bootstrap.sh for the whole
# story; the rehearsal found it there first.
LEGS="$(printf '%s' "${LEGS:-$ALL_LEGS}" | tr -s '[:space:]' ' ')"
has_leg() { case " $LEGS " in *" $1 "*) return 0 ;; *) return 1 ;; esac; }

for _l in $LEGS; do
    case " $(printf '%s' "$ALL_LEGS" | tr -s '[:space:]' ' ') " in
        *" $_l "*) ;;
        *) echo "campaign: LEGS names '$_l', which is not a leg. Known:" >&2
           printf '%s\n' "$ALL_LEGS" >&2; exit 2 ;;
    esac
done

echo "=========================================================================="
echo "yah264 cloud campaign"
echo "  sku         $SKU"
echo "  results     $RES"
echo "  cpu         $CPU_MODEL"
echo "  y264 cpuid  $Y264_CPU_NAME"
echo "  flags       $CPU_FLAGS"
echo "  kernel      $KERNEL"
echo "  commit      $Y264_COMMIT"
echo "  threads     t1 $PIN_DESC, tN = $TN (SMT as shipped)"
echo "  emulator    ${C_EMULATOR:-none detected}"
if c_can_time; then
    echo "  TIMING      ON -- these are measurements."
else
    echo "  TIMING      OFF -- $C_NO_TIME_WHY"
    echo "              Every leg still runs, at a reduced shape, and is"
    echo "              recorded REHEARSED. No number here is a speed number."
fi
echo "=========================================================================="
c_budget_start
echo

# ---------------------------------------------------------------------------
# The MHz log. It runs for the whole session beside every table, because the
# question it answers -- was the box at a steady clock while this row was
# taken -- cannot be asked afterwards.

MHZ_PID=""
mhz_start() {
    grep -q '^cpu MHz' /proc/cpuinfo 2>/dev/null || {
        echo "mhz: /proc/cpuinfo has no 'cpu MHz' line here; no drift log" \
            | tee "$RES/mhz.log"
        return 0
    }
    ( while :; do
          printf '%s %s\n' "$(date -u +%H:%M:%S)" \
              "$(awk '/^cpu MHz/{printf "%s ", $4}' /proc/cpuinfo)"
          sleep 30
      done ) > "$RES/mhz.log" 2>/dev/null &
    MHZ_PID=$!
    echo "mhz: logging every 30 s to mhz.log (pid $MHZ_PID)"
}
mhz_stop() {
    [ -n "$MHZ_PID" ] && kill "$MHZ_PID" 2>/dev/null
    MHZ_PID=""
}
mhz_drift() {   # the plan's 3% rule, reported not enforced: a repeat costs money
    [ -s "$RES/mhz.log" ] || { echo "mhz: no samples"; return; }
    awk '{ for (i = 2; i <= NF; i++) if ($i + 0 > 0) {
              if (mn == 0 || $i + 0 < mn) mn = $i + 0
              if ($i + 0 > mx) mx = $i + 0 } }
         END { if (mx > 0) printf "mhz: min %.0f max %.0f drift %.2f%%%s\n",
                      mn, mx, (mx - mn) / mx * 100,
                      ((mx - mn) / mx * 100 > 3) ? "  OVER 3% -- the plan says repeat this run" : ""
               else print "mhz: no numeric samples" }' "$RES/mhz.log"
}

# ---------------------------------------------------------------------------
# Identity: the md5 of every arm and every tier, over the board clips.
#
# It is the cheapest leg and the one the whole campaign rests on, because no
# SIMD kernel may move a byte: `Y264_SIMD_FORCE` is an identity axis, not just
# a speed axis, and a tier that encodes differently from the C path is a
# correctness failure that every table above it would otherwise inherit
# silently. This is also the ONLY place AVX-512 is checked against real output
# rather than against checkasm alone.

st_identity() {
    [ -x "$Y264" ] || { c_skip "no yah264 binary"; return; }
    # shellcheck source=/dev/null
    . "$C_SRC/scripts/parity-clips.sh"
    # THE TIER LIST COMES FROM THE ARCHITECTURE, not from a constant. On
    # x86-64 it is the three shipped tiers plus the gated one when the build
    # and the CPU both have it. Anywhere else -- and the dry run of this kit
    # happens on the author's aarch64 box -- there are no x86 tiers to force,
    # and a hardcoded list would compare `Y264_SIMD_FORCE=sse4` against the
    # default on a machine where that knob means something else entirely, then
    # report the difference as a kernel moving a byte. So the non-x86 case
    # says what it is and checks the one axis that exists there.
    local tiers frames tier_note=""
    case "$(uname -m)" in
        x86_64|amd64)
            tiers="none sse4 avx2"
            c_has_avx512 "$CK" && tiers="$tiers avx512" ;;
        *)  tiers="none"
            tier_note="# NOTE $(uname -m) is not x86-64: there are no x86 tiers to force here, so only the no-SIMD axis was checked." ;;
    esac
    frames="$(c_shape 48 8)"
    local out="$RES/identity.txt"
    {
        echo "# identity md5s, $SKU, $DATE"
        echo "# tiers: $tiers   frames: $frames   (Y264_SIMD_FORCE is an identity axis)"
        case " $tiers " in *" avx512 "*) ;;
            *) echo "# NOTE avx512 absent from this binary/CPU -- not covered here" ;; esac
        [ -n "$tier_note" ] && echo "$tier_note"
    } > "$out"
    local tier spec clip rate t mode rc=0
    for tier in $tiers; do
        : > "$RES/.md5.$tier"
        for spec in $CLIPS; do
            clip="${spec%%:*}"; rate="${spec##*:}"
            local src="$C_SRC/tests/corpus/$clip.y4m"
            [ -f "$src" ] || continue
            for t in 1 "$TN"; do
                for mode in "--crf 23" "--qp 26" "--bitrate $rate"; do
                    rm -f "$RES/.x.264"
                    # shellcheck disable=SC2086
                    if ! Y264_SIMD_FORCE="$tier" "$Y264" --input-y4m "$src" \
                        --frames "$frames" $mode --threads "$t" \
                        -o "$RES/.x.264" >/dev/null 2>"$RES/.x.err"; then
                        echo "$tier $clip t$t $mode: ENCODE FAILED: $(head -1 "$RES/.x.err")" >> "$out"
                        rc=1
                        continue
                    fi
                    printf '%s  %s t%s %s\n' \
                        "$(c_sha256 "$RES/.x.264" | cut -c1-32)" "$clip" "$t" "$mode" \
                        >> "$RES/.md5.$tier"
                done
            done
        done
    done
    rm -f "$RES/.x.264" "$RES/.x.err"
    for tier in $tiers; do
        [ "$tier" = none ] && continue
        if diff -q "$RES/.md5.none" "$RES/.md5.$tier" >/dev/null 2>&1; then
            echo "$tier vs none: IDENTICAL ($(wc -l < "$RES/.md5.$tier" | tr -d ' ') encodes)" >> "$out"
        else
            echo "$tier vs none: DIFFERS -- a kernel moved a byte" >> "$out"
            diff "$RES/.md5.none" "$RES/.md5.$tier" >> "$out"
            rc=1
        fi
    done
    cp -f "$RES/.md5.none" "$RES/identity-md5.txt" 2>/dev/null
    rm -f "$RES"/.md5.*
    cat "$out"
    return "$rc"
}

# ---------------------------------------------------------------------------
# The board. perf-comp-set.sh over the clip set, against BOTH x264 arms:
# `asm` is each encoder with its SIMD as shipped, `pure` is both encoders'
# -O3 auto-vectorized C -- the fair pure-C quadrant, which needs x264's own
# -fno-tree-vectorize stripped at configure time. bootstrap.sh built both arms
# that way; this only chooses between them.

board() {   # board <mode: asm|pure> <threads> <pin: 0|1> <outfile>
    local mode="$1" thr="$2" pin="$3" out="$4"
    [ -x "$Y264" ] || { c_skip "no yah264 binary"; return; }
    [ -x "${X264_ASM:-}" ] || { c_skip "no x264-asm arm (the x264 build stage did not finish)"; return; }
    [ -x "${X264_C:-}" ]   || { c_skip "no x264-noasm-autovec arm (the x264 build stage did not finish)"; return; }
    # perf-comp.sh exits 2 before it encodes anything when the libvmaf CLI is
    # not on the PATH, so this is a skip and not a failure. bootstrap.sh builds
    # one; no distribution packages it.
    [ -x "${VMAF_BIN:-}" ] && [ -d "${VMAF_MODELS_DIR:-}" ] \
        || { c_skip "no libvmaf CLI or v1 models (the build-vmaf stage skipped or failed)"; return; }
    local runner=()
    [ "$pin" = 1 ] && runner=(${PIN[@]+"${PIN[@]}"})
    {
        echo "# $SKU  $DATE  $CPU_MODEL"
        echo "# mode $mode, threads $thr$([ "$pin" = 1 ] && echo ", $PIN_DESC" || echo ", unpinned, SMT as shipped")"
        echo "# yah264 $Y264_COMMIT vs x264 $X264_COMMIT"
        c_can_time || echo "# REHEARSAL -- ${C_NO_TIME_WHY} NO NUMBER BELOW IS A SPEED NUMBER."
        echo
    } > "$out"
    SET_THREADS="$thr" SET_SECONDS="$SET_SECONDS" \
    YAH264="$Y264" X264_ASM="$X264_ASM" X264_C="$X264_C" \
    VMAF_V1_BIN="${VMAF_BIN:-}" VMAF_V1_MODELS="${VMAF_MODELS_DIR:-}" \
        "${runner[@]}" "$C_SRC/scripts/perf-comp-set.sh" "$mode" >> "$out" 2>&1 \
        || return 1
    c_can_time || c_rehearsal_note "plumbing only: $(basename "$out")"
    tail -12 "$out"
    return 0
}

st_board_t1_asm()   { board asm  1     1 "$RES/board-t1-asm.txt"; }
st_board_t1_purec() { board pure 1     1 "$RES/board-t1-purec.txt"; }
st_board_tN_asm()   { board asm  "$TN" 0 "$RES/board-tN-asm.txt"; }
st_board_tN_purec() { board pure "$TN" 0 "$RES/board-tN-purec.txt"; }

# ---------------------------------------------------------------------------
# ffboard: the same question with both encoders as LIBRARIES in one ffmpeg
# process, so the input path is out of the measurement by construction. It is
# the control, not the headline, and it needs the fork bootstrap.sh builds.

st_ffboard() {
    [ -x "${FFBOARD_FFMPEG:-}" ] || { c_skip "no ffmpeg fork (build-ffmpeg skipped or failed)"; return; }
    [ -d "${Y264LIB:-}" ] || { c_skip "no installed libyah264"; return; }
    [ -x "${VMAF_BIN:-}" ] && [ -d "${VMAF_MODELS_DIR:-}" ] \
        || { c_skip "no libvmaf CLI or v1 models (the build-vmaf stage skipped or failed)"; return; }
    local out="$RES/ffboard.txt" thr
    for thr in 1 "$TN"; do
        {
            echo "# ffboard, threads $thr, $SKU $DATE"
            c_can_time || echo "# REHEARSAL -- not a speed number."
        } >> "$out"
        # `env` rather than a prefix assignment, because SECONDS is one of
        # bash's own special variables and assigning it in a command prefix
        # is a different thing from putting it in the child's environment.
        ( cd "$C_SRC" && env FF="$FFBOARD_FFMPEG" Y264LIB="$Y264LIB" \
            X264LIB="${X264LIB_ASM:-}" THREADS="$thr" RC=crf \
            SECONDS="$SET_SECONDS" CORP="$C_SRC/tests/corpus" \
            VMAF_V1_BIN="${VMAF_BIN:-}" VMAF_V1_MODELS="${VMAF_MODELS_DIR:-}" \
            python3 scripts/ffboard.py ) >> "$out" 2>&1 || return 1
    done
    c_can_time || c_rehearsal_note "plumbing only: ffboard.txt"
    tail -12 "$out"
    return 0
}

# ---------------------------------------------------------------------------
# BD-rate at MATCHED ACHIEVED BITRATE. Each arm's CRF is solved onto the same
# byte targets independently, so the two CRF scales cancel and the comparison
# is a quality result rather than a ladder-placement result. It is a quality
# leg, so it is the one table here that means the same thing under emulation
# as on metal -- but it costs a dozen encodes per point, so the rehearsal runs
# it on one clip.

st_bd_at_rate() {
    [ -x "$Y264" ] || { c_skip "no yah264 binary"; return; }
    [ -x "${X264_ASM:-}" ] || { c_skip "no x264 arm"; return; }
    command -v ffmpeg >/dev/null 2>&1 || { c_skip "no ffmpeg to decode with"; return; }
    [ -x "${VMAF_BIN:-}" ] && [ -d "${VMAF_MODELS_DIR:-}" ] \
        || { c_skip "no libvmaf CLI or v1 models (the build-vmaf stage skipped or failed)"; return; }
    local out="$RES/bd-at-rate.txt" clip="${BD_CLIP:-foreman_cif}"
    local src="$C_SRC/tests/corpus/$clip.y4m"
    [ -f "$src" ] || { c_skip "no $clip.y4m in the corpus"; return; }
    local frames; frames="$(c_shape 120 24)"
    # Byte targets spanning the 55-95 VMAF band the project scores in. Five
    # points, which is the project's own floor for a BD sweep.
    local targets="${BD_TARGETS:-120000,170000,240000,340000,480000}"
    {
        echo "# BD-rate at matched achieved bitrate, $clip, $frames frames"
        echo "# $SKU $DATE, yah264 $Y264_COMMIT vs x264 $X264_COMMIT"
        [ "$frames" -ge 120 ] || echo "# NOTE $frames frames is below the project's 120-frame floor for a BD sweep."
    } > "$out"
    ( cd "$C_SRC" && VMAF_V1_BIN="${VMAF_BIN:-}" VMAF_V1_MODELS="${VMAF_MODELS_DIR:-}" python3 scripts/bd_at_rate.py \
        --clip "$clip" --src "$src" --frames "$frames" --targets "$targets" \
        --label-a yah264 --label-b x264 \
        --a "$Y264 --input-y4m {src} --frames $frames --preset medium --crf {q} --threads 1 -o {out}" \
        --b "$X264_ASM --preset medium --crf {q} --frames $frames --threads 1 -o {out} {src}" \
      ) >> "$out" 2>&1 || return 1
    cat "$out"
    return 0
}

# ---------------------------------------------------------------------------
# checkasm --bench, per tier. A kernel multiple is NOT a ship criterion in this
# project (a faster kernel is necessary, not sufficient) and it is not a
# encoder-level speed claim either. It is recorded because it is the only place
# a per-kernel x86 number exists at all, and because the AVX-512 decision needs
# to see which kernels widen and which do not.

bench_tier() {   # bench_tier <tier>
    local tier="$1"
    local out="$RES/checkasm-bench-$tier.txt"
    [ -x "$CK" ] || { c_skip "no checkasm binary"; return; }
    if [ "$tier" = avx512 ] && ! c_has_avx512 "$CK"; then
        c_skip "no AVX-512 in this build/CPU ($Y264_CPU_NAME)"
        return
    fi
    if ! c_can_time; then
        # The correctness rows still run -- that is free and it is the part
        # that means something here. The timings are written and stamped.
        { echo "# REHEARSAL -- $C_NO_TIME_WHY"
          echo "# The kernel timings below are the EMULATOR's. Not a multiple."
        } > "$out"
        "$CK" --isa "$tier" >> "$out" 2>&1 || return 1
        c_rehearsal_note "correctness only: checkasm-bench-$tier.txt"
        return 0
    fi
    { echo "# $SKU $DATE  $CPU_MODEL"
      echo "# checkasm --bench --isa $tier, $PIN_DESC"; } > "$out"
    CHECKASM_CPU="$PIN_CPU" "${PIN[@]+"${PIN[@]}"}" "$CK" --bench --isa "$tier" >> "$out" 2>&1 || return 1
    tail -8 "$out"
    return 0
}
st_bench_sse4()   { bench_tier sse4; }
st_bench_avx2()   { bench_tier avx2; }
st_bench_avx512() { bench_tier avx512; }

# ---------------------------------------------------------------------------
# The CPU-seconds column. instr-ratio.sh's Linux branch reads instructions,
# cycles and task-clock off `perf stat`, which on a stock kernel is NOT
# readable: perf_event_paranoid defaults to 2 or 4 and the counter comes back
# empty. The script prints NO-COUNTER rather than a plausible zero, and this
# stage tests the counter FIRST so the outcome is a named skip instead of a
# table of NO-COUNTER rows.
#
# It is also the leg an emulator cannot fake: an emulated instruction count is
# the emulator's own, which is why the x86 programme reads this on the rented
# metal and nowhere else.

st_instr_ratio() {
    command -v perf >/dev/null 2>&1 || { c_skip "no perf on PATH (linux-tools not installed)"; return; }
    if ! perf stat -x, -e instructions -- true >/dev/null 2>&1; then
        c_skip "perf counters unreadable; sysctl kernel.perf_event_paranoid=1 or run privileged"
        return
    fi
    if ! perf stat -x, -e instructions -- true 2>&1 | grep -qE '^[0-9]'; then
        c_skip "perf returns an empty counter (a VM without PMU passthrough)"
        return
    fi
    c_can_time || { c_skip "an emulated instruction count is the emulator's own"; return; }
    local out="$RES/instr-ratio.txt"
    { echo "# $SKU $DATE  $CPU_MODEL"; echo "# perf stat -e instructions,cycles,task-clock"; } > "$out"
    ( cd "$C_SRC" && YAH264="$Y264" X264_C="$X264_C" \
        "${PIN[@]+"${PIN[@]}"}" ./scripts/instr-ratio.sh "$SET_SECONDS" ) >> "$out" 2>&1 || return 1
    cat "$out"
    return 0
}

# ---------------------------------------------------------------------------
# The 10-bit legs. The 10-bit library has its own SSE4.2 and AVX2 tiers and no
# AVX-512 form, so its identity axis is none/sse4/avx2, and the reference is
# x264 built at --bit-depth=10 (bootstrap's build-x264-10), both arms. The clips
# are 10-bit Y4M the operator stages in CLIPS10_DIR; the box fetches none.

C_B10="${Y264_BUILD10:-$C_SRC/build-rel10}"
Y264_10="$C_B10/cli/yah264"
CLIPS10_DIR="${CLIPS10_DIR:-$CLOUD_WORK/clips10}"
FRAMES10="${FRAMES10:-$(c_shape 60 6)}"

clips10() { ls "$CLIPS10_DIR"/*.y4m 2>/dev/null; }

# the tiers this CPU can run, from the 10-bit binary's own feature word: an
# avx2 row on a part without avx2, fma3 and bmi2 would test the sse4 tier
# under the avx2 name. the line reads "checkasm: cpu <features>, <depth>";
# the features are matched as whole words between the prefix and the comma.
# a missing or reworded line, or an x86-64 part with no sse4, is a failure:
# read as "no tiers" it would pass every leg that uses it
has10() { case " $TIERS10_CPU " in *" $1 "*) return 0 ;; esac; return 1; }
tiers10() {
    local line
    line=$("$C_B10/tools/checkasm/checkasm" --list 2>/dev/null | head -1)
    echo "# $line" >&2
    case "$line" in
        "checkasm: cpu "*) ;;
        *) echo "# no 'checkasm: cpu' line from checkasm --list" >&2; return 1 ;;
    esac
    TIERS10_CPU=${line#checkasm: cpu }
    TIERS10_CPU=${TIERS10_CPU%%,*}
    case "$(uname -m)" in
        x86_64|amd64) has10 sse4 || { echo "# an x86-64 part reports no sse4" >&2; return 1; } ;;
    esac
    has10 sse4 && printf 'sse4 '
    has10 avx2 && has10 fma3 && has10 bmi2 && printf 'avx2 '
    return 0
}

st_checkasm10() {
    [ -x "$C_B10/tools/checkasm/checkasm" ] || { c_skip "no 10-bit build"; return; }
    local out="$RES/checkasm10.txt" isa rc=0 have
    have=$(tiers10 2> "$out") || { cat "$out"; return 1; }
    for isa in sse4 avx2; do
        case " $have " in
            *" $isa "*) "$C_B10/tools/checkasm/checkasm" --isa "$isa" >> "$out" 2>&1 || rc=1 ;;
            *) echo "checkasm10 $isa: SKIP, the CPU lacks this tier's features" >> "$out" ;;
        esac
    done
    grep -E '^#|checkasm:|FAIL|SKIP' "$out"
    return "$rc"
}

st_identity10() {
    [ -x "$Y264_10" ] || { c_skip "no 10-bit build"; return; }
    [ -n "$(clips10)" ] || { c_skip "no clips in $CLIPS10_DIR"; return; }
    local out="$RES/identity10.txt" clip t mode tier rc=0 have
    echo "# 10-bit identity, $SKU $DATE, $FRAMES10 frames" > "$out"
    have=$(tiers10 2>> "$out") || { cat "$out"; return 1; }
    for tier in none $have; do
        : > "$RES/.md5.$tier"
        for clip in $(clips10); do
            for t in 1 "$TN"; do
                for mode in "--crf 23" "--preset slow --crf 30"; do
                    # a stale or empty output would hash the same on every tier
                    rm -f "$RES/.x.264"
                    # shellcheck disable=SC2086
                    if ! Y264_SIMD_FORCE="$tier" "$Y264_10" --input-y4m "$clip" \
                        --frames "$FRAMES10" $mode --threads "$t" -o "$RES/.x.264" \
                        >/dev/null 2>"$RES/.x.err"; then
                        echo "$tier $(basename "$clip") t$t $mode: ENCODE FAILED" >> "$out"
                        rc=1; continue
                    fi
                    if [ ! -s "$RES/.x.264" ]; then
                        echo "$tier $(basename "$clip") t$t $mode: EMPTY OUTPUT" >> "$out"
                        rc=1; continue
                    fi
                    printf '%s  %s t%s %s\n' "$(c_sha256 "$RES/.x.264" | cut -c1-32)" \
                        "$(basename "$clip")" "$t" "$mode" >> "$RES/.md5.$tier"
                done
            done
        done
    done
    # above one thread the output may vary run to run, so only t1 must match
    for tier in sse4 avx2; do
        case " $have " in *" $tier "*) ;;
            *) echo "$tier vs none: SKIP, the CPU lacks this tier's features" >> "$out"; continue ;;
        esac
        if diff -q <(grep ' t1 ' "$RES/.md5.none") <(grep ' t1 ' "$RES/.md5.$tier") >/dev/null; then
            echo "$tier vs none, threads 1: IDENTICAL" >> "$out"
        else
            echo "$tier vs none, threads 1: DIFFERS -- a kernel moved a byte" >> "$out"; rc=1
        fi
        diff -q <(grep -v ' t1 ' "$RES/.md5.none") <(grep -v ' t1 ' "$RES/.md5.$tier") >/dev/null \
            && echo "$tier vs none, threads $TN: identical (informational)" >> "$out" \
            || echo "$tier vs none, threads $TN: differs (informational)" >> "$out"
    done
    cp -f "$RES/.md5.none" "$RES/identity10-md5.txt"
    rm -f "$RES"/.md5.* "$RES/.x.264" "$RES/.x.err"
    cat "$out"
    return "$rc"
}

# perf10 <outfile-of-rows> <label> <cmd...>: one pinned encode, its instructions
# and task-clock (CPU seconds) as one row
perf10() {
    local rows="$1" label="$2" rep; shift 2
    rep=$(perf stat -x, -e instructions,task-clock -- "$@" 2>&1 >/dev/null) || {
        echo "$label ENCFAIL" >> "$rows"; return 1; }
    local ins
    ins=$(printf '%s\n' "$rep" | awk -F, '$3 ~ /^instructions/ && $1 ~ /^[0-9]/ {print $1; exit}')
    printf '%s %s %s\n' "$label" "${ins:-NA}" \
        "$(printf '%s\n' "$rep" | awk -F, '$3 ~ /^task-clock/ {printf "%.3f", $1 / 1000; exit}')" >> "$rows"
}

# One thread, pinned: yah264's 10-bit library at each tier and C, x264's 10-bit
# build with its assembly and as the fair C build; medium preset, CRF 23. Each
# arm runs REPS times interleaved and the table takes the median CPU time.
st_board10_t1() {
    [ -x "$Y264_10" ] || { c_skip "no 10-bit build"; return; }
    [ -x "${X264_10_ASM:-}" ] && [ -x "${X264_10_C:-}" ] || { c_skip "no 10-bit x264 (build-x264-10)"; return; }
    [ -n "$(clips10)" ] || { c_skip "no clips in $CLIPS10_DIR"; return; }
    command -v perf >/dev/null 2>&1 || { c_skip "no perf"; return; }
    # a VM without PMU passthrough still has task-clock: the instruction
    # column then reads NA and the CPU-seconds column stands alone
    c_can_time || { c_skip "an emulated count is the emulator's own"; return; }
    local reps="${REPS10:-3}" rows="$RES/.board10.rows" out="$RES/board10-t1.txt" clip n
    : > "$rows"
    for clip in $(clips10); do
        n=$(basename "$clip" .y4m)
        for _ in $(seq 1 "$reps"); do
            perf10 "$rows" "$n y264-avx2" "${PIN[@]+"${PIN[@]}"}" "$Y264_10" --input-y4m "$clip" \
                --frames "$FRAMES10" --crf 23 --threads 1 -o /dev/null
            perf10 "$rows" "$n y264-sse4" env Y264_SIMD_FORCE=sse4 "${PIN[@]+"${PIN[@]}"}" "$Y264_10" \
                --input-y4m "$clip" --frames "$FRAMES10" --crf 23 --threads 1 -o /dev/null
            perf10 "$rows" "$n y264-c" env YAH264_NO_ASM=1 "${PIN[@]+"${PIN[@]}"}" "$Y264_10" \
                --input-y4m "$clip" --frames "$FRAMES10" --crf 23 --threads 1 -o /dev/null
            perf10 "$rows" "$n x264-asm" "${PIN[@]+"${PIN[@]}"}" "$X264_10_ASM" --preset medium \
                --crf 23 --threads 1 --frames "$FRAMES10" -o /dev/null "$clip"
            # x264's own dispatch capped at AVX2, the like-for-like arm on an
            # AVX-512 part (on an AVX2-only part it equals x264-asm)
            perf10 "$rows" "$n x264-avx2" "${PIN[@]+"${PIN[@]}"}" "$X264_10_ASM" --asm avx2 \
                --preset medium --crf 23 --threads 1 --frames "$FRAMES10" -o /dev/null "$clip"
            perf10 "$rows" "$n x264-c" "${PIN[@]+"${PIN[@]}"}" "$X264_10_C" --preset medium \
                --crf 23 --threads 1 --frames "$FRAMES10" -o /dev/null "$clip"
        done
    done
    {
        echo "# 10-bit, threads 1, $PIN_DESC, $FRAMES10 frames, medium/CRF 23, median of $reps"
        echo "# $SKU $DATE  $CPU_MODEL"
        python3 - "$rows" <<'PY'
import sys, statistics as st
rows, bad = {}, []
for l in open(sys.argv[1]):
    f = l.split()
    if len(f) == 4:
        rows.setdefault((f[0], f[1]), []).append((int(f[2]) if f[2] != "NA" else 0, float(f[3])))
    else:
        bad.append(" ".join(f))
clips = sorted({c for c, _ in rows} | {b.split()[0] for b in bad if b})
arms = ("y264-avx2", "y264-sse4", "y264-c", "x264-asm", "x264-avx2", "x264-c")
print(f"{'clip':<26}{'arm':<11}{'instr':>10}{'cpu s':>9}")
for c in clips:
    m = {}
    for a in ("y264-avx2", "y264-sse4", "y264-c", "x264-asm", "x264-avx2", "x264-c"):
        v = rows.get((c, a))
        if not v:
            continue
        m[a] = (st.median(i for i, _ in v), st.median(t for _, t in v))
        ins = f"{m[a][0] / 1e9:>9.2f}G" if m[a][0] else f"{'NA':>10}"
        print(f"{c:<26}{a:<11}{ins}{m[a][1]:>9.2f}")
    for y, x in (("y264-avx2", "x264-asm"), ("y264-avx2", "x264-avx2"),
                 ("y264-sse4", "x264-asm"), ("y264-c", "x264-c")):
        if y in m and x in m:
            ir = f"{m[y][0] / m[x][0]:.3f}" if m[x][0] else "NA"
            print(f"{c:<26}{y + '/' + x:<22} instr {ir}  cpu {m[y][1] / m[x][1]:.3f}")
missing = [f"{c} {a}" for c in clips for a in arms if (c, a) not in rows]
for b in bad:
    print(f"ENCFAIL {b}")
for x in missing:
    print(f"MISSING {x}")
sys.exit(1 if bad or missing or not rows else 0)
PY
    } > "$out"
    local rc=$?
    cp -f "$rows" "$RES/board10-t1.rows"
    cat "$out"
    return "$rc"
}

# Full threads, wall time: yah264 as shipped against x264's 10-bit asm build,
# alternated, median of REPS10. Reported apart from the one-thread table.
st_board10_tN() {
    [ -x "$Y264_10" ] && [ -x "${X264_10_ASM:-}" ] || { c_skip "no 10-bit arms"; return; }
    [ -n "$(clips10)" ] || { c_skip "no clips in $CLIPS10_DIR"; return; }
    c_can_time || { c_skip "an emulated wall time is the emulator's"; return; }
    local reps="${REPS10:-3}" out="$RES/board10-tN.txt" clip n t0 t1 rc=0 fail
    echo "# 10-bit, default threads (tN = $TN vCPUs), $FRAMES10 frames, medium/CRF 23, wall s, median of $reps" > "$out"
    for clip in $(clips10); do
        n=$(basename "$clip" .y4m)
        : > "$RES/.wall"
        fail=0
        for _ in $(seq 1 "$reps"); do
            t0=$(date +%s.%N)
            "$Y264_10" --input-y4m "$clip" --frames "$FRAMES10" --crf 23 -o /dev/null >/dev/null 2>&1 || fail=1
            t1=$(date +%s.%N); echo "y $(awk "BEGIN{print $t1 - $t0}")" >> "$RES/.wall"
            t0=$(date +%s.%N)
            "$X264_10_ASM" --preset medium --crf 23 --frames "$FRAMES10" -o /dev/null "$clip" >/dev/null 2>&1 || fail=1
            t1=$(date +%s.%N); echo "x $(awk "BEGIN{print $t1 - $t0}")" >> "$RES/.wall"
        done
        if [ "$fail" = 1 ]; then
            echo "$n ENCFAIL" >> "$out"; rc=1; continue
        fi
        python3 - "$RES/.wall" "$n" >> "$out" <<'PY'
import sys, statistics as st
w = {"y": [], "x": []}
for l in open(sys.argv[1]):
    k, v = l.split(); w[k].append(float(v))
y, x = st.median(w["y"]), st.median(w["x"])
print(f"{sys.argv[2]:<26} yah264 {y:7.2f}  x264 {x:7.2f}  ratio {y / x:.3f}")
PY
    done
    rm -f "$RES/.wall"
    cat "$out"
    return "$rc"
}

# ---------------------------------------------------------------------------
# The AVX-512 decision leg: AVX-512 against AVX2 on ONE BINARY, through
# Y264_SIMD_FORCE, with YAH264_NO_ASM=1 as the control column.
#
# One binary matters. Two builds differ in more than the tier -- inlining,
# layout, which translation units exist -- and the question here is narrow:
# does the wide tier pay for its clock behaviour on THIS part. The plan asks it
# of both an Intel and an AMD part before the gated option moves, because that
# is the whole reason it is gated.

st_avx512_ab() {
    [ -x "$Y264" ] || { c_skip "no yah264 binary"; return; }
    c_has_avx512 "$CK" || { c_skip "no AVX-512 in this build/CPU ($Y264_CPU_NAME); the gated tier is not here"; return; }
    # shellcheck source=/dev/null
    . "$C_SRC/scripts/parity-clips.sh"
    local out="$RES/avx512-ab.txt" frames; frames="$(c_shape 120 12)"
    {
        echo "# AVX-512 vs AVX2 on one binary, $SKU $DATE"
        echo "# $CPU_MODEL   cpuid: $Y264_CPU_NAME"
        echo "# threads 1, $PIN_DESC, $frames frames, --preset medium --crf 23"
        c_can_time || echo "# REHEARSAL -- $C_NO_TIME_WHY  Columns are not speed."
        echo
        printf '%-22s %10s %10s %10s %9s\n' clip "no-asm s" "avx2 s" "avx512 s" "512/2"
    } > "$out"
    local spec clip src t_c t_2 t_5
    for spec in $CLIPS; do
        clip="${spec%%:*}"
        src="$C_SRC/tests/corpus/$clip.y4m"
        [ -f "$src" ] || continue
        t_c=$(ab_time YAH264_NO_ASM=1  "$src" "$frames") || return 1
        t_2=$(ab_time Y264_SIMD_FORCE=avx2   "$src" "$frames") || return 1
        t_5=$(ab_time Y264_SIMD_FORCE=avx512 "$src" "$frames") || return 1
        awk -v c="$clip" -v a="$t_c" -v b="$t_2" -v d="$t_5" \
            'BEGIN{printf "%-22s %10.3f %10.3f %10.3f %8.2fx\n", c, a, b, d, (d>0)? b/d : 0}' >> "$out"
    done
    c_can_time || c_rehearsal_note "plumbing only: avx512-ab.txt"
    cat "$out"
    return 0
}

# MEDIAN OF THREE, off a monotonic clock, not the shell's SECONDS -- that has
# one-second resolution and these cells are seconds long. Python takes the
# clock because it is the same clock perf-comp.sh reads, so the two tables'
# numbers are commensurable.
ab_time() {   # ab_time <ENV=VAL> <src> <frames> -> seconds
    python3 - "$1" "$Y264" "$2" "$3" "${PIN[*]-}" <<'PY'
import os, statistics, subprocess, sys, time
assign, y264, src, frames, pin = sys.argv[1:6]
env = dict(os.environ)
k, _, v = assign.partition("=")
env[k] = v
cmd = (pin.split() if pin else []) + [
    y264, "--input-y4m", src, "--frames", frames,
    "--preset", "medium", "--crf", "23", "--threads", "1", "-o", os.devnull]
ts = []
for _ in range(3):
    t0 = time.monotonic()
    r = subprocess.run(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    ts.append(time.monotonic() - t0)
    if r.returncode != 0:
        sys.exit(f"ab_time: {assign} encode failed: {r.stderr.decode(errors='replace')[:400]}")
print(f"{statistics.median(ts):.3f}")
PY
}

# ---------------------------------------------------------------------------
# The push. It happens after every leg, not only at the end, because the thing
# that loses a session is an instance terminated with the tarball still on its
# local disk. The budget guard is the other half of the same rule.

push_results() {   # push_results <why>
    local tar="$CLOUD_WORK/results/$SKU-$DATE.tar.gz"
    {
        echo "# yah264 cloud campaign -- $SKU $DATE ($1)"
        echo "# $CPU_MODEL"
        echo "# cpuid: $Y264_CPU_NAME"
        echo "# kernel: $KERNEL"
        echo "# yah264 $Y264_COMMIT   x264 $X264_COMMIT"
        echo "# threads: t1 $PIN_DESC, tN = $TN (SMT as shipped)"
        c_can_time || echo "# TIMING OFF: $C_NO_TIME_WHY"
        echo
        mhz_drift
    } > "$RES/PROVENANCE.txt"
    cp -f "$C_OK" "$RES/bootstrap.ok" 2>/dev/null
    tar -czf "$tar" -C "$CLOUD_WORK/results" "$SKU/$DATE" || return 1
    echo "tarball: $tar ($(du -h "$tar" 2>/dev/null | cut -f1))"
    if [ "$(c_bucket_kind)" = none ]; then
        echo "push: BUCKET_URL is not set; the tarball stays on local disk."
        echo "      On a rented box that means it is lost when the instance goes."
        return 0
    fi
    if c_bucket_put "$tar" "$(basename "$tar")"; then
        echo "push: uploaded $(basename "$tar")"
    else
        echo "push: upload FAILED -- do not terminate the instance"
        return 1
    fi
    return 0
}

# ---------------------------------------------------------------------------

mhz_start
trap 'mhz_stop' EXIT INT TERM

run() {   # run <leg-name> <fn> <estimate-seconds>
    has_leg "$1" || { c_row "$1" SKIP - "not in LEGS"; return 0; }
    c_run_stage "$@" || true
    # A checkpoint push after each leg: what has been measured survives the
    # next leg's failure, and WALL_BUDGET_H's whole promise is that partial
    # results leave the box.
    if [ "${C_BUDGET_HIT:-0}" = 1 ]; then
        echo "campaign: wall budget reached -- pushing what exists and stopping."
        push_results "PARTIAL: wall budget"
        return 1
    fi
    return 0
}

while :; do
    run identity       st_identity        600 || break
    run board-t1-asm   st_board_t1_asm   2400 || break
    run board-t1-purec st_board_t1_purec 3000 || break
    run board-tN-asm   st_board_tN_asm   1200 || break
    run board-tN-purec st_board_tN_purec 1500 || break
    run ffboard        st_ffboard        2400 || break
    run bd-at-rate     st_bd_at_rate     3600 || break
    run bench-sse4     st_bench_sse4      600 || break
    run bench-avx2     st_bench_avx2      600 || break
    run bench-avx512   st_bench_avx512    600 || break
    run instr-ratio    st_instr_ratio     900 || break
    run avx512-ab      st_avx512_ab      1800 || break
    run checkasm10     st_checkasm10      300 || break
    run identity10     st_identity10      900 || break
    run board10-t1     st_board10_t1     3600 || break
    run board10-tN     st_board10_tN     1200 || break
    break
done

mhz_stop
echo
mhz_drift | tee -a "$RES/mhz-drift.txt"
echo
c_summary "yah264 cloud campaign -- $SKU $DATE" "$RES/campaign-report.txt"
echo
push_results "complete"

echo
if [ "$C_FAIL" = 0 ]; then
    echo "campaign: OK ($C_PASS passed, $C_SKIP skipped, $C_REHEARSED rehearsed)"
    exit 0
fi
echo "campaign: $C_FAIL leg(s) failed; the tarball is pushed either way."
exit 1
