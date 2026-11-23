#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""field_score_check.py - scripts/bdcompare.py on field-coded streams.

    python3 tests/field_score_check.py path/to/cli/y264

On a synthetic CIF clip, coded as frames and with --tff and --bff:

- bdcompare's coded-frame count is the clip's frame count, where ffprobe's
  packet count reads two per field-coded frame;
- with the v1 libvmaf and its models installed (VMAF_V1_BIN, VMAF_V1_MODELS),
  each stream scores, far above its score against the source one frame
  on, and a field-coded one lands within a few points of the frame-coded
  one at the same CRF. Without them that half is skipped.

Needs ffmpeg and ffprobe; exits 77 (skipped) without them.
"""
import os, shutil, subprocess, sys, tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts"))
import bdcompare  # noqa: E402

# CIF, not smaller: CAMBI refuses a coded size much under it
W, H, NF = 352, 288, 16


def clip(path, first=0):
    with open(path, "wb") as f:
        f.write(b"YUV4MPEG2 W%d H%d F25:1 Ip A1:1 C420jpeg\n" % (W, H))
        for n in range(first, first + NF):
            f.write(b"FRAME\n")
            # a fixed random texture panned 3 columns and 1 row a frame:
            # detail and motion enough that a frame scored against the wrong
            # source frame reads far lower
            y = bytearray(W * H)
            for r in range(H):
                for c in range(W):
                    u, v = (c + 3 * n) >> 1, (r + n) >> 1
                    y[r * W + c] = ((u * 2654435761 ^ v * 40503) >> 7) & 0xFF
            f.write(bytes(y))
            f.write(bytes([128 + (n & 7)]) * (W * H // 4))
            f.write(bytes([128 - (n & 7)]) * (W * H // 4))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    if not (shutil.which("ffmpeg") and shutil.which("ffprobe")):
        print("skip: needs ffmpeg and ffprobe")
        return 77
    enc = sys.argv[1]
    bdcompare.SCORE_CACHE = False
    src_dir = tempfile.mkdtemp(prefix="fldchk")
    src, late = os.path.join(src_dir, "c.y4m"), os.path.join(src_dir, "late.y4m")
    clip(src)
    clip(late, 1)
    model = bdcompare.v1_model(src)
    can_score = os.access(bdcompare.V1_BIN, os.X_OK) and os.path.exists(model)
    fails, scores = 0, {}
    try:
        for name, extra in (("frames", []), ("tff", ["--tff"]), ("bff", ["--bff"])):
            out = os.path.join(src_dir, name + ".264")
            r = subprocess.run([enc, "--input-y4m", src, "--crf", "30", "--bframes", "2",
                                "--threads", "1", "--no-progress", "-o", out] + extra,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if r.returncode != 0:
                print(f"FAIL {name}: exit {r.returncode}: {r.stderr.decode()[-300:]}")
                fails += 1
                continue
            n, pk = bdcompare._coded_frames(out), bdcompare._packets(out)
            want_pk = NF if name == "frames" else 2 * NF
            if n != NF or pk != want_pk:
                print(f"FAIL {name}: {n} coded frames and {pk} packets, want {NF} and {want_pk}")
                fails += 1
                continue
            msg = f"{n} frames from {pk} packets"
            if can_score:
                v = bdcompare._v1_of(out, src, src_dir)
                if v is None or not 0.0 < v["VMAF-v1"] <= 100.0:
                    print(f"FAIL {name}: v1 score {v}")
                    fails += 1
                    continue
                # the control: against the source one frame on, the same
                # stream must score far lower, or the check above is blind
                off = bdcompare._v1_of(out, late, src_dir)
                if off is None or off["VMAF-v1"] > v["VMAF-v1"] - 20.0:
                    print(f"FAIL {name}: one frame off the source scores {off}, aligned {v}")
                    fails += 1
                    continue
                scores[name] = v["VMAF-v1"]
                msg += f", VMAF-v1 {v['VMAF-v1']:.2f} ({off['VMAF-v1']:.2f} one frame off)"
            print(f"ok   {name}: {msg}")
        # a misaligned or unwoven field decode scores far under the frame
        # decode; coding the same pictures as fields costs a little, not that
        for name in ("tff", "bff"):
            if "frames" in scores and name in scores and scores[name] < scores["frames"] - 10.0:
                print(f"FAIL {name}: VMAF-v1 {scores[name]:.2f} against {scores['frames']:.2f} coded as frames")
                fails += 1
        if not can_score:
            print(f"note: no v1 libvmaf at {bdcompare.V1_BIN} or model at {model}; scores not checked")
    finally:
        shutil.rmtree(src_dir, ignore_errors=True)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
