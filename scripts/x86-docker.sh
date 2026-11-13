#!/usr/bin/env bash
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# x86-docker.sh -- the whole correctness battery as linux/amd64, on a machine
# that has no x86 in it (docs/simd.md).
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
# build whose unit leg runs checkasm over the 10-bit library's own tiers.
# The tier axis is Y264_SIMD_FORCE, a RUNTIME cap, so one build serves all
# three; -Dsimd= is the build-time twin and is what CI exercises instead.
#
# THE IDENTITY AXIS. Every kernel is bit-exact with C, so at threads 1 the
# sse4 and avx2 tiers must be byte-identical to none, at 8 bits on the board
# clips and at 10 bits on 4:2:0, 4:2:2 and 4:4:4 clips made from the corpus;
# a difference is a kernel defect.
#
# BOX RULE (docs/simd.md). An emulated run
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
#   X86_THREADS    encoder thread counts for the identity cmp  ("1")
#   X86_FRAMES     frames per identity encode, 0 = whole clip  (24)
#   X86_CLIPS      clip:kbps list                         (parity-clips.sh CLIPS)
#   QEMU_CPU       the CPU QEMU models; empty picks one per cap that lacks
#                  the next tier up (qemu64, Nehalem, Haswell-noTSX), max uncapped
set -uo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

IMAGE="${X86_IMAGE:-yah264-x86:24.04}"
CPUS="${X86_CPUS:-6}"
JOBS="${X86_JOBS:-4}"
# threads 1 only by default: output above one thread may vary run to run
# (owner, 2026-09-25), and at threads 1 a tier difference can only be a kernel.
ENC_THREADS="${X86_THREADS:-1}"
FRAMES="${X86_FRAMES:-24}"
QEMU_CPU="${QEMU_CPU:-}"
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

# the identity legs compare every tier with none, so they need it
if has_leg identity && ! printf ' %s ' "$TIERS" | grep -q ' none '; then
    echo "x86-docker: --tiers '$TIERS' has no none tier for the identity legs to compare against" >&2
    exit 2
fi

# ONE RUN PER WORKTREE. Two runs share the scratch directory: each other's
# x.264, err, .part and md5 lists, so one run's row would blame a kernel for
# the other's files. The lock is a kernel lock (flock) on a file this shell
# holds open, so it is freed by any death, kill -9 included, and there is no
# pid, clock or leftover to judge. bash has no flock of its own and macOS no
# flock(1), so perl takes it on the shell's descriptor. Containers are started
# with that descriptor closed and labelled with this tree, and a run refuses
# to start while one from a killed run is still writing to the scratch.
command -v perl >/dev/null 2>&1 || { echo "x86-docker: perl missing; it takes the run lock" >&2; exit 2; }
exec 9>>"$out/.lock.f"
perl -e 'eval { require Fcntl; 1 } or exit 4;
         open(my $fh, ">&=", 9) or exit 3;
         exit 0 if flock($fh, Fcntl::LOCK_EX() | Fcntl::LOCK_NB());
         exit 1 if $!{EWOULDBLOCK};
         print STDERR "$!\n"; exit 5' 2> "$out/.lock.err"
case $? in
    0) ;;
    1) echo "x86-docker: another run (pid $(cat "$out/.lock.pid" 2>/dev/null || echo '?')) holds $out/.lock.f" >&2
       exit 2 ;;
    4) echo "x86-docker: this perl has no Fcntl module; it takes the run lock" >&2; exit 2 ;;
    *) echo "x86-docker: flock on $out/.lock.f failed: $(cat "$out/.lock.err") (does this filesystem support it?)" >&2
       exit 2 ;;
esac
echo $$ > "$out/.lock.pid"
# the report is this run's from here on; a refused run never touches it
: > "$report"
trap 'exit 130' INT
trap 'exit 143' TERM

# compare_lists <backend> <tier> <label> <idir> <diff-name> <what moved>:
# one tier's md5 list against none's. A count that differs is the harness's
# fault (an encode went missing), never a kernel's.
compare_lists() {
    local be="$1" tier="$2" label="$3" base="$root/$4/md5.none" f="$root/$4/md5.$2" nb nf
    if [ ! -s "$base" ] || [ ! -s "$f" ]; then
        row "$be" "$tier" "$label" FAIL "an md5 list is missing or empty"
        return
    fi
    nb=$(wc -l < "$base" | tr -d ' '); nf=$(wc -l < "$f" | tr -d ' ')
    if [ "$nb" != "$nf" ]; then
        row "$be" "$tier" "$label" FAIL "harness fault: $nf encodes against none's $nb"
    elif diff -q "$base" "$f" >/dev/null; then
        row "$be" "$tier" "$label" PASS "$nf encodes byte-identical"
    else
        diff "$base" "$f" > "$out/$5"
        row "$be" "$tier" "$label" FAIL \
            "$(diff "$base" "$f" | grep -c '^[<>]') lines differ -- $6"
    fi
}

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
# tests/corpus itself may be a symlink chain (main checkout -> sibling tree
# -> its own corpus). Mounting only the final physical directory at its host
# path leaves the first hop dangling inside the container, so the physical
# directory is mounted ON TOP of /work/tests/corpus, as local/ is below.
if [ -L "$root/tests/corpus" ]; then
    cd_="$(cd "$root/tests/corpus" && pwd -P)"
    mounts+=(-v "$cd_:/work/tests/corpus:ro")
fi
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
# The two backends. Docker Desktop hands an x86-64 binary to whichever
# interpreter its VM has registered first, normally Rosetta, and that
# registration is global to the VM, which other sessions share. So this script
# never changes it. The rosetta backend runs everything the default way. The
# qemu backend runs the same containers, but every binary under test goes
# through the image's own qemu-x86_64, named on the command line: QR below is
# that wrapper, empty on the rosetta backend. Each capped run gets a CPU model
# without the tier above its cap, so code above the cap faults whatever path
# reached it. The encoder the Python tests start goes through it too
# (YAH264_WRAP); the shells, Python, meson and ffmpeg around them do not.
rosetta_enabled() {   # read-only: is Rosetta the VM's x86-64 interpreter
    docker run --rm --privileged --platform linux/arm64 alpine sh -c "
        mount -t binfmt_misc none /proc/sys/fs/binfmt_misc 2>/dev/null
        head -1 /proc/sys/fs/binfmt_misc/rosetta 2>/dev/null" 2>/dev/null 9>&- \
      | grep -q enabled
}
QRUN=""
qrun_path="/work/scratch/x86-docker/qrun"
cat > "$out/qrun" <<'QEOF'
#!/bin/sh
# an explicit QEMU, so a "qemu" row cannot have run under anything else. a
# capped run gets a model without the next tier up; QEMU_CPU overrides
m="${QEMU_CPU:-}"
if [ -z "$m" ]; then
    case "${Y264_SIMD_FORCE:-}" in
        none) m=qemu64 ;; sse4) m=Nehalem ;; avx2) m=Haswell-noTSX ;; *) m=max ;;
    esac
fi
exec /opt/qemu/qemu-x86_64 -cpu "$m" "$@"
QEOF
chmod +x "$out/qrun"

# cpu_for <tier>: the QEMU model a leg capped at <tier> runs on, one without
# the next tier up. It goes in as QEMU_CPU, not only through the cap, because
# the tests that start the encoder themselves drop the Y264_* variables
cpu_for() {
    if [ -n "$QEMU_CPU" ]; then echo "$QEMU_CPU"; return; fi
    case "$1" in
        none) echo qemu64 ;; sse4) echo Nehalem ;; avx2) echo Haswell-noTSX ;; *) echo max ;;
    esac
}

# dk <env-assignments...> -- <shell command>
# One container per call: the run is short-lived and stateless, which is what
# makes a SIGILL attributable to the case that provoked it.
dk() {
    local envs=() a
    while [ "$1" != "--" ]; do envs+=(-e "$1"); shift; done
    shift
    a=(docker run --rm --platform linux/amd64 --cpus "$CPUS" --label "yah264-x86-docker=$root"
       -u "$(id -u):$(id -g)" -w /work -v "$root:/work" ${mounts[@]+"${mounts[@]}"}
       -e "HOME=/tmp" -e "XDG_CACHE_HOME=/tmp/.cache"
       -e "QEMU_CPU=$QEMU_CPU" -e "QR=$QRUN" "${envs[@]}")
    "${a[@]}" "$IMAGE" bash -c "$*" 9>&-
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
        local keep="$QRUN"; QRUN="$qrun_path"
        dk "$@" > "$log.qemu" 2>&1; rc=$?
        QRUN="$keep"
        [ "$rc" = 0 ] && { row "$be" "$tier" "$leg" PASS "emulator (QEMU; Rosetta SIGILL)"; return 0; }
        row "$be" "$tier" "$leg" FAIL "rc=$rc under QEMU too, $(basename "$log").qemu"
        return 1
    fi
    row "$be" "$tier" "$leg" FAIL "rc=$rc, $(basename "$log")"
    return 1
}

# ---------------------------------------------------------------------------
# THE TIERS MUST BE THERE AND MUST RUN. Y264_SIMD_FORCE is read only in a
# dev_knobs build, the checkasm banner prints the CPU's features rather than
# the build's, and checkasm calls each kernel by name, so neither says the
# encoder reaches a kernel. A stale -Dsimd=none build, a dispatch guard that
# drops a tier, or a cap name mapped to the wrong mask each leave every tier
# running the same code, and every identity leg then passes. So each build
# directory's options are read back and set right once if they are not, and
# scripts/dispatch_probe.py watches the kernel calls of a real encode at each
# depth under each cap: none calls no kernel, and each tier calls a kernel
# of every family that has one at that tier, and none above it.
B8="scratch/x86-docker/build8"; B10="scratch/x86-docker/build10"
opts_of() {   # opts_of <dir>: "dev_knobs simd bit_depth" as the dir records them
    dk X=1 -- "python3 -c 'import json, sys
o = {x[\"name\"]: x[\"value\"] for x in json.load(sys.stdin)}
print(str(o.get(\"dev_knobs\")).lower(), o.get(\"simd\"), o.get(\"bit_depth\"))' < <(meson introspect --buildoptions $1)" \
        2>/dev/null | tr -d '\r' | tail -1
}
# build_dir <dir> <depth>: configure, build, read the options back and set
# them right once. 1 when the build or the reconfigure fails
build_dir() {
    local d="$1" depth="$2" want="true auto $2" got k s b log="build$2.log"
    local set_="-Ddev_knobs=true -Dsimd=auto -Dbit_depth=$2"
    if ! dk X=1 -- "set -e
        [ -f $d/build.ninja ] || meson setup $d $set_
        ninja -C $d -j$JOBS" > "$out/$log" 2>&1; then
        row build "${depth}bit" "build" FAIL "$log"
        return 1
    fi
    got="$(opts_of "$d")"
    read -r k s b <<< "$got"
    row build "${depth}bit" "build -Dsimd=${s:-?}" PASS "dev_knobs=${k:-?} bit_depth=${b:-?}, $log"
    [ "$got" = "$want" ] && return 0
    # a dir from another meson version refuses `configure`; setup
    # --reconfigure is the second try. the log holds the real error
    if dk X=1 -- "meson configure $d $set_ && ninja -C $d -j$JOBS" > "$out/devknobs$2.log" 2>&1 \
       || dk X=1 -- "meson setup --reconfigure $d $set_ && ninja -C $d -j$JOBS" \
              >> "$out/devknobs$2.log" 2>&1; then
        local now; now="$(opts_of "$d")"
        if [ "$now" = "$want" ]; then
            row build "${depth}bit" "reconfigure" NOTE "was dev_knobs=$k simd=$s bit_depth=$b; now $set_"
            return 0
        fi
        row build "${depth}bit" "reconfigure" FAIL "still '$now'; devknobs$2.log"
        return 1
    fi
    row build "${depth}bit" "reconfigure" FAIL "was dev_knobs=$k simd=$s bit_depth=$b; devknobs$2.log"
    return 1
}
# dispatch <dir> <label>: the dispatch probe on one build dir under this
# backend; a row either way, 0 when every depth and cap passes
dispatch() {
    local log="$BACKEND-dispatch-$2.log"
    if dk X=1 -- "python3 scripts/dispatch_probe.py --build $1 --tiers '$TIERS' --wrapper \"\$QR\" \
            --source tests/corpus/foreman_cif.y4m --work $1/dispatch-probe-$BACKEND" \
            > "$out/$log" 2>&1; then
        row "$BACKEND" - "dispatch $2" PASS "every kernel at its own tier under each cap, 8 and 10 bits, $log"
        return 0
    fi
    row "$BACKEND" - "dispatch $2" FAIL "$(grep -m1 'FAIL\|^dispatch: ' "$out/$log" | cut -c1-160); $log"
    return 1
}

echo "x86-docker: image $IMAGE, backends [$BACKENDS], tiers [$TIERS], legs [$LEGS]"
echo "x86-docker: --cpus $CPUS, ninja -j$JOBS, encoder threads [$ENC_THREADS], frames $FRAMES"
echo "x86-docker: no clocks are read; an emulated run has no speed to report."

build_image() {
    echo "x86-docker: building $IMAGE"
    docker build --platform linux/amd64 -t "$IMAGE" \
        -f docker/x86/Dockerfile docker/x86 > "$out/image-build.log" 2>&1 9>&- \
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
# the lock dies with a killed run, but its container can still be writing here
live="$(docker ps -q --filter "label=yah264-x86-docker=$root" 2>/dev/null 9>&- | tr '\n' ' ')"
if [ -n "$live" ]; then
    echo "x86-docker: containers from an earlier run of this tree are still running: $live" >&2
    echo "            wait for them, or stop them with docker stop $live" >&2
    exit 2
fi

# -- the builds, once for both backends ------------------------------------
# The compiler runs in the container the same way on either backend, so the
# QEMU legs execute the very ELF the Rosetta legs do; what the two backends
# differ in is executing it.
b8="$B8"; b10="$B10"
build8ok=1; build10ok=1
build_dir "$b8" 8 || build8ok=0
if has_leg test; then build_dir "$b10" 10 || build10ok=0; fi
# conformance.sh takes the encoder as a single path, so the QEMU legs give it this
cat > "$out/qcli" <<QEOF
#!/bin/sh
exec $qrun_path /work/$b8/cli/yah264 "\$@"
QEOF
chmod +x "$out/qcli"

for BACKEND in $BACKENDS; do
    echo
    echo "=== backend: $BACKEND ============================================"
    QRUN=""
    if [ "$BACKEND" = rosetta ]; then
        if ! rosetta_enabled; then
            echo "x86-docker: Rosetta is NOT registered for x86-64 in this Docker VM."
            echo "            Turn on Docker Desktop > Settings > General >"
            echo "            'Use Rosetta for x86_64/amd64 emulation on Apple Silicon',"
            echo "            or run this with --backend qemu. Not changing it from here."
            row rosetta - "backend select" FAIL "Rosetta not registered"
            continue
        fi
    else
        QRUN="$qrun_path"
        if ! dk X=1 -- "test -x /opt/qemu/qemu-x86_64" >/dev/null 2>&1; then
            row qemu - "qemu in the image" FAIL "no /opt/qemu/qemu-x86_64 in $IMAGE; rerun with --build-image"
            continue
        fi
    fi
    if [ "$build8ok" = 0 ]; then
        row "$BACKEND" - "legs" SKIP "build8 failed; see the build rows"
        continue
    fi

    # -- what does the emulated machine say it is ---------------------------
    arch="$(dk X=1 -- "uname -m" 2>/dev/null)"
    echo "x86-docker: uname -m = $arch"
    if [ "$BACKEND" = rosetta ]; then
        flags="$(dk X=1 -- "grep -o 'sse4_2\|avx2\|avx512[a-z]*\|fma\|bmi2' /proc/cpuinfo | sort -u | tr '\n' ' '" 2>/dev/null)"
        echo "x86-docker: /proc/cpuinfo: ${flags:-<none>}"
        case " $flags " in
            *" avx2 "*) : ;;
            *) row rosetta - "cpuinfo avx2" FAIL "no avx2 in /proc/cpuinfo under Rosetta" ;;
        esac
    else
        # the container itself runs under the VM's own interpreter, so its
        # /proc/cpuinfo says nothing about what QEMU gives the binaries; the
        # binary's own CPUID, below, is the probe that counts
        echo "x86-docker: binaries under test run in /opt/qemu/qemu-x86_64 -cpu ${QEMU_CPU:-<per cap>};"
        echo "            the feature probe that counts is the encoder's own CPUID, below."
    fi

    # The binary's own CPUID, printed by checkasm, and the AVX-512 assertion
    # that keeps an emulated pass from ever being read as AVX-512 coverage.
    cpuline="$(dk X=1 -- "\$QR $b8/tools/checkasm/checkasm --list 2>/dev/null | head -1" | tr -d '\r')"
    echo "x86-docker: $cpuline"
    case "$cpuline" in
        *avx512*) row "$BACKEND" - "avx512 absent" FAIL "the emulator reports AVX-512: $cpuline" ;;
        *) row "$BACKEND" - "avx512 absent" PASS "asserted absent; no emulated AVX-512 coverage" ;;
    esac
    case "$cpuline" in
        *avx2*) row "$BACKEND" - "cpuid avx2 + fma3" PASS "${cpuline#checkasm: cpu }" ;;
        *) row "$BACKEND" - "cpuid avx2 + fma3" FAIL "the binary does not see avx2: $cpuline" ;;
    esac
    # QEMU must be what runs the binaries: a model without AVX2 hides it,
    # which Rosetta never would
    if [ "$BACKEND" = qemu ]; then
        lo="$(dk QEMU_CPU=Nehalem -- "\$QR $b8/tools/checkasm/checkasm --list > /tmp/l 2>&1; rc=\$?
            head -1 /tmp/l; [ \$rc = 0 ] || echo \" (rc \$rc: died under -cpu Nehalem; above-SSE4.2 code runs on every path)\"" | tr -d '\r' | tr '\n' ' ')"
        case "$lo" in
            *"(rc "*) row qemu - "qemu runs the binaries" FAIL "$lo"; continue ;;
            *avx2*) row qemu - "qemu runs the binaries" FAIL "-cpu Nehalem still shows avx2 ($lo)"; continue ;;
            "checkasm: cpu "*) row qemu - "qemu runs the binaries" PASS "-cpu Nehalem hides avx2: ${lo#checkasm: cpu }" ;;
            *) row qemu - "qemu runs the binaries" FAIL "no banner under -cpu Nehalem: $lo"; continue ;;
        esac
    fi

    # -- the dispatch probe, before any tier leg ---------------------------
    tierok=0; ok10=0
    dispatch "$b8" build8 && tierok=1
    if has_leg test && [ "$build10ok" = 1 ]; then dispatch "$b10" build10 && ok10=1; fi

    # -- the 10-bit unit leg -------------------------------------------------
    # Each depth folds its own tier libraries, so this is where the 10-bit
    # kernels meet checkasm (inside meson test) and where a depth-generic
    # mistake in the build files shows up.
    if has_leg test; then
        if [ "$ok10" = 1 ]; then
            run_case "$BACKEND" 10bit "unit" "$out/$BACKEND-build10.log" X=1 \
                -- "YAH264_WRAP=\"\$QR\" meson test -C $b10 --print-errorlogs \${QR:+--wrapper \$QR --timeout-multiplier 10}"
        else
            row "$BACKEND" 10bit "unit" SKIP "build10 is not proven; see above"
        fi
    fi

    # -- the tier matrix ----------------------------------------------------
    # a tier the probe could not find would run the default code under its
    # name, so without the probe the matrix is skipped, not run
    [ "$tierok" = 1 ] || row "$BACKEND" - "tier legs" SKIP "the build's tiers are not proven; see above"
    for tier in $TIERS; do
        [ "$tierok" = 1 ] || break
        if has_leg test; then
            # YAH264= as well as BUILD=: `make test` hands regress.py no path
            # and regress.py defaults to build/cli/yah264, which in here is a
            # macOS build directory or nothing at all.
            run_case "$BACKEND" "$tier" "make test" "$out/$BACKEND-$tier-test.log" \
                "Y264_SIMD_FORCE=$tier" "QEMU_CPU=$(cpu_for "$tier")" \
                -- "make test BUILD=$b8 TEST_WRAP=\"\$QR\""
            # checkasm again, explicitly at this tier's rows. `none` has no
            # tier to name, so it runs the whole table with every kernel row
            # gated off by its own cap -- which is the C-only claim.
            isa_arg="--isa $tier"; [ "$tier" = none ] && isa_arg=""
            run_case "$BACKEND" "$tier" "checkasm $isa_arg" "$out/$BACKEND-$tier-checkasm.log" \
                "Y264_SIMD_FORCE=$tier" "QEMU_CPU=$(cpu_for "$tier")" -- "\$QR $b8/tools/checkasm/checkasm $isa_arg"
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
                "Y264_SIMD_FORCE=$tier" "QEMU_CPU=$(cpu_for "$tier")" "YAH264_CONF_FAST=1" "YAH264_CONF_JOBS=$CPUS" \
                -- "enc=$b8/cli/yah264; [ -n \"\$QR\" ] && enc=scratch/x86-docker/qcli
                    scripts/conformance.sh --fast \$enc"
        fi
    done

    # -- the identity cmp ---------------------------------------------------
    # Every tier encodes the ten board clips at CRF, CQP and ABR, at each
    # thread count, and the md5 of the .264 is compared against the `none`
    # tier's. No kernel may move a byte, so a single differing md5 is a
    # kernel defect.
    if has_leg identity && [ "$tierok" = 1 ]; then
        # The md5 lists are keyed by BACKEND, not by build directory: the QEMU
        # leg may be running out of the Rosetta build, and one leg overwriting
        # the other's lists would compare two backends by accident.
        idir="scratch/x86-docker/ident-$BACKEND"
        script="set -e
mkdir -p $idir
: > $idir/md5.\$Y264_SIMD_FORCE
missing=0
for spec in $CLIPS; do
    clip=\${spec%%:*}; rate=\${spec##*:}
    src=tests/corpus/\$clip.y4m
    [ -f \"\$src\" ] || { echo \"MISSING \$src\"; missing=1; continue; }
    for t in $ENC_THREADS; do
        for mode in '--crf 23' '--qp 26' \"--bitrate \$rate\"; do
            o=$idir/x.264
            rm -f \"\$o\"
            \$QR $b8/cli/yah264 --input-y4m \"\$src\" --frames $FRAMES \$mode \\
                --threads \$t -o \"\$o\" > /dev/null 2> $idir/err \\
                || { echo \"ENCODE FAILED \$clip t\$t \$mode: \$(head -1 $idir/err)\"; exit 1; }
            [ -s \"\$o\" ] || { echo \"EMPTY OUTPUT \$clip t\$t \$mode\"; exit 1; }
            echo \"\$(md5sum < \"\$o\" | cut -d' ' -f1)  \$clip t\$t \$mode\" \\
                >> $idir/md5.\$Y264_SIMD_FORCE
        done
    done
done
# a clip the container cannot see is a harness defect, never a pass
[ \"\$missing\" = 0 ]"
        mkdir -p "$root/$idir"
        for tier in $TIERS; do : > "$root/$idir/md5.$tier"; done
        for tier in $TIERS; do
            if dk "Y264_SIMD_FORCE=$tier" "QEMU_CPU=$(cpu_for "$tier")" -- "$script" > "$out/$BACKEND-$tier-ident.log" 2>&1; then
                row "$BACKEND" "$tier" "identity encodes" PASS "$BACKEND-$tier-ident.log"
            else
                row "$BACKEND" "$tier" "identity encodes" FAIL "$BACKEND-$tier-ident.log"
            fi
        done
        for tier in $TIERS; do
            [ "$tier" = none ] && continue
            compare_lists "$BACKEND" "$tier" "identity vs none" "$idir" \
                "$BACKEND-$tier-ident.diff" "a kernel moved a byte"
        done
    fi

    # The same at 10 bits, through the 10-bit library's own tiers: 10-bit
    # clips made inside the container from three corpus clips (4:2:0, 4:2:2,
    # 4:4:4, so every chroma path's kernels run), encoded at threads 1 at
    # CRF and CQP, md5 against the `none` tier. The CLI links both libraries
    # and picks the 10-bit one from the input.
    # FRAMES 0 means the whole clip: ffmpeg is then given no frame limit, and
    # each cached clip is keyed by the count.
    if has_leg identity && [ "$tierok" = 1 ]; then
        idir="scratch/x86-docker/ident10-$BACKEND"
        fflim=""; [ "$FRAMES" -gt 0 ] && fflim="-frames:v $FRAMES"
        script="set -e
mkdir -p $idir
: > $idir/md5.\$Y264_SIMD_FORCE
for spec in foreman_cif:yuv420p10le bus_cif:yuv422p10le mobile_cif:yuv444p10le; do
    clip=\${spec%%:*}; fmt=\${spec##*:}
    src=tests/corpus/\$clip.y4m
    [ -f \"\$src\" ] || { echo \"MISSING \$src\"; exit 1; }
    dst=$idir/\$clip-\$fmt-f$FRAMES.y4m
    # a cached clip with no frame in it is a failed earlier run, not a clip
    head -c 4096 \"\$dst\" 2>/dev/null | grep -q FRAME || rm -f \"\$dst\"
    if [ ! -s \"\$dst\" ]; then
        ffmpeg -v error -y -i \"\$src\" $fflim -pix_fmt \$fmt -strict -1 -f yuv4mpegpipe \"\$dst.part\"
        mv \"\$dst.part\" \"\$dst\"
    fi
    head -c 4096 \"\$dst\" | grep -q FRAME || { echo \"NO FRAMES in \$dst\"; exit 1; }
    for mode in '--crf 23' '--qp 26' '--preset slow --crf 30'; do
        o=$idir/x.264
        rm -f \"\$o\"
        \$QR $b8/cli/yah264 --input-y4m \"\$dst\" --frames $FRAMES \$mode --threads 1 \\
            -o \"\$o\" > /dev/null 2> $idir/err \\
            || { echo \"ENCODE FAILED \$clip \$fmt \$mode: \$(head -1 $idir/err)\"; exit 1; }
        [ -s \"\$o\" ] || { echo \"EMPTY OUTPUT \$clip \$fmt \$mode\"; exit 1; }
        echo \"\$(md5sum < \"\$o\" | cut -d' ' -f1)  \$clip \$fmt \$mode\" \\
            >> $idir/md5.\$Y264_SIMD_FORCE
    done
done"
        mkdir -p "$root/$idir"
        # last run's lists must not stand in for a tier that fails to write;
        # emptied rather than deleted, since a file deleted on the host can
        # stay unwritable through the container's mount for a while
        for tier in $TIERS; do : > "$root/$idir/md5.$tier"; done
        for tier in $TIERS; do
            if dk "Y264_SIMD_FORCE=$tier" "QEMU_CPU=$(cpu_for "$tier")" -- "$script" > "$out/$BACKEND-$tier-ident10.log" 2>&1; then
                row "$BACKEND" "$tier" "10-bit identity encodes" PASS "$BACKEND-$tier-ident10.log"
            else
                row "$BACKEND" "$tier" "10-bit identity encodes" FAIL "$BACKEND-$tier-ident10.log"
            fi
        done
        for tier in $TIERS; do
            [ "$tier" = none ] && continue
            compare_lists "$BACKEND" "$tier" "10-bit identity vs none" "$idir" \
                "$BACKEND-$tier-ident10.diff" "a 10-bit kernel moved a byte"
        done
    fi

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
    echo "real silicon (docs/simd.md)."
} | tee "$report"

echo
if [ "$fail" = 0 ]; then
    echo "x86-docker: PASS ($pass legs), report in scratch/x86-docker/report.txt"
    exit 0
fi
echo "x86-docker: FAIL ($fail of $((pass+fail)) legs), report in scratch/x86-docker/report.txt"
exit 1
