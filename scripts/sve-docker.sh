#!/usr/bin/env bash
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# sve-docker.sh -- the SVE tier's correctness battery, on a machine with no SVE
# (docs/simd.md).
#
# Apple silicon has no SVE, so the tier is EXECUTED under QEMU user mode inside
# a native arm64 Linux container: the compiler, the unit tests and the NEON leg
# run natively, and each SVE leg runs the same binary under qemu-aarch64 with a
# CPU model that has the vector length in question:
#
#   v1     -cpu neoverse-v1     SVE, no SVE2, 256-bit (the Graviton3 core)
#   n2     -cpu neoverse-n2     SVE + SVE2, 128-bit (the Graviton4 shape)
#   max512 -cpu max,sve-default-vector-length=64
#                               SVE + SVE2 at 512 bits: no shipping part, but a
#                               vector-length-agnostic kernel must be exact here
#                               too, and this is the cheap place to find out
#
# THE NATIVE LEG IS THE NEGATIVE CONTROL. The container's CPU is the Mac's, so
# the binary detects no SVE there: every SVE row reports `skip` and every
# encode takes the NEON path. The identity legs compare each emulated model's
# md5s with the native ones, so an SVE kernel that moves a byte shows as a
# difference against a run that cannot have used it.
#
# No clock is read; an emulated run has no speed to report, and an emulated
# instruction count is the emulator's.
#
# Usage:
#   scripts/sve-docker.sh                      # build, unit, checkasm, identity
#   scripts/sve-docker.sh --legs "checkasm"    # one leg
#   scripts/sve-docker.sh --build-image
#
# Knobs (environment):
#   SVE_IMAGE    image tag                             (yah264-sve:24.04)
#   SVE_CPUS     --cpus for the container              (4)
#   SVE_JOBS     ninja -j / parallel encodes           (4)
#   SVE_FRAMES   frames per identity encode            (12)
#   SVE_CLIPS    clip:kbps list                        (parity-clips.sh CLIPS)
#   SVE_MODELS   emulated models for the identity leg  ("v1 n2")
set -uo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

IMAGE="${SVE_IMAGE:-yah264-sve:24.04}"
CPUS="${SVE_CPUS:-4}"
JOBS="${SVE_JOBS:-4}"
FRAMES="${SVE_FRAMES:-12}"
MODELS="${SVE_MODELS:-v1 n2}"
LEGS="build unit checkasm identity"
BUILD_IMAGE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --legs) LEGS="$2"; shift 2 ;;
        --build-image) BUILD_IMAGE=1; shift ;;
        -h|--help) sed -n '28,40p' "$0"; exit 0 ;;
        *) echo "sve-docker: unknown argument '$1'" >&2; exit 2 ;;
    esac
done
has_leg() { case " $LEGS " in *" $1 "*) return 0 ;; *) return 1 ;; esac; }

# shellcheck source=/dev/null
. "$root/scripts/parity-clips.sh"
CLIPS="${SVE_CLIPS:-$CLIPS}"

out="$root/scratch/sve-docker"
mkdir -p "$out"
pass=0; fail=0
declare -a ROWS
row() {   # row <leg> <PASS|FAIL|NOTE> <detail>
    ROWS+=("$(printf '%-34s %-5s %s' "$1" "$2" "${3:-}")")
    case "$2" in PASS) pass=$((pass+1)) ;; FAIL) fail=$((fail+1)) ;; esac
    printf '  %-34s %-5s %s\n' "$1" "$2" "${3:-}"
}

qcpu() {   # the qemu -cpu argument for a model name
    case "$1" in
        v1)     echo "neoverse-v1" ;;
        n2)     echo "neoverse-n2" ;;
        max128) echo "max,sve-default-vector-length=16" ;;
        max256) echo "max,sve-default-vector-length=32" ;;
        max512) echo "max,sve-default-vector-length=64" ;;
        *) echo "sve-docker: unknown model '$1'" >&2; exit 2 ;;
    esac
}

# The corpus is mounted read-only at its physical path and over
# /work/tests/corpus, so a symlinked worktree corpus resolves inside.
mounts=()
if [ -d "$root/tests/corpus" ]; then
    cdir="$(cd "$root/tests/corpus" && pwd -P)"
    mounts+=(-v "$cdir:/work/tests/corpus:ro")
fi
if [ -L "$root/local" ]; then
    mounts+=(-v "$(cd "$root/local" && pwd -P):/work/local")
fi

dk() {   # dk <shell command>: one native arm64 container per call
    docker run --rm --platform linux/arm64 --cpus "$CPUS" \
        -u "$(id -u):$(id -g)" -w /work -v "$root:/work" \
        ${mounts[@]+"${mounts[@]}"} -e HOME=/tmp -e XDG_CACHE_HOME=/tmp/.cache \
        "$IMAGE" bash -c "$*"
}

if [ "$BUILD_IMAGE" = 1 ] || ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    docker build --platform linux/arm64 -t "$IMAGE" -f docker/sve/Dockerfile docker/sve \
        > "$out/image-build.log" 2>&1 || { echo "sve-docker: image build FAILED"; exit 1; }
fi

echo "sve-docker: image $IMAGE, legs [$LEGS], models [$MODELS], --cpus $CPUS"
B=scratch/sve-docker/build-gcc
BC=scratch/sve-docker/build-clang

if has_leg build; then
    for cc in gcc clang; do
        bd="scratch/sve-docker/build-$cc"
        if dk "set -e
            [ -f $bd/build.ninja ] || CC=$cc meson setup $bd --buildtype=release -Doptimization=3
            ninja -C $bd -j$JOBS" > "$out/build-$cc.log" 2>&1; then
            if grep -q 'Y264_HAVE_SVE=1' "$root/$bd/build.ninja"; then
                row "build $cc (release -O3)" PASS "SVE tier compiled in"
            else
                row "build $cc (release -O3)" FAIL "built, but without the SVE tier"
            fi
        else
            row "build $cc (release -O3)" FAIL "build-$cc.log"
        fi
    done
fi

# What each CPU model says it is, from the binary's own detection.
probe() { dk "${2:+qemu-aarch64 -cpu $2 }$B/tools/checkasm/checkasm --list | head -1"; }
native_line="$(probe native "")"
echo "sve-docker: native: $native_line"
case "$native_line" in
    *sve*) row "native has no SVE" FAIL "$native_line" ;;
    *) row "native has no SVE" PASS "${native_line#checkasm: }" ;;
esac
for m in v1 n2 max512; do
    l="$(probe "$m" "$(qcpu $m)")"
    # what each model must say: v1 is SVE at 256 without SVE2, n2 SVE2 at 128
    has() { printf '%s\n' "$l" | tr ' ,' '\n\n' | grep -qx "$1"; }
    ok=1
    case "$m" in
        v1)     has sve && has sve256 && ! has sve2 || ok=0 ;;
        n2)     has sve && has sve2 && ! has sve256 || ok=0 ;;
        max512) has sve && has sve2 && has sve256 || ok=0 ;;
    esac
    if [ "$ok" = 1 ]; then row "model $m detection" PASS "${l#checkasm: }"
    else row "model $m detection" FAIL "$l"; fi
done

if has_leg unit; then
    if dk "meson test -C $B --print-errorlogs" > "$out/unit.log" 2>&1; then
        row "meson test (native, NEON)" PASS "unit.log"
    else
        row "meson test (native, NEON)" FAIL "unit.log"
    fi
fi

if has_leg checkasm; then
    for bd in "$B" "$BC"; do
        cc="${bd##*-}"
        for m in v1 n2 max512; do
            log="$out/checkasm-$cc-$m.log"
            if dk "qemu-aarch64 -cpu $(qcpu "$m") $bd/tools/checkasm/checkasm" > "$log" 2>&1; then
                nsve="$(grep -c '^  ok .*_sve' "$log")"
                if [ "$nsve" -gt 0 ] && ! grep -q 'skip .*_sve' "$log"; then
                    row "checkasm $cc $m (all rows)" PASS "$nsve SVE groups ok, $(tail -1 "$log")"
                else
                    row "checkasm $cc $m (all rows)" FAIL "SVE rows did not run: $(basename "$log")"
                fi
            else
                row "checkasm $cc $m (all rows)" FAIL "$(basename "$log")"
            fi
        done
    done
    if dk "$B/tools/checkasm/checkasm --isa sve" > "$out/checkasm-native-sve.log" 2>&1 \
       && grep -q 'skip .*_sve' "$out/checkasm-native-sve.log"; then
        row "checkasm native --isa sve" PASS "SVE rows skip natively, as they must"
    else
        row "checkasm native --isa sve" FAIL "checkasm-native-sve.log"
    fi
fi

if has_leg identity; then
    # One script per arm; the arm's runner prefix comes in as $1. Encodes run
    # $JOBS at a time at --threads 1, where output is required to repeat.
    cat > "$out/ident.sh" <<EOF
#!/usr/bin/env bash
set -u
run="\$1"; list="\$2"; dir="\$(dirname "\$list")"; : > "\$list"
enc() {   # enc <clip> <rate> <mode-name> <mode args...>
    local clip=\$1 rate=\$2 name=\$3; shift 3
    local o="\$dir/\$(basename "\$list").\$clip.\$name.264"
    \$run $B/cli/yah264 --input-y4m tests/corpus/\$clip.y4m --frames $FRAMES "\$@" \\
        --threads 1 -o "\$o" > /dev/null 2>&1 || { echo "ENCODE FAILED \$clip \$name"; return 1; }
    echo "\$(md5sum < "\$o" | cut -c1-32)  \$clip \$name" >> "\$list"
    rm -f "\$o"
}
for spec in $CLIPS; do
    clip=\${spec%%:*}; rate=\${spec##*:}
    [ -f tests/corpus/\$clip.y4m ] || { echo "MISSING \$clip"; continue; }
    enc \$clip \$rate crf23 --crf 23 &
    enc \$clip \$rate qp26 --qp 26 &
    enc \$clip \$rate abr --bitrate \$rate &
    while [ "\$(jobs -rp | wc -l)" -ge $JOBS ]; do wait -n; done
done
wait
EOF
    idir=scratch/sve-docker/ident
    mkdir -p "$root/$idir"
    arms="native"
    for m in $MODELS; do arms="$arms $m"; done
    arms="$arms v1:neon v1:sve128"
    for arm in $arms; do
        m="${arm%%:*}"; force="${arm#*:}"; [ "$force" = "$arm" ] && force=""
        runner=""
        [ "$m" != native ] && runner="qemu-aarch64 -cpu $(qcpu "$m")"
        envp=""; [ -n "$force" ] && envp="env Y264_SIMD_FORCE=$force "
        tag="${arm/:/-}"
        dk "${envp}bash scratch/sve-docker/ident.sh '$runner' $idir/md5.$tag" \
            > "$out/ident-$tag.log" 2>&1
        sort -k2 "$root/$idir/md5.$tag" -o "$root/$idir/md5.$tag"
        n="$(grep -c . "$root/$idir/md5.$tag")"
        if grep -q 'FAILED\|MISSING' "$out/ident-$tag.log" "$root/$idir/md5.$tag"; then
            row "identity encodes $tag" FAIL "$(grep -h 'FAILED\|MISSING' "$out/ident-$tag.log" | head -2 | tr '\n' ' ')"
        else
            row "identity encodes $tag" PASS "$n encodes"
        fi
    done
    base="$root/$idir/md5.native"
    for arm in $arms; do
        [ "$arm" = native ] && continue
        tag="${arm/:/-}"
        f="$root/$idir/md5.$tag"
        if [ -s "$base" ] && diff -q "$base" "$f" >/dev/null; then
            row "identity $tag vs native" PASS "$(grep -c . "$f") encodes byte-identical"
        else
            diff "$base" "$f" > "$out/ident-$tag.diff"
            row "identity $tag vs native" FAIL "$(grep -c '^<' "$out/ident-$tag.diff") differ, ident-$tag.diff"
        fi
    done
fi

{
    echo "sve-docker report"
    echo "image $IMAGE   legs $LEGS   models $MODELS   frames $FRAMES"
    echo "clips $CLIPS"
    echo
    printf '%s\n' ${ROWS[@]+"${ROWS[@]}"}
    echo
    echo "pass $pass   fail $fail"
    echo
    echo "Emulated: QEMU user mode executes the SVE paths; it says nothing about"
    echo "their speed. Real Graviton parts are the only source of counts."
} | tee "$out/report.txt"
[ "$fail" = 0 ]
