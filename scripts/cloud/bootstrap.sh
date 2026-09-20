#!/usr/bin/env bash
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# bootstrap.sh -- put a bare x86-64 box into a state where campaign.sh can
# measure on it, and prove it got there (docs/x86-plan.md, wave 4).
#
# THE POINT. The owner has no x86 machine, so every x86 number this project
# will ever publish comes off a box that exists for a few hours and is then
# deleted. That box has to be built from nothing, without a person watching,
# and it has to SAY what it built -- because the failure mode of an unattended
# setup is not a crash, it is a campaign that runs to completion against a
# binary with no AVX-512 in it, or against an x264 whose C arm was never
# vectorized, and reports a plausible table nobody can tell is wrong.
#
# So every stage lands in one table as PASS, SKIP or FAIL, and the run ends by
# writing `bootstrap.ok` carrying the CPU's own name, its cpuid flags, the
# kernel and the compiler versions. campaign.sh sources that file and refuses
# to invent any of it.
#
# THE SAME SCRIPT RUNS IN TWO PLACES, which is the rehearsal criterion:
#
#   Docker, under Rosetta, with a local directory standing in for the bucket:
#       BUCKET_URL=file:///.../fake-bucket scripts/cloud/bootstrap.sh
#   The rented GCP instance:
#       CLOUD=1 BUCKET_URL=gs://<the bucket> scripts/cloud/bootstrap.sh
#
# CLOUD=1 is the whole difference: it turns on the AVX-512 build and the two
# SDE legs, and it asserts real silicon. Everything that cannot run where it is
# -- no AVX-512 under an emulator, no sde64 on an ARM host, no perf counters in
# a VM without PMU passthrough -- detects that for itself and prints a NAMED
# skip. Nothing fails for being in the wrong place.
#
# Usage:
#   scripts/cloud/bootstrap.sh
#
# Environment:
#   CLOUD=1            real silicon: AVX-512 build, SDE legs, full conformance
#   BUCKET_URL         file:///dir | gs://b/p | s3://b/p | https://host/p
#   YAH264_REPO        clone source (default: the public GitHub repo)
#   YAH264_COMMIT      commit/branch/tag to clone (default: main)
#   X264_REPO          x264 clone source        X264_COMMIT
#   FFMPEG_REPO        the fork with both wrappers   FFMPEG_BRANCH
#   FFMPEG_FORK        1 = build it (default: CLOUD's value)
#   VMAF_REPO / VMAF_TAG  the libvmaf source the `vmaf` CLI is built from
#   CLOUD_WORK         where everything lands (default: <tree>/scratch/cloud)
#   JOBS               compile parallelism (default: nproc, capped at 8)
#   WALL_BUDGET_H      hours; stages that will not fit are skipped by name
#   STAGES             space-separated subset to run; the rest say "not in
#                      STAGES" in the table. For re-running one stage on a box
#                      whose session has already paid for the others.
#   SKIP_APT=1         do not touch the package manager
#
# Asset names in the bucket (override if the staging used others):
#   CORPUS_TARBALL (corpus.tar)  SDE_TARBALL (sde-external.tar.xz)
#   JM_ARCHIVE / OPENH264_ARCHIVE default to the names the fetch scripts pin.
set -uo pipefail

C_SELF="$(cd "$(dirname "$0")" && pwd)"
C_TREE="$(cd "$C_SELF/../.." && pwd)"

CLOUD="${CLOUD:-0}"
CLOUD_WORK="${CLOUD_WORK:-$C_TREE/scratch/cloud}"
JOBS="${JOBS:-}"
YAH264_REPO="${YAH264_REPO:-https://github.com/terranvigil/yah264.git}"
YAH264_COMMIT="${YAH264_COMMIT:-main}"
X264_REPO="${X264_REPO:-https://code.videolan.org/videolan/x264.git}"
X264_COMMIT="${X264_COMMIT:-}"
FFMPEG_REPO="${FFMPEG_REPO:-https://github.com/terranvigil/FFmpeg.git}"
FFMPEG_BRANCH="${FFMPEG_BRANCH:-yah264}"
FFMPEG_FORK="${FFMPEG_FORK:-$CLOUD}"
VMAF_REPO="${VMAF_REPO:-https://github.com/Netflix/vmaf.git}"
VMAF_TAG="${VMAF_TAG:-v3.0.0}"
CORPUS_TARBALL="${CORPUS_TARBALL:-corpus.tar}"
SDE_TARBALL="${SDE_TARBALL:-sde-external.tar.xz}"

C_SRC="$CLOUD_WORK/src"
C_ASSETS="$CLOUD_WORK/assets"
C_LOGS="$CLOUD_WORK/logs/bootstrap"
C_X264="$CLOUD_WORK/x264"
C_FFB="$CLOUD_WORK/ffbuild"
C_SDE="$CLOUD_WORK/sde"
C_B8="$C_SRC/build-rel"
C_B10="$C_SRC/build-rel10"
C_OK="$CLOUD_WORK/bootstrap.ok"
C_MANIFEST="$C_ASSETS/SHA256SUMS"

# shellcheck source=scripts/cloud/lib.sh
. "$C_SELF/lib.sh"

mkdir -p "$CLOUD_WORK" "$C_ASSETS" "$C_LOGS"

# JOBS. Capped at 8 because nothing here scales past that and an unbounded -j
# on a 22-vCPU box competes with itself for memory bandwidth during the ffmpeg
# build. On the M5 under Rosetta the caller pins it lower still.
[ -n "$JOBS" ] || { JOBS="$(c_nproc)"; [ "$JOBS" -gt 8 ] && JOBS=8; }

c_detect_emulator

echo "=========================================================================="
echo "yah264 cloud bootstrap"
echo "  tree        $C_TREE"
echo "  work        $CLOUD_WORK"
echo "  CLOUD       $CLOUD   $([ "$CLOUD" = 1 ] && echo '(real silicon: AVX-512 build + SDE legs)' || echo '(rehearsal: no AVX-512, no SDE)')"
echo "  bucket      $(c_bucket_kind)$([ -n "${BUCKET_URL:-}" ] && echo " (configured)" || echo " -- not configured")"
echo "  emulator    ${C_EMULATOR:-none detected}"
echo "  jobs        $JOBS"
echo "  cpu         $(c_cpu_model)"
echo "  flags       $(c_cpu_flags)"
echo "=========================================================================="
c_budget_start
echo

# ---------------------------------------------------------------------------
# 1. Dependencies.
#
# The list is the Dockerfile's, plus unzip, which the JM archive needs and the
# image did not have. A box where apt is not usable -- an unprivileged
# container, which is exactly what the rehearsal runs in -- skips by name and
# the later stages fail on what is actually missing rather than on apt.

st_deps() {
    [ "${SKIP_APT:-0}" = 1 ] && { c_skip "SKIP_APT=1"; return; }
    command -v apt-get >/dev/null 2>&1 || { c_skip "no apt-get on this box"; return; }
    [ "$(id -u)" = 0 ] || { c_skip "not root; the image is expected to carry the deps already"; return; }
    export DEBIAN_FRONTEND=noninteractive
    apt-get update || return 1
    apt-get install -y --no-install-recommends \
        build-essential clang cmake curl ffmpeg git linux-tools-common \
        linux-tools-generic meson nasm ninja-build pkg-config python3 \
        python3-pip python3-numpy unzip xz-utils yasm || return 1
    # perf on a GCP instance comes from the kernel-matched package, which
    # linux-tools-generic is not; missing it is not fatal, it costs the
    # CPU-seconds column and campaign.sh says so.
    apt-get install -y --no-install-recommends "linux-tools-$(uname -r)" \
        || echo "note: no linux-tools-$(uname -r); perf may be unusable"
    return 0
}

# ---------------------------------------------------------------------------
# 2. The source.
#
# A CLONE, not the tree this script sits in, and at a NAMED commit. The box is
# ephemeral and the point of the campaign is a number attributable to one
# revision; a bind-mounted working tree with uncommitted edits in it is not
# one. The rehearsal clones the local worktree, which is the same operation
# against a different URL.

st_clone() {
    if [ -d "$C_SRC/.git" ]; then
        git -C "$C_SRC" fetch --all --tags || true
        git -C "$C_SRC" checkout -q "$YAH264_COMMIT" || return 1
    else
        git clone "$YAH264_REPO" "$C_SRC" || return 1
        git -C "$C_SRC" checkout -q "$YAH264_COMMIT" || return 1
    fi
    git -C "$C_SRC" --no-pager log --oneline -1
    return 0
}

# ---------------------------------------------------------------------------
# 3. The assets, and the hashes.
#
# The bucket is an IN-REGION CACHE, never the only way in: the corpus is ~12 GB
# and pulling it across a region boundary costs both money and a quarter of a
# session, but a missing tarball falls back to fetch_corpus.sh and the run goes
# on. Each file that arrives is checked against the bucket's own SHA256SUMS and
# against the hashes scripts/fetch_corpus.sh, fetch_jm.sh and fetch_openh264.sh
# already carry; anything neither list names is printed UNPINNED by name.
#
# SDE is the exception with no fallback. Intel's download is a click-through,
# so it cannot be fetched by a script at all: no tarball in the bucket means
# the two SDE legs skip, which is the correct outcome and not a failure.

st_assets() {
    local got=0
    if [ "$(c_bucket_kind)" = none ]; then
        c_skip "BUCKET_URL is not set; every asset falls back to its upstream fetch"
        return
    fi
    c_bucket_get SHA256SUMS "$C_MANIFEST" \
        && echo "manifest: $(wc -l < "$C_MANIFEST" | tr -d ' ') entries" \
        || echo "manifest: the bucket carries no SHA256SUMS; falling back to the tree's pinned hashes"

    local jm_ver oh_tag jm_name oh_name
    jm_ver="$(awk -F'"' '/^VER=/{print $2; exit}' "$C_SRC/scripts/fetch_jm.sh" 2>/dev/null)"
    oh_tag="$(awk -F'"' '/^TAG=/{print $2; exit}' "$C_SRC/scripts/fetch_openh264.sh" 2>/dev/null)"
    jm_name="${JM_ARCHIVE:-jm${jm_ver}.zip}"
    oh_name="${OPENH264_ARCHIVE:-openh264-${oh_tag}.tar.gz}"

    local rc=0 f
    for spec in "$CORPUS_TARBALL:$C_ASSETS/$CORPUS_TARBALL" \
                "$jm_name:$C_ASSETS/$jm_name" \
                "$oh_name:$C_ASSETS/$oh_name" \
                "$SDE_TARBALL:$C_ASSETS/$SDE_TARBALL"; do
        local name="${spec%%:*}" dest="${spec#*:}"
        if c_bucket_get "$name" "$dest"; then
            got=$((got + 1))
            c_verify "$dest" "$C_MANIFEST" "$name" || rc=1
        else
            echo "  absent    $name  (not in the bucket; upstream fallback or a named skip downstream)"
        fi
    done

    # The archives go where the fetch scripts look for them, so those scripts
    # verify the hash a second time against their own pin and build from it
    # without reaching the network at all.
    mkdir -p "$C_SRC/scratch/oracles/jm" "$C_SRC/scratch/oracles/openh264"
    f="$C_ASSETS/$jm_name"; [ -f "$f" ] && cp -f "$f" "$C_SRC/scratch/oracles/jm/$jm_name"
    f="$C_ASSETS/$oh_name"; [ -f "$f" ] && cp -f "$f" "$C_SRC/scratch/oracles/openh264/$oh_name"

    echo "fetched $got asset(s)"
    return "$rc"
}

st_corpus() {
    local dest="$C_SRC/tests/corpus" tar="$C_ASSETS/$CORPUS_TARBALL"
    mkdir -p "$dest"
    if [ -f "$tar" ]; then
        tar -xf "$tar" -C "$dest" || return 1
    elif [ -n "${CORPUS_DIR:-}" ] && [ -d "$CORPUS_DIR" ]; then
        # A directory handed in directly: what a box with the corpus on an
        # attached disk wants. Linked, never copied -- it is tens of gigabytes
        # and it is not ours to duplicate.
        #
        # RESOLVED WITH `pwd -P`, so the link points through the directory's
        # PHYSICAL path rather than through whatever name was handed in.
        # CORPUS_DIR is very often itself a symlink to a sibling tree, and a
        # link whose target path runs through another link only resolves where
        # every hop exists under the same name -- which is exactly what does
        # not hold inside a container, or on a box where only the final
        # directory was mounted. scripts/x86-docker.sh hit this same class of
        # bug from the other side on 2026-09-20 (main 7053c7a): a logical pwd
        # reported a symlinked corpus as already inside the mount, so nothing
        # was mounted and every clip read MISSING. This resolves the directory,
        # not a symlinked clip name within it; a corpus that is itself a farm
        # of per-clip symlinks wants the tarball path above.
        local n cdir
        cdir="$(cd "$CORPUS_DIR" && pwd -P)" || return 1
        for n in "$cdir"/*.y4m; do
            [ -e "$n" ] || continue
            ln -sf "$(cd "$(dirname "$n")" && pwd -P)/$(basename "$n")" "$dest/"
        done
        [ -f "$cdir/CLASSES" ] && cp -f "$cdir/CLASSES" "$dest/"
    else
        command -v curl >/dev/null 2>&1 || { c_skip "no corpus tarball, no CORPUS_DIR and no curl"; return; }
        ( cd "$C_SRC" && ./scripts/fetch_corpus.sh --res ) || return 1
    fi
    local have
    have="$(find "$dest" -maxdepth 1 -name '*.y4m' | wc -l | tr -d ' ')"
    echo "corpus: $have clips in $dest"
    [ "$have" -gt 0 ] || { c_skip "no clips landed; every clip-driven leg will skip"; return; }
    return 0
}

st_sde() {
    local tar="$C_ASSETS/$SDE_TARBALL"
    [ "$CLOUD" = 1 ] || { c_skip "CLOUD is not 1; SDE runs on an x86 host only"; return; }
    [ -f "$tar" ] || { c_skip "no $SDE_TARBALL in the bucket (Intel's download is a click-through; stage it by hand)"; return; }
    mkdir -p "$C_SDE"
    tar -xf "$tar" -C "$C_SDE" || return 1
    local bin
    bin="$(find "$C_SDE" -maxdepth 3 -name sde64 -type f | head -1)"
    [ -n "$bin" ] || { c_skip "no sde64 inside $SDE_TARBALL"; return; }
    chmod +x "$bin"
    echo "sde64: $bin"
    echo "$bin" > "$CLOUD_WORK/sde64.path"
    return 0
}

# ---------------------------------------------------------------------------
# 4. The builds.
#
# RELEASE, both depths. -Dsimd=auto compiles every tier the architecture has
# kernels for; -Davx512=true adds the gated tier and is set only under CLOUD=1,
# because a tier nothing can execute is a compile-only claim and this kit is
# careful never to file one as coverage.

st_build_y264() {
    local opts=(--buildtype=release -Dsimd=auto)
    [ "$CLOUD" = 1 ] && opts+=(-Davx512=true)
    ( cd "$C_SRC" || exit 1
      [ -f "$C_B8/build.ninja" ] || meson setup "$C_B8" "${opts[@]}" || exit 1
      ninja -C "$C_B8" -j"$JOBS" ) || return 1
    echo "cpu (the binary's own CPUID): $(c_y264_cpu_name "$C_B8/tools/checkasm/checkasm")"
    return 0
}

st_build_y264_10() {
    ( cd "$C_SRC" || exit 1
      [ -f "$C_B10/build.ninja" ] || meson setup "$C_B10" --buildtype=release -Dbit_depth=10 || exit 1
      ninja -C "$C_B10" -j"$JOBS" ) || return 1
    return 0
}

# THE `vmaf` CLI, BUILT FROM SOURCE, because every quality column in this
# campaign goes through it and no distribution packages it.
#
# This is not the same thing as ffmpeg's libvmaf filter. scripts/perf-comp.sh
# and scripts/bdcompare.py both shell out to the libvmaf CLI by name -- they
# want its --model, --feature and --subsample flags and its JSON -- and
# perf-comp.sh exits 2 before it encodes anything if that binary is not on the
# PATH. Ubuntu 24.04 packages no libvmaf in any component, so on a box built
# from docker/x86/Dockerfile there is none, and every board leg and the BD leg
# would have failed on the rented machine for want of a tool nothing had built.
# The image's own header says a cloud image that wants the library builds it
# from source the way this script builds the other tools. So it does.
#
# Static, so the CLI carries its models and needs no LD_LIBRARY_PATH: libvmaf
# 2.x and later compile vmaf_v0.6.1 and vmaf_v0.6.1neg in, and NEG is the model
# every quality read in this tree uses.
st_build_vmaf() {
    if command -v vmaf >/dev/null 2>&1; then
        echo "vmaf: already on PATH at $(command -v vmaf); not building one"
        command -v vmaf > "$CLOUD_WORK/vmaf.path"
        return 0
    fi
    command -v meson >/dev/null 2>&1 || { c_skip "no meson; the quality legs will skip"; return; }
    if [ ! -d "$CLOUD_WORK/vmaf/.git" ]; then
        git clone --branch "$VMAF_TAG" --depth 1 "$VMAF_REPO" "$CLOUD_WORK/vmaf" || return 1
    fi
    echo "libvmaf $VMAF_TAG at $(git -C "$CLOUD_WORK/vmaf" rev-parse --short HEAD)"
    ( cd "$CLOUD_WORK/vmaf/libvmaf" || exit 1
      [ -f build/build.ninja ] || meson setup build --buildtype=release \
          --default-library=static --prefix="$C_FFB/prefix-vmaf" || exit 1
      ninja -C build -j"$JOBS" || exit 1
      ninja -C build install ) || return 1
    local bin
    bin="$(find "$C_FFB/prefix-vmaf" "$CLOUD_WORK/vmaf/libvmaf/build" -name vmaf -type f -perm -u+x 2>/dev/null | head -1)"
    [ -n "$bin" ] || { c_skip "libvmaf built but produced no vmaf CLI"; return; }
    echo "$bin" > "$CLOUD_WORK/vmaf.path"
    echo "vmaf: $bin"
    "$bin" --version 2>&1 | head -2
    return 0
}

# THE FAIR x264 RECIPE, and it is the one thing here most easily got wrong.
# x264's configure adds -fno-tree-vectorize unconditionally, so a plain
# --disable-asm build is GENUINELY SCALAR while yah264's YAH264_NO_ASM=1 is a
# runtime dispatch switch over -O3 auto-vectorized objects. Comparing those two
# is the one lopsided quadrant, and it has flattered this project's own gap
# before. The C arm therefore strips that flag out of config.mak between
# configure and make. `x264 --asm 0` on a stock build does not substitute:
# the flag was applied when the binary was compiled.
st_build_x264() {
    command -v gcc >/dev/null 2>&1 || { c_skip "no gcc"; return; }
    if [ ! -d "$C_X264/.git" ]; then
        git clone "$X264_REPO" "$C_X264" || return 1
    fi
    [ -n "$X264_COMMIT" ] && { git -C "$C_X264" checkout -q "$X264_COMMIT" || return 1; }
    echo "x264 at $(git -C "$C_X264" rev-parse --short HEAD)"

    local common=(--disable-lavf --disable-ffms --disable-avs --disable-swscale)

    ( cd "$C_X264" || exit 1
      make distclean >/dev/null 2>&1
      ./configure "${common[@]}" --enable-static --enable-pic \
          --prefix="$C_FFB/prefix-x264-asm" || exit 1
      make -j"$JOBS" || exit 1
      cp -f x264 x264-asm
      make install >/dev/null || exit 1 ) || return 1

    ( cd "$C_X264" || exit 1
      make distclean >/dev/null 2>&1
      ./configure --disable-asm "${common[@]}" --enable-static --enable-pic \
          --prefix="$C_FFB/prefix-x264-c" || exit 1
      grep -q 'fno-tree-vectorize' config.mak && \
          sed -i.bak 's/-fno-tree-vectorize//g' config.mak
      grep -q 'fno-tree-vectorize' config.mak && \
          { echo "x264: -fno-tree-vectorize survived the strip -- the C arm would be scalar"; exit 1; }
      make -j"$JOBS" || exit 1
      cp -f x264 x264-noasm-autovec
      make install >/dev/null || exit 1 ) || return 1

    [ -x "$C_X264/x264-asm" ] && [ -x "$C_X264/x264-noasm-autovec" ] || return 1
    echo "arms: $C_X264/x264-asm  $C_X264/x264-noasm-autovec"
    return 0
}

# ffboard.py runs both encoders as LIBRARIES in one ffmpeg process, which is
# the control on the CLI board: it removes the question of whether the gap is
# partly our Y4M reader against theirs. It needs the fork, an installed
# libyah264 and an installed libx264, and it is the longest build here -- so
# it is CLOUD's by default and a named skip otherwise.
st_build_ffmpeg() {
    [ "$FFMPEG_FORK" = 1 ] || { c_skip "FFMPEG_FORK=0; ffboard.py will skip for want of the fork"; return; }
    [ -d "$C_FFB/prefix-x264-asm/lib/pkgconfig" ] || { c_skip "no installed libx264; the x264 stage did not finish"; return; }

    ( cd "$C_SRC" || exit 1
      meson setup "$CLOUD_WORK/build-shared" --buildtype=release -Dsimd=auto \
          --default-library=shared --prefix="$C_FFB/prefix-y264" >/dev/null 2>&1 \
        || meson configure "$CLOUD_WORK/build-shared" --prefix="$C_FFB/prefix-y264" || exit 1
      ninja -C "$CLOUD_WORK/build-shared" -j"$JOBS" || exit 1
      ninja -C "$CLOUD_WORK/build-shared" install || exit 1 ) || return 1

    if [ ! -d "$C_FFB/FFmpeg/.git" ]; then
        git clone --branch "$FFMPEG_BRANCH" --depth 50 "$FFMPEG_REPO" "$C_FFB/FFmpeg" || return 1
    fi
    echo "ffmpeg fork at $(git -C "$C_FFB/FFmpeg" rev-parse --short HEAD) ($FFMPEG_BRANCH)"

    local pcp="$C_FFB/prefix-y264/lib/pkgconfig:$C_FFB/prefix-x264-asm/lib/pkgconfig"
    pcp="$pcp:$C_FFB/prefix-y264/lib64/pkgconfig:$C_FFB/prefix-x264-asm/lib64/pkgconfig"
    ( cd "$C_FFB/FFmpeg" || exit 1
      PKG_CONFIG_PATH="$pcp" ./configure --prefix="$C_FFB/prefix-ffmpeg" \
          --enable-gpl --enable-libx264 --enable-libyah264 \
          --disable-doc --disable-htmlpages --disable-manpages || exit 1
      make -j"$JOBS" || exit 1 ) || return 1
    [ -x "$C_FFB/FFmpeg/ffmpeg" ] || return 1
    echo "ffmpeg: $C_FFB/FFmpeg/ffmpeg"
    return 0
}

st_oracles() {
    local rc=0
    command -v unzip >/dev/null 2>&1 \
        && { ( cd "$C_SRC" && ./scripts/fetch_jm.sh ) || rc=1; } \
        || echo "jm: no unzip on this box; the JM oracle is out (ffmpeg and openh264 remain)"
    ( cd "$C_SRC" && ./scripts/fetch_openh264.sh ) || rc=1
    [ "$rc" = 0 ] || echo "note: an oracle did not build; conformance falls back to the decoders it has"
    # Never fatal. The gate's default oracle is ffmpeg and it is in the image;
    # the other two widen the check and their absence narrows it, loudly.
    return 0
}

# ---------------------------------------------------------------------------
# 5. The correctness battery, before any measurement is taken.

st_test() {
    [ -x "$C_B8/cli/yah264" ] || { c_skip "no 8-bit build"; return; }
    ( cd "$C_SRC" && make test BUILD="$C_B8" YAH264="$C_B8/cli/yah264" ) || return 1
    return 0
}

st_conformance() {
    [ -x "$C_B8/cli/yah264" ] || { c_skip "no 8-bit build"; return; }
    local args=()
    # Full on the rented box; --fast under the rehearsal, where the box is
    # shared, the emulator is several times slower than native, and the leg is
    # being proved rather than relied on. The distinction is printed, not
    # implied.
    if [ "$CLOUD" != 1 ]; then
        args+=(--fast)
        echo "conformance: --fast (CLOUD is not 1; this is the rehearsal shape)"
    fi
    ( cd "$C_SRC" && YAH264_CONF_JOBS="$JOBS" \
        YAH264_CONF_DECODERS="${YAH264_CONF_DECODERS:-ffmpeg}" \
        ./scripts/conformance.sh ${args[@]+"${args[@]}"} "$C_B8/cli/yah264" ) || return 1
    return 0
}

st_checkasm() {
    local ck="$C_B8/tools/checkasm/checkasm"
    [ -x "$ck" ] || { c_skip "no checkasm binary"; return; }
    echo "cpu: $(c_y264_cpu_name "$ck")"
    "$ck" --isa all || return 1
    return 0
}

# SDE runs the AVX-512 rows against a MODEL of a part the box may not be, which
# is the only way to check Granite Rapids behaviour from a Sapphire Rapids
# instance and vice versa. It is an x86-host tool: on the rehearsal's ARM box
# there is nothing to skip toward, so it says so.
st_sde() {   # st_sde is parameterised through C_SDE_ARCH
    local ck="$C_B8/tools/checkasm/checkasm" sde
    [ "$CLOUD" = 1 ] || { c_skip "CLOUD is not 1; SDE runs on an x86 host only"; return; }
    [ -f "$CLOUD_WORK/sde64.path" ] || { c_skip "no sde64 (the SDE stage skipped or found none)"; return; }
    [ -x "$ck" ] || { c_skip "no checkasm binary"; return; }
    sde="$(cat "$CLOUD_WORK/sde64.path")"
    "$sde" "-$C_SDE_ARCH" -- "$ck" --isa all || return 1
    return 0
}
st_sde_spr() { C_SDE_ARCH=spr; st_sde; }
st_sde_gnr() { C_SDE_ARCH=gnr; st_sde; }

# ---------------------------------------------------------------------------
# 6. bootstrap.ok -- what campaign.sh is allowed to assume.

st_ok() {
    local ck="$C_B8/tools/checkasm/checkasm"
    {
        echo "# yah264 cloud bootstrap -- sourced by scripts/cloud/campaign.sh."
        echo "# Everything campaign.sh knows about this box it reads from here."
        echo "BOOT_DATE='$(date -u +%Y-%m-%dT%H:%M:%SZ)'"
        echo "BOOT_CLOUD='$CLOUD'"
        echo "BOOT_EMULATOR='${C_EMULATOR:-}'"
        echo "Y264_CPU_NAME='$(c_y264_cpu_name "$ck")'"
        echo "CPU_MODEL='$(c_cpu_model)'"
        echo "CPU_FLAGS='$(c_cpu_flags)'"
        echo "NPROC='$(c_nproc)'"
        echo "KERNEL='$(uname -srm)'"
        echo "CC_VERSION='$(${CC:-gcc} --version 2>/dev/null | head -1)'"
        echo "CLANG_VERSION='$(clang --version 2>/dev/null | head -1)'"
        echo "MESON_VERSION='$(meson --version 2>/dev/null)'"
        echo "FFMPEG_VERSION='$(ffmpeg -version 2>/dev/null | head -1)'"
        echo "Y264_SRC='$C_SRC'"
        echo "Y264_COMMIT='$(git -C "$C_SRC" rev-parse HEAD 2>/dev/null)'"
        echo "Y264_BUILD8='$C_B8'"
        echo "Y264_BUILD10='$C_B10'"
        echo "X264_DIR='$C_X264'"
        echo "X264_ASM='$C_X264/x264-asm'"
        echo "X264_C='$C_X264/x264-noasm-autovec'"
        echo "X264_COMMIT='$(git -C "$C_X264" rev-parse HEAD 2>/dev/null)'"
        echo "FFBOARD_FFMPEG='$C_FFB/FFmpeg/ffmpeg'"
        echo "Y264LIB='$C_FFB/prefix-y264'"
        echo "X264LIB_ASM='$C_FFB/prefix-x264-asm'"
        echo "X264LIB_C='$C_FFB/prefix-x264-c'"
        echo "VMAF_BIN='$( [ -f "$CLOUD_WORK/vmaf.path" ] && cat "$CLOUD_WORK/vmaf.path" )'"
        echo "SDE64='$( [ -f "$CLOUD_WORK/sde64.path" ] && cat "$CLOUD_WORK/sde64.path" )'"
        echo "CLOUD_WORK='$CLOUD_WORK'"
    } > "$C_OK"
    cat "$C_OK"
    return 0
}

# ---------------------------------------------------------------------------
# The run. Two stages are fatal -- there is no clone to build and no build to
# measure -- and every other failure is recorded and stepped over, so the table
# at the end is complete. A table that stops at the first problem hides the
# three behind it, and on a box that exists for four hours that costs a whole
# session.
#
# STAGES names a subset, for the case that actually happens on a rented box:
# one stage failed, it has been understood, and re-running the whole bootstrap
# to redo it would cost another hour of the session. A stage left out says so
# in the table, exactly like one that skipped itself.

ALL_STAGES="deps clone assets corpus sde-unpack build-y264 build-y264-10
            build-x264 build-vmaf build-ffmpeg oracles make-test conformance
            checkasm-all sde-spr sde-gnr bootstrap.ok"
# NORMALISED TO SINGLE SPACES FIRST, and that is not tidiness. ALL_STAGES is
# written across three lines for legibility, so the token at each line end is
# followed by a NEWLINE; `case " $STAGES " in *" $1 "*` then never matches it,
# and the stage skips as "not in STAGES" on a run that named no STAGES at all.
# The rehearsal caught it doing exactly that to build-y264-10 and conformance
# -- two real legs, silently unrunnable, each with a reason in the table that
# was untrue. campaign.sh had the same defect in ALL_LEGS.
STAGES="$(printf '%s' "${STAGES:-$ALL_STAGES}" | tr -s '[:space:]' ' ')"
want() { case " $STAGES " in *" $1 "*) return 0 ;; *) return 1 ;; esac; }

# And a name that matches nothing is a typo, not a request. Without this the
# whole run skips by name and reads like a deliberate, well-documented choice.
for _s in $STAGES; do
    case " $(printf '%s' "$ALL_STAGES" | tr -s '[:space:]' ' ') " in
        *" $_s "*) ;;
        *) echo "bootstrap: STAGES names '$_s', which is not a stage. Known:" >&2
           printf '%s\n' "$ALL_STAGES" >&2; exit 2 ;;
    esac
done

run() {   # run <stage> <fn> <estimate>
    want "$1" || { c_row "$1" SKIP - "not in STAGES"; return 0; }
    c_run_stage "$@"
}

fatal=0
run deps          st_deps           180
run clone         st_clone          120 || fatal=1
if [ "$fatal" = 0 ]; then
    run assets        st_assets         900
    run corpus        st_corpus         900
    run sde-unpack    st_sde            120
    run build-y264    st_build_y264     600 || fatal=1
fi
if [ "$fatal" = 0 ]; then
    run build-y264-10 st_build_y264_10  600
    run build-x264    st_build_x264     900
    run build-vmaf    st_build_vmaf     900
    run build-ffmpeg  st_build_ffmpeg  2400
    run oracles       st_oracles        600
    run make-test     st_test           600
    run conformance   st_conformance   2400
    run checkasm-all  st_checkasm       600
    run sde-spr       st_sde_spr        900
    run sde-gnr       st_sde_gnr        900
fi
run bootstrap.ok  st_ok              10

echo
c_summary "yah264 cloud bootstrap -- $(date -u +%Y-%m-%dT%H:%M:%SZ)" "$CLOUD_WORK/bootstrap-report.txt"

if [ "$C_FAIL" = 0 ] && [ -f "$C_OK" ]; then
    echo
    echo "bootstrap: OK. campaign.sh may run against $C_OK"
    exit 0
fi
echo
echo "bootstrap: $C_FAIL stage(s) failed; campaign.sh should not run until they are understood."
rm -f "$C_OK"
exit 1
