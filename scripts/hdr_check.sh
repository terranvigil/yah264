#!/usr/bin/env bash
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# hdr_check.sh -- the HDR check behind site/hdr.md: two HDR10 clips from
# Netflix Open Content, a CRF ladder at 1080p and 4K, each encode scored with
# HDR-VMAF v1.1.8 (and its PQ CAMBI).
#
#   YAH264=build-rel/cli/yah264 \
#   VMAF_HDR_BIN=/path/to/vmaf VMAF_HDR_MODELS=/path/to/vmaf/model \
#   WORK=/scratch/hdr scripts/hdr_check.sh
#
# With `meta` it checks the signaling instead and scores nothing: short
# spans of both clips as HDR10, Sol Levante as HLG and as SDR-compatible HLG,
# each encoded by yah264 and by x264 with the same color, mastering-display
# and light-level options, then what ffprobe and MediaInfo read from each,
# field by field. FF names an ffmpeg built with libyah264; with it the cases
# also go through `-c:v libyah264` against `-c:v libx264` (FFX, default the
# ffmpeg on PATH), into MP4 and MPEG-TS, plus a leg where the HDR metadata
# comes from the input's own VUI and SEI instead of from options.
#
#   YAH264=build/cli/yah264 X264=x264 FF=/path/to/ffmpeg \
#   WORK=/scratch/hdr scripts/hdr_check.sh meta
#
# It exits 1 when a field that matters differs (marked !!). Level and
# reference count print beside them (marked ~ when they differ) but don't
# count. FRAMES (48), KEYINT (24) and THREADS set the spans. KEYINT below
# FRAMES puts a second IDR in every stream, which is where a player that
# seeks or joins late has to find the metadata.
#
# VMAF_HDR_BIN is a libvmaf `vmaf` tool built with -Denable_zimg=true, which
# the HDR models need for their colorspace conversion. VMAF_HDR_MODELS is the
# repository's model/ directory, holding hdr_vmaf_v1.1.8 and its _hfr twin.
# Needs ffmpeg with zscale, curl, python3 and numpy. The downloads are about
# 17 GB (300 Sparks TIFF frames and 10 s of Sol Levante ProRes), and WORK needs
# about 45 GB in all. Every step writes under a temporary name and keeps its
# output only when it finishes, so an interrupted run picks up where it
# stopped, and a run over finished outputs just prints the table again.
# A step is redone when an input is newer than its output, to the second.
# Delete WORK/enc after rebuilding the encoder or changing THREADS or the
# options below, or the old encodes are reused. The site's numbers were made
# at the default THREADS=6; another thread count gives slightly different
# streams.
set -euo pipefail
unset CDPATH

abs() { case $1 in /*) echo "$1" ;; */*) echo "$PWD/$1" ;; *) [ -e "$1" ] && echo "$PWD/$1" || command -v "$1" || echo "$1" ;; esac; }
MODE=${1:-quality}
case $MODE in quality|meta) ;; *) echo "usage: hdr_check.sh [meta]" >&2; exit 2 ;; esac
Y=$(abs "${YAH264:-build-rel/cli/yah264}")
if [ $MODE = quality ]; then
  VMAF=$(abs "${VMAF_HDR_BIN:?set VMAF_HDR_BIN to a zimg-enabled libvmaf vmaf tool}")
  M=$(abs "${VMAF_HDR_MODELS:?set VMAF_HDR_MODELS to libvmaf model/ directory}")
fi
W=${WORK:?set WORK to a scratch directory with ~45 GB free}
CRFS=${CRFS:-18 22 26 30 34}
THREADS=${THREADS:-6}
NOC=https://s3.amazonaws.com/download.opencontent.netflix.com
# both titles were graded on a 1000-nit P3 D65 display
MD='G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1)'
COLOR=(--colorprim bt2020 --transfer smpte2084 --colormatrix bt2020nc --range limited)
VCOLOR=()
for s in ref dist; do
  VCOLOR+=("--color_range_$s" limited "--color_primaries_$s" bt2020 "--color_trc_$s" pq "--color_matrix_$s" bt2020nc)
done
frames() { case $1 in sol) echo 240 ;; sparks) echo 300 ;; esac; }
fps() { case $1 in sol) echo 24 ;; sparks) echo 60000/1001 ;; esac; }

die() { echo "hdr_check: $*" >&2; exit 1; }
[ -x "$Y" ] || die "no encoder at $Y (set YAH264)"
if [ $MODE = quality ]; then
  [ -x "$VMAF" ] || die "no vmaf tool at $VMAF"
  for d in hdr_vmaf_v1.1.8 hdr_vmaf_v1.1.8_hfr; do [ -d "$M/$d" ] || die "no $d under $M"; done
fi
for t in curl ffprobe shasum; do command -v $t > /dev/null || die "needs $t"; done
# grep reads the whole list: -q would quit early and SIGPIPE ffmpeg under pipefail
ffmpeg -hide_banner -filters 2> /dev/null | grep ' zscale ' > /dev/null || die "needs ffmpeg with zscale"
# the light levels need numpy; meta checks for it only if it has to prepare
[ $MODE = meta ] || python3 -c 'import numpy' 2> /dev/null || die "needs python3 with numpy"

mkdir -p "$W/sparks_tif" "$W/enc" || die "can't create $W"
cd "$W"
# the file being written, removed if the run stops: a 4K decode is ~7 GB.
# Paths in it are relative to WORK and hold no spaces.
TMP=
LOCKED=

# every process below this one, deepest first
descendants() { local p; for p in $(pgrep -P "$1"); do descendants "$p"; echo "$p"; done; }

# a kill or hangup reaches only bash, and the encoder, scorer or downloads
# it started would run on after the lock went. Stop them all, give them 10 s,
# then remove what was being written and the lock. A run that didn't get
# the lock touches nothing. errexit is off here, so no step can cut the rest
# short, and the run keeps the exit status it had.
cleanup() {
  local rc=$? kids n=0 p
  set +e
  trap '' INT TERM HUP
  [ -n "$LOCKED" ] || exit $rc
  # the tree is walked again on every pass, so a process started after the
  # first TERM (xargs starting the next download) is caught too
  # Only pids still running count: some pgreps list the walk's own subshell,
  # which has exited by the time this shell checks it.
  while kids=; for p in $(descendants $$); do kill -0 $p 2> /dev/null && kids="$kids $p"; done; [ -n "$kids" ]; do
    if [ $n -lt 100 ]; then kill -TERM $kids 2> /dev/null; else kill -KILL $kids 2> /dev/null; fi
    [ $n -lt 110 ] || break
    sleep 0.1; n=$((n + 1))
  done
  rm -f $TMP sparks_tif/*.tif.tmp 2> /dev/null
  rmdir .hdr_check.lock 2> /dev/null
  exit $rc
}
trap cleanup EXIT

# one run per WORK: two would write the same temporary names
if mkdir .hdr_check.lock 2> /dev/null; then
  LOCKED=1
elif [ -d .hdr_check.lock ]; then
  die "another hdr_check is using $W (or a killed one left $W/.hdr_check.lock)"
else
  die "can't create $W/.hdr_check.lock"
fi

# frames in a video file, counted by decoding packets
count() { ffprobe -v error -count_packets -select_streams v:0 -show_entries stream=nb_read_packets -of csv=p=0 "$1"; }

fetch() {
  if [ ! -s sol_src.mov ]; then
    TMP=sol_src.mov.tmp
    ffmpeg -v error -nostdin -ss 160 \
      -i "$NOC/SolLevante/hdr10/SolLevante_HDR10_r2020_ST2084_UHD_24fps_1000nit.mov" \
      -frames:v 240 -c:v copy -an -f mov -y sol_src.mov.tmp || die "Sol Levante download failed"
    [ "$(count sol_src.mov.tmp)" = 240 ] || die "Sol Levante download came out short"
    mv sol_src.mov.tmp sol_src.mov
    TMP=
  fi
  seq -f "%05g" 5880 6179 | xargs -P 8 -I{} sh -c '[ -s sparks_tif/{}.tif ] || {
    curl -sf --retry 3 -o sparks_tif/{}.tif.tmp "$0/sparks/HDR10_1000nit_PQ_2020_image_sequence/SPARKS_HDR10_4K_2020_PQ_1000nits_{}.tif" &&
    mv sparks_tif/{}.tif.tmp sparks_tif/{}.tif; }' "$NOC" || die "Sparks download failed"
}

# write $out through a temporary name; with a clip name, check its frame
# count. -xerror stops at a frame that won't decode, which ffmpeg would
# otherwise fill with a copy of the one before.
y4m() {
  local out=$1 clip=$2 prev=$TMP; shift 2
  TMP="$prev $out.tmp"
  ffmpeg -v error -nostdin -xerror "$@" -strict -1 -f yuv4mpegpipe -y "$out.tmp" ||
    die "ffmpeg failed writing $out"
  [ -z "$clip" ] || [ "$(count "$out.tmp")" = "$(frames $clip)" ] || die "$out has the wrong frame count"
  mv "$out.tmp" "$out"
  TMP=$prev
}

# true when $1 is missing, empty or older than any of the rest. The Sparks
# input is its 300 frames, not their folder, whose time moves when anything
# (a .DS_Store) is added to it.
stale() { local out=$1; shift; [ -s "$out" ] || return 0; for f; do [ "$f" -nt "$out" ] && return 0; done; return 1; }

# the TIFFs are untagged RGB, and DCI-wide; crop to UHD around the center
# the frames by name: a stray .tif in the folder must not count as an input
TIFS=()
for n in $(seq 5880 6179); do TIFS+=("sparks_tif/0$n.tif"); done
# -frames:v stops the read at 06179 even if a later number sits in the folder
SPARKS_IN=(-framerate 60000/1001 -start_number 5880 -i sparks_tif/%05d.tif -frames:v 300)
SPARKS_RGB="crop=3840:2160,setparams=color_primaries=bt2020:color_trc=smpte2084:colorspace=gbr:range=pc"

prep() {
  ! stale sol_2160.y4m sol_src.mov || y4m sol_2160.y4m sol -i sol_src.mov -vf "zscale=f=spline36,format=yuv420p10le"
  ! stale sparks_2160.y4m "${TIFS[@]}" || y4m sparks_2160.y4m sparks "${SPARKS_IN[@]}" \
    -vf "$SPARKS_RGB,zscale=m=2020_ncl:r=tv:f=spline36,format=yuv420p10le"
  for c in sol sparks; do
    ! stale ${c}_1080.y4m ${c}_2160.y4m || y4m ${c}_1080.y4m $c -i ${c}_2160.y4m -vf "zscale=w=1920:h=1080:f=spline36"
  done
  # light levels come from the masters, before 4:2:0: rebuilding RGB from
  # subsampled chroma overshoots at bright edges and inflates MaxCLL
  ! stale sol.cll sol_src.mov || cll sol 3840 2160 -i sol_src.mov -vf "zscale=t=linear:npl=10000:p=bt2020,format=gbrpf32le"
  ! stale sparks.cll "${TIFS[@]}" || cll sparks 3840 2160 "${SPARKS_IN[@]}" -vf "$SPARKS_RGB,zscale=t=linear:npl=10000,format=gbrpf32le"
}

# MaxCLL,MaxFALL in cd/m^2 per CTA-861.3: the brightest max(R,G,B) of any
# pixel, and the brightest frame's mean of it
cll() {
  local c=$1 w=$2 h=$3 prev=$TMP; shift 3
  TMP="$prev $c.cll.tmp"
  ffmpeg -v error -nostdin -xerror "$@" -f rawvideo - | python3 -c '
import sys, numpy as np
n, want = int(sys.argv[1]) * int(sys.argv[2]), int(sys.argv[3])
cll = fall = 0.0
got = 0
while True:
    b = sys.stdin.buffer.read(n * 12)
    if len(b) < n * 12:
        break
    m = np.frombuffer(b, np.float32).reshape(3, n).max(axis=0) * 10000
    cll, fall = max(cll, float(m.max())), max(fall, float(m.mean()))
    got += 1
if got != want or cll <= 0:
    sys.exit(f"cll: read {got} of {want} frames, MaxCLL {cll:.0f}")
print(f"{round(cll)},{round(fall)}")' "$w" "$h" "$(frames $c)" > $c.cll.tmp ||
    die "measuring the light levels of $c failed"
  mv $c.cll.tmp $c.cll
  TMP=$prev
}

model() {
  local d=hdr_vmaf_v1.1.8 h=""
  [ $1 = sparks ] && { d=hdr_vmaf_v1.1.8_hfr; h=_hfr; }
  [ $2 = 2160 ] && echo "$M/$d/hdr_vmaf_v1.1.8${h}_1d5h_2160.json" || echo "$M/$d/hdr_vmaf_v1.1.8${h}_3d0h.json"
}

# a score file that parses, covers every frame and holds scores in range
scored() {
  python3 -c '
import json, math, sys
j = json.load(open(sys.argv[1]))
m = j["pooled_metrics"]
cambi = next(v for k, v in m.items() if k.startswith("cambi"))
v, cm, cx = m["vmaf"]["mean"], cambi["mean"], cambi["max"]
num = all(type(x) in (int, float) and math.isfinite(x) for x in (v, cm, cx))
ok = len(j["frames"]) == int(sys.argv[2]) and num and 0 <= v <= 100 and 0 <= cm <= cx
sys.exit(not ok)' "$1" "$2" 2> /dev/null
}

# a stream decodes with no damaged frame and holds the clip's frame count.
# Strict decoding fails a frame ffmpeg would otherwise conceal.
intact() {
  local n
  ffmpeg -v error -nostdin -xerror -err_detect +explode -i "$1" -f null - || return 1
  n=$(count "$1") || return 1
  [ "$n" = "$(frames $2)" ]
}

sum() { shasum -a 256 "$1" | cut -d' ' -f1; }

failed() { echo "hdr_check: $1 failed, from $2:" >&2; tail -5 "$2" >&2; exit 1; }

one() {
  local c=$1 r=$2 q=$3 o=enc/$1_$2_crf$3
  # an encode older than its source or its light levels is stale
  if stale $o.264 $c.cll ${c}_${r}.y4m; then
    TMP="$o.264.tmp $o.264.sha256.tmp"
    # the old encode's stamp goes first, so it can never sit beside a new one
    rm -f $o.264.sha256
    "$Y" --input-y4m ${c}_${r}.y4m -o $o.264.tmp --crf $q --threads "$THREADS" --no-progress \
      "${COLOR[@]}" --mastering-display "$MD" --cll "$(cat $c.cll)" 2> $o.log || failed "encode $o" $o.log
    intact $o.264.tmp $c 2>> $o.log || failed "encode $o (damaged or short stream)" $o.log
    sum $o.264.tmp > $o.264.sha256.tmp
    # stamp first: a kill between the two leaves the old encode, still stale
    mv $o.264.sha256.tmp $o.264.sha256
    mv $o.264.tmp $o.264
    TMP=
  fi
  # a score older than its encode belongs to an earlier encode
  # a kept score is checked again, in case it was edited or cut since
  if stale $o.json $o.264 || ! scored $o.json "$(frames $c)"; then
    # An encode kept before stamps existed is checked in full once. A failed
    # check moves nothing, since a Ctrl-C that ffmpeg catches fails it too.
    if [ ! -s $o.264.sha256 ]; then
      intact $o.264 $c || die "$o.264 has no stamp and its check failed (interrupted, or the file is damaged); run again, and delete it if it fails again"
      TMP=$o.264.sha256.tmp
      sum $o.264 > $o.264.sha256.tmp
      mv $o.264.sha256.tmp $o.264.sha256
      TMP=
    fi
    # an encode changed since it passed its check would fail every rescore,
    # or be scored damaged; set it aside so the next run encodes again
    if [ "$(sum $o.264)" != "$(cat $o.264.sha256)" ]; then
      mv $o.264 $o.264.bad
      die "$o.264 doesn't match its stamp; moved to $o.264.bad, run again to redo it"
    fi
    TMP="$o.y4m $o.json.tmp"
    y4m $o.y4m $c -i $o.264 -pix_fmt yuv420p10le
    "$VMAF" --reference ${c}_${r}.y4m --distorted $o.y4m --threads "$THREADS" "${VCOLOR[@]}" \
      --model path="$(model $c $r)" --json --output $o.json.tmp > $o.vmaf.log 2>&1 || failed "score $o" $o.vmaf.log
    scored $o.json.tmp "$(frames $c)" || failed "score $o (bad output)" $o.vmaf.log
    mv $o.json.tmp $o.json
    rm -f $o.y4m
    TMP=
  fi
  python3 - "$o" "$(frames $c)" "$(fps $c)" <<'EOF'
import json, os, sys
from fractions import Fraction
o, want, fps = sys.argv[1], int(sys.argv[2]), Fraction(sys.argv[3])
j = json.load(open(o + ".json"))
m = j["pooled_metrics"]
cambi = next(v for k, v in m.items() if k.startswith("cambi"))
c, r, q = os.path.basename(o).split("_")
secs = want / fps
print(f"{c:7s} {r:>5s}p  {q:>6s}  {os.path.getsize(o + '.264') * 8 / float(secs) / 1e6:7.2f} Mbit/s"
      f"  HDR-VMAF {m['vmaf']['mean']:6.2f}  CAMBI mean {cambi['mean']:5.2f} max {cambi['max']:5.2f}")
EOF
}

# ---- meta: the signaling check ----

# what ffprobe and MediaInfo read from two streams of one case, side by side
compare() {
  python3 - "$@" <<'EOF'
import json, re, subprocess, sys
case, a, b = sys.argv[1:4]
def run(cmd):
    return json.loads(subprocess.run(cmd, capture_output=True, text=True, check=True).stdout)
def share(n, of):
    return "all" if n == of and of else "none" if n == 0 else f"{n} of {of}"
# What precedes each IDR picture in the elementary stream itself. A container
# can carry the same metadata in its own boxes, and ffprobe then shows it on
# every frame whatever the stream holds, so this reads the bytes.
def inband(f):
    es = subprocess.run(["ffmpeg", "-v", "error", "-i", f, "-map", "0:v:0", "-c", "copy",
                         "-bsf:v", "h264_mp4toannexb", "-f", "h264", "-"],
                        capture_output=True, check=True).stdout
    idrs, pend = [], set()
    for n in re.split(b"\x00\x00\x01", es)[1:]:
        if not n:
            continue
        t = n[0] & 0x1f
        if t == 7:
            pend.add("sps")
        elif t == 6:
            b, i = re.sub(b"\x00\x00\x03", b"\x00\x00", n[1:]), 0
            while i < len(b) and b[i] != 0x80:
                pt = sz = 0
                while b[i] == 0xff: pt += 255; i += 1
                pt += b[i]; i += 1
                while b[i] == 0xff: sz += 255; i += 1
                sz += b[i]; i += 1 + sz
                pend.add(pt)
        elif t in (1, 5):
            if t == 5 and len(n) > 1 and n[1] & 0x80:   # first_mb_in_slice 0
                idrs.append(pend)
            pend = set()
    return idrs
def probe(f):
    j = run(["ffprobe", "-v", "error", "-of", "json", "-select_streams", "v:0", "-show_streams", f])
    j = j["streams"][0]
    # side data on every keyframe: a player that starts or seeks at a later
    # IDR reads it there, not from the first one
    k = run(["ffprobe", "-v", "error", "-of", "json", "-select_streams", "v:0", "-skip_frame", "nokey",
             "-show_frames", "-show_entries", "frame=key_frame,side_data_list", f])["frames"]
    r = {f"stream {x}": str(j.get(x, "-")) for x in
         ("profile", "pix_fmt", "color_range", "color_space", "color_transfer", "color_primaries",
          "chroma_location")}
    r["~keyframes"] = str(len(k))
    idrs = inband(f)
    for name, pt in (("mastering display (137)", 137), ("light level (144)", 144),
                     ("alternative transfer (147)", 147)):
        r[f"in-band {name} SEI at IDRs"] = share(sum(pt in x for x in idrs), len(idrs))
    r["~in-band SPS at IDRs"] = share(sum("sps" in x for x in idrs), len(idrs))
    r["stream side data"] = ",".join(sorted(d["side_data_type"] for d in j.get("side_data_list", []))) or "-"
    r["~level"] = str(j.get("level", "-"))
    want = {"Mastering display metadata": {
                "  primaries R G B, white": ("red_x", "red_y", "green_x", "green_y", "blue_x", "blue_y",
                                             "white_point_x", "white_point_y"),
                "  luminance min max": ("min_luminance", "max_luminance")},
            "Content light level metadata": {"  MaxCLL MaxFALL": ("max_content", "max_average")}}
    for t, rows in want.items():
        got = [next((d for d in fr.get("side_data_list", []) if d["side_data_type"] == t), None)
               for fr in k]
        r[f"keyframes with {t}"] = share(sum(g is not None for g in got), len(k))
        first = next((g for g in got if g), None)
        for name, keys in rows.items():
            # 34000/50000 -> 34000: the numerators are the SEI's own fields
            r[name] = " ".join(str(first.get(x, "-")).split("/")[0] for x in keys) if first else "-"
    try:
        m = run(["mediainfo", "--Output=JSON", f])
    except (OSError, subprocess.CalledProcessError):
        return r
    v = next((t for t in m["media"]["track"] if t["@type"] == "Video"), {})
    for x in ("HDR_Format", "HDR_Format_Compatibility", "Format_Profile", "BitDepth", "colour_range",
              "colour_primaries", "transfer_characteristics", "transfer_characteristics_Original",
              "matrix_coefficients", "MasteringDisplay_ColorPrimaries", "MasteringDisplay_Luminance",
              "MaxCLL", "MaxFALL"):
        r[f"mediainfo {x}"] = v.get(x, "-")
    r["~mediainfo Format_Level"] = v.get("Format_Level", "-")
    r["~mediainfo Format_Settings_RefFrames"] = v.get("Format_Settings_RefFrames", "-")
    return r
ra, rb = probe(a), probe(b)
bad = 0
print(f"== {case}")
print(f"   {'field':46s} {'x264':34s} yah264")
for x in ra:
    same = ra[x] == rb.get(x)
    info = x.startswith("~")
    bad += not same and not info
    mark = "  " if same else (" ~" if info else "!!")
    print(f"{mark} {x.lstrip('~'):46s} {ra[x]:34s} {rb.get(x, '-')}")
sys.exit(1 if bad else 0)
EOF
}

meta() {
  local F=${FRAMES:-48} K=${KEYINT:-24} X FF=${FF:-} FFX=${FFX:-ffmpeg} bad=0 c
  X=$(abs "${X264:-x264}")
  [ -x "$X" ] || die "no x264 at $X (set X264)"
  if [ -n "$FF" ]; then
    FF=$(abs "$FF")
    "$FF" -hide_banner -encoders 2> /dev/null | grep ' libyah264 ' > /dev/null || die "$FF has no libyah264"
    "$FFX" -hide_banner -encoders 2> /dev/null | grep ' libx264 ' > /dev/null || die "$FFX has no libx264 (set FFX)"
  fi
  # the spans come from the prepared 1080p sources; the full prep runs only
  # when one of them is missing
  if [ ! -s sol_1080.y4m ] || [ ! -s sparks_1080.y4m ] || [ ! -s sol.cll ] || [ ! -s sparks.cll ]; then
    python3 -c 'import numpy' 2> /dev/null || die "needs python3 with numpy"
    fetch
    prep
  fi
  mkdir -p meta
  for c in sol sparks; do
    ! stale meta/$c.y4m ${c}_1080.y4m || y4m meta/$c.y4m "" -i ${c}_1080.y4m -frames:v "$F"
  done
  # HLG from the PQ grade at its 1000-nit peak
  ! stale meta/sol_hlg.y4m meta/sol.y4m || y4m meta/sol_hlg.y4m "" -i meta/sol.y4m -vf \
    "zscale=tin=smpte2084:min=2020_ncl:pin=2020:rin=tv:t=arib-std-b67:m=2020_ncl:p=2020:r=tv:npl=1000:f=spline36,format=yuv420p10le"

  local PQ=(--colorprim bt2020 --transfer smpte2084 --colormatrix bt2020nc --range tv)
  local HLG=(--colorprim bt2020 --transfer arib-std-b67 --colormatrix bt2020nc --range tv)
  local HLGC=(--colorprim bt2020 --transfer bt2020-10 --colormatrix bt2020nc --range tv
              --alternative-transfer arib-std-b67)
  local src opts
  for c in hdr10_sparks hdr10_sol hlg_sol hlgcompat_sol; do
    case $c in
      hdr10_*) src=meta/${c#hdr10_}.y4m
               opts=("${PQ[@]}" --mastering-display "$MD" --cll "$(cat ${c#hdr10_}.cll)") ;;
      hlg_sol) src=meta/sol_hlg.y4m; opts=("${HLG[@]}") ;;
      hlgcompat_sol) src=meta/sol_hlg.y4m; opts=("${HLGC[@]}") ;;
    esac
    "$X" --profile high10 --output-depth 10 --keyint "$K" --threads "$THREADS" --frames "$F" \
      "${opts[@]}" -o meta/$c.x264.264 "$src" 2> meta/$c.x264.log || failed "x264 $c" meta/$c.x264.log
    "$Y" --input-y4m "$src" --keyint "$K" --threads "$THREADS" --frames "$F" --no-progress \
      "${opts[@]}" -o meta/$c.yah264.264 2> meta/$c.yah264.log || failed "yah264 $c" meta/$c.yah264.log
    compare "cli $c" meta/$c.x264.264 meta/$c.yah264.264 || bad=1
  done
  [ -n "$FF" ] || { [ $bad = 0 ]; return; }

  # through ffmpeg: the color fields tagged on the frames, the HDR10
  # metadata as each wrapper's private options. The tags go on with
  # setparams because ffmpeg takes an encoder's primaries and transfer from
  # its input frames, so -color_primaries and -color_trc alone don't reach
  # it. ffmpeg has no option for the alternative transfer, so the
  # SDR-compatible HLG case stays CLI-only.
  local FFPQ=(-vf setparams=color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc:range=tv)
  local FFHLG=(-vf setparams=color_primaries=bt2020:color_trc=arib-std-b67:colorspace=bt2020nc:range=tv)
  local cll ext
  cll=$(cat sparks.cll)
  for ext in mp4 ts; do
    "$FFX" -v error -nostdin -i meta/sparks.y4m -c:v libx264 -preset medium -g "$K" -threads "$THREADS" \
      "${FFPQ[@]}" -x264-params "mastering-display=$MD:cll=$cll" -y meta/ff_hdr10.x264.$ext ||
      die "libx264 hdr10 .$ext failed"
    "$FF" -v error -nostdin -i meta/sparks.y4m -c:v libyah264 -g "$K" -threads "$THREADS" \
      "${FFPQ[@]}" -mastering-display "$MD" -cll "$cll" -y meta/ff_hdr10.yah264.$ext ||
      die "libyah264 hdr10 .$ext failed"
    compare "ffmpeg hdr10_sparks .$ext" meta/ff_hdr10.x264.$ext meta/ff_hdr10.yah264.$ext || bad=1
    "$FFX" -v error -nostdin -i meta/sol_hlg.y4m -c:v libx264 -preset medium -g "$K" -threads "$THREADS" \
      "${FFHLG[@]}" -y meta/ff_hlg.x264.$ext || die "libx264 hlg .$ext failed"
    "$FF" -v error -nostdin -i meta/sol_hlg.y4m -c:v libyah264 -g "$K" -threads "$THREADS" \
      "${FFHLG[@]}" -y meta/ff_hlg.yah264.$ext || die "libyah264 hlg .$ext failed"
    compare "ffmpeg hlg_sol .$ext" meta/ff_hlg.x264.$ext meta/ff_hlg.yah264.$ext || bad=1
    # no options: the color and the HDR10 metadata come from the input, the
    # x264 CLI's HDR10 stream above, through ffmpeg's decoder
    "$FFX" -v error -nostdin -i meta/hdr10_sparks.x264.264 -c:v libx264 -preset medium -g "$K" \
      -threads "$THREADS" -y meta/ff_side.x264.$ext || die "libx264 side-data .$ext failed"
    "$FF" -v error -nostdin -i meta/hdr10_sparks.x264.264 -c:v libyah264 -g "$K" \
      -threads "$THREADS" -y meta/ff_side.yah264.$ext || die "libyah264 side-data .$ext failed"
    compare "ffmpeg side data .$ext" meta/ff_side.x264.$ext meta/ff_side.yah264.$ext || bad=1
  done
  [ $bad = 0 ]
}

if [ $MODE = meta ]; then
  meta || { echo "hdr_check: the signaling differs (!! above)" >&2; exit 1; }
  exit 0
fi

fetch
prep
for c in sol sparks; do for r in 1080 2160; do for q in $CRFS; do one $c $r $q; done; done; done
