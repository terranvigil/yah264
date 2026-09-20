#!/usr/bin/env bash
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# x86-docker.sh -- the whole correctness battery as linux/amd64, on a machine
# that has no x86 in it (docs/x86-plan.md, W0d).
#
# THE POINT. Every x86 claim this project makes before it rents real silicon is
# made from inside a container here, so the thing that matters is knowing
# exactly what an emulated pass is worth. Two emulators, because they fail
# differently: Rosetta translates ahead of time and is near native, and has a
# history of illegal-instruction bugs on the wide paths; QEMU's TCG interprets,
# is several times slower, and is the independent second opinion. NEITHER has
# AVX-512, so this script ASSERTS its absence and prints the assertion: an
# emulated green must never be read as AVX-512 coverage, and the only way to be
# sure of that is to say out loud, in the report, that the tier was not there.
#
# THE MATRIX: {rosetta, qemu} x {none, sse4, avx2} x {make test incl. checkasm,
# conformance --fast, identity cmp against the `none` tier}, plus a 10-bit
# build (C only -- the x86 tier libraries fold into the 8-bit library alone).
# The tier axis is Y264_SIMD_FORCE, a RUNTIME cap, so one build serves all
# three; -Dsimd= is the build-time twin and is what CI exercises instead.
#
# THE NEGATIVE CONTROL. On a tree with no x86 kernels the sse4 and avx2 tiers
# must be byte-identical to none, because there is nothing in the binary for
# the cap to cap. A difference there is a harness bug and not an encoder
# finding, and that is the whole reason to run this matrix BEFORE the kernels
# land rather than with them.
#
# BOX RULE (docs/x86-plan.md, "Box rule for every Docker run"). An emulated run
# is CPU load like an encode. Coordinate it the way a timed leg is coordinated,
# keep jobs x encoder threads inside the cap, and prefer Rosetta for the
# routine matrix with QEMU reserved for an item's final gate. This script reads
# NO clocks and prints no speed: under emulation a wall time means nothing.
#
# Usage:
#   scripts/x86-docker.sh                       # rosetta, all three tiers
#   scripts/x86-docker.sh --backend qemu        # the final gate
#   scripts/x86-docker.sh --backend both
#   scripts/x86-docker.sh --build-image         # (re)build the image first
#   scripts/x86-docker.sh --backend qemu --legs "identity conformance"
#
# Knobs (environment):
#   X86_IMAGE      image tag                              (yah264-x86:24.04)
#   X86_CPUS       --cpus given to every container        (6)
#   X86_JOBS       ninja -j inside the container          (4)
#   X86_THREADS    encoder thread counts for the identity cmp  ("1 4")
#   X86_FRAMES     frames per identity encode, 0 = whole clip  (24)
#   X86_CLIPS      clip:kbps list                         (parity-clips.sh CLIPS)
#   QEMU_CPU       the CPU QEMU models                    (max)
set -uo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

IMAGE="${X86_IMAGE:-yah264-x86:24.04}"
CPUS="${X86_CPUS:-6}"
JOBS="${X86_JOBS:-4}"
ENC_THREADS="${X86_THREADS:-1 4}"
FRAMES="${X86_FRAMES:-24}"
QEMU_CPU="${QEMU_CPU:-max}"
BACKENDS="rosetta"
TIERS="none sse4 avx2"
LEGS="test conformance identity"
BUILD_IMAGE=0

while [ $# -gt 0 ]; do
    case "$1" in
        --backend)  BACKENDS="$2"; shift 2 ;;
        --backends) BACKENDS="$2"; shift 2 ;;
        --tiers)    TIERS="$2"; shift 2 ;;
        --legs)     LEGS="$2"; shift 2 ;;
        --build-image) BUILD_IMAGE=1; shift ;;
        -h|--help) sed -n '38,48p' "$0"; exit 0 ;;
        *) echo "x86-docker: unknown argument '$1'" >&2; exit 2 ;;
    esac
done
[ "$BACKENDS" = both ] && BACKENDS="rosetta qemu"

# shellcheck source=/dev/null
. "$root/scripts/parity-clips.sh"
CLIPS="${X86_CLIPS:-$CLIPS}"

out="$root/scratch/x86-docker"
mkdir -p "$out"
report="$out/report.txt"
: > "$report"

# ---------------------------------------------------------------------------
# Result bookkeeping. Every leg lands in the same table whatever it did, so a
# skipped leg is visible as a skip rather than as an absence.
pass=0; fail=0; skip=0
declare -a ROWS
row() {   # row <backend> <tier> <leg> <PASS|FAIL|SKIP|NOTE> <detail>
    ROWS+=("$(printf '%-8s %-6s %-22s %-5s %s' "$1" "$2" "$3" "$4" "${5:-}")")
    case "$4" in
        PASS) pass=$((pass+1)) ;;
        FAIL) fail=$((fail+1)) ;;
        SKIP) skip=$((skip+1)) ;;
    esac
    printf '  %-8s %-6s %-22s %-5s %s\n' "$1" "$2" "$3" "$4" "${5:-}"
}
has_leg() { case " $LEGS " in *" $1 "*) return 0 ;; *) return 1 ;; esac; }

# ---------------------------------------------------------------------------
# The mounts. The worktree goes in read-write at /work; the corpus goes in
# READ-ONLY and is never copied, because it is ~25 GB of Y4M that is not ours
# to redistribute and a layer holding it would be both slow and wrong.
#
# tests/corpus is a farm of symlinks, and in a worktree those symlinks point
# through ANOTHER checkout before they reach the files. An absolute symlink
# only resolves inside a container if its target path exists there too, so
# rather than hardcode anything (which scripts/hygiene_check.sh forbids, and
# rightly) this walks each clip's symlink chain and mounts every directory it
# passes through at its own path.
chain_dirs() {
    local p="$1" d t
    # pwd -P, because tests/corpus itself may be the symlink (the main
    # checkout points it at a sibling tree): a logical pwd reports that
    # directory as inside /work, nothing is mounted, and every clip reads
    # MISSING inside the container.
    while [ -L "$p" ]; do
        d="$(cd "$(dirname "$p")" && pwd -P)"
        echo "$d"
        t="$(readlink "$p")"
        case "$t" in /*) p="$t" ;; *) p="$d/$t" ;; esac
    done
    [ -e "$p" ] && echo "$(cd "$(dirname "$p")" && pwd -P)"
}
mounts=()
corpus_dirs="$(for f in "$root"/tests/corpus/*.y4m; do chain_dirs "$f"; done | sort -u)"
for d in $corpus_dirs; do
    case "$d" in "$root"/*) continue ;; esac   # already inside /work
    mounts+=(-v "$d:$d:ro")
done
# local/ is the same kind of symlink and is NOT read-only: regress.py, which
# `make test` runs, keeps its work and its per-revision results under
# local/regress. Without this the unit leg dies on a missing directory and
# reads as an x86 failure, which it is not.
# It is mounted ON TOP of /work/local rather than at its host path, because
# that symlink is RELATIVE and a relative symlink resolves against the
# container's /work, not the host's worktree parent.
if [ -L "$root/local" ]; then
    ld="$(cd "$root/local" && pwd)"
    mounts+=(-v "$ld:/work/local")
fi
# The container gets its OWN conformance fixture cache, mounted over the
# tree's. conformance.sh keys that cache by the ffmpeg that built it as well as
# by FIXVER now, so the two runs no longer collide even without this mount. The
# two ffmpegs do not agree about what a Y4M header says: Ubuntu's writes
# XCOLORRANGE=LIMITED on the 10-bit and 4:2:2 clips where Homebrew's writes
# nothing, that tag reaches the VUI, and the bitstream moves. Shared, a Docker
# run changed the result of the next NATIVE run in the same tree, and a round
# went to a difference nobody introduced. It cost this item one. The mount
# stays because it is also what keeps a container's writes out of the worktree.
mkdir -p "$root/scratch/x86-docker/fixtures"
mounts+=(-v "$root/scratch/x86-docker/fixtures:/work/tests/.fixtures")

# ---------------------------------------------------------------------------
# The two backends. Docker Desktop registers BOTH interpreters for the x86-64
# ELF magic -- Rosetta and qemu-x86_64 -- and the kernel hands a binary to the
# first enabled match, so selecting QEMU means disabling the Rosetta entry for
# the duration and putting it back afterwards. That is a change to the shared
# Docker VM, so it is announced, trapped, and restored even on a failure.
binfmt() {   # binfmt <0|1>: 0 disables the Rosetta entries, 1 re-enables them
    docker run --rm --privileged --platform linux/arm64 alpine sh -c "
        mount -t binfmt_misc none /proc/sys/fs/binfmt_misc 2>/dev/null
        for e in rosetta rosetta-wrapper; do
            [ -f /proc/sys/fs/binfmt_misc/\$e ] && echo $1 > /proc/sys/fs/binfmt_misc/\$e
        done
        head -1 /proc/sys/fs/binfmt_misc/rosetta 2>/dev/null" >/dev/null 2>&1
}
rosetta_enabled() {
    docker run --rm --privileged --platform linux/arm64 alpine sh -c "
        mount -t binfmt_misc none /proc/sys/fs/binfmt_misc 2>/dev/null
        head -1 /proc/sys/fs/binfmt_misc/rosetta 2>/dev/null" 2>/dev/null \
      | grep -q enabled
}
restore_rosetta() { binfmt 1; }

# dk <env-assignments...> -- <shell command>
# One container per call: the run is short-lived and stateless, which is what
# makes a SIGILL attributable to the case that provoked it.
dk() {
    local envs=() a
    while [ "$1" != "--" ]; do envs+=(-e "$1"); shift; done
    shift
    a=(docker run --rm --platform linux/amd64 --cpus "$CPUS"
       -u "$(id -u):$(id -g)" -w /work -v "$root:/work" ${mounts[@]+"${mounts[@]}"}
       -e "HOME=/tmp" -e "XDG_CACHE_HOME=/tmp/.cache" "${envs[@]}")
    [ "$BACKEND" = qemu ] && a+=(-e "QEMU_CPU=$QEMU_CPU")
    "${a[@]}" "$IMAGE" bash -c "$*"
}

# A Rosetta case that dies of SIGILL (128+4) is Rosetta's defect and not the
# encoder's, so it is rerun under QEMU and the Rosetta row is marked
# "emulator": the case still has to pass, it just does not get to pass here.
SIGILL=132
run_case() {   # run_case <backend> <tier> <leg> <log> <env...> -- <cmd>
    local be="$1" tier="$2" leg="$3" log="$4"; shift 4
    dk "$@" > "$log" 2>&1
    local rc=$?
    if [ "$rc" = 0 ]; then row "$be" "$tier" "$leg" PASS "$(basename "$log")"; return 0; fi
    if [ "$be" = rosetta ] && [ "$rc" = $SIGILL ]; then
        row "$be" "$tier" "$leg" NOTE "SIGILL under Rosetta -- rerunning under QEMU"
        local keep="$BACKEND"; BACKEND=qemu
        binfmt 0
        dk "$@" > "$log.qemu" 2>&1; rc=$?
        binfmt 1
        BACKEND="$keep"
        [ "$rc" = 0 ] && { row "$be" "$tier" "$leg" PASS "emulator (QEMU; Rosetta SIGILL)"; return 0; }
        row "$be" "$tier" "$leg" FAIL "rc=$rc under QEMU too, $(basename "$log").qemu"
        return 1
    fi
    row "$be" "$tier" "$leg" FAIL "rc=$rc, $(basename "$log")"
    return 1
}

# ---------------------------------------------------------------------------
echo "x86-docker: image $IMAGE, backends [$BACKENDS], tiers [$TIERS], legs [$LEGS]"
echo "x86-docker: --cpus $CPUS, ninja -j$JOBS, encoder threads [$ENC_THREADS], frames $FRAMES"
echo "x86-docker: no clocks are read; an emulated run has no speed to report."

build_image() {
    echo "x86-docker: building $IMAGE"
    docker build --platform linux/amd64 -t "$IMAGE" \
        -f docker/x86/Dockerfile docker/x86 > "$out/image-build.log" 2>&1 \
      || { echo "x86-docker: image build FAILED, see scratch/x86-docker/image-build.log"; exit 1; }
}
[ "$BUILD_IMAGE" = 1 ] && build_image

# WAIT for the daemon before believing it. A Docker Desktop that has just been
# restarted answers `docker image inspect` with "no such image" for a minute or
# so before its image store is up, and taking that at face value turned a whole
# matrix into a one-line refusal twice in this item's first day. Ask a few
# times, and only build when the answer has been the same for a while.
img_ready() { docker image inspect "$IMAGE" >/dev/null 2>&1; }
if ! img_ready; then
    echo "x86-docker: $IMAGE not visible yet; waiting for the daemon"
    for _ in 1 2 3 4 5 6 7 8 9 10 11 12; do
        sleep 5
        img_ready && break
    done
fi
img_ready || { echo "x86-docker: $IMAGE really is absent; building it"; build_image; }
img_ready || { echo "x86-docker: no image $IMAGE and it will not build"; exit 1; }

for BACKEND in $BACKENDS; do
    echo
    echo "=== backend: $BACKEND ============================================"
    if [ "$BACKEND" = qemu ]; then
        if rosetta_enabled; then
            echo "x86-docker: Rosetta is the default interpreter for x86-64; disabling it"
            echo "            for this leg and restoring it on the way out."
            binfmt 0
            trap restore_rosetta EXIT INT TERM
        fi
        if rosetta_enabled; then
            row qemu - "backend select" FAIL "could not disable the Rosetta binfmt entry"
            continue
        fi
    else
        if ! rosetta_enabled; then
            echo "x86-docker: Rosetta is NOT registered for x86-64 in this Docker VM."
            echo "            Turn on Docker Desktop > Settings > General >"
            echo "            'Use Rosetta for x86_64/amd64 emulation on Apple Silicon',"
            echo "            or run this with --backend qemu. Not flipping it from here."
            row rosetta - "backend select" FAIL "Rosetta not registered"
            continue
        fi
    fi

    # -- step one, always: what does the emulated machine say it is ----------
    flags="$(dk X=1 -- "grep -o 'sse4_2\|avx2\|avx512[a-z]*\|fma\|bmi2' /proc/cpuinfo | sort -u | tr '\n' ' '" 2>/dev/null)"
    arch="$(dk X=1 -- "uname -m" 2>/dev/null)"
    echo "x86-docker: uname -m = $arch"
    if [ "$BACKEND" = rosetta ]; then
        echo "x86-docker: /proc/cpuinfo: ${flags:-<none>}"
        case " $flags " in
            *" avx2 "*) : ;;
            *) row rosetta - "cpuinfo avx2" FAIL "no avx2 in /proc/cpuinfo under Rosetta" ;;
        esac
    else
        # qemu-user passes the HOST's /proc/cpuinfo straight through, so under
        # QEMU that file describes an Apple core and says nothing about what
        # the guest can execute. The only honest probe is the binary's own
        # CPUID, which is what the checkasm banner prints below.
        echo "x86-docker: /proc/cpuinfo is the host's under qemu-user (QEMU_CPU=$QEMU_CPU);"
        echo "            the feature probe that counts is the encoder's own CPUID, below."
    fi

    bdir="scratch/x86-docker/$BACKEND"
    # THE QEMU LEG REUSES THE ROSETTA BUILD, and that is sound rather than a
    # corner cut: both backends run the same Ubuntu gcc over the same sources
    # with the same flags, so the ELF is the same ELF, and what the two
    # emulators differ in is EXECUTING it. Compiling again under TCG would cost
    # an hour to produce the same object files. X86_QEMU_BUILD=1 forces the
    # separate build when the compiler itself is the thing in question.
    if [ "$BACKEND" = qemu ] && [ "${X86_QEMU_BUILD:-0}" != 1 ] \
       && [ -f "$root/scratch/x86-docker/rosetta/build8/build.ninja" ]; then
        bdir="scratch/x86-docker/rosetta"
        row qemu - "build" NOTE "reusing the Rosetta build; same compiler, same ELF"
    fi
    b8="$bdir/build8"; b10="$bdir/build10"

    # -- the build ----------------------------------------------------------
    if dk X=1 -- "set -e
        [ -f $b8/build.ninja ] || meson setup $b8 -Dsimd=auto
        ninja -C $b8 -j$JOBS" > "$out/$BACKEND-build8.log" 2>&1; then
        row "$BACKEND" - "build -Dsimd=auto" PASS "$BACKEND-build8.log"
    else
        row "$BACKEND" - "build -Dsimd=auto" FAIL "$BACKEND-build8.log"
        continue
    fi

    # The binary's own CPUID, printed by checkasm, and the AVX-512 assertion
    # that keeps an emulated pass from ever being read as AVX-512 coverage.
    cpuline="$(dk X=1 -- "$b8/tools/checkasm/checkasm --list 2>/dev/null | head -1")"
    echo "x86-docker: $cpuline"
    case "$cpuline" in
        *avx512*) row "$BACKEND" - "avx512 absent" FAIL "the emulator reports AVX-512: $cpuline" ;;
        *) row "$BACKEND" - "avx512 absent" PASS "asserted absent; no emulated AVX-512 coverage" ;;
    esac
    case "$cpuline" in
        *avx2*) row "$BACKEND" - "cpuid avx2 + fma3" PASS "${cpuline#checkasm: cpu }" ;;
        *) row "$BACKEND" - "cpuid avx2 + fma3" FAIL "the binary does not see avx2: $cpuline" ;;
    esac

    # -- the 10-bit build, C only ------------------------------------------
    # The tier libraries are folded into the 8-bit library alone, so a 10-bit
    # build is the pure-C arm of the x86 shape and the place a depth-generic
    # mistake in the build files shows up.
    if has_leg test; then
        if dk X=1 -- "set -e
            [ -f $b10/build.ninja ] || meson setup $b10 -Dbit_depth=10
            ninja -C $b10 -j$JOBS
            meson test -C $b10 --print-errorlogs" > "$out/$BACKEND-build10.log" 2>&1; then
            row "$BACKEND" 10bit "build + unit (C only)" PASS "$BACKEND-build10.log"
        else
            row "$BACKEND" 10bit "build + unit (C only)" FAIL "$BACKEND-build10.log"
        fi
    fi

    # -- the tier matrix ----------------------------------------------------
    for tier in $TIERS; do
        if has_leg test; then
            # YAH264= as well as BUILD=: `make test` hands regress.py no path
            # and regress.py defaults to build/cli/yah264, which in here is a
            # macOS build directory or nothing at all.
            run_case "$BACKEND" "$tier" "make test" "$out/$BACKEND-$tier-test.log" \
                "Y264_SIMD_FORCE=$tier" "YAH264=/work/$b8/cli/yah264" \
                -- "make test BUILD=$b8"
            # checkasm again, explicitly at this tier's rows. `none` has no
            # tier to name, so it runs the whole table with every kernel row
            # gated off by its own cap -- which is the C-only claim.
            isa_arg="--isa $tier"; [ "$tier" = none ] && isa_arg=""
            run_case "$BACKEND" "$tier" "checkasm $isa_arg" "$out/$BACKEND-$tier-checkasm.log" \
                "Y264_SIMD_FORCE=$tier" -- "$b8/tools/checkasm/checkasm $isa_arg"
        fi
        if has_leg conformance; then
            # YAH264_CONF_JOBS is pinned to the container's CPU allowance,
            # because conformance.sh sizes its job pool from nproc and nproc
            # reports the VM's core count, not the cgroup quota: left alone it
            # runs -P 18 inside --cpus 6. The quota bounds what the box
            # actually loses either way, but 18 jobs contending for 6 cores is
            # slower than 6, and the number in the log should be the number of
            # cores the run was allowed.
            run_case "$BACKEND" "$tier" "conformance --fast" "$out/$BACKEND-$tier-conf.log" \
                "Y264_SIMD_FORCE=$tier" "YAH264_CONF_FAST=1" "YAH264_CONF_JOBS=$CPUS" \
                -- "scripts/conformance.sh --fast $b8/cli/yah264"
        fi
    done

    # -- the identity cmp ---------------------------------------------------
    # Every tier encodes the ten board clips at CRF, CQP and ABR, at each
    # thread count, and the md5 of the .264 is compared against the `none`
    # tier's. No kernel moves a byte, so on this tree -- which has no x86
    # kernels at all -- a single differing md5 is a defect in this harness.
    if has_leg identity; then
        # The md5 lists are keyed by BACKEND, not by build directory: the QEMU
        # leg may be running out of the Rosetta build, and one leg overwriting
        # the other's lists would compare two backends by accident.
        idir="scratch/x86-docker/ident-$BACKEND"
        script="set -e
mkdir -p $idir
: > $idir/md5.\$Y264_SIMD_FORCE
for spec in $CLIPS; do
    clip=\${spec%%:*}; rate=\${spec##*:}
    src=tests/corpus/\$clip.y4m
    [ -f \"\$src\" ] || { echo \"MISSING \$src\"; continue; }
    for t in $ENC_THREADS; do
        for mode in '--crf 23' '--qp 26' \"--bitrate \$rate\"; do
            o=$idir/x.264
            $b8/cli/yah264 --input-y4m \"\$src\" --frames $FRAMES \$mode \\
                --threads \$t -o \"\$o\" > /dev/null 2>&1
            echo \"\$(md5sum < \"\$o\" | cut -d' ' -f1)  \$clip t\$t \$mode\" \\
                >> $idir/md5.\$Y264_SIMD_FORCE
        done
    done
done"
        mkdir -p "$root/$idir"
        for tier in $TIERS; do
            if dk "Y264_SIMD_FORCE=$tier" -- "$script" > "$out/$BACKEND-$tier-ident.log" 2>&1; then
                row "$BACKEND" "$tier" "identity encodes" PASS "$BACKEND-$tier-ident.log"
            else
                row "$BACKEND" "$tier" "identity encodes" FAIL "$BACKEND-$tier-ident.log"
            fi
        done
        base="$root/$idir/md5.none"
        for tier in $TIERS; do
            [ "$tier" = none ] && continue
            f="$root/$idir/md5.$tier"
            if [ ! -s "$base" ] || [ ! -s "$f" ]; then
                row "$BACKEND" "$tier" "identity vs none" FAIL "an md5 list is missing or empty"
            elif diff -q "$base" "$f" >/dev/null; then
                row "$BACKEND" "$tier" "identity vs none" PASS "$(wc -l < "$f" | tr -d ' ') encodes byte-identical"
            else
                diff "$base" "$f" > "$out/$BACKEND-$tier-ident.diff"
                row "$BACKEND" "$tier" "identity vs none" FAIL \
                    "$(diff "$base" "$f" | grep -c '^<') differ -- HARNESS BUG, this tree has no x86 kernels"
            fi
        done
    fi

    if [ "$BACKEND" = qemu ]; then restore_rosetta; trap - EXIT INT TERM; fi
done

# ---------------------------------------------------------------------------
{
    echo "x86-docker report"
    echo "image      $IMAGE"
    echo "backends   $BACKENDS"
    echo "tiers      $TIERS"
    echo "legs       $LEGS"
    echo "clips      $CLIPS"
    echo "frames     $FRAMES   threads  $ENC_THREADS"
    echo
    printf '%s\n' ${ROWS[@]+"${ROWS[@]}"}
    echo
    echo "pass $pass   fail $fail   skip $skip"
    echo
    echo "AVX-512 is absent from both emulators by construction. Nothing above"
    echo "is AVX-512 coverage, whatever it says; that tier is first executed on"
    echo "real silicon (docs/x86-plan.md, session A)."
} | tee "$report"

echo
if [ "$fail" = 0 ]; then
    echo "x86-docker: PASS ($pass legs), report in scratch/x86-docker/report.txt"
    exit 0
fi
echo "x86-docker: FAIL ($fail of $((pass+fail)) legs), report in scratch/x86-docker/report.txt"
exit 1
