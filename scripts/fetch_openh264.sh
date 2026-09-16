#!/usr/bin/env bash
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# fetch_openh264.sh -- fetch and build Cisco's openh264 h264dec console decoder
# into tools/openh264/, for use as a SECOND conformance oracle beside ffmpeg
# (scripts/conformance.sh, YAH264_CONF_DECODERS).
#
# WHY A SOURCE BUILD AND NOT THE PACKAGE. The Homebrew openh264 formula ships
# the library and headers only; the h264dec CLI that the conformance gate needs
# is built by the project's own Makefile and is not installed by any package on
# this box. So the gate either has no openh264 arm or builds one.
#
# NOTHING IS VENDORED. The source is downloaded to a scratch directory, pinned
# by tag AND sha256, built, and only the resulting BINARY is installed under
# tools/openh264/. No file of theirs enters the repository or the git index:
# tools/openh264/ is gitignored, and this script is the whole record of where
# the binary came from. (Their tree also carries .asm files and a non-project
# licence, both of which scripts/hygiene_check.sh refuses inside src/ include/
# cli/ tools/ -- another reason the checkout lives in scratch/.)
#
# Usage: fetch_openh264.sh [--force]
# Prints the installed binary's path on success.
set -euo pipefail

TAG="v2.6.0"
SHA256="558544ad358283a7ab2930d69a9ceddf913f4a51ee9bf1bfb9e377322af81a69"
URL="https://github.com/cisco/openh264/archive/refs/tags/$TAG.tar.gz"

root="$(cd "$(dirname "$0")/.." && pwd)"
dest="$root/tools/openh264"
bin="$dest/h264dec"
work="$root/scratch/oracles/openh264"

force=0
for a in "$@"; do
    case "$a" in
        --force) force=1 ;;
        *) echo "unknown arg: $a (usage: fetch_openh264.sh [--force])" >&2; exit 2 ;;
    esac
done

if [ "$force" = 0 ] && [ -x "$bin" ]; then
    echo "$bin"; exit 0
fi

command -v curl >/dev/null || { echo "fetch_openh264: curl required" >&2; exit 2; }
command -v make >/dev/null || { echo "fetch_openh264: make required" >&2; exit 2; }

mkdir -p "$work" "$dest"
tar="$work/openh264-$TAG.tar.gz"
if [ ! -s "$tar" ]; then
    echo "fetch_openh264: downloading $TAG"
    curl -sSL --max-time 900 -o "$tar.part" "$URL"
    mv -f "$tar.part" "$tar"
fi
# A pinned hash, not just a pinned tag: a tag can be moved, and a decoder that
# is not the decoder this gate was validated against is worse than no oracle.
have="$(shasum -a 256 "$tar" | cut -d' ' -f1)"
if [ "$have" != "$SHA256" ]; then
    echo "fetch_openh264: sha256 mismatch for $tar" >&2
    echo "  want $SHA256" >&2
    echo "  have $have" >&2
    exit 3
fi

src="$work/openh264-${TAG#v}"
rm -rf "$src"
tar -xzf "$tar" -C "$work"
[ -d "$src" ] || { echo "fetch_openh264: no $src after extract" >&2; exit 3; }

os="$(uname -s | tr '[:upper:]' '[:lower:]')"
[ "$os" = "darwin" ] || os="linux"
arch="$(uname -m)"
case "$arch" in arm64|aarch64) arch=arm64 ;; x86_64) arch=x86_64 ;; esac

echo "fetch_openh264: building h264dec ($os/$arch)"
# -j4, not -j$(ncpu): other worktrees build and measure on this box at the same
# time, and the repo rule is jobs x threads <= 18.
make -C "$src" OS="$os" ARCH="$arch" -j4 h264dec >"$work/build.log" 2>&1 || {
    echo "fetch_openh264: build failed; tail of $work/build.log:" >&2
    tail -30 "$work/build.log" >&2
    exit 4
}
[ -x "$src/h264dec" ] || { echo "fetch_openh264: no h264dec produced" >&2; exit 4; }
cp -f "$src/h264dec" "$bin"
printf '%s %s\n' "$TAG" "$SHA256" > "$dest/VERSION"
echo "$bin"
