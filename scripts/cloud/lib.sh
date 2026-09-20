# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# lib.sh -- the machinery bootstrap.sh and campaign.sh share. Sourced, never
# run.
#
# It exists because the two scripts have to agree, exactly, about four things,
# and a rule duplicated in two files is a rule that drifts on the first edit:
#
#   1. WHAT A STAGE IS. Every stage is timed, logged to its own file, and lands
#      in one table as PASS, SKIP, FAIL or REHEARSED -- so a leg that did not
#      run is VISIBLE as a named skip rather than as an absence. The whole
#      point of the rehearsal is to read that table before a paid hour is
#      spent, and a table that omits what it could not do cannot be read.
#   2. WHERE THE BUCKET IS. file:// for the rehearsal, gs:// / s3:// / https://
#      for the real thing. Nothing here has a bucket for a default; the URL
#      arrives in the environment or the fetch stages skip by name.
#   3. WHETHER A CLOCK MAY BE READ. See can_time() below. This is the rule the
#      kit is most likely to break by accident and the one that would quietly
#      invalidate everything downstream of it.
#   4. HOW THE WALL BUDGET ENDS A RUN. A rented box is billed by the second and
#      a run that overshoots its budget with the results still on its local
#      disk has bought nothing. Every stage asks the guard first.
#
# shellcheck shell=bash

# ---------------------------------------------------------------------------
# Small portable helpers.

c_sha256() {   # c_sha256 <file> -> the hex digest
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
    else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

c_now() { date +%s; }

c_hms() {      # c_hms <seconds> -> h:mm:ss
    local s="$1"
    printf '%d:%02d:%02d' $((s / 3600)) $(((s % 3600) / 60)) $((s % 60))
}

c_slug() {     # c_slug <string> -> a safe directory name
    printf '%s' "$1" | tr '[:upper:]' '[:lower:]' | tr -cs 'a-z0-9.-' '-' | sed 's/^-*//; s/-*$//'
}

# ---------------------------------------------------------------------------
# The result table.
#
# A row carries its own status, so the summary is assembled from the rows
# rather than from a running commentary. C_FAIL is what the exit status is
# read off; C_SKIP is never an error, which is the contract the brief sets for
# every leg that cannot run where it is.

C_ROWS=()
C_PASS=0; C_FAIL=0; C_SKIP=0; C_REHEARSED=0
C_SKIP_REASON=""

c_row() {      # c_row <stage> <status> <seconds|-> <detail>
    C_ROWS+=("$(printf '%-26s %-9s %9s  %s' "$1" "$2" "$3" "${4:-}")")
    case "$2" in
        PASS)      C_PASS=$((C_PASS + 1)) ;;
        FAIL)      C_FAIL=$((C_FAIL + 1)) ;;
        SKIP)      C_SKIP=$((C_SKIP + 1)) ;;
        REHEARSED) C_REHEARSED=$((C_REHEARSED + 1)) ;;
    esac
    printf '  %-26s %-9s %9s  %s\n' "$1" "$2" "$3" "${4:-}"
}

# A stage function returns 0 for done, 77 for "cannot run here", anything else
# for a failure. 77 is automake's convention and is used here for the same
# reason: a skip has to be a first-class outcome of the work, not a branch the
# caller took before the work started, because only the stage itself knows that
# sde64 is missing or that the CPU has no AVX-512.
c_skip() { C_SKIP_REASON="$1"; return 77; }

# c_rehearsed marks a leg whose PLUMBING ran but whose numbers are not a
# measurement -- see can_time(). It is not a skip: the command really did
# execute end to end, which is exactly what the rehearsal is for.
C_REHEARSED_NOTE=""
c_rehearsal_note() { C_REHEARSED_NOTE="$1"; }

c_run_stage() {  # c_run_stage <name> <fn> [estimated-seconds]
    local name="$1" fn="$2" est="${3:-0}" log t0 t1 rc
    log="$C_LOGS/$name.log"
    mkdir -p "$C_LOGS"

    if ! c_budget_ok "$est"; then
        c_row "$name" SKIP - "wall budget: $(c_hms "$(c_budget_left)") left, stage wants ~$(c_hms "$est")"
        C_BUDGET_HIT=1
        return 0
    fi

    C_SKIP_REASON=""; C_REHEARSED_NOTE=""
    echo ">> $name  ($(date -u +%H:%M:%SZ))"
    t0=$(c_now)
    { "$fn"; } >"$log" 2>&1
    rc=$?
    t1=$(c_now)
    local dt=$((t1 - t0))

    case "$rc" in
        0)
            if [ -n "$C_REHEARSED_NOTE" ]; then
                c_row "$name" REHEARSED "$(c_hms "$dt")" "$C_REHEARSED_NOTE"
            else
                c_row "$name" PASS "$(c_hms "$dt")" "$name.log"
            fi
            return 0 ;;
        77)
            c_row "$name" SKIP "$(c_hms "$dt")" "${C_SKIP_REASON:-no reason given}"
            return 0 ;;
        *)
            c_row "$name" FAIL "$(c_hms "$dt")" "rc=$rc, $name.log"
            echo "---- tail of $log ----" >&2
            tail -40 "$log" >&2
            echo "----------------------" >&2
            return 1 ;;
    esac
}

c_summary() {   # c_summary <title> <file>
    {
        echo "$1"
        echo
        printf '%-26s %-9s %9s  %s\n' stage status wall detail
        printf '%-26s %-9s %9s  %s\n' -------------------------- --------- --------- ------
        printf '%s\n' ${C_ROWS[@]+"${C_ROWS[@]}"}
        echo
        echo "pass $C_PASS   fail $C_FAIL   skip $C_SKIP   rehearsed $C_REHEARSED"
        [ "${C_BUDGET_HIT:-0}" = 1 ] && echo "PARTIAL: the wall budget ended the run; skipped stages above say so."
        if ! c_can_time; then
            echo
            echo "NO TIMING. $C_NO_TIME_WHY"
            echo "Every leg above marked REHEARSED proves the kit runs. None of them"
            echo "is a speed number and none may be quoted as one."
        fi
    } | tee "$2"
}

# ---------------------------------------------------------------------------
# The wall budget.
#
# WALL_BUDGET_H is hours, fractional allowed. Unset means no budget and no
# guard. The deadline is absolute, fixed once at c_budget_start, so a stage
# that overruns eats the NEXT stage's room rather than sliding the end.

C_DEADLINE=0
C_BUDGET_HIT=0

c_budget_start() {
    if [ -n "${WALL_BUDGET_H:-}" ]; then
        C_DEADLINE=$(awk -v n="$(c_now)" -v h="$WALL_BUDGET_H" 'BEGIN{printf "%d", n + h * 3600}')
        echo "wall budget ${WALL_BUDGET_H}h; deadline $(date -u -r "$C_DEADLINE" +%H:%M:%SZ 2>/dev/null || date -u -d "@$C_DEADLINE" +%H:%M:%SZ 2>/dev/null)"
    fi
}

c_budget_left() {
    [ "$C_DEADLINE" = 0 ] && { echo 999999; return; }
    local left=$((C_DEADLINE - $(c_now)))
    [ "$left" -lt 0 ] && left=0
    echo "$left"
}

# A stage runs only if its estimate fits in what is left, with C_BUDGET_RESERVE
# held back for the push. The reserve is the point: a budget spent down to zero
# leaves the tarball on a disk that is about to be deleted.
C_BUDGET_RESERVE="${C_BUDGET_RESERVE:-300}"
c_budget_ok() {   # c_budget_ok <estimated-seconds>
    [ "$C_DEADLINE" = 0 ] && return 0
    local left; left=$(c_budget_left)
    [ "$left" -gt $(( ${1:-0} + C_BUDGET_RESERVE )) ]
}

# ---------------------------------------------------------------------------
# Where we are, and whether a clock here means anything.
#
# THE RULE (docs/x86-plan.md; the owner's box rule). Under an emulator a wall
# time is the EMULATOR's, and an instruction count is the emulator's too. The
# rehearsal proves the kit runs; it proves nothing about how fast anything is.
# So the kit does not decide this per leg or per reviewer: CLOUD=1 asserts real
# silicon, the detector below looks for an emulator anyway, and a clock is read
# only when both agree.
#
# Detection, in order of how much it is trusted:
#   - /run/rosetta or a rosetta binfmt entry: Docker Desktop's Rosetta.
#   - a qemu-x86_64 binfmt entry.
#   - /proc/cpuinfo with no "model name", or one naming Apple or QEMU:
#     qemu-user passes the HOST's cpuinfo straight through, so on Apple silicon
#     that file describes an Apple core.
#   - no "cpu MHz" line at all.
# An inconclusive answer counts as emulated, because the cost of wrongly
# believing a number is far higher than the cost of not printing one.

C_EMULATOR=""
C_NO_TIME_WHY=""

c_detect_emulator() {
    C_EMULATOR=""
    if [ -e /run/rosetta ] || [ -e /proc/sys/fs/binfmt_misc/rosetta ]; then
        C_EMULATOR="rosetta"
    elif [ -e /proc/sys/fs/binfmt_misc/qemu-x86_64 ] || [ -n "${QEMU_CPU:-}" ]; then
        C_EMULATOR="qemu"
    elif [ -r /proc/cpuinfo ]; then
        local model
        model="$(awk -F': ' '/^model name/{print $2; exit}' /proc/cpuinfo)"
        case "$model" in
            "")            C_EMULATOR="unknown (no model name in /proc/cpuinfo)" ;;
            *Apple*)       C_EMULATOR="qemu (cpuinfo is the host's: $model)" ;;
            *QEMU*|*Virtual\ CPU*) C_EMULATOR="qemu ($model)" ;;
        esac
        grep -q '^cpu MHz' /proc/cpuinfo || \
            [ -n "$C_EMULATOR" ] || C_EMULATOR="unknown (no cpu MHz line)"
    else
        C_EMULATOR="unknown (no /proc/cpuinfo)"
    fi
}

c_can_time() {
    if [ "${CLOUD:-0}" != 1 ]; then
        C_NO_TIME_WHY="CLOUD is not 1, so this is the rehearsal and not the rented box."
        return 1
    fi
    if [ -n "$C_EMULATOR" ] && [ "${ALLOW_EMULATED_TIMING:-0}" != 1 ]; then
        C_NO_TIME_WHY="CLOUD=1 was set but an emulator was detected ($C_EMULATOR). Refusing to read a clock."
        return 1
    fi
    C_NO_TIME_WHY=""
    return 0
}

# The number of frames / repeats a leg uses. Under the rehearsal every timed
# leg shrinks to the smallest shape that still exercises every line of it,
# because the only thing being proved is that the plumbing runs.
c_shape() {   # c_shape <cloud-value> <rehearsal-value>
    if c_can_time; then echo "$1"; else echo "$2"; fi
}

# ---------------------------------------------------------------------------
# The bucket.
#
# One-time staging puts the corpus, the JM and openh264 archives, the SDE
# tarball and a SHA256SUMS manifest in a private bucket in the campaign's own
# region; the rehearsal points BUCKET_URL at a local directory instead and the
# same four functions serve both. NO DEFAULT: a bucket name in this tree is a
# hygiene failure and, worse, a standing invitation to read someone else's.

c_bucket_kind() {
    case "${BUCKET_URL:-}" in
        "")          echo none ;;
        file://*)    echo file ;;
        gs://*)      echo gs ;;
        s3://*)      echo s3 ;;
        http://*|https://*) echo http ;;
        /*)          echo file ;;
        *)           echo unknown ;;
    esac
}

c_bucket_base() {
    local u="${BUCKET_URL:-}"
    u="${u#file://}"
    printf '%s' "${u%/}"
}

c_bucket_get() {   # c_bucket_get <name> <dest> -> 0 got it, 1 not there
    local name="$1" dest="$2" base; base="$(c_bucket_base)"
    mkdir -p "$(dirname "$dest")"
    case "$(c_bucket_kind)" in
        file) [ -f "$base/$name" ] || return 1
              cp -f "$base/$name" "$dest" ;;
        gs)   command -v gcloud >/dev/null 2>&1 || return 1
              gcloud storage cp "$base/$name" "$dest" >/dev/null 2>&1 || return 1 ;;
        s3)   command -v aws >/dev/null 2>&1 || return 1
              aws s3 cp "$base/$name" "$dest" >/dev/null 2>&1 || return 1 ;;
        http) curl -fsSL --max-time 3600 -o "$dest.part" "$base/$name" || return 1
              mv -f "$dest.part" "$dest" ;;
        *)    return 1 ;;
    esac
    [ -s "$dest" ]
}

c_bucket_put() {   # c_bucket_put <local-file> [remote-name]
    local f="$1" name="${2:-$(basename "$1")}" base; base="$(c_bucket_base)"
    case "$(c_bucket_kind)" in
        file) mkdir -p "$base" && cp -f "$f" "$base/$name" ;;
        gs)   command -v gcloud >/dev/null 2>&1 && gcloud storage cp "$f" "$base/$name" >/dev/null ;;
        s3)   command -v aws >/dev/null 2>&1 && aws s3 cp "$f" "$base/$name" >/dev/null ;;
        *)    echo "bucket: no writable bucket configured; $f stays on local disk"; return 1 ;;
    esac
}

# ---------------------------------------------------------------------------
# The sha256 list, read out of the tree rather than restated here.
#
# scripts/fetch_corpus.sh carries a hash per clip, scripts/fetch_jm.sh and
# scripts/fetch_openh264.sh one each for their archive. Those are the project's
# pinned hashes and this reads them from the scripts, so re-pinning a clip in
# one place re-pins it here. Most corpus rows are "-" on purpose (nothing has
# verified them yet), and the bucket's own SHA256SUMS covers what those cannot:
# what is left over is printed UNPINNED, by name. Never silently.

# The clip rows are `"<name>  <url>  <sha256-or-dash>  <class>"`, and the url
# field starts with the script's own $base variable rather than with http, so
# the hash is found by SHAPE -- the one field of exactly 64 hex characters --
# and not by position. length() rather than a {64} interval, because interval
# expressions are not portable across every awk this runs under.
#
# The two archives are named as they are in the BUCKET, which means reading
# VER and TAG out of their fetch scripts: a hash filed under a name nothing
# looks up is a check that silently never runs, which is the failure this
# whole function exists to avoid.
c_pinned_hashes() {   # -> "<name> <sha256>" lines, from the fetch scripts
    local s="$C_SRC/scripts" ver tag
    awk '/^[[:space:]]*"[A-Za-z0-9_]+[[:space:]]/ {
             gsub(/"/, "")
             for (i = 2; i <= NF; i++)
                 if (length($i) == 64 && $i ~ /^[0-9a-f]+$/) { print $1 ".y4m", $i; break }
         }' "$s/fetch_corpus.sh" 2>/dev/null
    ver="$(awk -F'"' '/^VER=/{print $2; exit}' "$s/fetch_jm.sh" 2>/dev/null)"
    tag="$(awk -F'"' '/^TAG=/{print $2; exit}' "$s/fetch_openh264.sh" 2>/dev/null)"
    awk -F'"' -v n="${JM_ARCHIVE:-jm$ver.zip}" \
        '/^SHA256=/ { print n, $2 }' "$s/fetch_jm.sh" 2>/dev/null
    awk -F'"' -v n="${OPENH264_ARCHIVE:-openh264-$tag.tar.gz}" \
        '/^SHA256=/ { print n, $2 }' "$s/fetch_openh264.sh" 2>/dev/null
}

# c_verify <file> <manifest-or-empty> <logical-name>
# The manifest is a plain "sha256  name" list as sha256sum writes one.
c_verify() {
    local f="$1" manifest="$2" name="${3:-$(basename "$1")}" want="" have
    if [ -n "$manifest" ] && [ -f "$manifest" ]; then
        want="$(awk -v n="$name" '$2 == n || $2 == "*"n {print $1; exit}' "$manifest")"
    fi
    if [ -z "$want" ]; then
        want="$(c_pinned_hashes | awk -v n="$name" '$1 == n {print $2; exit}')"
    fi
    if [ -z "$want" ]; then
        echo "  UNPINNED  $name  (no hash in the bucket manifest and none in the fetch scripts)"
        return 0
    fi
    have="$(c_sha256 "$f")"
    if [ "$have" != "$want" ]; then
        echo "  MISMATCH  $name"
        echo "    want $want"
        echo "    have $have"
        return 1
    fi
    echo "  ok        $name"
    return 0
}

# ---------------------------------------------------------------------------
# What the machine says it is.
#
# Two sources, and they are not interchangeable. /proc/cpuinfo is what the
# kernel says, and under qemu-user it is the HOST's file and describes an Apple
# core. The binary's own CPUID, printed by checkasm's banner, is what the
# encoder will actually dispatch on. Where they disagree the binary wins, and
# the disagreement itself is worth printing.

c_cpu_model() {
    awk -F': ' '/^model name/{print $2; exit}' /proc/cpuinfo 2>/dev/null || echo unknown
}

c_cpu_flags() {
    awk '/^flags/{ $1=""; $2=""; print; exit }' /proc/cpuinfo 2>/dev/null \
      | tr ' ' '\n' | grep -E '^(sse4_2|avx|avx2|avx512[a-z]*|fma|bmi[12]|avx_vnni|avx512_vnni)$' \
      | sort -u | tr '\n' ' '
}

c_nproc() { getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1; }

# The encoder's own view: `checkasm --list` prints "checkasm: cpu <names>".
c_y264_cpu_name() {   # c_y264_cpu_name <checkasm-binary>
    [ -x "$1" ] || { echo unknown; return; }
    "$1" --list 2>/dev/null | head -1 | sed 's/^checkasm: *//'
}

c_has_avx512() {      # c_has_avx512 <checkasm-binary>
    case "$(c_y264_cpu_name "$1") $(c_cpu_flags)" in *avx512*) return 0 ;; esac
    return 1
}
