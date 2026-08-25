#!/usr/bin/env bash
# Copyright (c) 2026, the next264 authors
# SPDX-License-Identifier: BSD-2-Clause
#
# hygiene_check.sh -- repository hygiene, the part a machine can check.
#
# Five things that should never be true of this tree, each of which has been
# true at least once. None of them is a matter of taste; each is either a
# licensing problem or a file that will not mean anything to anyone else.
#
# Exit 0 = clean. Any finding prints its file and exits 1.
set -uo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
fail=0

# 1. Nothing under a copyleft licence. This ships BSD-2-Clause, and a single
#    GPL notice in the tree makes the whole distribution question harder than
#    it needs to be.
hits=$(grep -rliE "GNU General Public License|SPDX-License-Identifier: *(GPL|AGPL|LGPL)" \
        src include cli tools tests scripts 2>/dev/null | grep -v 'hygiene_check.sh' || true)
if [ -n "$hits" ]; then
    echo "HYGIENE: copyleft licence notice in the tree:"; printf '  %s\n' $hits; fail=1
fi

# 2. No patch or diff files. A unified diff carries its target's source in the
#    context lines, whatever it adds, so a checked-in patch quietly
#    redistributes whatever it was made against.
hits=$(git ls-files '*.patch' '*.diff' 2>/dev/null || true)
if [ -n "$hits" ]; then
    echo "HYGIENE: a patch/diff is checked in; its context lines carry someone else's source:"
    printf '  %s\n' $hits; fail=1
fi

# 3. Every source file states its own licence. LICENSE covers the repository,
#    but a file that travels on its own -- vendored, pasted into an issue, read
#    in isolation -- carries nothing without a header.
missing=""
for f in $(git ls-files '*.c' '*.h' '*.py' '*.sh'); do
    head -12 "$f" | grep -q 'SPDX-License-Identifier' || missing="$missing $f"
done
if [ -n "$missing" ]; then
    echo "HYGIENE: source file(s) with no SPDX licence header:"
    printf '  %s\n' $missing; fail=1
fi

# 4. No absolute home-directory paths. They leak the local disk layout and go
#    stale the moment a checkout moves.
hits=$(git grep -nE '/(Users|home)/[a-z]' -- . 2>/dev/null \
        | grep -v 'hygiene_check.sh' | head -10 || true)
if [ -n "$hits" ]; then
    echo "HYGIENE: absolute home-directory path in a tracked file:"
    printf '  %s\n' "$hits"; fail=1
fi

# 5. No assembly without a conversation. The project has none today and its own
#    measurement says it does not need any: the NEON intrinsics tie hand asm on
#    this target. So the day a .S appears is the day to ask where it came from
#    and who is going to maintain it. ASM_OK=1 once that has happened.
if [ "${ASM_OK:-0}" != 1 ]; then
    hits=$(find src include cli tools -name '*.S' -o -name '*.asm' 2>/dev/null || true)
    if [ -n "$hits" ]; then
        echo "HYGIENE: assembly in the tree -- confirm its origin, then ASM_OK=1:"
        printf '  %s\n' $hits; fail=1
    fi
fi

[ "$fail" = 0 ] && echo "hygiene: clean (no copyleft notice, no checked-in patch, every file licensed, no home paths, no unexplained asm)"
exit $fail
