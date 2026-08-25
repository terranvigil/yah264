#!/usr/bin/env bash
# Copyright (c) 2026, the next264 authors
# SPDX-License-Identifier: BSD-2-Clause
#
# provenance_check.sh -- the automated half of CONTRIBUTING.md's clean-room policy.
#
# next264 ships BSD-2-Clause and must not become a derivative work of any
# GPL encoder. Most of that policy is a judgement a human makes while writing;
# this checks the part a machine can: that no GPL notice, no upstream
# attribution string, and no assembly file has appeared in the shipped tree.
#
# Assembly is included deliberately even though hand asm is ALLOWED by the
# engineering rules. The project has zero .S files today and its own measurement
# says it does not need them -- our intrinsics tie x264's hand asm on this target
# (3.68 vs 3.66 ns on satd8x8, asm-campaign round 9). So the day one appears is
# the day someone should be asked where it came from. Set ASM_OK=1 to allow it
# once that conversation has happened.
#
# Exit 0 = clean. Any finding is printed with its file and exits 1.
set -uo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
TREE="src include cli tools tests/*.c"
fail=0

hits=$(grep -rliE "GNU General Public License|SPDX-License-Identifier: *GPL" \
        src include cli tools 2>/dev/null || true)
if [ -n "$hits" ]; then
    echo "PROVENANCE: GPL notice in the shipped tree:"; printf '  %s\n' $hits; fail=1
fi

# Upstream attribution strings. Prose that merely NAMES x264 is fine and common
# (the docs and comments compare against it constantly); a copyright line or an
# authorship credit is not.
hits=$(grep -rlE "Copyright .*(x264|x265|VideoLAN)|Authors: .*(Loren Merritt|Laurent Aimar|Fiona Glaser)" \
        src include cli tools 2>/dev/null || true)
if [ -n "$hits" ]; then
    echo "PROVENANCE: upstream attribution in the shipped tree:"; printf '  %s\n' $hits; fail=1
fi

if [ "${ASM_OK:-0}" != 1 ]; then
    hits=$(find src include cli tools -name '*.S' -o -name '*.asm' 2>/dev/null || true)
    if [ -n "$hits" ]; then
        echo "PROVENANCE: assembly in the tree -- confirm it was written here, then ASM_OK=1:"
        printf '  %s\n' $hits; fail=1
    fi
fi

# Nothing we ship may LINK against the reference tree. Prose that names x264 is
# fine and pervasive -- the comments compare against it constantly, and rule 4
# lets our public enums pin x264's numeric values so a ported constant selects
# the tool it names (src/encoder/params.c does exactly that, in static asserts).
# So match an actual include or link, never a mention.
hits=$(grep -rlE "^[[:space:]]*#[[:space:]]*include[[:space:]]*[<\"].*x264|-lx264" \
        src include cli tools 2>/dev/null || true)
hits="$hits $(grep -rlE "\.\./x264|-lx264" --include='meson.build' . 2>/dev/null | grep -v '/build' || true)"
hits=$(echo $hits)
if [ -n "$hits" ]; then
    echo "PROVENANCE: a shipped target includes or links the x264 tree:"; printf '  %s\n' $hits; fail=1
fi


# ---------------------------------------------------------------------------
# Added after the 2026-08-25 pre-publication audit. The four checks above look
# only at src/include/cli/tools, and the audit found the real exposure was
# everywhere else: verbatim reference source inside fenced blocks in docs/, and
# 800 lines of it inside .patch files under scripts/. Rule 6 says nothing
# derived from a reference tree is checked in; these make that machine-checkable.

# 6a. No patch file anywhere in the tree. A unified diff against a GPL tree
#     carries that tree's source in its context lines, whatever it adds.
hits=$(git ls-files '*.patch' '*.diff' 2>/dev/null || true)
if [ -n "$hits" ]; then
    echo "PROVENANCE: a patch/diff is checked in -- its context lines are upstream source:"
    printf '  %s\n' $hits
    echo "  (measurement patches belong outside the repo; see docs/instruments.md section 6)"
    fail=1
fi

# 6b. No fenced code block carrying another encoder's internals. Their struct
#     arrows and symbol prefixes are the tell; prose that merely names them is
#     fine and pervasive.
hits=$(git ls-files '*.md' | while read -r f; do
    awk -v F="$f" '
        /^```/ { infence = !infence; if (infence) { hits=0; start=NR } else if (hits>=2) print F":"start; next }
        infence && (/x264_[a-z_]/ || /(^|[^A-Za-z_0-9])(h|rcc|rce|cb|a|m)->/ || /fenc->/ || /fdec->/) { hits++ }
    ' "$f"
done)
if [ -n "$hits" ]; then
    echo "PROVENANCE: fenced block(s) containing reference-encoder internals:"
    printf '  %s\n' $hits; fail=1
fi

# 6c. Copying verbs and source citations. A line number only exists relative to
#     a checkout, so citing one asserts a tree was open while this was written.
#     Describe the behaviour instead; the technical fact survives, the claim does not.
hits=$(git grep -niE "(direct |a )?port(ed)? (of|from) x264|x264-faithful|transliterat|copied (from|verbatim) x264" \
        -- src include cli tools tests bench docs scripts 2>/dev/null \
        | grep -viE "(do|does|did|not|never|cannot|can.t)[a-z ']* port" \
        | grep -v '^scripts/provenance_check.sh:' | head -40 || true)
if [ -n "$hits" ]; then
    echo "PROVENANCE: copying verbs in the tree (describe behaviour, do not claim a port):"
    printf '  %s\n' "$hits"; fail=1
fi

hits=$(git grep -nE "(^|[^/[:alnum:]_])(common|encoder|filters|input|output)/[a-z_0-9]+\.(c|h):[0-9]+" \
        -- src include cli tools tests bench docs 2>/dev/null \
        | grep -vE "src/(common|encoder|dsp)/" | head -40 || true)
if [ -n "$hits" ]; then
    echo "PROVENANCE: citation to a reference tree's source file AND line:"
    printf '  %s\n' "$hits"; fail=1
fi

# 6d. No shipped identifier may carry another encoder's name into the binary.
hits=$(git grep -nE "(N264|n264)_[A-Za-z_]*X264|[a-z_]+_x264\b|\bx264[a-z_]*mode\b" \
        -- src include cli 2>/dev/null | head -30 || true)
if [ -n "$hits" ]; then
    echo "PROVENANCE: shipped identifier carries a reference encoder's name:"
    printf '  %s\n' "$hits"; fail=1
fi

# 6e. Every source file states its own licence. The LICENSE file covers the
#     repository, but a file that travels on its own -- vendored, pasted into an
#     issue, read in isolation -- carries no provenance without a header. For a
#     project whose whole claim is provenance, an unlabelled file is a question
#     mark, so this is enforced rather than encouraged.
missing=""
for f in $(git ls-files '*.c' '*.h' '*.py' '*.sh'); do
    head -12 "$f" | grep -q 'SPDX-License-Identifier' || missing="$missing $f"
done
if [ -n "$missing" ]; then
    echo "PROVENANCE: source file(s) with no SPDX licence header:"
    printf '  %s\n' $missing; fail=1
fi

# 6f. No comment may name another encoder's INTERNAL symbol. Their public API
#     constants are a different matter and are allowed: include/next264.h
#     documents which of our enum values match theirs so a caller porting an
#     integration knows what carries over, and that is interoperability, not
#     derivation. An internal name is not -- it can only have come from reading
#     their source, and it tells a reader that is where ours came from.
#     Allowlist is deliberately narrow: X264_* public header constants only.
hits=$(git grep -nE '\b(x264|x265)_[a-z][a-z_0-9]+' -- src include cli 2>/dev/null \
        | grep -viE 'x264_config|X264_[A-Z]' | head -20 || true)
if [ -n "$hits" ]; then
    echo "PROVENANCE: comment names another encoder's internal symbol:"
    printf '  %s\n' "$hits"; fail=1
fi

# 6g. No tracked file may publish a developer's home directory. It leaks the
#     local disk layout and it goes stale the moment a checkout moves.
hits=$(git grep -nE '/(Users|home)/[a-z]' -- . 2>/dev/null | grep -v 'provenance_check.sh' | head -10 || true)
if [ -n "$hits" ]; then
    echo "PROVENANCE: absolute home-directory path in a tracked file:"
    printf '  %s\n' "$hits"; fail=1
fi

[ "$fail" = 0 ] && echo "provenance: clean (no GPL notice/attribution/asm/linkage, no checked-in patch, no verbatim block, no port claim, no cited line, no borrowed identifier)"
exit $fail
