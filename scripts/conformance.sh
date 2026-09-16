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

FIXVER=1                        # bump to invalidate cached fixtures

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
          }
          if (w != "" && h != "") printf "%s %sx%s\n", pf, w, h }'
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

check_twopass() {   # check_twopass <src>
    local src="$1"
    "$enc" --input-y4m "$src" --cabac --bframes 3 --pass 1 \
        --stats "$work/2p.stats" --threads 1 -o /dev/null 2>/dev/null || true
    "$enc" --input-y4m "$src" --cabac --bframes 3 --pass 2 \
        --stats "$work/2p.stats" --bitrate 600 --threads 1 \
        -o "$work/2p.264" --dump-recon "$work/2p.rec.y4m" 2>/dev/null || true
    recon_match "2-pass" "$work/2p.rec.y4m" "$work/2p.264" "$work/2p"
    [ "$RM_F" -eq 0 ] && echo "  ok   recon-match (pass 2)"
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

add "2-pass rate control" check_twopass "$S/syn_motion.y4m"

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
        set -- $(grep '^SUMMARY ' "$r" | tail -1)   # SUMMARY t f
        grep -v -e '^SECTION ' -e '^SUMMARY ' -e '^DEC ' "$r" || true
        tests=$((tests + $2)); fails=$((fails + $3))
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
