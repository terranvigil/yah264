#!/bin/sh
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Symbol check: the two encoder libraries must share no name.
#
# Item C3-10bit. One command line links BOTH archives, and a name defined in
# both is not a link error - the linker resolves it from whichever archive it
# scanned first, so a 10-bit object can end up calling 8-bit code and the only
# symptom is wrong pixels. That failure has no other tripwire, so it gets this
# one.
#
# The 10-bit library takes a _10 suffix on every symbol it defines: the public
# API through include/yah264.h (YAH264_API), the internals through
# src/common/symbols10.h, force-included by meson. A new non-static internal
# symbol that nobody adds to that header fails HERE. Regenerate the header with
# scripts/gen_symbols10.py.
#
# usage: scripts/symbol_check.sh [builddir...]   (default: build)
# env: NM=path/to/nm

set -u

NM=${NM:-nm}
dirs=$*
[ -n "$dirs" ] || dirs="build"

syms() {
    # -g is portable; --defined-only is not, so drop U lines by hand.
    # Apple's nm prefixes an underscore, GNU's does not.
    "$NM" -g "$1" 2>/dev/null |
        awk 'NF == 3 && $2 != "U" { print $3 }' |
        sed 's/^_//' | sort -u
}

rc=0
for d in $dirs; do
    [ -d "$d" ] || continue
    set -- "$d"/libyah264*.a
    [ -f "$1" ] || { echo "symbol_check: $d has no encoder archive" >&2
                     rc=1; continue; }
    n=0
    for a in "$@"; do n=$((n + 1)); done
    if [ "$n" -lt 2 ]; then
        echo "symbol_check: $d builds only one library ($1); the one CLI" >&2
        echo "symbol_check: needs both depths - meson.build regressed" >&2
        rc=1
        continue
    fi
    for a in "$@"; do
        for b in "$@"; do
            [ "$a" \< "$b" ] || continue
            syms "$a" > "$d/.syma"
            syms "$b" > "$d/.symb"
            shared=$(comm -12 "$d/.syma" "$d/.symb")
            rm -f "$d/.syma" "$d/.symb"
            if [ -n "$shared" ]; then
                echo "symbol_check: FAIL $a and $b define the same names:" >&2
                echo "$shared" | sed 's/^/  /' >&2
                echo "symbol_check: regenerate src/common/symbols10.h" >&2
                echo "symbol_check: (scripts/gen_symbols10.py)" >&2
                rc=1
            else
                echo "symbol_check: OK $(basename "$a") x $(basename "$b")"
            fi
        done
    done
done

exit $rc
