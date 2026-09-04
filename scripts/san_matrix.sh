#!/bin/bash
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: BSD-2-Clause
#
# san_matrix.sh - the encoder under AddressSanitizer + UndefinedBehaviorSanitizer
# over the inputs a recon-match gate never sees: odd sizes (33x17, 16x16, 8x8),
# one- and two-frame clips with B-frames on, keyint 1, qp 0, tiny and huge
# bitrates, 4:2:2, 4:4:4, CBR, 2-pass, direct temporal, the hardware backend,
# at 1 to 12 threads. Found two memory bugs on 2026-09-04 (a one-row frame's
# half-pel band, a 4:2:2 B snapshot) that 318 conformance cells had not.
#
#   scripts/san_matrix.sh            # builds build-san/ (once), runs 21 cases
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
run "cbr vbv t8"      $Y --input-y4m $C/foreman_cif.y4m --frames 60 --bitrate 300 --vbv-maxrate 300 --vbv-bufsize 300 --threads 8 -o /dev/null
run "abr t12 board"   $Y --input-y4m $C/bus_cif.y4m --frames 60 --bitrate 400 --threads 12 -o /dev/null
run "crf t12 cavlc"   $Y --input-y4m $C/bus_cif.y4m --frames 40 --crf 30 --cavlc --threads 12 -o /dev/null
run "2pass p1"        $Y --input-y4m $C/foreman_cif.y4m --frames 40 --bitrate 400 --pass 1 --stats $S/a6wd/san2p.log --threads 4 -o /dev/null
run "2pass p2"        $Y --input-y4m $C/foreman_cif.y4m --frames 40 --bitrate 400 --pass 2 --stats $S/a6wd/san2p.log --threads 4 -o /dev/null
run "direct temporal" $Y --input-y4m $C/foreman_cif.y4m --frames 40 --crf 26 --direct temporal --threads 8 -o /dev/null
run "hw auto"         $Y --input-y4m $C/foreman_cif.y4m --frames 40 --bitrate 400 --hw auto --threads 4 -o /dev/null
echo "SAN-DONE: $BAD case(s) with reports"; exit $BAD
