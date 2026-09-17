#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Does an --open-gop stream really recover at its recovery point?

The claim a recovery_point SEI makes is narrow and testable: start decoding at
that access unit, with nothing before it but the parameter sets, and every
picture from the recovery point onward IN OUTPUT ORDER is the picture a decode
from the stream's own IDR would have produced. The pictures that precede it in
output order -- the leading B frames, which reference an anchor the cut threw
away -- are explicitly not covered, and this checker does not look at them.

So: cut the elementary stream at the chosen recovery point, paste the parameter
sets in front of it, decode both that and the whole stream, and compare the
LAST n - k frames of each, where k is the recovery point's display index. The
tail is what makes the leading-B count irrelevant: however many the decoder
chose to emit from the cut stream, the exact frames are the ones at the end.

    scripts/recovery_check.py --stream out.264 --recon out.rec.y4m --from 60

`--recon` is the encoder's own --dump-recon, so a pass says the cut decode
matched the encoder's reconstruction and not merely some other decode of the
same bytes. Exit 0 clean, 1 mismatch, 2 the stream carries no recovery point
(which for an --open-gop encode is itself a failure, and is why it is not 0).
"""
import argparse
import os
import subprocess
import sys
import tempfile


def nal_units(buf):
    """(start_of_nal_header, start_of_start_code) for every NAL, in order."""
    out = []
    i = 0
    n = len(buf)
    while True:
        j = buf.find(b"\x00\x00\x01", i)
        if j < 0:
            break
        sc = j - 1 if j > 0 and buf[j - 1] == 0 else j
        out.append((j + 3, sc))
        i = j + 3
    return out


def framemd5(path, extra=()):
    cmd = ["ffmpeg", "-v", "error", *extra, "-i", path, "-f", "framemd5", "-"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return [l.split(",")[-1].strip() for l in r.stdout.splitlines()
            if l and not l.startswith("#")]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stream", required=True)
    ap.add_argument("--recon", help="the encoder's --dump-recon")
    ap.add_argument("--from", dest="k", type=int, default=-1,
                    help="display index of the recovery point to cut at")
    ap.add_argument("--nth", type=int, default=1,
                    help="which recovery point to cut at (1 = the first)")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--count", action="store_true",
                    help="print 'idr=<n> rp=<n>' for the stream and stop")
    a = ap.parse_args()

    buf = open(a.stream, "rb").read()
    if a.count:
        idr = rps = 0
        for hdr, _ in nal_units(buf):
            t = buf[hdr] & 0x1F
            idr += t == 5
            rps += t == 6 and buf[hdr + 1] == 6
        print(f"idr={idr} rp={rps}")
        return 0
    sets, rp = [], None
    seen = 0
    for hdr, sc in nal_units(buf):
        t = buf[hdr] & 0x1F
        if t in (7, 8) and rp is None:
            sets.append((hdr, sc))
        # payloadType is the first byte of the SEI RBSP, and 6 is
        # recovery_point. No ff-escaping to unwind: 6 is below 255.
        if t == 6 and buf[hdr + 1] == 6:
            seen += 1
            if seen == a.nth:
                rp = sc
                break
    if rp is None:
        print(f"recovery_check: {a.stream}: no recovery_point SEI "
              f"(wanted #{a.nth}, found {seen})")
        return 2

    if not a.recon or a.k < 0:
        print("recovery_check: --recon and --from are required without --count")
        return 1
    ref = framemd5(a.recon)
    if not ref:
        print(f"recovery_check: cannot read {a.recon}")
        return 1
    tail = len(ref) - a.k
    if tail <= 0:
        print(f"recovery_check: --from {a.k} is past the {len(ref)}-frame recon")
        return 1

    head = b""
    for hdr, sc in sets:
        end = min((s for h, s in nal_units(buf) if s > sc), default=len(buf))
        head += buf[sc:end]
    fd, cut = tempfile.mkstemp(suffix=".264")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(head + buf[rp:])
        # -f h264 rather than letting the demuxer probe: a cut that leaves only
        # a picture or two scores below the raw-H.264 probe threshold and the
        # open fails outright, which would read as "the recovery point decoded
        # nothing" on exactly the short tails this checker is most useful on.
        got = framemd5(cut, ("-f", "h264"))
    finally:
        os.unlink(cut)

    if len(got) < tail:
        print(f"recovery_check: the cut stream decoded {len(got)} frame(s), "
              f"fewer than the {tail} the recovery point covers")
        return 1
    if got[-tail:] != ref[-tail:]:
        bad = sum(1 for x, y in zip(got[-tail:], ref[-tail:]) if x != y)
        print(f"recovery_check: {bad}/{tail} frame(s) after the recovery point "
              f"differ from the encoder's reconstruction")
        return 1
    if not a.quiet:
        print(f"recovery_check: {tail} frame(s) from display index {a.k} "
              f"exact from a cold start at recovery point #{a.nth}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
