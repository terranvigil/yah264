#!/usr/bin/env bash
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The determinism contract behind per-shot re-encoding (docs/engine-interface.md):
# a GOP re-encoded ALONE, with the same parameters and the same pinned frame
# threads, reproduces its byte range in the full encode. Encodes the input with
# GOP boundaries on its scene cuts and every GOP written to its own segment,
# then cuts each GOP's frames out of the source, encodes them alone and compares.
#
# Informational since 2026-09-25: the owner ruled that repeatable byte-exact
# output is not a requirement, so a GOP that does not reproduce is a WARN and
# the exit status is 0 unless an encode fails (exit 1). STRICT=1 makes a WARN
# exit 1, for when the question is whether the promise still holds.
#
# Usage: scripts/shot_determinism.sh [input.y4m] [extra yah264 args...]
#   default input: local/corpus/ms_cif_30.y4m; K=<frame threads> (default 2),
#   CRF=<crf> (default 26), YAH264=<binary>.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IN="${1:-$ROOT/local/corpus/ms_cif_30.y4m}"; shift || true
BIN="${YAH264:-$ROOT/build/cli/yah264}"; K="${K:-2}"; CRF="${CRF:-26}"
if [ ! -f "$IN" ] && [ "$(basename "$IN")" = ms_cif_30.y4m ]; then
    echo "building $IN from the gate corpus (scripts/make_multishot.py)"
    python3 "$ROOT/scripts/make_multishot.py" --out "$(dirname "$IN")" ms_cif_30 >/dev/null
fi
. "$ROOT/scripts/scratch.sh"
y264_scratch_dir shotdet W
"$BIN" --input-y4m "$IN" --crf "$CRF" --threads 4 --gop-threads "$K" --cut-split \
       --segment-out "$W/seg%d.264" --frame-stats "$W/fs.jsonl" "$@" -o "$W/full.264" 2>"$W/full.log" \
    || { echo "shot_determinism: the full encode failed:"; tail -3 "$W/full.log"; exit 1; }
python3 - "$IN" "$W" <<'PY'
import json, sys, collections
src, W = sys.argv[1], sys.argv[2]
rows = [json.loads(l) for l in open(W + "/fs.jsonl")]
gops = collections.OrderedDict()
for r in rows: gops.setdefault(r["gop"], []).append(r["frame"])
with open(src, "rb") as f:
    hdr = f.readline(); tags = dict((t[:1], t[1:]) for t in hdr.split()[1:])
    w, h = int(tags[b"W"]), int(tags[b"H"]); fsz = w * h * 3 // 2
    frames = []
    while True:
        fh = f.readline()
        if not fh: break
        frames.append(f.read(fsz))
with open(W + "/ranges.txt", "w") as o:
    for g, fr in gops.items():
        a, b = min(fr), max(fr) + 1
        with open(f"{W}/g{g}.y4m", "wb") as gf:
            gf.write(hdr)
            for i in range(a, b): gf.write(b"FRAME\n" + frames[i])
        o.write(f"{g} {a} {b}\n")
PY
fail=0; n=0
while read -r g a b; do
    "$BIN" --input-y4m "$W/g$g.y4m" --crf "$CRF" --threads "$K" --gop-threads "$K" "$@" -o "$W/alone$g.264" 2>/dev/null
    [ -s "$W/alone$g.264" ] || { echo "  FAIL GOP $g frames $a-$((b-1)): the alone encode produced nothing"; exit 1; }
    if cmp -s "$W/seg$g.264" "$W/alone$g.264"; then echo "  ok   GOP $g frames $a-$((b-1)) ($(stat -f %z "$W/seg$g.264") bytes)"; else echo "  WARN GOP $g frames $a-$((b-1)): alone differs from its segment (informational)"; fail=1; fi
    n=$((n+1))
done < "$W/ranges.txt"
if [ "$fail" = 0 ]; then
    echo "shot_determinism: $n/$n GOPs reproduce alone (K=$K, CRF $CRF, $(basename "$IN"))"
else
    echo "shot_determinism: WARN some GOPs do not reproduce alone (informational, owner 2026-09-25)"
    [ "${STRICT:-0}" = 1 ] && exit 1
fi
exit 0
