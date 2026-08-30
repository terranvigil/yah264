#!/usr/bin/env bash
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: BSD-2-Clause
#
# recon_thread_gate.sh -- recon-match on the THREADED path, which
# scripts/conformance.sh structurally cannot reach.
#
# conformance.sh asks the right question (does the bitstream decode to the
# encoder's own reconstruction?) and answers it only for serial encoding, since
# --dump-recon forces the serial loop. A defect that needs threads on passes all
# 476 of its cases. One did, for six days: RCP_LAG=1 on threaded ABR emitted B
# frames that decode to something the encoder never built.
#
# Every differing frame is a conformance break, not a quality difference.
#
#   scripts/recon_thread_gate.sh                      # default matrix
#   CLIPS='bus_cif' THREADS='1 12' scripts/recon_thread_gate.sh
#   Y264_RCP_LAG=1 scripts/recon_thread_gate.sh       # arm a suspect
set -uo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${RECONCMP:-$root/build/tools/reconcmp/reconcmp}"
FFMPEG="${FFMPEG:-ffmpeg}"
CLIPS="${CLIPS:-bus_cif foreman_cif}"
THREADS="${THREADS:-1 12}"
FRAMES="${FRAMES:-60}"
BITRATE="${BITRATE:-400}"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
[ -x "$BIN" ] || { echo "recon_thread_gate: build tools/reconcmp first" >&2; exit 2; }

fail=0
for clip in $CLIPS; do
  src="$root/tests/corpus/$clip.y4m"
  [ -f "$src" ] || { printf '  %-16s (missing)\n' "$clip"; continue; }
  for th in $THREADS; do
    if ! "$BIN" "$src" "$WORK/o.264" "$WORK/rec.yuv" "$FRAMES" "$th" "$BITRATE" 2>"$WORK/err"; then
      printf '  %-16s t%-3s ENCODER FAILED: %s\n' "$clip" "$th" "$(head -1 "$WORK/err")"
      fail=1; continue
    fi
    if ! "$FFMPEG" -v error -y -f h264 -i "$WORK/o.264" -pix_fmt yuv420p \
         -f rawvideo "$WORK/dec.yuv" 2>"$WORK/err"; then
      printf '  %-16s t%-3s DECODE FAILED: %s\n' "$clip" "$th" "$(head -1 "$WORK/err")"
      fail=1; continue
    fi
    # Guard the PRODUCER. Comparing two files cannot tell a real result from two
    # missing ones, and an earlier version of this script printed "recon-match
    # ok" for a run whose recon file did not exist.
    for f in "$WORK/rec.yuv" "$WORK/dec.yuv"; do
      if [ ! -s "$f" ]; then
        printf '  %-16s t%-3s EMPTY OUTPUT: %s\n' "$clip" "$th" "$(basename "$f")"
        fail=1; continue 2
      fi
    done
    res=$(python3 - "$WORK/rec.yuv" "$WORK/dec.yuv" "$src" <<'PY'
import sys, re
rec, dec, src = sys.argv[1], sys.argv[2], sys.argv[3]
hdr = open(src,'rb').readline().decode('latin1')
W = int(re.search(r' W(\d+)', hdr).group(1)); H = int(re.search(r' H(\d+)', hdr).group(1))
fsz = W*H*3//2
r = open(rec,'rb').read(); d = open(dec,'rb').read()
n = min(len(r)//fsz, len(d)//fsz)
bad = [i for i in range(n) if r[i*fsz:(i+1)*fsz] != d[i*fsz:(i+1)*fsz]]
print(n, len(bad), bad[0] if bad else -1)
PY
)
    n=$(echo "$res" | awk '{print $1}')
    bad=$(echo "$res" | awk '{print $2}')
    first=$(echo "$res" | awk '{print $3}')
    if [ "${n:-0}" -lt "$FRAMES" ]; then
      printf '  %-16s t%-3s SHORT: %s of %s frames comparable\n' "$clip" "$th" "${n:-0}" "$FRAMES"
      fail=1; continue
    fi
    if [ "${bad:-1}" -gt 0 ]; then
      printf '  %-16s t%-3s %3s frames  MISMATCH %s (first at %s)\n' "$clip" "$th" "$n" "$bad" "$first"
      fail=1
    else
      printf '  %-16s t%-3s %3s frames  recon-match ok\n' "$clip" "$th" "$n"
    fi
  done
done
if [ $fail -ne 0 ]; then
  echo "RECON-THREAD-GATE: FAILED (see rows above: a MISMATCH means the encoder"
  echo "  emitted frames it cannot itself reproduce; anything else means the check"
  echo "  could not run, which is not a pass)"
  exit 1
fi
echo "RECON-THREAD-GATE: all pass"
