#!/bin/bash
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# san_matrix.sh - the encoder under AddressSanitizer + UndefinedBehaviorSanitizer
# over the inputs a recon-match gate never sees: odd sizes (33x17, 16x16, 8x8),
# one- and two-frame clips with B-frames on, keyint 1, qp 0, tiny and huge
# bitrates, 4:2:2, 4:4:4, 10-bit (native and upshifted from 8), CBR, 2-pass,
# direct temporal, the hardware backend, at 1 to 12 threads. Found two memory bugs on 2026-09-04 (a one-row frame's
# half-pel band, a 4:2:2 B snapshot) that 318 conformance cells had not.
#
#   scripts/san_matrix.sh            # builds build-san/ (once), runs 26 cases
#   FF=/path/to/ffmpeg scripts/san_matrix.sh
#
# Exit status is the number of cases with a sanitiser report. ~3 min.
cd "$(dirname "$0")/.." || exit 1
S=${SAN_WORK:-build-san/work}; mkdir -p "$S/a6wd"
B=build-san
[ -f $B/build.ninja ] || meson setup $B -Db_sanitize=address,undefined -Dbuildtype=debugoptimized -Db_lundef=false > $S/san-setup.log 2>&1 || { echo SETUP-FAIL; exit 1; }
ninja -C $B > $S/san-build.log 2>&1 || { echo BUILD-FAIL; tail -5 $S/san-build.log; exit 1; }
Y=$B/cli/yah264; C=tests/corpus; FF=${FF:-$(command -v ffmpeg)}
BAD=0
export ASAN_OPTIONS=detect_leaks=0:abort_on_error=0 UBSAN_OPTIONS=print_stacktrace=1
run() { # label, args...
  local label=$1; shift
  out=$( "$@" 2>&1 >/dev/null ); rc=$?
  errs=$(echo "$out" | grep -cE "ERROR: AddressSanitizer|runtime error:|SUMMARY:")
  echo "$label rc=$rc san=$errs"; [ "$errs" -gt 0 ] && BAD=$((BAD+1))
  [ $errs -gt 0 ] && echo "$out" | grep -E "runtime error|ERROR: AddressSanitizer|#[0-3] " | head -12 | sed 's/^/    /'
}
# synthetic odd inputs
mk() { $FF -v error -y -f lavfi -i "testsrc2=size=$1:rate=25" -frames:v $2 -pix_fmt $3 $4; }
mk 33x17 12 yuv420p $S/a6wd/odd33.y4m; mk 16x16 3 yuv420p $S/a6wd/t16.y4m; mk 8x8 2 yuv420p $S/a6wd/t8.y4m; mk 352x288 1 yuv420p $S/a6wd/one.y4m; mk 352x288 2 yuv420p $S/a6wd/two.y4m; mk 200x120 20 yuv422p $S/a6wd/o422.y4m; mk 200x120 20 yuv444p $S/a6wd/o444.y4m; $FF -v error -y -f lavfi -i "testsrc2=size=96x64:rate=25" -frames:v 20 -pix_fmt yuv420p10le -strict -1 $S/a6wd/o10.y4m
run "odd33 t1"        $Y --input-y4m $S/a6wd/odd33.y4m --crf 26 --threads 1 -o /dev/null
run "odd33 t8"        $Y --input-y4m $S/a6wd/odd33.y4m --crf 26 --threads 8 -o /dev/null
run "16x16 t4"        $Y --input-y4m $S/a6wd/t16.y4m --crf 26 --threads 4 -o /dev/null
run "8x8 t1"          $Y --input-y4m $S/a6wd/t8.y4m --crf 26 --threads 1 -o /dev/null
run "one frame b3"    $Y --input-y4m $S/a6wd/one.y4m --crf 26 --threads 4 -o /dev/null
run "two frames b3"   $Y --input-y4m $S/a6wd/two.y4m --crf 26 --threads 4 -o /dev/null
run "two frames abr"  $Y --input-y4m $S/a6wd/two.y4m --bitrate 300 --threads 4 -o /dev/null
run "keyint1 t4"      $Y --input-y4m $C/foreman_cif.y4m --frames 20 --crf 26 --keyint 1 --threads 4 -o /dev/null
run "qp0 t1"          $Y --input-y4m $C/foreman_cif.y4m --frames 6 --qp 0 --threads 1 -o /dev/null
run "qp51 abr tiny"   $Y --input-y4m $C/foreman_cif.y4m --frames 30 --bitrate 5 --threads 4 -o /dev/null
run "abr huge"        $Y --input-y4m $C/foreman_cif.y4m --frames 30 --bitrate 200000 --threads 4 -o /dev/null
run "422 t4"          $Y --input-y4m $S/a6wd/o422.y4m --crf 26 --threads 4 -o /dev/null
run "444 t4"          $Y --input-y4m $S/a6wd/o444.y4m --crf 26 --threads 4 -o /dev/null
run "10bit t4"        $Y --input-y4m $S/a6wd/o10.y4m --crf 26 --threads 4 -o /dev/null
run "10bit t1"        $Y --input-y4m $S/a6wd/o10.y4m --crf 26 --threads 1 -o /dev/null
run "10bit upshift"   $Y --input-y4m $S/a6wd/odd33.y4m --crf 26 --output-depth 10 --threads 4 -o /dev/null
run "cbr vbv t8"      $Y --input-y4m $C/foreman_cif.y4m --frames 60 --bitrate 300 --vbv-maxrate 300 --vbv-bufsize 300 --threads 8 -o /dev/null
run "abr t12 board"   $Y --input-y4m $C/bus_cif.y4m --frames 60 --bitrate 400 --threads 12 -o /dev/null
run "crf t12 cavlc"   $Y --input-y4m $C/bus_cif.y4m --frames 40 --crf 30 --cavlc --threads 12 -o /dev/null
run "2pass p1"        $Y --input-y4m $C/foreman_cif.y4m --frames 40 --bitrate 400 --pass 1 --stats $S/a6wd/san2p.log --threads 4 -o /dev/null
run "2pass p2"        $Y --input-y4m $C/foreman_cif.y4m --frames 40 --bitrate 400 --pass 2 --stats $S/a6wd/san2p.log --threads 4 -o /dev/null
run "direct temporal" $Y --input-y4m $C/foreman_cif.y4m --frames 40 --crf 26 --direct temporal --threads 8 -o /dev/null
run "hw auto"         $Y --input-y4m $C/foreman_cif.y4m --frames 40 --bitrate 400 --hw auto --threads 4 -o /dev/null
# Constrained intra narrows what intra prediction may read, so it reaches
# neighbour combinations a default encode never produces (top and left
# available, above-left not); an odd geometry puts them against a frame edge
# as well.
run "cintra odd33"    $Y --input-y4m $S/a6wd/odd33.y4m --crf 26 --constrained-intra --threads 4 -o /dev/null
run "cintra 444 b2"   $Y --input-y4m $S/a6wd/o444.y4m --crf 26 --constrained-intra --bframes 2 --threads 4 -o /dev/null
# --slices indexes the decision grids from a row that is not 0 and writes one
# NAL per slice into one buffer, so the cases worth an allocator are the ones
# where the arithmetic can walk off: an odd geometry whose slices split
# unevenly, one slice per macroblock row (the count clamp), and a count above
# the row count (the clamp from the other side).
run "slices4 t4"      $Y --input-y4m $C/foreman_cif.y4m --frames 40 --crf 26 --slices 4 --bframes 3 --threads 4 -o /dev/null
run "slices3 odd33"   $Y --input-y4m $S/a6wd/odd33.y4m --crf 26 --slices 3 --threads 4 -o /dev/null
run "slices row t8"   $Y --input-y4m $C/foreman_cif.y4m --frames 20 --crf 26 --slices 18 --threads 8 -o /dev/null
# --open-gop keeps pictures from before the key in the buffer while forbidding
# them to the pictures after it, so the list builders run with a candidate set
# they trim: a short keyint makes that happen every few frames, an odd geometry
# puts it against the border logic, and one thread against many says whether
# the trim is a function of the schedule.
run "opengop k4 t8"   $Y --input-y4m $C/foreman_cif.y4m --frames 40 --crf 26 --open-gop --keyint 4 --bframes 3 --ref 4 --threads 8 -o /dev/null
run "opengop k2 t1"   $Y --input-y4m $C/foreman_cif.y4m --frames 24 --crf 26 --open-gop --keyint 2 --threads 1 -o /dev/null
run "opengop odd33"   $Y --input-y4m $S/a6wd/odd33.y4m --crf 26 --open-gop --keyint 3 --threads 4 -o /dev/null
run "slices clamp"    $Y --input-y4m $S/a6wd/two.y4m --crf 26 --slices 200 --threads 4 -o /dev/null
run "slices cavlc"    $Y --input-y4m $C/bus_cif.y4m --frames 40 --crf 30 --cavlc --slices 4 --threads 12 -o /dev/null
# PAFF (item C2-PAFF-1). The field views are stride-doubled halves of the frame
# planes with their own border reach, so the odd geometry and the border cells
# are the ones worth having under the sanitisers; 33x17 pads to two macroblock
# rows, which is the smallest field pair the encoder can build.
$FF -v error -y -f lavfi -i "testsrc2=size=176x144:rate=50" -vf "tinterlace=mode=interleave_top,setfield=tff" -frames:v 12 -pix_fmt yuv420p $S/a6wd/tff.y4m
run "tff t1"          $Y --input-y4m $S/a6wd/tff.y4m --crf 26 --tff --cabac --transform-8x8 --threads 1 -o /dev/null
run "tff t8"          $Y --input-y4m $S/a6wd/tff.y4m --crf 26 --tff --cabac --transform-8x8 --threads 8 -o /dev/null
run "bff cavlc t4"    $Y --input-y4m $S/a6wd/tff.y4m --crf 26 --bff --cavlc --threads 4 -o /dev/null
run "tff 16x16 t4"    $Y --input-y4m $S/a6wd/t16.y4m --crf 26 --tff --cabac --ref 3 --threads 4 -o /dev/null
run "tff qp0 t1"      $Y --input-y4m $S/a6wd/tff.y4m --qp 0 --tff --cabac --ref 5 --threads 1 -o /dev/null
run "tff cbr t8"      $Y --input-y4m $S/a6wd/tff.y4m --bitrate 300 --vbv-maxrate 300 --vbv-bufsize 300 --tff --threads 8 -o /dev/null
# B fields and field slices (C2-PAFF-2). The slices cell is here because that is
# what found the bug: slice cuts sized by the FRAME let the emit walk rows the
# per-picture record array never had, and only the sanitiser could say so. The
# keyint-1 cell opens one encoder per GOP, which is the shape that put a dozen
# warms inside one table builder at once.
run "tff b3 t8"       $Y --input-y4m $S/a6wd/tff.y4m --crf 26 --tff --cabac --bframes 3 --threads 8 -o /dev/null
run "tff b3 pyr t4"   $Y --input-y4m $S/a6wd/tff.y4m --crf 26 --tff --cabac --bframes 3 --b-pyramid normal --ref 3 --threads 4 -o /dev/null
run "tff b3 tdir t4"  $Y --input-y4m $S/a6wd/tff.y4m --qp 26 --tff --cabac --bframes 3 --direct temporal --ref 3 --threads 4 -o /dev/null
run "tff slices4 t4"  $Y --input-y4m $S/a6wd/tff.y4m --crf 26 --tff --cabac --slices 4 --bframes 3 --threads 4 -o /dev/null
run "bff slices cavlc" $Y --input-y4m $S/a6wd/tff.y4m --qp 30 --bff --cavlc --slices 4 --bframes 2 --threads 4 -o /dev/null
run "tff keyint1 t8"  $Y --input-y4m $S/a6wd/tff.y4m --qp 26 --tff --cabac --keyint 1 --ref 1 --threads 8 -o /dev/null

# --nal-hrd (B-hrd). The filler path is the one worth a sanitiser: it writes a
# payload whose length is computed per access unit into a buffer sized once at
# open, so an arithmetic slip there is an overflow rather than a wrong number.
# The tff cell exercises a pic_timing per FIELD, and the slices cell the
# access-unit opener that now fires on the first slice only.
run "hrd vbr t8"      $Y --input-y4m $C/bus_cif.y4m --frames 40 --crf 26 --cabac --vbv-maxrate 400 --vbv-bufsize 400 --nal-hrd vbr --threads 8 -o /dev/null
run "hrd cbr t8"      $Y --input-y4m $C/bus_cif.y4m --frames 40 --crf 30 --cabac --vbv-maxrate 400 --vbv-bufsize 400 --nal-hrd cbr --threads 8 -o /dev/null
run "hrd cbr slices"  $Y --input-y4m $C/bus_cif.y4m --frames 40 --crf 30 --cabac --vbv-maxrate 400 --vbv-bufsize 400 --nal-hrd cbr --slices 4 --threads 4 -o /dev/null
run "hrd tff vbr"     $Y --input-y4m $S/a6wd/tff.y4m --crf 26 --tff --cabac --vbv-maxrate 300 --vbv-bufsize 300 --nal-hrd vbr --threads 4 -o /dev/null
echo "SAN-DONE: $BAD case(s) with reports"; exit $BAD
