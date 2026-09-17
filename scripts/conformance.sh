#!/usr/bin/env bash
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# conformance.sh - encode with yah264, decode with an independent decoder, and
# assert the encoder's own reconstruction matches the decoder's output exactly
# (recon-match). This is the Phase 1 gate and runs in CI. ffmpeg's native H.264
# decoder is the default oracle; openh264 and the JVT reference decoder (JM
# ldecod) join it on request.
#
# MORE THAN ONE DECODER, AND WHY. ffmpeg is one implementation, and a recon that
# matches it matches one reading of the specification. YAH264_CONF_DECODERS
# names the set:
#
#   YAH264_CONF_DECODERS="ffmpeg openh264 jm" scripts/conformance.sh --fast
#
# Each decoder runs on every clip it is CAPABLE of, and the run prints what each
# one actually checked -- because the failure mode of a multi-oracle gate is a
# decoder that quietly checks nothing and still reads green. openh264 is the
# narrow one: measured on 2.6.0 it refuses frame_mbs_only_flag == 0 outright,
# writes nothing for 4:2:2 or 4:4:4, and -- the dangerous one -- MIS-DECODES B
# SLICES WHILE EXITING 0, so cells with B frames are skipped by stream property,
# not by trusting its exit status. The JM decodes everything here, interlaced
# included, which is why it is the second oracle for the field-picture work.
# Both binaries come from scripts/fetch_openh264.sh and scripts/fetch_jm.sh;
# neither is vendored.
#
# The encoder is lossy from Phase 1 on, so the decode does not equal the input;
# what must be bit-exact is decode == encoder reconstruction (via --dump-recon).
# Every clip is tested across a range of QPs.
#
# The checks are independent, so they run in parallel (xargs -P). Recon-match
# encodes are pinned to --threads 1 (the encoder defaults to all cores) so the
# job pool, not the encoder, owns the parallelism; thread-count independence is
# proved separately in the determinism/threading sections. Synthetic inputs are
# generated once into a cached fixtures dir and reused across runs.
#
# Usage: scripts/conformance.sh [--fast] [path/to/yah264]
#   --fast   dev-loop mode: 3 QPs, short corpus, skip the ffprobe codec probe.
# Env: YAH264_CONF_JOBS      parallelism (default: cores)
#      YAH264_CONF_FAST      1 = fast mode (same as --fast)
#      YAH264_CONF_DECODERS  space-separated: ffmpeg openh264 jm (default ffmpeg)
set -euo pipefail

FIXVER=4                        # bump to invalidate cached fixtures

root="$(cd "$(dirname "$0")/.." && pwd)"
SELF="$root/scripts/conformance.sh"
fixdir="$root/tests/.fixtures/v$FIXVER"

compute_config() {
    if [ "${YAH264_CONF_FAST:-0}" = 1 ]; then
        QPS="0 26 51"; CORPUS_FRAMES=48; DO_PROBE=0
    else
        # Bound corpus clips to 96 frames even in full mode: the corpus recon-match
        # gates content-geometry paths (crop / chroma format / 8x8), all exercised
        # within the first GOP, so the full-length clip adds runtime not coverage.
        # Untruncated 1080p clips at qp 0 (lossless) ran 12+ min each and made full
        # conformance impractical once the 720p/1080p corpus landed (2026-07 corpus
        # broadening). Override with YAH264_CONF_CORPUS_FRAMES=0 for the full clips.
        QPS="0 6 18 26 37 51"; CORPUS_FRAMES="${YAH264_CONF_CORPUS_FRAMES:-96}"; DO_PROBE=1
    fi
}

# ---------------------------------------------------------------------------
# Check functions. Each prints human-readable lines plus a final
# "SUMMARY <tests> <fails>" line the aggregator parses. All recon encodes are
# --threads 1 (see header). Output filenames are keyed per-check so parallel
# jobs never collide in the shared work dir.
# ---------------------------------------------------------------------------

md5frames() {   # md5frames <file>  -> per-frame framemd5 digests, or empty on failure
    ffmpeg -v error -i "$1" -f framemd5 - 2>/dev/null | grep -v '^#' | awk '{print $NF}'
}

rawmd5frames() {    # rawmd5frames <file.yuv> <pix_fmt> <WxH>
    # A headerless decoder dump has to be told its own geometry. framemd5 digests
    # the decoded PLANE DATA, so a raw yuv420p frame and the same frame out of a
    # y4m hash identically -- which is what makes these comparable with the
    # encoder's recon without a conversion step in between.
    ffmpeg -v error -f rawvideo -pix_fmt "$2" -s "$3" -i "$1" -f framemd5 - 2>/dev/null |
        grep -v '^#' | awk '{print $NF}'
}

y4m_geom() {    # y4m_geom <file.y4m> -> "<pix_fmt> <WxH>"
    head -c 200 "$1" 2>/dev/null | head -1 | awk '
        { w=""; h=""; pf="yuv420p"
          for (i = 1; i <= NF; i++) {
              if ($i ~ /^W[0-9]+$/) w = substr($i, 2)
              else if ($i ~ /^H[0-9]+$/) h = substr($i, 2)
              else if ($i ~ /^C422/) pf = "yuv422p"
              else if ($i ~ /^C444/) pf = "yuv444p"
              if ($i ~ /^C4[0-9][0-9]p10/) depth = "10le"
              else if ($i ~ /^C4[0-9][0-9]p12/) depth = "12le"
          }
          if (w != "" && h != "") printf "%s%s %sx%s\n", pf, depth, w, h }'
}

dec_skip() {    # dec_skip <decoder> <probe-output>  -> a reason, or empty for "go"
    # Gate on what is IN THE STREAM, never on the decoder's exit status: openh264
    # returns 0 after writing wrong pictures for B slices, so "it didn't complain"
    # is not evidence. The probe is scripts/h264_syntax.py, which reads the SPS
    # and the slice headers and decodes no slice data.
    local d="$1" p="$2" v
    cap() { printf '%s\n' "$p" | tr ' ' '\n' | sed -n "s/^$1=//p"; }
    [ "$(cap ok)" = 1 ] || { echo "stream did not parse"; return; }
    case "$d" in
        openh264)
            v="$(cap chroma)";   [ "$v" = 1 ] || { echo "openh264 decodes 4:2:0 only (chroma_format_idc $v)"; return; }
            v="$(cap bitdepth)"; [ "$v" = 8 ] || { echo "openh264 decodes 8-bit only (bit depth $v)"; return; }
            v="$(cap mbs_only)"; [ "$v" = 1 ] || { echo "openh264 refuses frame_mbs_only_flag 0"; return; }
            v="$(cap has_b)";    [ "$v" = 0 ] || { echo "openh264 mis-decodes B slices (2.6.0)"; return; }
            ;;
    esac
}

dec_decode() {  # dec_decode <decoder> <stream.264> <pix_fmt> <WxH> <workprefix>
    # -> per-frame digests on stdout, empty when the decoder produced nothing
    local d="$1" str="$2" pf="$3" sz="$4" p="$5"
    case "$d" in
        ffmpeg)
            md5frames "$str"
            ;;
        openh264)
            "$root/scripts/openh264-shim.sh" --decode "$str" "$p.oh.yuv" >/dev/null 2>&1 || return 0
            rawmd5frames "$p.oh.yuv" "$pf" "$sz"
            rm -f "$p.oh.yuv"
            ;;
        jm)
            # ldecod writes log.dec and dataDec.txt into the CURRENT directory
            # under fixed names, and this pool runs jobs in parallel in one work
            # dir, so each invocation gets its own.
            rm -rf "$p.jm"; mkdir -p "$p.jm"
            ( cd "$p.jm" && "$root/tools/jm/ldecod" -p InputFile="$str" \
                  -p OutputFile="$p.jm.yuv" >/dev/null 2>&1 ) || true
            rawmd5frames "$p.jm.yuv" "$pf" "$sz"
            rm -rf "$p.jm" "$p.jm.yuv"
            ;;
    esac
}

# recon_match <label> <recon.y4m> <stream.264> <workprefix>
#   The whole multi-decoder comparison in one place: every configured decoder
#   that CAN read the stream decodes it and must reproduce the encoder's own
#   reconstruction exactly. Sets RM_T (comparisons made), RM_F (failures) and
#   appends a "DEC <name> <checked> <skipped> <fails>" tally line so the run can
#   report what each oracle really covered.
recon_match() {
    local label="$1" rec="$2" str="$3" p="$4"
    local a b d pf sz probe reason
    RM_T=0; RM_F=0
    a="$(md5frames "$rec")"
    if [ -z "$a" ]; then
        RM_T=1; RM_F=1
        echo "  FAIL $label: the encoder's own reconstruction does not decode"
        return
    fi
    read -r pf sz <<<"$(y4m_geom "$rec")"
    if [ -z "${sz:-}" ]; then
        RM_T=1; RM_F=1
        echo "  FAIL $label: no geometry in the recon y4m header"
        return
    fi
    probe=""
    for d in $DECODERS; do
        if [ "$d" != ffmpeg ]; then
            [ -z "$probe" ] && probe="$(python3 "$root/scripts/h264_syntax.py" --probe "$str" 2>/dev/null | tr '\n' ' ')"
            reason="$(dec_skip "$d" "$probe")"
            if [ -n "$reason" ]; then
                echo "DEC $d 0 1 0"
                continue
            fi
        fi
        RM_T=$((RM_T + 1))
        b="$(dec_decode "$d" "$str" "$pf" "$sz" "$p.$d")"
        if [ -n "$b" ] && [ "$a" = "$b" ]; then
            echo "DEC $d 1 0 0"
        else
            echo "  FAIL $label [$d]: reconstruction != decode"
            echo "DEC $d 1 0 1"
            RM_F=$((RM_F + 1))
        fi
    done
}

check_clip() {  # check_clip <name> <src> [extra-flags] [qp-list override]
    local name="$1" src="$2" extra="${3:-}" ok_qps=0 t=0 f=0
    local qps="${4:-$QPS}"
    local qp codec out rec
    for qp in $qps; do
        out="$work/$name.$qp.264"; rec="$work/$name.$qp.rec.y4m"
        # shellcheck disable=SC2086
        "$enc" --input-y4m "$src" --qp "$qp" --threads 1 $extra \
            -o "$out" --dump-recon "$rec" 2>/dev/null || true
        if [ "$DO_PROBE" = 1 ]; then
            codec="$(ffprobe -v error -select_streams v:0 \
                     -show_entries stream=codec_name -of csv=p=0 "$out" 2>/dev/null || true)"
            if [ "$codec" != "h264" ]; then
                echo "  FAIL $name qp$qp: not recognised as H.264 (got '$codec')"
                t=$((t + 1)); f=$((f + 1)); continue
            fi
        fi
        recon_match "$name qp$qp" "$rec" "$out" "$work/$name.$qp"
        t=$((t + RM_T)); f=$((f + RM_F))
        [ "$RM_F" -eq 0 ] && [ "$RM_T" -gt 0 ] && ok_qps=$((ok_qps + 1))
    done
    [ "$ok_qps" -gt 0 ] && echo "  ok   $name (recon-match over $ok_qps QPs)"
    echo "SUMMARY $t $f"
}

check_scenecut_kf() {   # check_scenecut_kf <src>
    local src="$1" kf
    "$enc" --input-y4m "$src" --qp 26 --cabac --keyint 100 --threads 1 \
        -o "$work/sc.264" 2>/dev/null || true
    kf="$(ffprobe -v error -show_entries frame=pict_type -of csv=p=0 \
          "$work/sc.264" 2>/dev/null | grep -c I || true)"
    if [ "${kf:-0}" -ge 2 ]; then
        echo "  ok   mid-GOP cut forces a keyframe ($kf I-frames)"; echo "SUMMARY 1 0"
    else
        echo "  FAIL scene cut not detected (${kf:-0} I-frames)"; echo "SUMMARY 1 1"
    fi
}

check_flash_kf() {      # check_flash_kf <src> <expected-I-frames> <label>
    local src="$1" want="$2" label="$3" kf
    local out="$work/flash_$(basename "$src" .y4m).264"   # unique: $work is shared
    "$enc" --input-y4m "$src" --qp 26 --cabac --bframes 3 --keyint 100 --threads 1 \
        -o "$out" 2>/dev/null || true
    kf="$(ffprobe -v error -show_entries frame=pict_type -of csv=p=0 \
          "$out" 2>/dev/null | grep -c I || true)"
    if [ "${kf:-0}" -eq "$want" ]; then
        echo "  ok   $label (${kf} I-frames, want $want)"; echo "SUMMARY 1 0"
    else
        echo "  FAIL $label (${kf:-0} I-frames, want $want)"; echo "SUMMARY 1 1"
    fi
}

check_determinism() {   # check_determinism <label> <src> [feat]
    local label="$1" src="$2" feat="${3:-}"
    local p="$work/det_$label"     # unique per label: $work is shared, jobs parallel
    # shellcheck disable=SC2086
    "$enc" --input-y4m "$src" $feat --threads 1 -o "$p.1.264" 2>/dev/null || true
    # shellcheck disable=SC2086
    "$enc" --input-y4m "$src" $feat --threads 1 -o "$p.2.264" 2>/dev/null || true
    if cmp -s "$p.1.264" "$p.2.264"; then
        echo "  ok   byte-identical across runs ($label)"; echo "SUMMARY 1 0"
    else
        echo "  FAIL output differs between runs ($label)"; echo "SUMMARY 1 1"
    fi
}

check_threading() {     # check_threading <label> <src> <feat>
    # Y264_STQ=0: single-thread quality mode makes t1 output DELIBERATELY
    # differ from t2+ (owner policy, 2026-08-20). This canary exists to catch
    # RACES, so it compares with the deliberate variance pinned off; stq's own
    # identity gates live in its ship commit.
    # Y264_RCP_LAG=0: the ABR decide runs one burst ahead at threads > 1 and
    # in step single-threaded (default since 2026-09-03), so ABR output is
    # thread-variant by design; pinned off here for the same reason as the
    # carry, so the canary keeps reading races.
    # Y264_DIRECT_AUTO=0: the per-slice direct rule (the default since
    # 2026-09-03) is decided from the skippability counts folded so far, and
    # under the staircase that fold happens at burst launch, so the SAME clip
    # legitimately picks temporal on different slices at different thread
    # counts (repeat-deterministic at any fixed count: scripts/stair_determ.sh).
    # Pinned off here so the canary keeps reading races, not that design.
    # Y264_RC_CARRY=0: the ABR carry across GOP instances (2026-09-02) chains
    # each GOP to the one handed out W places earlier, W = worker count, so its
    # bits are deterministic PER thread count and differ ACROSS counts by
    # design; pinned off here for the same reason, its own gates are in its
    # ship commit (local/records/rc-carry-2026-09-02.md).
    local label="$1" src="$2" feat="$3"
    local p="$work/th_$label" lbl="${feat:-baseline}"
    # shellcheck disable=SC2086
    Y264_STQ=0 Y264_RC_CARRY=0 Y264_DIRECT_AUTO=0 Y264_RCP_LAG=0 "$enc" --input-y4m "$src" --qp 26 --keyint 3 $feat --threads 1 -o "$p.1.264" 2>/dev/null || true
    # shellcheck disable=SC2086
    Y264_RC_CARRY=0 Y264_DIRECT_AUTO=0 Y264_RCP_LAG=0 "$enc" --input-y4m "$src" --qp 26 --keyint 3 $feat --threads 2 -o "$p.2.264" 2>/dev/null || true
    # shellcheck disable=SC2086
    Y264_RC_CARRY=0 Y264_DIRECT_AUTO=0 Y264_RCP_LAG=0 "$enc" --input-y4m "$src" --qp 26 --keyint 3 $feat --threads 8 -o "$p.8.264" 2>/dev/null || true
    if cmp -s "$p.1.264" "$p.2.264" && cmp -s "$p.1.264" "$p.8.264"; then
        echo "  ok   byte-identical across threads 1/2/8 ($lbl)"; echo "SUMMARY 1 0"
    else
        echo "  FAIL output depends on thread count ($lbl)"; echo "SUMMARY 1 1"
    fi
    # The ABR carry across per-GOP encoders (Y264_RC_CARRY, the CLI's
    # rc_state/rc_import handoff) is thread-count-VARIANT by design, so its
    # gate is repeat-determinism at one fixed count: two runs at threads 8
    # with three GOPs must be byte-identical.
    Y264_RC_CARRY=1 "$enc" --input-y4m "$src" --bitrate 300 --keyint 4 $feat --threads 8 -o "$p.c1.264" 2>/dev/null || true
    Y264_RC_CARRY=1 "$enc" --input-y4m "$src" --bitrate 300 --keyint 4 $feat --threads 8 -o "$p.c2.264" 2>/dev/null || true
    if [ -s "$p.c1.264" ] && cmp -s "$p.c1.264" "$p.c2.264"; then
        echo "  ok   ABR carry across GOPs repeat-deterministic at threads 8 ($lbl)"; echo "SUMMARY 1 0"
    else
        echo "  FAIL ABR carry across GOPs not reproducible at threads 8 ($lbl)"; echo "SUMMARY 1 1"
    fi
}

check_threaded_decode() {   # check_threaded_decode <src>
    local src="$1" a
    "$enc" --input-y4m "$src" --qp 26 --keyint 3 --cabac --transform-8x8 --bframes 2 \
        --threads 8 -o "$work/thd.264" 2>/dev/null || true
    a="$(md5frames "$work/thd.264")"
    if [ -n "$a" ]; then
        echo "  ok   threaded stream decodes"; echo "SUMMARY 1 0"
    else
        echo "  FAIL threaded stream fails to decode"; echo "SUMMARY 1 1"
    fi
}

check_rc() {    # check_rc <label> <src> <spec>   -- recon-match + thread determinism
    local label="$1" src="$2" spec="$3" t=0 f=0
    local p="$work/rc_$label"
    # shellcheck disable=SC2086
    "$enc" --input-y4m "$src" $spec --threads 1 -o "$p.264" --dump-recon "$p.rec.y4m" 2>/dev/null || true
    recon_match "rc $label" "$p.rec.y4m" "$p.264" "$p"
    t=$((t + RM_T)); f=$((f + RM_F))
    [ "$RM_F" -eq 0 ] && echo "  ok   recon-match ($spec)"
    # A cell that DECLARES a buffer model gets the declaration read back out of
    # its own stream. Keyed on the spec rather than on the label, so the check
    # follows the flag wherever it is added and a cell cannot quietly declare an
    # HRD that nothing verifies. hrd_check exit 3 is "the stream carries no
    # HRD", which for a cell that asked for one means the flag did not take --
    # a failure, not a skip, and exactly the vacuous pass that checker exists to
    # refuse.
    case "$spec" in
      *--nal-hrd*)
        t=$((t + 1))
        if python3 "$root/scripts/hrd_check.py" "$p.264" --quiet >/dev/null 2>&1; then
            echo "  ok   HRD clean, read back out of the stream ($spec)"
        else
            echo "  FAIL hrd_check rejected the declared buffer model ($spec)"
            f=$((f + 1))
        fi
        ;;
    esac
    t=$((t + 1))
    # shellcheck disable=SC2086
    Y264_STQ=0 Y264_RC_CARRY=0 Y264_DIRECT_AUTO=0 Y264_RCP_LAG=0 "$enc" --input-y4m "$src" $spec --keyint 6 --threads 1 -o "$p.1.264" 2>/dev/null || true
    # shellcheck disable=SC2086
    Y264_RC_CARRY=0 Y264_DIRECT_AUTO=0 Y264_RCP_LAG=0 "$enc" --input-y4m "$src" $spec --keyint 6 --threads 4 -o "$p.4.264" 2>/dev/null || true
    if cmp -s "$p.1.264" "$p.4.264"; then
        echo "  ok   deterministic across threads ($spec)"
    else
        echo "  FAIL thread-dependent ($spec)"; f=$((f + 1))
    fi
    echo "SUMMARY $t $f"
}

check_fnwrap() {   # check_fnwrap <src>  -- frame_num wrap under a B-pyramid (review 2026-09-04)
    # MaxFrameNum is 256 with a pyramid; two references per 4-frame mini-GOP
    # wrap it after ~512 display frames. The DPB's sliding window must evict
    # by FrameNumWrap or the first reference after the wrap is thrown out and
    # the decoder's DPB diverges (missing references, then a slot -1 index).
    local src="$1" p="$work/fnwrap"
    "$enc" --input-y4m "$src" --keyint 2000 --no-scenecut --bframes 3 --ref 1 --qp 30 \
        --threads 1 -o "$p.264" --dump-recon "$p.rec.y4m" 2>/dev/null || true
    recon_match "frame_num wrap" "$p.rec.y4m" "$p.264" "$p"
    [ "$RM_F" -eq 0 ] && echo "  ok   recon-match across the frame_num wrap (560 frames, b-pyramid)"
    echo "SUMMARY $RM_T $RM_F"
}

# The field twin. frame_num advances once per PAIR, so the wrap comes at the
# same 560 frames -- but a field list is addressed in picNum, which is
# 2*FrameNumWrap + 1, and the reordering this encoder writes walks it by
# subtracting 2 from a running predecessor. Both of those are modulo
# MaxPicNum at the decoder, and this is the cell that says so.
check_fnwrap_paff() {   # check_fnwrap_paff <src>
    local src="$1" p="$work/fnwrap_paff"
    "$enc" --input-y4m "$src" --keyint 2000 --no-scenecut --tff --cabac --ref 3 --qp 30 \
        --threads 1 -o "$p.264" --dump-recon "$p.rec.y4m" 2>/dev/null || true
    recon_match "frame_num wrap (fields)" "$p.rec.y4m" "$p.264" "$p"
    [ "$RM_F" -eq 0 ] && echo "  ok   recon-match across the frame_num wrap (560 frames, field pairs)"
    echo "SUMMARY $RM_T $RM_F"
}

# --pass 3 reads the stats AND writes them back, so the gate is two things at
# once: the pass-3 stream must recon-match, and the file it leaves behind must
# still feed a pass 2. Both halves run serially, because --dump-recon does.
# --profile is a constraint, so half its contract is what it REFUSES. A
# refusal that exits 0 and writes an empty file is the failure mode this
# checks for: status must be nonzero AND no stream may appear.
check_profile_refusals() {  # check_profile_refusals <src>
    local src="$1" t=0 f=0 rc sz
    local cases=(
        "baseline|--profile baseline --cabac"
        "baseline|--profile baseline --bframes 3"
        "baseline|--profile baseline --transform-8x8"
        "main|--profile main --transform-8x8"
        "main|--profile main --cqm jvt"
        "high10|--profile high10"
        "bogus|--profile nosuchprofile"
    )
    local c lbl args out
    for c in "${cases[@]}"; do
        lbl="${c%%|*}"; args="${c#*|}"
        out="$work/prof_refuse.$$.264"
        rm -f "$out"
        # shellcheck disable=SC2086
        "$enc" --input-y4m "$src" --qp 26 --threads 1 $args -o "$out" 2>/dev/null
        rc=$?
        sz=$([ -f "$out" ] && wc -c < "$out" || echo 0)
        t=$((t + 1))
        if [ "$rc" -ne 0 ] && [ "$sz" -eq 0 ]; then
            :
        else
            echo "  FAIL [$args] should be refused (status $rc, $sz bytes)"
            f=$((f + 1))
        fi
        rm -f "$out"
    done
    [ "$f" -eq 0 ] && echo "  ok   $t impossible --profile combinations refused, no stream written"
    echo "SUMMARY $t $f"
}

# The level the SPS declares, read back by an independently transcribed
# Table A-1 (scripts/level_check.py), with --strict-mv so the VUI's own
# vertical MV bound has to fit the level too. --exact additionally demands the
# declared level equals the lowest conformant one, i.e. the encoder's auto pick
# is not merely legal but minimal.
check_level() {     # check_level <name> <src> [flags] [level_check extra]
    local name="$1" src="$2" flags="${3:-}" extra="${4:-}" t=0 f=0
    local out="$work/lvl_$name.264"
    # shellcheck disable=SC2086
    "$enc" --input-y4m "$src" --qp 26 --threads 1 $flags -o "$out" 2>/dev/null || true
    t=1
    # shellcheck disable=SC2086
    if python3 "$root/scripts/level_check.py" "$out" --y4m "$src" --strict-mv --quiet $extra; then
        echo "  ok   $name level + vertical MV bound conformant"
    else
        echo "  FAIL $name level check"; f=1
    fi
    echo "SUMMARY $t $f"
}

# --stitchable claims the SPS stops depending on --ref and --bframes. That is a
# claim about two encodes at once, so the check IS two encodes: different ref
# and bframe counts at the same geometry must produce the same SPS NAL, and
# without the flag they must not (or the flag is measuring nothing).
check_stitch_sps() {    # check_stitch_sps <src>
    local src="$1" t=0 f=0 a b c d
    "$enc" --input-y4m "$src" --qp 26 --threads 1 --cabac --bframes 3 --ref 4 --stitchable \
        -o "$work/st_a.264" 2>/dev/null || true
    "$enc" --input-y4m "$src" --qp 26 --threads 1 --cabac --bframes 0 --ref 1 --stitchable \
        -o "$work/st_b.264" 2>/dev/null || true
    "$enc" --input-y4m "$src" --qp 26 --threads 1 --cabac --bframes 3 --ref 4 \
        -o "$work/st_c.264" 2>/dev/null || true
    "$enc" --input-y4m "$src" --qp 26 --threads 1 --cabac --bframes 0 --ref 1 \
        -o "$work/st_d.264" 2>/dev/null || true
    a="$(first_nal_md5 "$work/st_a.264")"; b="$(first_nal_md5 "$work/st_b.264")"
    c="$(first_nal_md5 "$work/st_c.264")"; d="$(first_nal_md5 "$work/st_d.264")"
    t=$((t + 1))
    if [ -n "$a" ] && [ "$a" = "$b" ]; then
        echo "  ok   --stitchable: the SPS is the same at ref4/b3 and ref1/b0"
    else
        echo "  FAIL --stitchable: SPS still moves with --ref/--bframes"; f=$((f + 1))
    fi
    t=$((t + 1))
    if [ -n "$c" ] && [ "$c" != "$d" ]; then
        echo "  ok   without it the SPS does move, so the check is measuring something"
    else
        echo "  FAIL the control arm: SPS identical without --stitchable too"; f=$((f + 1))
    fi
    echo "SUMMARY $t $f"
}

first_nal_md5() {   # first_nal_md5 <annexb file> -> md5 of the first NAL
    python3 - "$1" <<'PY'
import sys, hashlib
d = open(sys.argv[1], "rb").read()
i = d.find(b"\x00\x00\x00\x01")
j = d.find(b"\x00\x00\x00\x01", i + 4)
print(hashlib.md5(d[i:j if j > 0 else len(d)]).hexdigest() if i >= 0 else "")
PY
}

# --input-raw with the right geometry must reproduce the Y4M encode exactly.
# The oracle is the same clip with its header stripped, so any disagreement is
# the reader and not the encoder. The geometry, frame rate and sample aspect
# come out of the Y4M header the raw file no longer has.
check_raw_equiv() {     # check_raw_equiv <y4m src>
    local src="$1" t=0 f=0 th
    # Keyed on the source: this runs once per fixture and the pool runs the
    # calls CONCURRENTLY in one shared work dir. A fixed name here had the two
    # depth arms overwriting each other's input and reading as a failure.
    local key; key="$(basename "$src" .y4m)"
    local raw="$work/raw_in.$key.yuv"
    local hdr wh fps sar sarflag
    hdr="$(head -c 200 "$src" | head -1)"
    wh="$(printf '%s' "$hdr" | awk '{for(i=1;i<=NF;i++){if($i~/^W/)w=substr($i,2);if($i~/^H/)h=substr($i,2)}print w "x" h}')"
    fps="$(printf '%s' "$hdr" | awk '{for(i=1;i<=NF;i++)if($i~/^F/){s=substr($i,2);gsub(":","/",s);print s}}')"
    sar="$(printf '%s' "$hdr" | awk '{for(i=1;i<=NF;i++)if($i~/^A/)print substr($i,2)}')"
    [ -z "$fps" ] && fps="25/1"
    sarflag=""
    [ -n "$sar" ] && [ "$sar" != "0:0" ] && sarflag="--sar $sar"
    local pf csp
    pf="$(y4m_geom "$src" | awk '{print $1}')"
    [ -z "$pf" ] && pf=yuv420p
    # the flag spells the format the way the Y4M C tag does: 420/422/444 with
    # an optional p10, which is also the one field raw input cannot carry.
    csp="$(printf '%s' "$pf" | sed -e 's/^yuv//' -e 's/le$//')"
    ffmpeg -v error -i "$src" -f rawvideo -pix_fmt "$pf" "$raw" -y 2>/dev/null
    for th in 1 4; do
        t=$((t + 1))
        "$enc" --input-y4m "$src" --qp 26 --cabac --threads "$th" \
            -o "$work/raw_a.$key.$th.264" 2>/dev/null || true
        # shellcheck disable=SC2086
        "$enc" --input-raw "$raw" --input-res "$wh" --input-csp "$csp" --fps "$fps" $sarflag \
            --qp 26 --cabac --threads "$th" -o "$work/raw_b.$key.$th.264" 2>/dev/null || true
        if [ -s "$work/raw_a.$key.$th.264" ] && cmp -s "$work/raw_a.$key.$th.264" "$work/raw_b.$key.$th.264"; then
            echo "  ok   --input-raw == the same clip as Y4M, byte for byte (t$th)"
        else
            echo "  FAIL --input-raw differs from the Y4M encode (t$th)"; f=$((f + 1))
        fi
    done
    echo "SUMMARY $t $f"
}

# --seek N must equal encoding a clip whose first N frames were already gone.
check_seek_equiv() {    # check_seek_equiv <src>
    local src="$1" t=1 f=0 pf key
    key="$(basename "$src" .y4m)"
    # The reference has to come back at the SOURCE's depth, or the two arms
    # would be comparing an 8-bit encode against a 10-bit one.
    pf="$(y4m_geom "$src" | awk '{print $1}')"; [ -z "$pf" ] && pf=yuv420p
    ffmpeg -v error -i "$src" -vf trim=start_frame=7 -frames:v 24 \
        -pix_fmt "$pf" -strict -1 -f yuv4mpegpipe "$work/seek_ref.$key.y4m" -y 2>/dev/null
    "$enc" --input-y4m "$src" --seek 7 --frames 24 --qp 26 --cabac --threads 1 \
        -o "$work/seek_a.$key.264" 2>/dev/null || true
    "$enc" --input-y4m "$work/seek_ref.$key.y4m" --frames 24 --qp 26 --cabac --threads 1 \
        -o "$work/seek_b.$key.264" 2>/dev/null || true
    if cmp -s "$work/seek_a.$key.264" "$work/seek_b.$key.264"; then
        echo "  ok   --seek 7 == a clip trimmed to frame 7, byte for byte"
    else
        echo "  FAIL --seek 7 differs from the pre-trimmed clip"; f=1
    fi
    echo "SUMMARY $t $f"
}

# --crop-rect must equal encoding a clip that was already cropped.
check_crop_equiv() {    # check_crop_equiv <src>
    local src="$1" t=0 f=0 th pf wh cw ch key
    key="$(basename "$src" .y4m)"
    pf="$(y4m_geom "$src" | awk '{print $1}')"; [ -z "$pf" ] && pf=yuv420p
    wh="$(y4m_geom "$src" | awk '{print $2}')"
    cw=$(( ${wh%x*} - 32 )); ch=$(( ${wh#*x} - 32 ))
    ffmpeg -v error -i "$src" -vf "crop=$cw:$ch:16:16" -frames:v 8 \
        -pix_fmt "$pf" -strict -1 -f yuv4mpegpipe "$work/crop_ref.$key.y4m" -y 2>/dev/null
    for th in 1 4; do
        t=$((t + 1))
        # --frames on BOTH arms: the reference is trimmed to 8 and a fixture
        # longer than that would otherwise put 12 frames against 8.
        "$enc" --input-y4m "$src" --crop-rect 16,16,16,16 --frames 8 --qp 26 --cabac --threads "$th" \
            -o "$work/crop_a.$key.$th.264" 2>/dev/null || true
        "$enc" --input-y4m "$work/crop_ref.$key.y4m" --frames 8 --qp 26 --cabac --threads "$th" \
            -o "$work/crop_b.$key.$th.264" 2>/dev/null || true
        if cmp -s "$work/crop_a.$key.$th.264" "$work/crop_b.$key.$th.264"; then
            echo "  ok   --crop-rect == a pre-cropped clip, byte for byte (t$th)"
        else
            echo "  FAIL --crop-rect differs from the pre-cropped clip (t$th)"; f=$((f + 1))
        fi
    done
    echo "SUMMARY $t $f"
}

check_pass3() {     # check_pass3 <src>
    local src="$1" a b t=0 f=0
    local st="$work/p3.stats"
    "$enc" --input-y4m "$src" --cabac --bframes 3 --pass 1 \
        --stats "$st" --threads 1 -o /dev/null 2>/dev/null || true
    "$enc" --input-y4m "$src" --cabac --bframes 3 --pass 3 \
        --stats "$st" --bitrate 600 --threads 1 \
        -o "$work/p3.264" --dump-recon "$work/p3.rec.y4m" 2>/dev/null || true
    t=$((t + 1))
    a="$(md5frames "$work/p3.rec.y4m")"; b="$(md5frames "$work/p3.264")"
    if [ -n "$a" ] && [ "$a" = "$b" ]; then
        echo "  ok   pass3 recon-match"
    else
        echo "  FAIL pass3 recon mismatch"; f=$((f + 1))
    fi
    "$enc" --input-y4m "$src" --cabac --bframes 3 --pass 2 \
        --stats "$st" --bitrate 600 --threads 1 \
        -o "$work/p3b.264" --dump-recon "$work/p3b.rec.y4m" 2>/dev/null || true
    t=$((t + 1))
    a="$(md5frames "$work/p3b.rec.y4m")"; b="$(md5frames "$work/p3b.264")"
    if [ -n "$a" ] && [ "$a" = "$b" ]; then
        echo "  ok   pass2 over the stats pass3 rewrote"
    else
        echo "  FAIL pass2 cannot use the stats pass3 wrote"; f=$((f + 1))
    fi
    echo "SUMMARY $t $f"
}

check_twopass() {   # check_twopass <name> <src> [pass-2 args]
    local name="$1" src="$2" args="${3:-}"
    "$enc" --input-y4m "$src" --cabac --bframes 3 --pass 1 \
        --stats "$work/2p.$name.stats" --threads 1 -o /dev/null 2>/dev/null || true
    # shellcheck disable=SC2086
    "$enc" --input-y4m "$src" --cabac --bframes 3 --pass 2 \
        --stats "$work/2p.$name.stats" --bitrate 600 --threads 1 $args \
        -o "$work/2p.$name.264" --dump-recon "$work/2p.$name.rec.y4m" 2>/dev/null || true
    recon_match "2-pass $name" "$work/2p.$name.rec.y4m" "$work/2p.$name.264" "$work/2p.$name"
    [ "$RM_F" -eq 0 ] && echo "  ok   $name recon-match (pass 2)"
    echo "SUMMARY $RM_T $RM_F"
}

# ---------------------------------------------------------------------------
# Worker mode: run one check into its own result file, then exit 0 (so xargs
# never aborts the pool). Globals come from the exported environment.
# ---------------------------------------------------------------------------
if [ "${1:-}" = "__worker" ]; then
    res="$2"; section="$3"; fn="$4"; shift 4
    compute_config
    enc="${YAH264_ENC:?}"
    work="${YAH264_CONF_WORK:?}"
    DECODERS="${YAH264_CONF_DECODERS:-ffmpeg}"
    set +e
    { echo "SECTION $section"; "$fn" "$@"; } >"$res" 2>&1
    exit 0
fi

# ---------------------------------------------------------------------------
# Main.
# ---------------------------------------------------------------------------
enc=""
for a in "$@"; do
    case "$a" in
        --fast) YAH264_CONF_FAST=1 ;;
        *)      enc="$a" ;;
    esac
done
: "${YAH264_CONF_FAST:=0}"
enc="${enc:-$root/build/cli/yah264}"
compute_config
JOBS="${YAH264_CONF_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 8)}"

if [ ! -x "$enc" ]; then
    echo "conformance: encoder not found at $enc (build first)" >&2
    exit 2
fi
command -v ffmpeg  >/dev/null || { echo "conformance: ffmpeg required" >&2; exit 2; }
command -v ffprobe >/dev/null || { echo "conformance: ffprobe required" >&2; exit 2; }

# --- decoders ------------------------------------------------------------
# A NAMED DECODER THAT IS NOT THERE IS AN ERROR, never a silent fallback to
# ffmpeg. "conformance green with three decoders" has to mean three decoders
# ran; a run that quietly dropped two and still printed a pass is the exact
# shape of gate that stops gating without anyone noticing.
DECODERS="${YAH264_CONF_DECODERS:-ffmpeg}"
for d in $DECODERS; do
    case "$d" in
        ffmpeg) ;;
        openh264)
            [ -x "${OPENH264_DEC:-$root/tools/openh264/h264dec}" ] || {
                echo "conformance: openh264 requested but no h264dec -- run scripts/fetch_openh264.sh" >&2
                exit 2; }
            ;;
        jm)
            [ -x "$root/tools/jm/ldecod" ] || {
                echo "conformance: jm requested but no ldecod -- run scripts/fetch_jm.sh" >&2
                exit 2; }
            ;;
        *)  echo "conformance: unknown decoder '$d' (want: ffmpeg openh264 jm)" >&2; exit 2 ;;
    esac
done

work="$(mktemp -d)"
resdir="$work/results"
mkdir -p "$resdir"
trap 'rm -rf "$work"' EXIT
export YAH264_ENC="$enc" YAH264_CONF_WORK="$work" YAH264_CONF_FAST
export YAH264_CONF_DECODERS="$DECODERS"

# --- fixtures: generate once, reuse across runs --------------------------
mkdir -p "$fixdir"
genlavfi() {    # genlavfi <name> <lavfi-spec> <frames> [extra-ffmpeg-args...]
    local name="$1" spec="$2" frames="$3"; shift 3
    local out="$fixdir/$name.y4m"
    [ -f "$out" ] && return
    ffmpeg -v error -f lavfi -i "$spec" -frames:v "$frames" "$@" \
        -pix_fmt yuv420p -f yuv4mpegpipe "$out.tmp.$$"
    mv "$out.tmp.$$" "$out"
}
echo "conformance: preparing fixtures in $fixdir"
for geom in 320x240 176x144 210x146 178x100 62x50 16x16; do
    genlavfi "syn_$geom" "testsrc=size=$geom:rate=25" 8
done
genlavfi syn_motion "testsrc2=size=320x240:rate=30" 12
genlavfi syn_long   "testsrc2=size=64x48:rate=25" 560
genlavfi syn_noise  "nullsrc=size=192x160:rate=25,geq=random(1)*256:128:128" 3
genlavfi syn_fade   "testsrc2=size=320x240:rate=25" 12 -vf "fade=t=out:st=0.1:d=0.4"
genlavfi sc_a       "smptebars=size=176x144:rate=25" 12
genlavfi sc_b       "testsrc2=size=176x144:rate=25" 12
# 4:2:2 clip (genlavfi hardcodes yuv420p, so build this one inline). Chroma is
# full-height here, exercising the 4:2:2 B-frame reconstruction path.
if [ ! -f "$fixdir/syn_422.y4m" ]; then
    ffmpeg -v error -f lavfi -i "testsrc2=size=176x144:rate=25" -frames:v 24 \
        -pix_fmt yuv422p -strict -1 -f yuv4mpegpipe "$fixdir/syn_422.y4m.tmp.$$"
    mv "$fixdir/syn_422.y4m.tmp.$$" "$fixdir/syn_422.y4m"
fi
# 4:4:4 clip (full-res chroma). Short on purpose: it gates the B_Skip chroma
# reconstruction path, whose direct-prediction chroma must be stored at full-res
# 16x16 geometry (origin mbx*16). 5 frames of motion at qp26 exercises B_Skip on
# non-flat chroma. Use motion content (flat chroma would hide a wrong-origin copy).
if [ ! -f "$fixdir/syn_444.y4m" ]; then
    ffmpeg -v error -f lavfi -i "testsrc2=size=176x144:rate=25" -frames:v 5 \
        -pix_fmt yuv444p -strict -1 -f yuv4mpegpipe "$fixdir/syn_444.y4m.tmp.$$"
    mv "$fixdir/syn_444.y4m.tmp.$$" "$fixdir/syn_444.y4m"
fi
# 16-frame 4:4:4 clip: full-length 4:4:4 + B recon-match, i.e. long enough for a
# reference-plane drift to accumulate and persist (chroma deblock corrupting a
# ref plane that later B frames predict from would show by ~frame 5). The B_Skip
# full-res fix (origin mbx*16) plus the already-format-aware chroma deblock loop
# (full-res 16-wide, 4 internal edges, luma-style filter for ChromaArrayType==3)
# make this recon-match clean at qp<=26. Still pinned to qp26: qp37/51 trip a
# separate 4:4:4 chroma quant mismatch that hits even the intra frame 0.
if [ ! -f "$fixdir/syn_444_16.y4m" ]; then
    ffmpeg -v error -f lavfi -i "testsrc2=size=176x144:rate=25" -frames:v 16 \
        -pix_fmt yuv444p -strict -1 -f yuv4mpegpipe "$fixdir/syn_444_16.y4m.tmp.$$"
    mv "$fixdir/syn_444_16.y4m.tmp.$$" "$fixdir/syn_444_16.y4m"
fi
# 10-bit clips (item C3-10bit). The input's sample width picks the encoder
# now, so a C420p10 fixture is the whole selection: nothing on the command line
# says 10-bit. 4:2:2 and 4:4:4 at 10 bits cross the two axes -- the chroma
# geometry is a runtime property of one library, the sample width chooses
# between two -- which is the pair a wrong cast would break.
for D10 in 420 422 444; do
    if [ ! -f "$fixdir/syn_p10_$D10.y4m" ]; then
        ffmpeg -v error -f lavfi -i "testsrc2=size=320x240:rate=30" -frames:v 12 \
            -pix_fmt "yuv${D10}p10le" -strict -1 -f yuv4mpegpipe \
            "$fixdir/syn_p10_$D10.y4m.tmp.$$"
        mv "$fixdir/syn_p10_$D10.y4m.tmp.$$" "$fixdir/syn_p10_$D10.y4m"
    fi
done
# A cropped 10-bit clip: 210x146 is neither dimension a multiple of 16, so the
# SPS crop offsets and the 10-bit sample width are exercised together.
if [ ! -f "$fixdir/syn_p10_crop.y4m" ]; then
    ffmpeg -v error -f lavfi -i "testsrc2=size=210x146:rate=25" -frames:v 8 \
        -pix_fmt yuv420p10le -strict -1 -f yuv4mpegpipe \
        "$fixdir/syn_p10_crop.y4m.tmp.$$"
    mv "$fixdir/syn_p10_crop.y4m.tmp.$$" "$fixdir/syn_p10_crop.y4m"
fi
# Interlaced clips (item C2-PAFF-1). tinterlace weaves two source frames into
# one, so the Y4M carries It / Ib and the CLI field-codes it without a flag --
# which is itself part of what these cells check. syn_tff_crop is 178x100: seven
# macroblock rows, an odd count, so the coded height pads to eight and the
# doubled vertical crop unit has to take the extra row back off.
if [ ! -f "$fixdir/syn_tff.y4m" ]; then
    ffmpeg -v error -f lavfi -i "testsrc2=size=320x240:rate=50" \
        -vf "tinterlace=mode=interleave_top,setfield=tff" -frames:v 24 \
        -pix_fmt yuv420p -f yuv4mpegpipe "$fixdir/syn_tff.y4m.tmp.$$"
    mv "$fixdir/syn_tff.y4m.tmp.$$" "$fixdir/syn_tff.y4m"
fi
if [ ! -f "$fixdir/syn_bff.y4m" ]; then
    ffmpeg -v error -f lavfi -i "testsrc2=size=320x240:rate=50" \
        -vf "tinterlace=mode=interleave_bottom,setfield=bff" -frames:v 24 \
        -pix_fmt yuv420p -f yuv4mpegpipe "$fixdir/syn_bff.y4m.tmp.$$"
    mv "$fixdir/syn_bff.y4m.tmp.$$" "$fixdir/syn_bff.y4m"
fi
if [ ! -f "$fixdir/syn_tff_crop.y4m" ]; then
    ffmpeg -v error -f lavfi -i "testsrc2=size=178x100:rate=50" \
        -vf "tinterlace=mode=interleave_top,setfield=tff" -frames:v 24 \
        -pix_fmt yuv420p -f yuv4mpegpipe "$fixdir/syn_tff_crop.y4m.tmp.$$"
    mv "$fixdir/syn_tff_crop.y4m.tmp.$$" "$fixdir/syn_tff_crop.y4m"
fi
if [ ! -f "$fixdir/sc_cut.y4m" ]; then
    ffmpeg -v error -i "$fixdir/sc_a.y4m" -i "$fixdir/sc_b.y4m" \
        -filter_complex "[0:v][1:v]concat=n=2:v=1" \
        -pix_fmt yuv420p -f yuv4mpegpipe "$fixdir/sc_cut.y4m.tmp.$$"
    mv "$fixdir/sc_cut.y4m.tmp.$$" "$fixdir/sc_cut.y4m"
fi
# Flash fixtures: scene A, a brief B flash, then scene A again (A..A B[..B] A..A).
# A one-frame flash must NOT force a keyframe (x264 --preset medium suppresses
# it; b-adapt-fast looks one frame past a cut candidate); a two-frame flash is
# long enough that x264 medium *does* cut, so we must too. Gates the deferred
# scene-cut / flash-suppression logic.
for FL in 1 2; do
    if [ ! -f "$fixdir/flash$FL.y4m" ]; then
        ffmpeg -v error -i "$fixdir/sc_a.y4m" -i "$fixdir/sc_b.y4m" -i "$fixdir/sc_a.y4m" \
            -filter_complex \
            "[1:v]trim=end_frame=$FL,setpts=PTS-STARTPTS[b];[0:v][b][2:v]concat=n=3:v=1" \
            -pix_fmt yuv420p -f yuv4mpegpipe "$fixdir/flash$FL.y4m.tmp.$$"
        mv "$fixdir/flash$FL.y4m.tmp.$$" "$fixdir/flash$FL.y4m"
    fi
done
S="$fixdir"      # shorthand for job specs below

# --- build the job list --------------------------------------------------
jobs=(); jobn=0
add() {     # add <section> <fn> [args...]
    local res cmd a
    printf -v res '%s/%04d' "$resdir" "$jobn"; jobn=$((jobn + 1))
    printf -v cmd '%q __worker %q %q' "$SELF" "$res" "$1"; shift
    for a in "$@"; do printf -v cmd '%s %q' "$cmd" "$a"; done
    jobs+=("$cmd")
}

for geom in 320x240 176x144 210x146 178x100 62x50 16x16; do
    add "synthetic clips" check_clip "syn_$geom" "$S/syn_$geom.y4m"
done
add "synthetic clips" check_clip syn_motion "$S/syn_motion.y4m"
add "synthetic clips" check_clip syn_noise  "$S/syn_noise.y4m"
add "frame_num wrap (b-pyramid, 560 frames)" check_fnwrap "$S/syn_long.y4m"
add "frame_num wrap (b-pyramid, 560 frames)" check_fnwrap_paff "$S/syn_long.y4m"

add "multiple references (CAVLC IPPP)" check_clip mref3_motion    "$S/syn_motion.y4m"  "--ref 3"
add "multiple references (CAVLC IPPP)" check_clip mref5_motion    "$S/syn_motion.y4m"  "--ref 5"
add "multiple references (CAVLC IPPP)" check_clip mref4_8x8       "$S/syn_motion.y4m"  "--ref 4 --transform-8x8"
add "multiple references (CAVLC IPPP)" check_clip mref3_crop      "$S/syn_178x100.y4m" "--ref 3"
add "multiple references (CAVLC IPPP)" check_clip mref2_aq        "$S/syn_motion.y4m"  "--ref 2 --aq-strength 1.0"
add "multiple references (CAVLC IPPP)" check_clip mref3_cabac     "$S/syn_motion.y4m"  "--cabac --ref 3"
add "multiple references (CAVLC IPPP)" check_clip mref5_cabac     "$S/syn_motion.y4m"  "--cabac --ref 5"
add "multiple references (CAVLC IPPP)" check_clip mref4_cabac_8x8 "$S/syn_motion.y4m"  "--cabac --ref 4 --transform-8x8"
add "multiple references (CAVLC IPPP)" check_clip mref3_cabac_crop "$S/syn_178x100.y4m" "--cabac --ref 3"

add "8x8 transform (High profile)" check_clip t8_cavlc      "$S/syn_320x240.y4m" "--transform-8x8"
add "8x8 transform (High profile)" check_clip t8_cabac      "$S/syn_320x240.y4m" "--cabac --transform-8x8"
add "8x8 transform (High profile)" check_clip t8_crop       "$S/syn_178x100.y4m" "--transform-8x8"
add "8x8 transform (High profile)" check_clip t8_motion     "$S/syn_motion.y4m"  "--transform-8x8"
add "8x8 transform (High profile)" check_clip t8_motion_cbc "$S/syn_motion.y4m"  "--cabac --transform-8x8"
add "8x8 transform (High profile)" check_clip t8_bframes    "$S/syn_motion.y4m"  "--cabac --bframes 2 --transform-8x8"

# Baseline-shaped cells: P only, 4:2:0, progressive, 8-bit. The suite is
# B-heavy because the encoder's defaults are, and the extra oracles are not
# equally capable -- openh264 decodes ONLY this shape -- so with
# YAH264_CONF_DECODERS unset these are six ordinary recon-match cells, and with
# it set they are the cells where a second and third implementation get to
# disagree with us. Without them "openh264 green" would mean "openh264 skipped
# everything".
add "baseline-shaped (multi-decoder coverage)" check_clip nob_cavlc "$S/syn_motion.y4m"  "--bframes 0"
add "baseline-shaped (multi-decoder coverage)" check_clip nob_cabac "$S/syn_motion.y4m"  "--cabac --bframes 0"
add "baseline-shaped (multi-decoder coverage)" check_clip nob_8x8   "$S/syn_motion.y4m"  "--cabac --bframes 0 --transform-8x8"
add "baseline-shaped (multi-decoder coverage)" check_clip nob_mref  "$S/syn_motion.y4m"  "--cabac --bframes 0 --ref 4"
add "baseline-shaped (multi-decoder coverage)" check_clip nob_crop  "$S/syn_178x100.y4m" "--cabac --bframes 0"
add "baseline-shaped (multi-decoder coverage)" check_clip nob_intra "$S/syn_320x240.y4m" "--cabac --bframes 0 --keyint 1"

# PAFF field pictures (item C2-PAFF-1). Every one of these streams carries
# frame_mbs_only_flag 0 and field_pic_flag 1, which openh264 refuses at the SPS
# (docs/instruments.md section 5), so the JM is the second oracle here and the
# skip is by stream property as everywhere else. The recon is woven back into
# frames, so the comparison is the ordinary per-frame one.
add "PAFF field pictures" check_clip paff_tff_cavlc "$S/syn_tff.y4m"      "--tff --cavlc"
add "PAFF field pictures" check_clip paff_bff_cabac "$S/syn_bff.y4m"      "--bff --cabac"
add "PAFF field pictures" check_clip paff_8x8       "$S/syn_tff.y4m"      "--tff --cabac --transform-8x8"
add "PAFF field pictures" check_clip paff_8x8_cavlc "$S/syn_tff.y4m"      "--tff --cavlc --transform-8x8"
add "PAFF field pictures" check_clip paff_mref3     "$S/syn_tff.y4m"      "--tff --cabac --ref 3"
add "PAFF field pictures" check_clip paff_mref5     "$S/syn_bff.y4m"      "--bff --cabac --ref 5"
add "PAFF field pictures" check_clip paff_crop      "$S/syn_tff_crop.y4m" "--tff --cabac --transform-8x8"
add "PAFF field pictures" check_clip paff_crop_cavlc "$S/syn_tff_crop.y4m" "--tff --cavlc"
add "PAFF field pictures" check_clip paff_intra     "$S/syn_tff.y4m"      "--tff --cabac --transform-8x8 --keyint 1"
# No flag at all: the Y4M's It tag is what selects field coding, and
# --no-interlaced is what refuses to take it.
add "PAFF field pictures" check_clip paff_auto      "$S/syn_tff.y4m"      "--cabac --transform-8x8"
add "PAFF field pictures" check_clip paff_off       "$S/syn_tff.y4m"      "--cabac --no-interlaced"
add "PAFF field pictures" check_clip paff_cintra    "$S/syn_tff.y4m"      "--tff --cabac --transform-8x8 --constrained-intra"
# 16x16: one macroblock row before the pad, one FIELD macroblock row after it,
# which is the smallest field pair the encoder can build -- and a height that
# is a multiple of 4, which the doubled crop unit requires.
add "PAFF field pictures" check_clip paff_tiny      "$S/syn_16x16.y4m"    "--tff --cabac --ref 3"
# The Y4M tag is a property of the input, not a request: a named --bframes
# wins over it and the encode is a frame one. The stream this cell compares
# is therefore progressive, which is exactly the claim.
add "PAFF field pictures" check_clip paff_bwins     "$S/syn_tff.y4m"      "--cabac --bframes 2"
# ...and the same for --slices, which field coding refuses until B fields land.
add "PAFF field pictures" check_clip paff_slwins    "$S/syn_tff.y4m"      "--cabac --bframes 0 --slices 4"
add "PAFF field pictures" check_rc   paff_crf   "$S/syn_tff.y4m" "--tff --cabac --crf 26"
add "PAFF field pictures" check_rc   paff_abr   "$S/syn_tff.y4m" "--tff --cabac --bitrate 400"
add "PAFF field pictures" check_rc   paff_cvbr  "$S/syn_tff.y4m" "--tff --cabac --bitrate 400 --vbv-maxrate 400 --vbv-bufsize 400"
add "PAFF field pictures" check_threading paff "$S/syn_tff.y4m" "--tff --cabac --transform-8x8"
add "PAFF field pictures" check_determinism paff_tff "$S/syn_tff.y4m" "--tff --cabac --transform-8x8 --qp 26"
add "PAFF field pictures" check_determinism paff_bff "$S/syn_bff.y4m" "--bff --cavlc --qp 30"

add "implicit weighted biprediction" check_clip wp_b2_cavlc   "$S/syn_motion.y4m" "--bframes 2"
add "implicit weighted biprediction" check_clip wp_b3_cabac   "$S/syn_motion.y4m" "--cabac --bframes 3"
add "implicit weighted biprediction" check_clip wp_fade_cavlc "$S/syn_fade.y4m"   ""
add "implicit weighted biprediction" check_clip wp_fade_cabac "$S/syn_fade.y4m"   "--cabac"
add "implicit weighted biprediction" check_clip wp_fade_mref3 "$S/syn_fade.y4m"   "--ref 3"
add "implicit weighted biprediction" check_clip wp_fade_mref4 "$S/syn_fade.y4m"   "--cabac --ref 4"

add "multiple references + B frames" check_clip mref3_b1        "$S/syn_motion.y4m"  "--ref 3 --bframes 1"
add "multiple references + B frames" check_clip mref3_b1_cabac  "$S/syn_motion.y4m"  "--cabac --ref 3 --bframes 1"
add "multiple references + B frames" check_clip mref3_b2        "$S/syn_motion.y4m"  "--ref 3 --bframes 2"
add "multiple references + B frames" check_clip mref3_b3_cabac  "$S/syn_motion.y4m"  "--cabac --ref 3 --bframes 3"
add "multiple references + B frames" check_clip mref4_b3_8x8    "$S/syn_motion.y4m"  "--cabac --ref 4 --bframes 3 --transform-8x8"
add "multiple references + B frames" check_clip mref3_b2_crop   "$S/syn_178x100.y4m" "--cabac --ref 3 --bframes 2"
add "multiple references + B frames" check_clip mref3_b3_keyint "$S/syn_motion.y4m"  "--cabac --ref 3 --bframes 3 --keyint 5"
add "multiple references + B frames" check_clip mref3_b2_fade   "$S/syn_fade.y4m"    "--cabac --ref 3 --bframes 2"

add "b-pyramid (hierarchical B)" check_clip bpyr_b2       "$S/syn_motion.y4m"  "--bframes 2"
add "b-pyramid (hierarchical B)" check_clip bpyr_b3_cabac "$S/syn_motion.y4m"  "--cabac --bframes 3"
add "b-pyramid (hierarchical B)" check_clip bpyr_keyint   "$S/syn_motion.y4m"  "--cabac --bframes 3 --keyint 5"
add "b-pyramid (hierarchical B)" check_clip bpyr_8x8_crop "$S/syn_178x100.y4m" "--cabac --bframes 3 --transform-8x8"

add "scene-cut detection" check_clip scenecut_cabac "$S/sc_cut.y4m" "--cabac --bframes 2 --keyint 100"
add "scene-cut detection" check_scenecut_kf "$S/sc_cut.y4m"
add "scene-cut detection" check_flash_kf "$S/flash1.y4m" 1 "one-frame flash suppressed"
add "scene-cut detection" check_flash_kf "$S/flash2.y4m" 2 "two-frame flash still cuts"
add "scene-cut detection" check_clip flash1_recon "$S/flash1.y4m" "--cabac --bframes 3 --keyint 100"
add "scene-cut detection" check_clip flash2_recon "$S/flash2.y4m" "--cabac --bframes 3 --keyint 100"

add "lookahead window" check_clip la6_b2      "$S/syn_motion.y4m" "--cabac --bframes 2 --rc-lookahead 6 --keyint 8"
add "lookahead window" check_clip la8_b3_mref "$S/syn_motion.y4m" "--cabac --bframes 3 --ref 3 --rc-lookahead 8"
add "lookahead window" check_clip la_cut      "$S/sc_cut.y4m"     "--cabac --bframes 2 --keyint 100"
add "lookahead window" check_clip badapt_b3   "$S/syn_motion.y4m" "--cabac --bframes 3 --keyint 5"
add "lookahead window" check_clip badapt_off  "$S/syn_motion.y4m" "--cabac --bframes 3 --b-adapt 0"
add "lookahead window" check_clip badapt_cut  "$S/sc_cut.y4m"     "--bframes 3 --keyint 100"
add "lookahead window" check_clip psy_off     "$S/syn_motion.y4m" "--psy-rd 0"
add "lookahead window" check_clip psy_strong  "$S/syn_motion.y4m" "--cabac --psy-rd 2.0 --bframes 2"
add "lookahead window" check_clip dtemp_b1    "$S/syn_motion.y4m" "--direct temporal --bframes 1"
add "lookahead window" check_clip dtemp_b3    "$S/syn_motion.y4m" "--cabac --direct temporal --bframes 3"
add "lookahead window" check_clip dtemp_mref  "$S/syn_motion.y4m" "--cabac --direct temporal --bframes 2 --ref 3"
add "lookahead window" check_clip dtemp_keyint "$S/syn_motion.y4m" "--cabac --direct temporal --bframes 3 --keyint 5"

add "adaptive quantization (per-MB QP)" check_clip aq_p       "$S/syn_motion.y4m"  "--aq-strength 1.0"
add "adaptive quantization (per-MB QP)" check_clip aq_p_cabac "$S/syn_motion.y4m"  "--cabac --aq-strength 1.0"
add "adaptive quantization (per-MB QP)" check_clip aq_b_cabac "$S/syn_motion.y4m"  "--cabac --bframes 3 --aq-strength 0.8"
add "adaptive quantization (per-MB QP)" check_clip aq_crop    "$S/syn_178x100.y4m" "--cabac --aq-strength 1.0"

add "determinism" check_determinism base   "$S/syn_320x240.y4m"
# 4:2:2 + B frames: guards the format-aware B chroma reconstruction (a chroma
# prediction hardcoded to 4:2:0 geometry left the lower half of the 8x16 chroma
# block uninitialized, making 4:2:2+B output nondeterministic).
add "determinism" check_determinism c422_b2 "$S/syn_422.y4m" "--cabac --bframes 2"
add "determinism" check_determinism c422_b2_cavlc "$S/syn_422.y4m" "--bframes 2"
add "determinism" check_determinism c444_b1 "$S/syn_444.y4m" "--cabac --bframes 1"

# 4:4:4 + B frames: recon-match gate for the B_Skip chroma path. B_Skip stored
# only the top-left 8x8 of the full-res 16x16 chroma prediction, at origin mbx*8
# instead of mbx*16, so the encoder's committed chroma diverged from the decoder
# (which reconstructs the full direct prediction). Pinned to qp26: qp51 trips a
# separate 4:4:4 quantization mismatch unrelated to this path.
add "4:4:4 B-frame recon" check_clip c444_b1_cabac "$S/syn_444.y4m" "--cabac --bframes 1" "26"
add "4:4:4 B-frame recon" check_clip c444_b1_cavlc "$S/syn_444.y4m" "--bframes 1"        "26"
# Full-length (16-frame) 4:4:4 + B: guards against chroma-deblock reference drift
# (a wrong 4:4:4 chroma edge geometry would corrupt a ref plane and diverge ~frame 5).
add "4:4:4 B-frame recon" check_clip c444_16_b1_cabac "$S/syn_444_16.y4m" "--cabac --bframes 1" "26"
add "4:4:4 B-frame recon" check_clip c444_16_b2_cabac "$S/syn_444_16.y4m" "--cabac --bframes 2" "26"
add "4:4:4 B-frame recon" check_clip c444_16_b1_cavlc "$S/syn_444_16.y4m" "--bframes 1"        "26"

# 4:4:4 high-QP intra recon: 4:4:4 codes Cb/Cr like luma but must quantise them
# with the chroma QP (QPc), not the luma QP. QPc==QPY below QP 30, so the bug only
# bites at high QP (recon != decode on chroma from qp>=30). All-intra (--keyint 1)
# isolates it from the B path; qp51 is the worst case.
add "4:4:4 intra recon" check_clip c444_intra_cabac "$S/syn_444.y4m" "--cabac --keyint 1" "51"
add "4:4:4 intra recon" check_clip c444_intra_cavlc "$S/syn_444.y4m" "--keyint 1"         "51"

# --- 10-bit (item C3-10bit) ----------------------------------------------
# One binary, two libraries, and the y4m C tag picks between them. These cells
# are the recon-match gate on the 10-bit one: the decoder's yuv420p10le output
# has to equal the encoder's own 10-bit reconstruction, at both entropy coders,
# with B frames, with the 8x8 transform and on a cropped picture.
add "10-bit" check_clip p10_cabac  "$S/syn_p10_420.y4m" "--cabac"
add "10-bit" check_clip p10_cavlc  "$S/syn_p10_420.y4m" ""
add "10-bit" check_clip p10_b3     "$S/syn_p10_420.y4m" "--cabac --bframes 3"
add "10-bit" check_clip p10_8x8    "$S/syn_p10_420.y4m" "--cabac --transform-8x8 --bframes 3"
add "10-bit" check_clip p10_crop   "$S/syn_p10_crop.y4m" "--cabac --transform-8x8"
add "10-bit" check_clip p10_422    "$S/syn_p10_422.y4m" "--cabac --bframes 1" "26"
add "10-bit" check_clip p10_444    "$S/syn_p10_444.y4m" "--cabac --bframes 1" "26"
# --output-depth 10 on 8-bit input: the upshift path, which is the only read
# path that converts rather than copying.
add "10-bit" check_clip p10_up     "$S/syn_motion.y4m" "--cabac --bframes 3 --output-depth 10"
add "10-bit" check_determinism p10_b3 "$S/syn_p10_420.y4m" "--cabac --bframes 3 --qp 26"
add "10-bit" check_threading p10 "$S/syn_p10_420.y4m" "--cabac --transform-8x8 --bframes 2"

add "threading" check_threading base "$S/syn_320x240.y4m" ""
add "threading" check_threading cbc  "$S/syn_320x240.y4m" "--cabac"
add "threading" check_threading t8   "$S/syn_320x240.y4m" "--transform-8x8"
add "threading" check_threading mix  "$S/syn_320x240.y4m" "--cabac --transform-8x8 --bframes 2"
# 4:2:2/4:4:4 reached the GOP-parallel path on 2026-08-13; before that they were
# routed to the serial encoder, so nothing here exercised the reader's chroma
# geometry under threads. These two are the guard on that.
add "threading" check_threading c422 "$S/syn_422.y4m" "--cabac --transform-8x8 --bframes 2"
add "threading" check_threading c444 "$S/syn_444.y4m" "--cabac --transform-8x8 --bframes 2"
add "threading" check_threaded_decode "$S/syn_320x240.y4m"

add "ABR rate control" check_rc abr1 "$S/syn_motion.y4m" "--bitrate 800"
add "ABR rate control" check_rc abr2 "$S/syn_motion.y4m" "--cabac --bitrate 1500 --bframes 3"

add "CRF rate control" check_rc crf1 "$S/syn_motion.y4m" "--crf 26"
add "CRF rate control" check_rc crf2 "$S/syn_motion.y4m" "--cabac --crf 22 --bframes 3"

add "VBV constrained rate" check_rc vbv "$S/syn_motion.y4m" \
    "--cabac --crf 16 --vbv-maxrate 600 --vbv-bufsize 600 --bframes 2"

# --- nal-hrd: the buffer model written INTO the stream ---------------------
# Each of these carries its own verification: check_rc sees --nal-hrd in the
# spec and reads the declaration back with hrd_check.py, which is told nothing
# on its command line and takes BitRate, CpbSize, the initial delay and every
# removal time out of the bitstream. The cbr cell is the one that exercises
# filler, and the tff cell is the one that exercises a pic_timing per FIELD --
# two access units per input frame, one clock tick apart, with the pic_struct
# parity that says which half of the pair each one is.
add "nal-hrd" check_rc hrd_vbr "$S/syn_motion.y4m" \
    "--cabac --crf 20 --vbv-maxrate 600 --vbv-bufsize 600 --nal-hrd vbr"
add "nal-hrd" check_rc hrd_cbr "$S/syn_motion.y4m" \
    "--cabac --crf 20 --vbv-maxrate 600 --vbv-bufsize 600 --nal-hrd cbr"
add "nal-hrd" check_rc hrd_bfr "$S/syn_motion.y4m" \
    "--cabac --bitrate 800 --vbv-maxrate 800 --vbv-bufsize 800 --bframes 3 --nal-hrd cbr"
add "nal-hrd" check_rc hrd_tff "$S/syn_tff.y4m" \
    "--tff --cabac --crf 26 --vbv-maxrate 600 --vbv-bufsize 600 --nal-hrd vbr"
add "nal-hrd" check_rc hrd_sl  "$S/syn_motion.y4m" \
    "--cabac --crf 20 --vbv-maxrate 600 --vbv-bufsize 600 --slices 4 --nal-hrd vbr"

add "2-pass rate control" check_twopass base "$S/syn_motion.y4m"

# --- promoted plumbing flags ---------------------------------------------
# One line per flag that changes the stream. Each was reachable before as a
# Y264_* variable or not at all; the point of the cell is that the flag's
# stream still decodes to the encoder's own reconstruction.
add "plumbing flags" check_clip pl_aqmode1   "$S/syn_motion.y4m"  "--cabac --aq-mode 1 --aq-strength 1.0"
add "plumbing flags" check_clip pl_aqmode2   "$S/syn_motion.y4m"  "--aq-mode 2 --aq-strength 1.0"
add "plumbing flags" check_clip pl_nombtree  "$S/syn_motion.y4m"  "--cabac --bframes 3 --no-mbtree"
add "plumbing flags" check_clip pl_nodctdec  "$S/syn_motion.y4m"  "--cabac --no-dct-decimate"
add "plumbing flags" check_clip pl_nodctdec8 "$S/syn_motion.y4m"  "--cabac --transform-8x8 --no-dct-decimate"
add "plumbing flags" check_clip pl_nopskip   "$S/syn_motion.y4m"  "--cabac --no-fast-pskip"
add "plumbing flags" check_clip pl_nopskip_c "$S/syn_178x100.y4m" "--no-fast-pskip"
add "plumbing flags" check_clip pl_nopsy     "$S/syn_motion.y4m"  "--cabac --bframes 2 --no-psy"
add "plumbing flags" check_clip pl_noasm     "$S/syn_motion.y4m"  "--cabac --transform-8x8 --no-asm"
add "plumbing flags" check_twopass pl_ratios "$S/syn_motion.y4m" "--ipratio 2.0 --pbratio 1.6"
add "plumbing flags" check_twopass pl_blurs  "$S/syn_motion.y4m" "--cplxblur 5 --qblur 2"

# The literals that became flags. Every one of these writes a different slice
# header, PPS or SPS than the default does, so the cell is asking whether the
# decoder reads back exactly what the encoder reconstructed from it.
add "plumbing flags" check_clip pl_deblock_n  "$S/syn_motion.y4m"  "--cabac --deblock -3:-3"
add "plumbing flags" check_clip pl_deblock_p  "$S/syn_motion.y4m"  "--deblock 2:6"
add "plumbing flags" check_clip pl_deblock_b  "$S/syn_motion.y4m"  "--cabac --bframes 3 --deblock -6:2"
add "plumbing flags" check_clip pl_deblock_c  "$S/syn_178x100.y4m" "--cabac --transform-8x8 --deblock 1:-1"
add "plumbing flags" check_clip pl_nodeblock  "$S/syn_motion.y4m"  "--cabac --bframes 2 --no-deblock"
add "plumbing flags" check_clip pl_nodeblock4 "$S/syn_422.y4m"     "--cabac --no-deblock"
add "plumbing flags" check_clip pl_cqpo_pos   "$S/syn_motion.y4m"  "--cabac --chroma-qp-offset 6"
add "plumbing flags" check_clip pl_cqpo_neg   "$S/syn_motion.y4m"  "--chroma-qp-offset -6"
add "plumbing flags" check_clip pl_cqpo_ext   "$S/syn_motion.y4m"  "--cabac --bframes 2 --chroma-qp-offset 12"
add "plumbing flags" check_clip pl_cqpo_422   "$S/syn_422.y4m"     "--cabac --chroma-qp-offset -12"
add "plumbing flags" check_clip pl_cqpo_444   "$S/syn_444_16.y4m"  "--cabac --bframes 1 --chroma-qp-offset 4" "26"
add "plumbing flags" check_clip pl_bpyr_none  "$S/syn_motion.y4m"  "--cabac --bframes 3 --b-pyramid none"
add "plumbing flags" check_clip pl_bpyr_none2 "$S/syn_motion.y4m"  "--bframes 2 --b-pyramid none"
add "plumbing flags" check_clip pl_noweightb  "$S/syn_fade.y4m"    "--cabac --bframes 3 --no-weightb"
add "plumbing flags" check_clip pl_noweightb2 "$S/syn_motion.y4m"  "--bframes 2 --no-weightb"
add "plumbing flags" check_clip pl_spsid      "$S/syn_motion.y4m"  "--cabac --sps-id 31"
add "plumbing flags" check_clip pl_mvrange    "$S/syn_motion.y4m"  "--cabac --mvrange 32"
add "plumbing flags" check_clip pl_qpbounds   "$S/syn_motion.y4m"  "--cabac --qpmin 20 --qpmax 30"
add "plumbing flags" check_rc   pl_qprc       "$S/syn_motion.y4m"  "--cabac --bitrate 800 --qpmin 18 --qpmax 40 --qpstep 2"
add "plumbing flags" check_rc   pl_vbvinit    "$S/syn_motion.y4m"  "--cabac --bitrate 800 --vbv-maxrate 800 --vbv-bufsize 200 --vbv-init 0.4"
add "plumbing flags" check_pass3 "$S/syn_motion.y4m"

# constrained_intra_pred_flag. In a P or B slice an intra macroblock may read
# neither the samples nor the intra mode of an inter-coded neighbour, and the
# decoder derives that availability for itself, so a recon-match IS the claim:
# analysis, reconstruction and the mode predictor all have to agree with it or
# the pictures part. syn_motion is the fixture that leaves inter neighbours
# around an intra macroblock in the first place. Both entropy coders, because
# the intra mode predictor is written on two separate paths; 8x8 on both, for
# the I_8x8 half; 4:2:2 and 4:4:4, where chroma is a different shape and then
# coded like luma. syn_noise carries the cell that no other clip here produces:
# the above-left neighbour withdrawing on its own, with the top and the left
# still available, which is the only way PLANE and the three corner-reading
# 4x4 modes get refused while the block still has both edges (104-204
# macroblocks per 40 frames; the natural clips produce none).
add "constrained intra" check_clip cintra_p        "$S/syn_motion.y4m"  "--constrained-intra"
add "constrained intra" check_clip cintra_p_cabac  "$S/syn_motion.y4m"  "--cabac --constrained-intra"
add "constrained intra" check_clip cintra_b3_cabac "$S/syn_motion.y4m"  "--cabac --bframes 3 --constrained-intra"
add "constrained intra" check_clip cintra_8x8      "$S/syn_motion.y4m"  "--cabac --transform-8x8 --bframes 2 --constrained-intra"
add "constrained intra" check_clip cintra_8x8_cav  "$S/syn_motion.y4m"  "--transform-8x8 --constrained-intra"
add "constrained intra" check_clip cintra_crop     "$S/syn_178x100.y4m" "--cabac --constrained-intra"
add "constrained intra" check_clip cintra_noise    "$S/syn_noise.y4m"   "--cabac --bframes 2 --constrained-intra"
add "constrained intra" check_clip cintra_422      "$S/syn_422.y4m"     "--cabac --bframes 2 --constrained-intra" "26"
add "constrained intra" check_clip cintra_444      "$S/syn_444_16.y4m"  "--cabac --bframes 1 --constrained-intra" "26"
add "constrained intra" check_threading cintra "$S/syn_320x240.y4m" "--cabac --bframes 2 --constrained-intra"

# --slices cuts each picture into N independently decodable slices on
# macroblock-row boundaries. Every cell here asks the one question the encoder
# cannot ask itself: does a decoder that resets its prediction, its entropy
# state and its QP chain at each slice start reconstruct what we did. The
# spread is over what crosses a slice edge -- the intra mode predictor and the
# reference samples (both entropy coders, 4x4 and 8x8), the CAVLC nC and
# mb_skip_run, the CABAC contexts and the mvd/ref_idx neighbours (b3), the
# temporal-direct colocated derivation, the AQ per-macroblock QP offsets
# against the per-slice mb_qp_delta chain, and a cropped picture whose slice
# cuts land on rows the crop removes. slices3 on syn_178x100 is 7 rows over 3
# slices, i.e. an UNEVEN split, which is the case a "rows / slices" that
# divides exactly never reaches.
add "slices" check_clip slices2_cavlc   "$S/syn_motion.y4m"  "--slices 2"
add "slices" check_clip slices4_cabac   "$S/syn_motion.y4m"  "--cabac --slices 4"
add "slices" check_clip slices4_b3_8x8  "$S/syn_motion.y4m"  "--cabac --bframes 3 --transform-8x8 --slices 4"
add "slices" check_clip slices4_b3_cav  "$S/syn_motion.y4m"  "--bframes 3 --slices 4"
add "slices" check_clip slices3_crop    "$S/syn_178x100.y4m" "--cabac --slices 3"
add "slices" check_clip slices4_aq      "$S/syn_motion.y4m"  "--cabac --aq-strength 1.0 --slices 4"
add "slices" check_clip slices4_dtemp   "$S/syn_motion.y4m"  "--cabac --bframes 3 --direct temporal --ref 3 --slices 4"
add "slices" check_clip slices_row      "$S/syn_320x240.y4m" "--cabac --bframes 2 --slices 15"
add "slices" check_clip slices4_422     "$S/syn_422.y4m"     "--cabac --bframes 2 --slices 4" "26"
add "slices" check_clip slices4_444     "$S/syn_444_16.y4m"  "--cabac --bframes 1 --slices 4" "26"
add "slices" check_threading slices4 "$S/syn_320x240.y4m" "--cabac --bframes 2 --slices 4"
add "slices" check_determinism slices4 "$S/syn_320x240.y4m" "--qp 26 --cabac --bframes 2 --slices 4"

# --profile writes a profile_idc and a constraint_set byte the stream was
# checked against, and baseline additionally turns weighted prediction off.
# Each cell asks whether a decoder that reads the header gets back what the
# encoder reconstructed under it.
add "profile" check_clip pr_baseline  "$S/syn_motion.y4m"  "--profile baseline"
add "profile" check_clip pr_baseline2 "$S/syn_178x100.y4m" "--profile baseline"
add "profile" check_clip pr_baseline3 "$S/syn_fade.y4m"    "--profile baseline"
add "profile" check_clip pr_main      "$S/syn_motion.y4m"  "--profile main --cabac --bframes 3"
add "profile" check_clip pr_main_cav  "$S/syn_motion.y4m"  "--profile main --cavlc --bframes 2"
add "profile" check_clip pr_high      "$S/syn_motion.y4m"  "--profile high --cabac --transform-8x8"
add "profile" check_clip pr_high_cqm  "$S/syn_motion.y4m"  "--profile high --cabac --cqm jvt"
add "profile" check_clip pr_high422   "$S/syn_motion.y4m"  "--profile high422 --cabac"
add "profile" check_clip pr_high422b  "$S/syn_422.y4m"     "--profile high422 --cabac --bframes 2"
add "profile" check_clip pr_high444   "$S/syn_444_16.y4m"  "--profile high444 --cabac --bframes 1" "26"
add "profile" check_profile_refusals "$S/syn_motion.y4m"

# The declared level, and the VUI's vertical MV bound inside it. --exact on the
# auto-level cells: the encoder's pick must be the lowest conformant one, not
# merely a legal one. The --mvrange cell is the one --strict-mv was added for.
add "level" check_level lv_auto     "$S/syn_motion.y4m"  ""                          "--exact"
add "level" check_level lv_auto_b3  "$S/syn_motion.y4m"  "--cabac --bframes 3 --ref 4" "--exact"
add "level" check_level lv_crop     "$S/syn_178x100.y4m" "--cabac"                    "--exact"
add "level" check_level lv_prof_bl  "$S/syn_motion.y4m"  "--profile baseline"         "--exact"
add "level" check_level lv_prof_m   "$S/syn_motion.y4m"  "--profile main --cabac"     "--exact"
add "level" check_level lv_prof_h   "$S/syn_motion.y4m"  "--profile high --cabac --transform-8x8" "--exact"
add "level" check_level lv_prof_422 "$S/syn_422.y4m"     "--profile high422 --cabac"  "--exact"
add "level" check_level lv_prof_444 "$S/syn_444_16.y4m"  "--profile high444 --cabac"  "--exact"
add "level" check_level lv_mvrange  "$S/syn_motion.y4m"  "--cabac --mvrange 64"       "--exact"
add "level" check_level lv_forced   "$S/syn_motion.y4m"  "--cabac --level 5.1"        ""

# Stream-level signalling: new NAL and SEI syntax beside the parameter sets and
# inside every access unit. None of it moves a sample, so what the cell asks is
# whether a decoder still reads the picture back after the new bytes are there.
add "signalling" check_clip sg_aud       "$S/syn_motion.y4m"  "--cabac --aud"
add "signalling" check_clip sg_aud_b     "$S/syn_motion.y4m"  "--cabac --bframes 3 --aud"
add "signalling" check_clip sg_picstruct "$S/syn_motion.y4m"  "--cabac --pic-struct"
add "signalling" check_clip sg_aud_ps    "$S/syn_motion.y4m"  "--cabac --bframes 2 --aud --pic-struct"
add "signalling" check_clip sg_fp_sbs    "$S/syn_motion.y4m"  "--cabac --frame-packing 3"
add "signalling" check_clip sg_fp_tb     "$S/syn_motion.y4m"  "--frame-packing 4"
add "signalling" check_clip sg_fp_alt    "$S/syn_motion.y4m"  "--cabac --frame-packing 5"
add "signalling" check_clip sg_cll       "$S/syn_motion.y4m"  "--cabac --cll 1000,400"
add "signalling" check_clip sg_mastering "$S/syn_motion.y4m"  "--cabac --mastering-display G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1)"
add "signalling" check_clip sg_hdr10     "$S/syn_motion.y4m"  "--cabac --colorprim bt2020 --transfer smpte2084 --colormatrix bt2020 --cll 1000,400 --mastering-display G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1)"
add "signalling" check_clip sg_alttrc    "$S/syn_motion.y4m"  "--cabac --alternative-transfer arib-std-b67"
add "signalling" check_clip sg_overscan  "$S/syn_motion.y4m"  "--cabac --overscan crop"
add "signalling" check_clip sg_vformat   "$S/syn_motion.y4m"  "--cabac --videoformat pal"
add "signalling" check_clip sg_vf_colour "$S/syn_motion.y4m"  "--cabac --videoformat ntsc --colorprim bt709 --transfer bt709 --colormatrix bt709"
add "signalling" check_clip sg_stitch    "$S/syn_motion.y4m"  "--cabac --bframes 3 --ref 4 --stitchable"
add "signalling" check_clip sg_fakeint   "$S/syn_motion.y4m"  "--cabac --fake-interlaced"
add "signalling" check_clip sg_fakeint_b "$S/syn_motion.y4m"  "--cabac --bframes 3 --fake-interlaced"
add "signalling" check_clip sg_fakeint_o "$S/syn_178x100.y4m" "--cabac --fake-interlaced"
add "signalling" check_clip sg_fakeint_c "$S/syn_422.y4m"     "--cabac --fake-interlaced"
add "signalling" check_clip sg_all       "$S/syn_motion.y4m"  "--cabac --bframes 2 --aud --pic-struct --stitchable --overscan show --videoformat component"
add "signalling" check_stitch_sps "$S/syn_motion.y4m"
add "level" check_level lv_stitch   "$S/syn_motion.y4m"  "--cabac --bframes 3 --ref 4 --stitchable" ""
add "level" check_level lv_fakeint  "$S/syn_motion.y4m"  "--cabac --fake-interlaced"                ""

# The input layer. Every one of these has an EXACT oracle: doing the same thing
# with ffmpeg beforehand and encoding the result must give the same bytes, so
# the cell is a cmp against a pre-processed clip rather than a decode.
add "input layer" check_raw_equiv  "$S/syn_motion.y4m"
add "input layer" check_seek_equiv "$S/syn_long.y4m"
add "input layer" check_crop_equiv "$S/syn_320x240.y4m"
add "input layer" check_clip cli_tune_still "$S/syn_motion.y4m" "--tune stillimage"
add "input layer" check_clip cli_tune_fast  "$S/syn_motion.y4m" "--tune fastdecode"
add "input layer" check_clip cli_tune_fast8 "$S/syn_motion.y4m" "--tune fastdecode --bframes 3"
add "input layer" check_clip cli_crop       "$S/syn_320x240.y4m" "--cabac --crop-rect 16,16,16,16"
add "input layer" check_clip cli_crop_422   "$S/syn_422.y4m"     "--cabac --crop-rect 16,0,16,0"

# The same three oracles at 10 bits, so the input layer and the depth dispatch
# are shown to compose rather than assumed to. The raw arm carries the depth in
# --input-csp, because a raw file has no C tag and there is no --input-depth.
add "input layer" check_raw_equiv  "$S/syn_p10_420.y4m"
add "input layer" check_raw_equiv  "$S/syn_p10_422.y4m"
add "input layer" check_seek_equiv "$S/syn_p10_420.y4m"
add "input layer" check_crop_equiv "$S/syn_p10_420.y4m"
add "input layer" check_clip cli_crop_p10 "$S/syn_p10_420.y4m" "--cabac --crop-rect 16,16,16,16"
add "input layer" check_clip cli_seek_p10 "$S/syn_p10_420.y4m" "--cabac --seek 3"

# corpus clips, if fetched (truncated in fast mode)
if compgen -G "$root/tests/corpus/*.y4m" >/dev/null; then
    for src in "$root"/tests/corpus/*.y4m; do
        base="corpus_$(basename "$src" .y4m)"
        use="$src"
        if [ "$CORPUS_FRAMES" -gt 0 ]; then
            use="$fixdir/${base}_${CORPUS_FRAMES}f.y4m"
            if [ ! -f "$use" ]; then
                ffmpeg -v error -i "$src" -frames:v "$CORPUS_FRAMES" \
                    -f yuv4mpegpipe "$use.tmp.$$" && mv "$use.tmp.$$" "$use"
            fi
        fi
        # one job per QP so the big CIF clips load-balance instead of forming
        # a serial long pole at the tail of the pool.
        for qp in $QPS; do
            add "corpus clips" check_clip "$base" "$use" "" "$qp"
        done
    done
fi

# --- run the pool --------------------------------------------------------
mode="full"; [ "$YAH264_CONF_FAST" = 1 ] && mode="fast"
echo "conformance: $jobn checks, $mode mode, -P $JOBS, decoders: $DECODERS"
printf '%s\0' "${jobs[@]}" | xargs -0 -P "$JOBS" -n1 bash -c 'eval "$1"' _ || true

# --- aggregate in job order ---------------------------------------------
tests=0; fails=0; section=""
for r in "$resdir"/*; do
    [ -f "$r" ] || continue
    sec="$(sed -n 's/^SECTION //p;q' "$r")"
    if [ "$sec" != "$section" ]; then
        section="$sec"; echo "conformance: $section"
    fi
    if grep -q '^SUMMARY ' "$r"; then
        # EVERY SUMMARY line, not the last one. A job may report more than one
        # verdict -- check_threading reports the cross-thread identity and then
        # the ABR carry's repeat-determinism -- and reading only the last one
        # DISCARDS the earlier ones. It discarded a real failure: --slices 4
        # was thread-variant while the run printed "1217/1217 passed", because
        # the carry check that followed it passed and overwrote the count.
        set -- $(awk '$1 == "SUMMARY" { t += $2; f += $3 } END { print t+0, f+0 }' "$r")
        grep -v -e '^SECTION ' -e '^SUMMARY ' -e '^DEC ' "$r" || true
        tests=$((tests + $1)); fails=$((fails + $2))
    else
        grep -v -e '^SECTION ' -e '^DEC ' "$r" || true
        echo "  FAIL (worker produced no summary)"
        tests=$((tests + 1)); fails=$((fails + 1))
    fi
done

# --- what each oracle actually covered -----------------------------------
# Printed unconditionally, because the number that matters about a multi-decoder
# gate is not "did it pass" but "how much did each decoder get to look at".
# openh264 skips every B-frame, 4:2:2, 4:4:4 and interlaced cell by design, so a
# green run with it configured still leaves most of the corpus checked by the
# other two -- and the line below is where that is visible instead of assumed.
if [ "$DECODERS" != "ffmpeg" ]; then
    echo "conformance: per-decoder coverage"
    for d in $DECODERS; do
        set -- $(cat "$resdir"/* 2>/dev/null | awk -v d="$d" '
            $1 == "DEC" && $2 == d { c += $3; s += $4; f += $5 }
            END { printf "%d %d %d\n", c, s, f }')
        echo "  $d: $1 checked, $2 skipped (cannot decode), $3 mismatched"
    done
fi

echo "conformance: $((tests - fails))/$tests passed"
[ "$fails" -eq 0 ]
