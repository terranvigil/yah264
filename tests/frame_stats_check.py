#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""frame_stats_check.py - --frame-stats against the stream it describes.

    python3 tests/frame_stats_check.py path/to/cli/y264

On a synthetic CIF clip, no corpus and no ffmpeg needed, for frame
pictures, a sliced encode and --tff/--bff field coding, on the GOP-parallel
path and the one-instance path (--dump-recon):

- one line per input frame, every frame named once;
- line n matches the stream's n-th coded frame: its frame is that frame's
  display index (read from the POCs), its type is the first picture's slice
  type, its bytes are all of the frame's slice NALs, start codes included,
  so a sliced or field-coded frame counts all of its pictures, and a
  field-coded line's "fields" give each field's slice type and bytes as the
  stream has them. A frame-coded line carries no "fields" key.

Cells cover frame threads inside one instance (--threads 3 on one GOP, the
one-instance path at 3 threads, a two-pass) as well as the GOP-parallel
path.
"""
import json, os, subprocess, sys, tempfile

# CIF and 64 frames: a smaller picture gets one frame thread, and an input
# much past the CLI's 32-frame window is needed before calls start to end
# between the two fields of a pair
W, H, NF = 352, 288, 64


def clip(path, interlace=b"Ip"):
    with open(path, "wb") as f:
        f.write(b"YUV4MPEG2 W%d H%d F25:1 %s A1:1 C420jpeg\n" % (W, H, interlace))
        for n in range(NF):
            f.write(b"FRAME\n")
            y = bytearray(W * H)
            for r in range(H):
                for c in range(W):
                    y[r * W + c] = ((c + 2 * n) * 7 + (r + n) * 5 + ((c * r + n) * 31 >> 5)) & 0xFF
            f.write(bytes(y))
            f.write(bytes([128 + (n & 15)]) * (W * H // 4))
            f.write(bytes([128 - (n & 15)]) * (W * H // 4))


class _Bits:
    """An Exp-Golomb reader over a NAL's payload, emulation bytes removed."""

    def __init__(self, ebsp):
        self.b = bytes(ebsp).replace(b"\x00\x00\x03", b"\x00\x00")
        self.p = 0

    def u(self, n):
        v = 0
        for _ in range(n):
            v = v << 1 | (self.b[self.p >> 3] >> (7 - (self.p & 7)) & 1)
            self.p += 1
        return v

    def ue(self):
        z = 0
        while not self.u(1):
            z += 1
        return (1 << z) - 1 + self.u(z)

    def se(self):
        k = self.ue()
        return (k + 1) // 2 if k & 1 else -(k // 2)


def _sps(b):
    """The SPS fields a slice header and the POC need."""
    prof = b.u(8)
    b.u(16)
    sid = b.ue()
    sep = 0
    if prof in (100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135):
        cf = b.ue()
        if cf == 3:
            sep = b.u(1)
        b.ue(); b.ue(); b.u(1)
        if b.u(1):
            for i in range(12 if cf == 3 else 8):
                if b.u(1):
                    last = nxt = 8
                    for _ in range(16 if i < 6 else 64):
                        if nxt:
                            nxt = (last + b.se() + 256) % 256
                        last = nxt or last
    sps = {"sep": sep, "lmfn": b.ue() + 4, "poc": b.ue()}
    if sps["poc"] != 0:
        raise ValueError(f"POC type {sps['poc']}: only type 0 is read here")
    sps["lpoc"] = b.ue() + 4
    b.ue(); b.u(1); b.ue(); b.ue()
    sps["fmo"] = b.u(1)
    return sid, sps


def stream_frames(path, ppf):
    """The stream's frames in coding order, each {bytes, pics: [(type, bytes)
    per picture], disp}: slice NALs from their start code to the next, a new
    picture at every first_mb_in_slice 0, ppf pictures to a frame. disp is the
    frame's display index, from its first picture's POC (type 0, MSB carried
    from the last reference picture) ranked inside its IDR period."""
    data = open(path, "rb").read()
    starts, i = [], 0
    while True:
        i = data.find(b"\x00\x00\x01", i)
        if i < 0:
            break
        starts.append(i - 1 if i > 0 and data[i - 1] == 0 else i)
        i += 3
    sps, pps, pics = {}, {}, []
    prev_msb = prev_lsb = 0
    for k, s in enumerate(starts):
        end = starts[k + 1] if k + 1 < len(starts) else len(data)
        h = s + (4 if data[s + 2] == 0 else 3)
        t, ref_idc = data[h] & 0x1F, data[h] >> 5 & 3
        if t == 7:
            sid, v = _sps(_Bits(data[h + 1:end]))
            sps[sid] = v
        elif t == 8:
            b = _Bits(data[h + 1:end])
            pid, sid = b.ue(), b.ue()
            pps[pid] = sid
        if t not in (1, 5):
            continue
        b = _Bits(data[h + 1:h + 32])
        if b.ue() == 0:                             # first_mb_in_slice
            st = b.ue()
            q = sps[pps[b.ue()]]
            if q["sep"]:
                b.u(2)
            b.u(q["lmfn"])
            if not q["fmo"] and b.u(1):
                b.u(1)
            if t == 5:
                b.ue()
                prev_msb = prev_lsb = 0
            lsb, half = b.u(q["lpoc"]), 1 << (q["lpoc"] - 1)
            msb = prev_msb
            if lsb < prev_lsb and prev_lsb - lsb >= half:
                msb += 2 * half
            elif lsb > prev_lsb and lsb - prev_lsb > half:
                msb -= 2 * half
            if ref_idc:
                prev_msb, prev_lsb = msb, lsb
            pics.append(["PBI??"[st % 5], 0, msb + lsb, t == 5])
        pics[-1][1] += end - s
    frames = []
    for j in range(0, len(pics), ppf):
        group = pics[j:j + ppf]
        frames.append({"bytes": sum(p[1] for p in group), "pics": [(p[0], p[1]) for p in group],
                       "poc": group[0][2], "idr": group[0][3]})
    period = []
    for n, f in enumerate(frames + [{"idr": True}]):
        if f["idr"] and period:
            for rank, m in enumerate(sorted(period, key=lambda m: frames[m]["poc"])):
                frames[m]["disp"] = period[0] + rank
            period = []
        period.append(n)
    return frames


def check(lines, frames, field):
    """What's wrong with the stats lines against the stream's frames, or []."""
    bad = []
    names = sorted(l["frame"] for l in lines)
    if names != list(range(NF)):
        bad.append(f"{len(lines)} lines naming {names[:8]}..., want frames 0..{NF - 1}")
    if len(frames) != len(lines):
        bad.append(f"{len(lines)} lines for {len(frames)} coded frames")
    for n, (l, f) in enumerate(zip(lines, frames)):
        fl = l.get("fields")
        want = None if not field else [{"type": t, "bytes": b} for t, b in f["pics"]]
        if (l["bytes"] != f["bytes"] or fl != want or l["type"] != f["pics"][0][0]
                or l["frame"] != f["disp"]):
            bad.append(f"line {n}: frame {l['frame']} bytes {l['bytes']} type {l['type']} fields {fl}; "
                       f"the stream's coded frame {n} is display frame {f['disp']} with {f['bytes']} "
                       f"bytes in {f['pics']}")
            break
    return bad


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    enc = sys.argv[1]
    fails = 0
    vbv = ["--bitrate", "400", "--vbv-maxrate", "400", "--vbv-bufsize", "400"]
    with tempfile.TemporaryDirectory(prefix="fstchk") as d:
        src, src_it = os.path.join(d, "c.y4m"), os.path.join(d, "it.y4m")
        clip(src)
        clip(src_it, b"It")
        recon, stats = os.path.join(d, "r.y4m"), os.path.join(d, "2pass.stats")
        gop = ["--threads", "2", "--keyint", "12"]
        serial = ["--threads", "1", "--dump-recon", recon]
        cells = []
        for name, extra in (("frames", []), ("slices", ["--slices", "3"]), ("tff", ["--tff"]),
                            ("bff", ["--bff"]), ("tff-sliced", ["--tff", "--slices", "2"])):
            cells += [(f"{name}/gop", gop + ["--crf", "26"] + extra, None),
                      (f"{name}/serial", serial + ["--crf", "26"] + extra, None)]
        # frame threads inside one instance: a call ends between the two
        # fields of a pair, and a frame's index can come out in another call
        cells += [("tff-abr/gop", gop + ["--tff"] + vbv, None),
                  ("tff-abr/serial", serial + ["--tff"] + vbv, None),
                  ("tff/t3", ["--threads", "3", "--keyint", "250", "--crf", "26", "--tff"], None),
                  ("tff/serial-t3", ["--threads", "3", "--dump-recon", recon, "--crf", "26", "--tff"], None),
                  ("slices/t3", ["--threads", "3", "--keyint", "250", "--crf", "26", "--slices", "4"], None),
                  ("tff-sliced/t3", ["--threads", "3", "--keyint", "250", "--crf", "26", "--tff",
                                     "--slices", "4"], None),
                  ("tff/2pass-t2", ["--threads", "2", "--tff", "--bitrate", "400", "--pass", "2",
                                    "--stats", stats],
                   ["--threads", "2", "--tff", "--bitrate", "400", "--pass", "1", "--stats", stats])]
        # an interlaced Y4M field-codes with no option asking for it
        cells.append(("y4m-It/t3", ["--threads", "3", "--keyint", "250", "--crf", "26"], None))
        for tag, args, first in cells:
            field = "--tff" in args or "--bff" in args or tag.startswith("y4m-It")
            csrc = src_it if tag.startswith("y4m-It") else src
            out, fst = os.path.join(d, "o.264"), os.path.join(d, "s.jsonl")
            for f in (out, fst):        # a stale file from the last cell must not pass
                if os.path.exists(f):
                    os.unlink(f)
            base = [enc, "--input-y4m", csrc, "--bframes", "2", "--no-progress"]
            if first:
                r = subprocess.run(base + ["-o", os.devnull] + first,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                if r.returncode != 0:
                    print(f"FAIL {tag}: pass 1 exit {r.returncode}: {r.stderr.decode()[-300:]}")
                    fails += 1
                    continue
            r = subprocess.run(base + ["-o", out, "--frame-stats", fst] + args,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if r.returncode != 0:
                print(f"FAIL {tag}: exit {r.returncode}: {r.stderr.decode()[-300:]}")
                fails += 1
                continue
            lines = [json.loads(l) for l in open(fst)]
            bad = check(lines, stream_frames(out, 2 if field else 1), field)
            if bad:
                print(f"FAIL {tag}: " + "; ".join(bad))
                fails += 1
            else:
                print(f"ok   {tag}: {len(lines)} lines, {sum(l['bytes'] for l in lines)} slice bytes")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
