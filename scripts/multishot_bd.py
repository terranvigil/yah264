#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""BD-rate on the multi-shot sequences: the across-shot allocation gate.

The ten-clip board is single-shot by construction and cannot see how bits
move between shots; this encodes the S0 multi-shot sequences
(local/corpus/<seq>.y4m with a <seq>.cuts file: "<first frame> <name>" per
shot) at several CRF points for every arm and reports, per sequence,
BD-VMAF-v1 for every arm pair, the per-shot VMAF v1 at each point and the
per-shot bit share. Arms are "name=<command template>" with {src} {q} {out};
"name=env:VAR=val ..." runs our binary (YAH264, default build/cli/yah264)
at --threads 0 under that environment, "x264" runs the reference at medium.

  scripts/multishot_bd.py --arms flat=env: aqdc=env:Y264_AQ_DC=0.4 x264 \\
      --seqs ms_cif_30,ms_720p_50,ms_1080p_25 --points 22,26,30,34

Encodes and VMAF json files are cached under --work by (seq, arm, q); use a
fresh --work after a rebuild. BD sign: negative = the first arm of the pair
spends fewer bits at equal VMAF v1. Scoring is scripts/bdcompare.py's v1 path
(10-bit 4:2:0 both sides, model by the source's height and frame rate, CAMBI at
the stream's bit depth); VMAF_V1_BIN and VMAF_V1_MODELS override the binary and
the model directory, VMAF_THREADS (default 8) the libvmaf threads.
"""
import argparse, json, math, os, shlex, subprocess, sys
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bdcompare                                                # noqa: E402


def bd_rate(r_ref, m_ref, r_test, m_test):
    deg = min(3, len(m_ref) - 1)
    f1 = np.polyfit(m_ref, np.log(r_ref), deg)
    f2 = np.polyfit(m_test, np.log(r_test), deg)
    lo, hi = max(min(m_ref), min(m_test)), min(max(m_ref), max(m_test))
    if hi <= lo:
        return None
    xs = np.linspace(lo, hi, 100)
    return (math.exp(np.mean(np.polyval(f2, xs) - np.polyval(f1, xs))) - 1) * 100


def sizes(path):
    p = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "frame=pkt_size",
                        "-of", "csv=p=0", path], capture_output=True, text=True)
    return [int(x.strip().rstrip(",")) for x in p.stdout.split() if x.strip().rstrip(",").isdigit()]


def vmaf_v1(src, bitstream, js):
    """Score every frame with VMAF v1 into js. The reference reaches 10-bit on
    a FIFO, so no full-size 10-bit copy of a multi-shot source lands on disk."""
    model = bdcompare.v1_model(src)
    if not os.path.exists(model) or not os.access(bdcompare.V1_BIN, os.X_OK):
        sys.exit(f"multishot_bd: needs {bdcompare.V1_BIN} and {model}")
    w, h, _ = bdcompare._y4m_geometry(src)
    vf = ["-vf", bdcompare.V1_SCALE_VF] if bdcompare.v1_scaled(w, h) else []
    dec = js + ".dec10.y4m"
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", bitstream, "-fps_mode", "passthrough",
                    *vf, "-pix_fmt", "yuv420p10le", "-strict", "-1", dec], check=True)
    ref = js + ".ref10.y4m"
    if os.path.exists(ref):
        os.unlink(ref)
    os.mkfifo(ref)
    conv = subprocess.Popen(["ffmpeg", "-v", "error", "-nostdin", "-y", "-i", src, *vf, "-pix_fmt",
                             "yuv420p10le", "-strict", "-1", "-f", "yuv4mpegpipe", ref],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    r = subprocess.Popen([bdcompare.V1_BIN, "-q", "-r", ref, "-d", dec, "--json", "-o", js,
                          "--model", f"path={model}:"
                          + bdcompare.v1_model_opts(w, h, bdcompare._enc_bitdepth(bitstream)),
                          "--threads", os.environ.get("VMAF_THREADS", "8")],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # a reference ffmpeg that fails before it opens the FIFO leaves libvmaf
    # blocked in open() for ever; the whole sequence is encoded, so the two
    # sides end together and a failed reference is a real failure
    while True:
        try:
            r.wait(timeout=0.5)
            break
        except subprocess.TimeoutExpired:
            if conv.poll() not in (None, 0):
                r.kill()
                r.wait()
                break
    conv.kill()
    conv.wait()
    os.unlink(ref); os.unlink(dec)
    if r.returncode:
        if os.path.exists(js):
            os.unlink(js)
        sys.exit(f"vmaf failed on {bitstream} (exit {r.returncode})")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--arms", nargs="+", required=True)
    ap.add_argument("--seqs", default="ms_cif_30,ms_720p_50,ms_1080p_25")
    ap.add_argument("--points", default="22,26,30,34")
    ap.add_argument("--corpus", default=os.path.join(ROOT, "local", "corpus"))
    ap.add_argument("--work", default=os.path.join(ROOT, "local", "multishot_work"))
    ap.add_argument("--x264", default=os.environ.get("X264", os.path.join(ROOT, "..", "x264", "x264")))
    ap.add_argument("--threads", default="0", help="our --threads (0 = auto)")
    a = ap.parse_args()
    ours = os.environ.get("YAH264", os.path.join(ROOT, "build", "cli", "yah264"))
    os.makedirs(a.work, exist_ok=True)
    pts = [int(x) for x in a.points.split(",")]
    arms = {}
    for spec in a.arms:
        name, _, tmpl = spec.partition("=")
        if name == "x264" and not tmpl:
            tmpl = f"{shlex.quote(a.x264)} --preset medium --crf {{q}} --threads 12 --demuxer y4m -o {{out}} {{src}}"
        elif tmpl.startswith("env:"):
            env = tmpl[4:].strip()
            tmpl = (f"env {env} " if env else "") + f"{shlex.quote(ours)} --input-y4m {{src}} --crf {{q}} --threads {a.threads} -o {{out}}"
        arms[name] = tmpl
    for seq in a.seqs.split(","):
        src = os.path.join(a.corpus, seq + ".y4m")
        cuts = [ln.split() for ln in open(os.path.join(a.corpus, seq + ".cuts")) if ln.strip()]
        names = [c[1] if len(c) > 1 else f"shot{i}" for i, c in enumerate(cuts)]
        cuts = [int(c[0]) for c in cuts]
        res = {}
        for arm, tmpl in arms.items():
            for q in pts:
                out = os.path.join(a.work, f"{seq}_{arm}_{q}.264")
                js = os.path.join(a.work, f"{seq}_{arm}_{q}.v1-{bdcompare.V1_RULE}.json")
                if not os.path.exists(js):
                    cmd = tmpl.format(src=shlex.quote(src), q=q, out=shlex.quote(out))
                    r = subprocess.run(cmd, shell=True, capture_output=True, text=True)
                    if r.returncode:
                        sys.exit(f"encode failed: {cmd}\n{r.stderr[-400:]}")
                    vmaf_v1(src, out, js)
                per = [f["metrics"]["vmaf_v1"] for f in json.load(open(js))["frames"]]
                sz = sizes(out)
                n = len(per); b = cuts + [n]
                shots = [float(np.mean(per[b[i]:b[i + 1]])) for i in range(len(cuts))]
                share = [sum(sz[b[i]:b[i + 1]]) / max(1, sum(sz)) * 100 for i in range(len(cuts))]
                res[(arm, q)] = (os.path.getsize(out) * 8, float(np.mean(per)), shots, share)
                print(f"{seq:12s} {arm:8s} crf{q}: {res[(arm, q)][0] / 1000:9.0f} kbit  v1 {res[(arm, q)][1]:6.2f}"
                      f"  shots " + " ".join(f"{s:5.1f}" for s in shots)
                      + "  share% " + " ".join(f"{s:4.1f}" for s in share), flush=True)
        print(f"{seq}: shots = " + ", ".join(names))
        arm_names = list(arms)
        for i, t in enumerate(arm_names):
            for r in arm_names[i + 1:]:
                rr = [res[(r, q)][0] for q in pts]; mr = [res[(r, q)][1] for q in pts]
                rt = [res[(t, q)][0] for q in pts]; mt = [res[(t, q)][1] for q in pts]
                bd = bd_rate(rr, mr, rt, mt)
                print(f"BD-VMAF-v1 {seq} {t} vs {r}: " + (f"{bd:+.2f}%" if bd is not None else "no overlap"), flush=True)


if __name__ == "__main__":
    main()
