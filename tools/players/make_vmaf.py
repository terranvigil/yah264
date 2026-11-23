#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""
make_vmaf.py - build the per-frame VMAF data (and browser-playable MP4s) that
the comparison and inspection players load.

It takes a reference plus one or two encodings, decodes each to raw frames,
runs the standalone `vmaf` CLI per frame, remuxes/transcodes the videos to MP4
so a browser can play them, and writes a JSON file the players read.

It scores VMAF v1 only, with the repo's scorer (scripts/bdcompare.py): the
libvmaf at VMAF_V1_BIN, the v1.0.16 model from VMAF_V1_MODELS picked by the
source's size and frame rate, both sides at 10-bit, and a picture smaller
than 1920x1080 scaled up to fit it. YAH264_VMAF_MODEL=/path/to/model.json
names a model file instead.

Inputs may be:
  - reference: .y4m, .mp4, .mov, or a raw .264 Annex-B stream
  - encoding:  a .264 Annex-B stream (yah264 / x264 output) or an .mp4

Examples
--------
  # two encodings -> data-compare.json (feeds compare.html)
  ./make_vmaf.py --reference src.y4m \\
      --enc yah264:yah264.264 --enc x264:x264.264 -o site

  # one encoding -> data-inspect.json (feeds inspect.html)
  ./make_vmaf.py --reference src.y4m --enc yah264:yah264.264 -o site
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "..", "scripts"))
import bdcompare                                                # noqa: E402

FFMPEG = os.environ.get("FFMPEG", "ffmpeg")
VMAF = bdcompare.V1_BIN


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, **kw)


def need(tool):
    if shutil.which(tool) is None and not os.path.exists(tool):
        sys.exit(f"make_vmaf: '{tool}' not found (set FFMPEG or VMAF_V1_BIN to override)")


def probe_fps(path):
    """Frames-per-second as a float, from ffprobe. Falls back to 30."""
    p = run(["ffprobe", "-v", "error", "-select_streams", "v:0",
             "-show_entries", "stream=r_frame_rate", "-of",
             "default=nokey=1:noprint_wrappers=1", path])
    txt = p.stdout.decode().strip()
    m = re.match(r"(\d+)/(\d+)", txt)
    if m and int(m.group(2)) != 0:
        return int(m.group(1)) / int(m.group(2))
    try:
        return float(txt)
    except ValueError:
        return 30.0


def to_y4m(src, dst):
    """Decode any input to a yuv420p Y4M for VMAF."""
    p = run([FFMPEG, "-v", "error", "-y", "-i", src, "-pix_fmt", "yuv420p", dst])
    if p.returncode != 0 or not os.path.exists(dst):
        sys.exit(f"make_vmaf: failed to decode {src} to Y4M\n{p.stderr.decode()}")


def to_mp4(src, dst):
    """Produce a browser-playable H.264 MP4.

    A .264 Annex-B stream is already H.264, so remux it (stream copy) into MP4.
    Anything else is transcoded near-lossless so the reference stays faithful.
    faststart moves the moov atom up front so playback starts immediately.
    """
    if src.lower().endswith(".264"):
        p = run([FFMPEG, "-v", "error", "-y", "-i", src, "-c", "copy",
                 "-movflags", "+faststart", dst])
        if p.returncode == 0 and os.path.exists(dst):
            return
        # some Annex-B streams need re-timestamping; fall through to transcode
    p = run([FFMPEG, "-v", "error", "-y", "-i", src, "-c:v", "libx264",
             "-crf", "12", "-pix_fmt", "yuv420p", "-movflags", "+faststart", dst])
    if p.returncode != 0 or not os.path.exists(dst):
        sys.exit(f"make_vmaf: failed to build MP4 from {src}\n{p.stderr.decode()}")


def v1_model_path(ref_y4m):
    """The v1 model file: YAH264_VMAF_MODEL when set, else the one the repo's
    scorer picks for this source's size and frame rate."""
    env_path = os.environ.get("YAH264_VMAF_MODEL")
    if env_path:
        if not os.path.exists(env_path):
            sys.exit(f"make_vmaf: YAH264_VMAF_MODEL names {env_path}, which doesn't exist")
        return env_path
    model = bdcompare.v1_model(ref_y4m)
    if not os.path.exists(model):
        sys.exit(f"make_vmaf: no v1 model at {model} (set VMAF_V1_MODELS or YAH264_VMAF_MODEL)")
    return model


def vmaf_per_frame(ref_y4m, enc_path, fps, work):
    """Score one encoding against the reference on VMAF v1, the way the BD
    gate does: both sides at 10-bit, a picture below 1920x1080 scaled up to
    fit it, CAMBI told the coded size and depth. Returns (list of
    {frame,time,vmaf}, model name)."""
    model = v1_model_path(ref_y4m)
    w, h, _ = bdcompare._y4m_geometry(ref_y4m)
    ebd = bdcompare._enc_bitdepth(enc_path)
    vf = ["-vf", bdcompare.V1_SCALE_VF] if bdcompare.v1_scaled(w, h) else []
    js = os.path.join(work, "v1.json")
    data = bdcompare.v1_pass(enc_path, ref_y4m, work, js,
                             ["--model", f"path={model}:{bdcompare.v1_model_opts(w, h, ebd)}"],
                             vf)
    if data is None:
        sys.exit(f"make_vmaf: the v1 pass failed for {enc_path} (needs {VMAF} and a "
                 "stream the same size as the reference)")
    frames = []
    for fr in data.get("frames", []):
        v = fr.get("metrics", {}).get("vmaf_v1")
        if v is None:
            continue
        n = fr.get("frameNum", len(frames))
        frames.append({"frame": n, "time": round(n / fps, 4),
                       "vmaf": round(v, 4)})
    return frames, os.path.splitext(os.path.basename(model))[0]


def parse_enc(spec):
    """'label:path' or just 'path' (label defaults to the file's basename)."""
    if ":" in spec and not os.path.exists(spec):
        label, path = spec.split(":", 1)
        return label, path
    return os.path.splitext(os.path.basename(spec))[0], spec


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--reference", required=True, help="reference/original video")
    ap.add_argument("--enc", action="append", required=True, metavar="LABEL:PATH",
                    help="an encoding to score; repeat once more for a 2-way compare")
    ap.add_argument("-o", "--out", default="site", help="output directory (default: site)")
    ap.add_argument("--fps", type=float, default=None, help="override frame rate")
    ap.add_argument("--no-video", action="store_true",
                    help="only write JSON, skip building MP4s (reuse existing ones)")
    args = ap.parse_args()

    if len(args.enc) > 2:
        sys.exit("make_vmaf: at most two --enc encodings are supported")
    for t in (FFMPEG, "ffprobe", VMAF):
        need(t)
    if not os.path.exists(args.reference):
        sys.exit(f"make_vmaf: reference not found: {args.reference}")

    os.makedirs(args.out, exist_ok=True)
    work = tempfile.mkdtemp(prefix="y264vmaf_")

    ref_y4m = os.path.join(work, "ref.y4m")
    to_y4m(args.reference, ref_y4m)
    fps = args.fps or probe_fps(ref_y4m)

    # reference MP4 so the inspect player and slider have the "truth" to show
    ref_mp4 = "reference.mp4"
    if not args.no_video:
        to_mp4(args.reference, os.path.join(args.out, ref_mp4))

    encs = []
    model_used = ""
    for i, spec in enumerate(args.enc):
        label, path = parse_enc(spec)
        if not os.path.exists(path):
            sys.exit(f"make_vmaf: encoding not found: {path}")
        tag = chr(ord("A") + i)
        frames, model = vmaf_per_frame(ref_y4m, path, fps, work)
        model_used = model
        mp4 = f"enc{tag}.mp4"
        if not args.no_video:
            to_mp4(path, os.path.join(args.out, mp4))
        vals = [f["vmaf"] for f in frames]
        encs.append({
            "label": label, "video": mp4, "frames": frames,
            "avgVmaf": round(sum(vals) / len(vals), 3) if vals else 0,
            "minVmaf": round(min(vals), 3) if vals else 0,
            "maxVmaf": round(max(vals), 3) if vals else 0,
        })
        print(f"  {tag}: {label:16} {len(frames)} frames  "
              f"avg {encs[-1]['avgVmaf']:6.2f}  min {encs[-1]['minVmaf']:6.2f}  "
              f"model {model}")

    doc = {
        "reference": ref_mp4,
        "fps": round(fps, 4),
        "model": model_used,
        "encodings": encs,
    }

    name = "data-compare.json" if len(encs) == 2 else "data-inspect.json"
    outp = os.path.join(args.out, name)
    with open(outp, "w") as f:
        json.dump(doc, f, indent=2)
    print(f"\nwrote {outp}  (fps {fps:.3f}, {len(encs)} encoding(s))")
    if len(encs) == 2:
        print(f"open compare.html?data={name} to compare, or inspect.html?data={name}")
    else:
        print(f"open inspect.html?data={name}")


if __name__ == "__main__":
    main()
