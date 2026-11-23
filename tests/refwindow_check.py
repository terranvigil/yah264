#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""refwindow_check.py - the reference window at --ref 15 and 16 without a pyramid.

    python3 tests/refwindow_check.py path/to/cli/y264

The SPS declares at most 15 reference frames. With flat B frames (--bframes 1,
or --b-pyramid none) the B frames are not references, so a P frame is decoded
with all 15 slots holding past anchors and may use every one of them. A B frame
is decoded after its future anchor, which takes one slot, so its list 0 can
reach only 14 past anchors. Each case reads num_ref_idx_l0_active out of every
slice header and wants exactly that: P frames reach 15 and never more, B frames
reach 14 and never more. Every stream must also decode under ffmpeg to the
encoder's own reconstruction.

Skips (exit 77) when ffmpeg is absent, since it makes the clip and decodes.
"""
import hashlib, os, shlex, shutil, subprocess, sys, tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts"))
import h264_syntax as hs  # noqa: E402

WRAP = shlex.split(os.environ.get("YAH264_WRAP", ""))
W, H, N = 64, 48, 72


def l0_sizes(path):
    """max_num_ref_frames and, per slice type, the largest list 0 seen"""
    sps = pps = None
    most = {"P": 0, "B": 0}
    with open(path, "rb") as f:
        nals = hs.split_nals(f.read())
    for nal in nals:
        t, rb = nal["type"], nal["rbsp"]
        if t == 7:
            sps = hs.parse_sps(rb)
        elif t == 8:
            br = hs.Bits(rb)
            br.ue(); br.ue(); br.u1()
            bottom = br.u1()
            if br.ue() != 0:
                raise ValueError("slice groups")
            pps = {"bottom": bottom, "l0": br.ue() + 1}
        elif t in (1, 5):
            br = hs.Bits(rb)
            br.ue()
            st = br.ue() % 5
            br.ue()
            br.u(sps["log2_max_frame_num"])
            field = 0
            if not sps["frame_mbs_only_flag"]:
                field = br.u1()
                if field:
                    br.u1()
            if t == 5:
                br.ue()
            br.u(sps["log2_max_poc_lsb"])
            if pps["bottom"] and not field:
                br.se()
            if st == 1:
                br.u1()                 # direct_spatial_mv_pred_flag
            if st in (0, 1):
                n = pps["l0"]
                if br.u1():             # num_ref_idx_active_override_flag
                    n = br.ue() + 1
                k = "B" if st == 1 else "P"
                most[k] = max(most[k], n)
    return sps["max_num_ref_frames"], most


def md5s(data, size):
    return [hashlib.md5(data[i:i + size]).hexdigest() for i in range(0, len(data), size)]


def recon(path, size):
    out = []
    with open(path, "rb") as f:
        f.readline()
        while f.readline():
            out.append(hashlib.md5(f.read(size)).hexdigest())
    return out


def main():
    cli = sys.argv[1]
    ff = shutil.which("ffmpeg")
    if not ff:
        print("refwindow_check: no ffmpeg, skipped")
        return 77
    env = {k: v for k, v in os.environ.items() if not k.startswith("Y264_")}
    bad = 0
    with tempfile.TemporaryDirectory(prefix="y264refwin-") as d:
        src = os.path.join(d, "src.y4m")
        subprocess.run([ff, "-v", "error", "-y", "-f", "lavfi", "-i",
                        f"testsrc2=size={W}x{H}:rate=25", "-frames:v", str(N),
                        "-pix_fmt", "yuv420p", src], check=True)
        out, rec = os.path.join(d, "o.264"), os.path.join(d, "r.y4m")
        cases = []
        for ref in (15, 16):
            for bf in ("0", "1", "2", "3"):
                for ent in ("--cabac", "--cavlc"):
                    cases.append((ref, bf, ent))
        for ref, bf, ent in cases:
            for p in (out, rec):
                if os.path.exists(p):
                    os.remove(p)
            args = ["--qp", "28", "--ref", str(ref), "--bframes", bf, "--b-pyramid", "none",
                    "--b-adapt", "0", "--keyint", "250", "--no-scenecut", ent, "--threads", "1"]
            r = subprocess.run(WRAP + [cli, "--input-y4m", src, "-o", out, "--dump-recon", rec,
                                       "--no-progress"] + args, capture_output=True, env=env)
            tag = f"--ref {ref} --bframes {bf} {ent}"
            if r.returncode or not os.path.exists(out):
                bad += 1
                print(f"FAIL {tag}: exit {r.returncode}")
                continue
            mrf, most = l0_sizes(out)
            want = {"P": 15, "B": 14 if bf != "0" else 0}
            ok = mrf == 15 and most == want
            dec = subprocess.run([ff, "-v", "error", "-i", out, "-f", "rawvideo", "-pix_fmt", "yuv420p", "-"],
                                 capture_output=True).stdout
            size = W * H * 3 // 2
            a = recon(rec, size)
            rm = len(a) == N and a == md5s(dec, size)
            bad += (not ok) + (not rm)
            print(f"{'ok  ' if ok and rm else 'FAIL'} {tag}: max_num_ref_frames {mrf}, "
                  f"list 0 up to P {most['P']} B {most['B']} (want P {want['P']} B {want['B']}), "
                  f"recon-match {'yes' if rm else 'NO'}")
    print(f"refwindow_check: {bad} failure(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
