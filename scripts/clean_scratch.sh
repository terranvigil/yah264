#!/usr/bin/env bash
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# clean_scratch.sh -- sweep harness scratch that outlived its run. `make
# clean-scratch` is the front door.
#
# The traps in scripts/scratch.sh and scripts/scratch.py cover every ordinary
# exit, including Ctrl-C and a TERM from a killed agent. They cannot cover a
# SIGKILL, a panic or a power cut, and two weeks of those put 409 GB under
# $TMPDIR on 2026-09-19 and filled the data volume. This is for those.
#
# What it will touch, and nothing else:
#   * $TMPDIR entries matching a prefix in scripts/scratch-prefixes.txt
#   * that are directories
#   * last modified more than DAYS days ago (default 1)
# It never looks outside $TMPDIR, never follows a symlink out, and never
# touches the named cache ($Y264_CACHE), which is where the things that are
# meant to survive a run live.
#
#   DRY=1   list, remove nothing
#   DAYS=7  only entries older than a week
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
prefixes="$here/scratch-prefixes.txt"
tmp="${TMPDIR:-/tmp}"
tmp="${tmp%/}"
days="${DAYS:-1}"; case "$days" in ''|*[!0-9]*) days=1 ;; esac; [ "$days" -ge 1 ] || days=1
dry="${DRY:-0}"; [ -n "$dry" ] || dry=0

[ -f "$prefixes" ] || { echo "clean-scratch: no $prefixes" >&2; exit 2; }
[ -d "$tmp" ] || { echo "clean-scratch: no temp directory at $tmp" >&2; exit 2; }

# One glob per prefix: the live namespace plus the names used before it
# existed. A prefix shorter than three characters would be a net, not a name.
globs=()
while read -r kind value _; do
    case "$kind" in
        ns|legacy) ;;
        *) continue ;;
    esac
    [ ${#value} -ge 3 ] || { echo "clean-scratch: refusing prefix '$value' (too short)" >&2; exit 2; }
    globs+=("$value*")
done < <(grep -v '^[[:space:]]*#' "$prefixes" | grep -v '^[[:space:]]*$')

[ ${#globs[@]} -gt 0 ] || { echo "clean-scratch: no prefixes listed in $prefixes" >&2; exit 2; }

# -maxdepth 1 so a scratch tree is removed whole and nothing below $TMPDIR's
# own children is ever considered; -mtime +N-1 is BSD find's "older than N
# days"; the name tests are OR-ed inside one group.
args=(-maxdepth 1 -type d "(")
first=1
for g in "${globs[@]}"; do
    [ $first -eq 1 ] || args+=(-o)
    args+=(-name "$g")
    first=0
done
args+=(")" -mtime "+$((days - 1))")

found=0; freed=0
while IFS= read -r d; do
    [ -n "$d" ] || continue
    [ "$d" = "$tmp" ] && continue
    kb=$(du -sk "$d" 2>/dev/null | awk '{print $1}'); [ -n "$kb" ] || kb=0
    printf '  %8.1f MB  %s\n' "$(echo "$kb" | awk '{print $1/1024}')" "$d"
    found=$((found + 1)); freed=$((freed + kb))
    [ "$dry" = "1" ] || rm -rf -- "$d"
done < <(find "$tmp" "${args[@]}" -print 2>/dev/null)

verb="removed"; [ "$dry" = "1" ] && verb="would remove"
if [ "$found" -eq 0 ]; then
    echo "clean-scratch: nothing older than $days day(s) under $tmp"
else
    printf 'clean-scratch: %s %d director%s, %.2f GB\n' \
        "$verb" "$found" "$([ "$found" -eq 1 ] && echo y || echo ies)" \
        "$(echo "$freed" | awk '{print $1/1048576}')"
fi
