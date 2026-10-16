#!/usr/bin/env bash
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# fetch_jm.sh -- fetch and build the JVT reference decoder (JM ldecod) into
# tools/jm/, for use as a THIRD conformance oracle beside ffmpeg and openh264
# (scripts/conformance.sh, YAH264_CONF_DECODERS).
#
# WHY THE JM. ffmpeg and openh264 are both fast production decoders that share
# an ancestry of pragmatic shortcuts; the JM is the specification written out as
# C, and it is the only decoder here that implements the whole of H.264 -- in
# particular field pictures, which openh264 refuses outright (see
# docs/instruments.md section 5). It is slow, so it is opt-in.
#
# CLEAN-ROOM: CONTRIBUTING rule 4 allows the JM as an ORACLE -- run it, compare
# its output -- and nothing else. Only ldecod is built; nothing in this tree
# reads the JM's source, and its encoder is not built at all.
#
# NOTHING IS VENDORED. The archive is downloaded to a scratch directory, pinned
# by version AND sha256, built there, and only the resulting BINARY is installed
# under tools/jm/ (gitignored). No file of theirs enters the git index.
#
# Usage: fetch_jm.sh [--force]
# Prints the installed binary's path on success.
set -euo pipefail

VER="19.0"
SHA256="5a87ec1b112423748897fb771f249ac6b7cc8a50c3b56350275857346eed9e1f"
URL="https://iphome.hhi.de/suehring/tml/download/jm$VER.zip"

root="$(cd "$(dirname "$0")/.." && pwd)"
dest="$root/tools/jm"
bin="$dest/ldecod"
work="$root/scratch/oracles/jm"

force=0
for a in "$@"; do
    case "$a" in
        --force) force=1 ;;
        *) echo "unknown arg: $a (usage: fetch_jm.sh [--force])" >&2; exit 2 ;;
    esac
done

if [ "$force" = 0 ] && [ -x "$bin" ]; then
    echo "$bin"; exit 0
fi

command -v curl  >/dev/null || { echo "fetch_jm: curl required" >&2; exit 2; }
command -v unzip >/dev/null || { echo "fetch_jm: unzip required" >&2; exit 2; }
command -v make  >/dev/null || { echo "fetch_jm: make required" >&2; exit 2; }

mkdir -p "$work" "$dest"
zip="$work/jm$VER.zip"
if [ ! -s "$zip" ]; then
    echo "fetch_jm: downloading JM $VER"
    curl -sSL --max-time 1800 -o "$zip.part" "$URL"
    mv -f "$zip.part" "$zip"
fi
have="$(shasum -a 256 "$zip" | cut -d' ' -f1)"
if [ "$have" != "$SHA256" ]; then
    echo "fetch_jm: sha256 mismatch for $zip" >&2
    echo "  want $SHA256" >&2
    echo "  have $have" >&2
    exit 3
fi

src="$work/JM"
rm -rf "$src"
unzip -q -o "$zip" -d "$work"
[ -d "$src" ] || { echo "fetch_jm: no $src after extract" >&2; exit 3; }

# The JM predates two decades of compiler tightening: it relies on implicit
# declarations, K&R-era conversions and non-prototype definitions that clang 16+
# rejects outright. These are DIAGNOSTIC relaxations only -- no source is
# touched, and the decoder's arithmetic is unchanged, which is the whole point
# of using it as an oracle.
relax="-Wno-implicit-function-declaration -Wno-int-conversion -Wno-deprecated-non-prototype"
relax="$relax -Wno-implicit-int -Wno-return-type -Wno-incompatible-pointer-types -Wno-unused-command-line-argument"

echo "fetch_jm: building ldecod"
make -C "$src" -j4 CC="${CC:-cc}" CFLAGS="-O2 $relax" ldecod >"$work/build.log" 2>&1 || {
    echo "fetch_jm: build failed; tail of $work/build.log:" >&2
    tail -30 "$work/build.log" >&2
    exit 4
}
out=""
for c in "$src/bin/ldecod.exe" "$src/bin/ldecod" "$src/ldecod/ldecod"; do
    [ -x "$c" ] && { out="$c"; break; }
done
[ -n "$out" ] || { echo "fetch_jm: no ldecod produced" >&2; exit 4; }
cp -f "$out" "$bin"
printf 'JM %s %s\n' "$VER" "$SHA256" > "$dest/VERSION"
echo "$bin"
