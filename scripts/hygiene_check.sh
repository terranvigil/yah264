#!/usr/bin/env bash
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
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

# 1. Nothing under anyone else's licence. This ships GPL-2.0-or-later with a
#    commercial licence beside it, and the clean-room rule is about provenance,
#    not compatibility: a file carrying another SPDX id, or a licence preamble
#    pasted in from elsewhere, is code that came from somewhere else.
hits=$(grep -rlE "SPDX-License-Identifier: *[A-Za-z0-9.+-]+" \
        src include cli tools tests scripts 2>/dev/null | grep -v 'hygiene_check.sh' \
      | xargs grep -LE "SPDX-License-Identifier: *GPL-2.0-or-later" 2>/dev/null || true)
hits="$hits $(grep -rliE "GNU (General|Lesser|Affero) (General )?Public License" \
        src include cli tools tests scripts 2>/dev/null | grep -v 'hygiene_check.sh' || true)"
hits="$(echo $hits)"
if [ -n "$hits" ]; then
    echo "HYGIENE: a file under a licence other than the project's:"; printf '  %s\n' $hits; fail=1
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

# 6. No inline assembly in the x86 kernels. The project's SIMD is C11
#    intrinsics on every architecture, and the x86 tiers are where that rule is
#    easiest to break: the idiom is everywhere in the field, and an __asm__
#    block reads as a small local shortcut. It is not one. An intrinsic is
#    typed, the compiler schedules it around the code either side, and
#    checkasm's page guard means something against it; an asm block is none of
#    those, and item 5's origin question applies to it word for word. The
#    CPUID/xgetbv probe in src/common/cpu.c is outside this directory on
#    purpose and stays legal.
if [ "${ASM_OK:-0}" != 1 ]; then
    hits=$(grep -rln '__asm__\|asm volatile' src/dsp/x86 2>/dev/null || true)
    if [ -n "$hits" ]; then
        echo "HYGIENE: inline asm under src/dsp/x86 -- the x86 kernels are C11 intrinsics:"
        printf '  %s\n' $hits; fail=1
    fi
fi

# 7. No -march= in a meson file. The per-tier flags are -m<feature>
#    (-msse4.2; -mavx2 -mfma -mbmi2; -mavx512f -mavx512bw -mavx512vl), which
#    say exactly what a translation unit may emit and nothing about what it
#    should be tuned for. -march= raises the tuning to one vendor's part as
#    well, which on a tier meant to run everywhere is a portability claim made
#    by accident; and applied a level too high it turns a baseline dispatching
#    file into one that faults before it can dispatch.
hits=$(git ls-files '*meson.build' '*meson_options.txt' 2>/dev/null \
        | xargs grep -nE '\-march=' 2>/dev/null | head -10 || true)
if [ -n "$hits" ]; then
    echo "HYGIENE: -march= in a meson file -- the per-tier flags are -m<feature>:"
    printf '  %s\n' "$hits"; fail=1
fi

[ "$fail" = 0 ] && echo "hygiene: clean (no foreign licence, no checked-in patch, every file licensed, no home paths, no unexplained asm, none under src/dsp/x86, no -march=)"
exit $fail
