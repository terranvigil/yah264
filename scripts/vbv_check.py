#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: BSD-2-Clause
"""vbv_check.py - stream-side VBV/CPB compliance checker.

Parses an Annex-B H.264 elementary stream into access units (one slice NAL
per frame in yah264; leading SPS/PPS/SEI bytes attach to the following AU),
then simulates the decoder buffer with the encoder's own leaky-bucket law:

    fill = bufsize                        # start full (encoder assumption)
    per frame: fill = min(fill + maxrate/fps, bufsize); fill -= au_bits
    underflow when fill < 0 at any frame

Reports every underflow (frame index, magnitude) plus min-fill and the
overflow-clamp count, and exits 1 on any underflow so gates can assert.

Usage: vbv_check.py <stream.264> --maxrate KBPS --bufsize KBIT
       (--y4m SRC.y4m | --fps N[:D]) [--quiet]

FRAME RATE IS NOT OPTIONAL AND NOT A CONSTANT. Both numbers this tool reports
are fps-scaled: the bucket refills by maxrate/fps per frame, and the average
bitrate is bits*fps/frames. The corpus mixes 24, 25, 29.97 (30000:1001), 30 and
50 fps, so a single hardcoded rate is wrong for most of it -- and understating
it is wrong in the direction that PASSES a stream it should fail. Check a 50fps
clip at 29.97: the per-frame refill maxrate/fps comes out 1.67x too LARGE, so
the bucket is 1.67x more forgiving than the real one, and the reported average
bits*fps/frames reads 1.67x too LOW. A stream comfortably over its cap prints as
comfortably under it. That is not hypothetical -- it is how the capped-VBR
compliance gate first reported 12/36 clean cells when the true figure was 5/36
(the local measurement records).

Prefer `--y4m SRC.y4m`, which reads the rate out of the source clip's own Y4M
header, over `--fps`, which is a number a caller can get wrong. `--fps` is kept
for streams whose source y4m is not to hand, and one of the two is required --
there is deliberately no default, because a default framerate is the bug.
"""
import argparse
import sys


def parse_aus(data):
    """Split an Annex-B stream into per-frame bit counts. Returns a list of
    (au_bytes, first_slice_nal_type). Non-slice NALs (SPS/PPS/SEI/AUD) and
    their start codes count toward the NEXT slice's access unit -- that is
    what the decoder's buffer receives before the frame is removed."""
    aus = []
    n = len(data)
    i = 0
    starts = []  # (offset_of_startcode, nal_type)
    while i + 3 < n:
        if data[i] == 0 and data[i + 1] == 0:
            if data[i + 2] == 1:
                starts.append((i, data[i + 3] & 0x1F))
                i += 3
                continue
            if i + 4 < n and data[i + 2] == 0 and data[i + 3] == 1:
                starts.append((i, data[i + 4] & 0x1F))
                i += 4
                continue
        i += 1
    if not starts:
        return []
    # AU boundaries: each slice NAL (type 1 or 5) ends an AU at the NEXT
    # start code (or EOF). Everything since the previous AU's end belongs
    # to this AU.
    au_start = starts[0][0]
    for k, (off, typ) in enumerate(starts):
        if typ in (1, 5):
            end = starts[k + 1][0] if k + 1 < len(starts) else n
            aus.append((end - au_start, typ))
            au_start = end
    return aus


def fps_from_y4m(path):
    """Frame rate out of a Y4M header's F<num>:<den> token.

    The header is the first line of the file, space separated, e.g.
    `YUV4MPEG2 W1280 H720 F50:1 Ip A1:1 C420mpeg2`. Reading it costs one line
    of I/O and cannot disagree with the clip the way a caller-supplied number
    can. Raises rather than falling back to any default: a wrong frame rate is
    silent, a missing one should not be.
    """
    with open(path, "rb") as f:
        header = f.readline().decode("ascii", "replace").split()
    if not header or header[0] != "YUV4MPEG2":
        raise ValueError(f"{path}: not a Y4M file (no YUV4MPEG2 signature)")
    for tok in header[1:]:
        if tok.startswith("F"):
            rate = tok[1:]
            num, _, den = rate.partition(":")
            den = den or "1"
            if float(den) == 0.0:
                raise ValueError(f"{path}: Y4M header has F{rate} (zero denominator)")
            return float(num) / float(den)
    raise ValueError(f"{path}: Y4M header carries no F<num>:<den> frame-rate token")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("stream")
    ap.add_argument("--maxrate", type=float, required=True, help="kbit/s")
    ap.add_argument("--bufsize", type=float, required=True, help="kbit")
    # Exactly one of these, and no default. See the module docstring: a default
    # frame rate is precisely the defect this gate exists to catch.
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--y4m", help="source clip; read the rate from its Y4M header (preferred)")
    src.add_argument("--fps", help="N or N:D, when the source y4m is not to hand")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    if args.y4m:
        try:
            fps = fps_from_y4m(args.y4m)
        except (OSError, ValueError) as e:
            print(f"vbv_check: {e}", file=sys.stderr)
            return 2
        fps_src = f"{args.y4m} Y4M header"
    else:
        if ":" in args.fps:
            num, den = args.fps.split(":")
            fps = float(num) / float(den)
        else:
            fps = float(args.fps)
        fps_src = "--fps"
    if fps <= 0:
        print(f"vbv_check: frame rate resolved to {fps} from {fps_src}", file=sys.stderr)
        return 2

    with open(args.stream, "rb") as f:
        data = f.read()
    aus = parse_aus(data)
    if not aus:
        print(f"{args.stream}: no access units found", file=sys.stderr)
        return 2

    size = args.bufsize * 1000.0
    rate = args.maxrate * 1000.0 / fps
    fill = size
    min_fill = size
    underflows = []
    clamps = 0
    total_bits = 0
    for idx, (au_bytes, typ) in enumerate(aus):
        bits = au_bytes * 8
        total_bits += bits
        fill += rate
        if fill > size:
            fill = size
            clamps += 1
        fill -= bits
        if fill < min_fill:
            min_fill = fill
        if fill < 0:
            underflows.append((idx, typ, bits, fill))
            fill = 0  # decoder stalls; keep checking the tail

    kbps = total_bits * fps / len(aus) / 1000.0
    status = "UNDERFLOW" if underflows else "OK"
    # The frame rate is printed on the verdict line, not just accepted, because
    # both numbers on this line scale with it. A transcript that does not say
    # which rate it used cannot be audited after the fact -- which is how the
    # 12/36 reading survived long enough to be quoted.
    print(f"{status} {args.stream}: {len(aus)} frames @ {fps:.4g} fps "
          f"[{fps_src}], {kbps:.1f} kbps "
          f"(maxrate {args.maxrate:.0f}, bufsize {args.bufsize:.0f} kbit), "
          f"min-fill {min_fill / 1000.0:.1f} kbit "
          f"({100.0 * min_fill / size:.1f}%), "
          f"{len(underflows)} underflow(s), {clamps} overflow-clamp(s)")
    if underflows and not args.quiet:
        for idx, typ, bits, fl in underflows[:20]:
            print(f"  frame {idx} (nal {typ}): {bits} bits, "
                  f"fill {fl / 1000.0:.1f} kbit", file=sys.stderr)
    return 1 if underflows else 0


if __name__ == "__main__":
    sys.exit(main())
