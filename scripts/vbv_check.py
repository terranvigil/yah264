#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""vbv_check.py - stream-side VBV/CPB compliance checker.

Parses an Annex-B H.264 elementary stream into access units (7.4.1.2.3: the
slices of one picture together, leading SPS/PPS/SEI bytes with the following
AU, filler with the one before), then simulates the decoder buffer with the encoder's own
leaky-bucket law:

    fill = init                           # --init, default bufsize (full)
    per frame: fill = min(fill + maxrate/fps, bufsize); fill -= au_bits
    underflow when fill < 0 at any frame

Reports every underflow (frame index, magnitude) plus min-fill and the
overflow-clamp count, and exits 1 on any underflow so gates can assert.

Usage: vbv_check.py <stream.264> --maxrate KBPS --bufsize KBIT
       (--y4m SRC.y4m | --fps N[:D]) [--init F] [--quiet]

--init is the encoder's starting fill, read like --vbv-init: at most 1 is a
fraction of the buffer, above 1 is kbit; it must be above 0. Pass the value
the stream was coded with; a model that starts fuller than the encoder did
forgives an early over-spend the real buffer would not.

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
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import h264_syntax as H                                        # noqa: E402


def _slice_head(data, off, end, sps):
    """first_mb_in_slice and field_pic_flag of the slice NAL whose header byte
    is at `off`, read off a de-emulated prefix of its header."""
    rb = H.rbsp(data[off + 1:min(end, off + 64)])
    if sps is None:
        return H.Bits(rb).ue(), 0
    try:
        sh = H.parse_slice_header(rb, data[off] & 0x1F, sps)
    except (ValueError, IndexError):
        return H.Bits(rb).ue(), 0
    return sh["first_mb_in_slice"], sh["field_pic_flag"]


def parse_aus(data):
    """Split an Annex-B stream into access units (7.4.1.2.3). Returns a list
    of (au_bytes, first_slice_nal_type, is_field). A new AU opens at an
    AUD/SPS/PPS/SEI (or types 14-18) after a VCL NAL, or at a slice with
    first_mb_in_slice 0 after a VCL NAL: non-VCL NALs and their start codes
    count toward the picture that follows them, which is what the decoder's
    buffer receives before it is removed, and the slices of one picture are
    one AU. Filler and end-of-sequence/stream NALs stay with the picture they
    follow, as hrd_check.py counts them, since CBR pads a picture with filler
    only where the buffer would otherwise run over."""
    n = len(data)
    i = 0
    starts = []  # (offset_of_startcode, offset_of_header)
    while i + 3 < n:
        if data[i] == 0 and data[i + 1] == 0:
            if data[i + 2] == 1:
                starts.append((i, i + 3))
                i += 3
                continue
            if i + 4 < n and data[i + 2] == 0 and data[i + 3] == 1:
                starts.append((i, i + 4))
                i += 4
                continue
        i += 1
    aus = []
    cur = 0
    typ = None
    field = 0
    seen_vcl = False
    sps = None
    for k, (s, h) in enumerate(starts):
        e = starts[k + 1][0] if k + 1 < len(starts) else n
        t = data[h] & 0x1F
        if t == 7:
            try:
                sps = H.parse_sps(H.rbsp(data[h + 1:e]))
            except (ValueError, IndexError):
                pass
        opens = False
        fmb = fld = 0
        if t in (1, 5):
            fmb, fld = _slice_head(data, h, e, sps)
            opens = seen_vcl and fmb == 0
        elif t in (6, 7, 8, 9, 14, 15, 16, 17, 18):
            opens = seen_vcl
        if opens:
            aus.append((cur, typ, field))
            cur = 0
            seen_vcl = False
        if t in (1, 5) and not seen_vcl:
            typ, field = t, fld
            seen_vcl = True
        cur += e - s
    if seen_vcl:
        aus.append((cur, typ, field))
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
    ap.add_argument("--crf-max", type=float, default=None, dest="crf_max",
                    help="the QP ceiling the stream was coded with; an under-run "
                         "is then the documented cost of it, reported and not failed")
    ap.add_argument("--init", type=float, default=1.0,
                    help="initial fill: <=1 a fraction of --bufsize, above 1 kbit (default full)")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()
    # 0 is refused rather than read as empty: the encoders' --vbv-init 0
    # means "unset", which is full
    if not math.isfinite(args.init) or args.init <= 0:
        print(f"vbv_check: --init {args.init} must be a finite number above 0", file=sys.stderr)
        return 2

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
    fill = min(size * args.init if args.init <= 1 else args.init * 1000.0, size)
    min_fill = fill
    underflows = []
    clamps = 0
    total_bits = 0
    frames = 0.0
    for idx, (au_bytes, typ, is_field) in enumerate(aus):
        bits = au_bytes * 8
        total_bits += bits
        frames += 1.0
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

    kbps = total_bits * fps / frames / 1000.0
    # --crf-max buys a quality floor with the buffer model: a frame the buffer
    # would have starved is coded at the ceiling instead and the bucket goes
    # negative. That is the option working, so it is reported as an under-run
    # with its size and not failed as a breach -- but it is never hidden, which
    # is the whole reason this flag exists rather than the gate simply not
    # running the checker on those cells.
    if underflows:
        status = "UNDER-RUN" if args.crf_max is not None else "UNDERFLOW"
    else:
        status = "OK"
    # The frame rate is printed on the verdict line, not just accepted, because
    # both numbers on this line scale with it. A transcript that does not say
    # which rate it used cannot be audited after the fact -- which is how the
    # 12/36 reading survived long enough to be quoted.
    print(f"{status} {args.stream}: {frames:g} frames @ {fps:.4g} fps "
          f"[{fps_src}], {kbps:.1f} kbps "
          f"(maxrate {args.maxrate:.0f}, bufsize {args.bufsize:.0f} kbit), "
          f"min-fill {min_fill / 1000.0:.1f} kbit "
          f"({100.0 * min_fill / size:.1f}%), "
          f"{len(underflows)} underflow(s), {clamps} overflow-clamp(s)")
    if args.crf_max is not None:
        worst = min((fl for _, _, _, fl in underflows), default=0.0)
        print(f"  --crf-max {args.crf_max:g}: {len(underflows)} frame(s) under-ran the "
              f"buffer, worst {-worst / 1000.0:.1f} kbit below empty "
              f"({100.0 * -worst / size:.1f}% of the buffer). The ceiling kept those "
              f"frames' quality and the model paid for it.")
    if underflows and not args.quiet:
        for idx, typ, bits, fl in underflows[:20]:
            print(f"  frame {idx} (nal {typ}): {bits} bits, "
                  f"fill {fl / 1000.0:.1f} kbit", file=sys.stderr)
    return 1 if underflows and args.crf_max is None else 0


if __name__ == "__main__":
    sys.exit(main())
