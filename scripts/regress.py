#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: BSD-2-Clause
"""Seeded option-matrix regression harness, after x264's tools/test_x264.py.

x264's harness had three ideas worth keeping: a random cartesian product over
an option table (seeded, so a failure replays), verification through a REAL
decoder rather than self-comparison, and results keyed to the git revision so
runs are comparable across commits. This is those three ideas in Python 3,
fitted to this tree's rules:

  - every assertion is on the PRODUCER (exit code, frame count, decode
    errors), never only a comparison of two outputs -- two empty files match;
  - decoded frame count == input frame count is a hard assert (the RCP_LAG
    defect emitted streams that decoded fine and LOST frames);
  - each cell is encoded twice and must be byte-identical (catches the
    lookahead-race class that TSan missed);
  - wall/cpu-seconds/cores are RECORDED per cell but never gated -- a loaded
    box cannot time anything, so perf rows are for cross-revision reading
    (bench/ boards stay the timing authority);
  - runs on a fresh clone: with no --clip it generates a deterministic
    synthetic clip via ffmpeg lavfi (the corpus is gitignored and must not be
    a dependency of coverage tests).

Usage:
  scripts/regress.py                     # smoke tier: 8 cells, synthetic clip
  scripts/regress.py --tier full         # 50 cells
  scripts/regress.py --seed 7 --cell 3   # replay one cell of a seeded run
  scripts/regress.py --clip tests/corpus/foreman_cif.y4m --frames 120

Results append to local/regress/results.jsonl, one row per cell:
{rev, when, seed, cell, args, ok, fails, frames, kbps, psnr, wall, cpu, cores}.
A future perf/regression pass reads that file and diffs rows between two revs
at matched (seed, cell); nothing is built on it yet.

The JM reference decoder is the strongest oracle x264's harness used; wire it
via JM_DECODER=<ldecod binary> and each cell is also decoded with JM and its
frame count checked. Optional -- ffmpeg is the always-on decoder.
"""
import argparse, hashlib, json, os, random, re, resource, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
YAH264 = os.environ.get("YAH264", os.path.join(ROOT, "build", "cli", "yah264"))
FFMPEG = os.environ.get("FFMPEG", "ffmpeg")
JM = os.environ.get("JM_DECODER", "")

# The option table. Each axis is a list of alternatives; a cell draws one
# entry per axis. "" = axis absent, mirroring x264's ("", "--flag") pairs.
# --pass 2 and --dump-recon are deliberately absent: 2-pass needs a stats
# plumbing step (TODO) and recon forces the serial path, which conformance.sh
# already covers.
AXES = [
    ("rc",        ["--crf 26", "--crf 40", "--qp 30", "--bitrate 400",
                   "--bitrate 400 --vbv-maxrate 600 --vbv-bufsize 600",
                   "--bitrate 400 --pass 2"]),
    ("preset",    ["", "--preset ultrafast", "--preset veryfast",
                   "--preset veryslow"]),
    ("entropy",   ["", "--cavlc"]),
    ("bframes",   ["", "--bframes 0", "--bframes 1"]),
    ("transform", ["", "--no-transform-8x8"]),
    ("threads",   ["--threads 1", "--threads 2", "--threads 0"]),
    ("gop",       ["", "--keyint 25", "--no-scenecut"]),
    ("tune",      ["", "--tune zerolatency", "--tune animation"]),
    ("misc",      ["", "--direct temporal", "--aq-strength 0",
                   "--trellis 2", "--ref 1", "--cqm jvt", "--sar 16:11"]),
    # The hardware mode (docs/videotoolbox-plan.md): a quarter of the draws
    # go through VideoToolbox when the machine has it and the format is
    # 4:2:0 8-bit; everywhere else `auto` falls back to our encoder with one
    # line, so the cell still exercises the CLI's fall-back path.
    ("hw",        ["", "", "", "--hw auto"]),
]


def sh(cmd, **kw):
    return subprocess.run(cmd, shell=isinstance(cmd, str),
                          capture_output=True, text=True, **kw)


def git_rev():
    r = sh(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"])
    rev = r.stdout.strip() if r.returncode == 0 else "unknown"
    d = sh(["git", "-C", ROOT, "status", "--porcelain"])
    if d.returncode == 0 and d.stdout.strip():
        rev += "+dirty"
    return rev


# The format axis draws a CLIP, not a flag: chroma format and bit depth ride
# the Y4M header and the encoder follows it. Weighted so 8-bit 4:2:0 stays the
# bulk of coverage (the non-420 paths are real but rarer in the field).
# Bit depth is a BUILD option (-Dbit_depth=10), not runtime: probe_formats()
# asks the binary and keeps only the depths this build accepts, so the axis
# always matches the binary under test (point YAH264 at a 10-bit build and
# the 10-bit format joins the draw).
FORMATS = ["yuv420p", "yuv420p", "yuv420p", "yuv420p",
           "yuv422p", "yuv444p", "yuv420p10le"]


def probe_formats(work):
    kept = []
    for fmt in dict.fromkeys(FORMATS):          # unique, order kept
        clip = synth_clip(work, 2, fmt)
        r = sh(f"{YAH264} --input-y4m {clip} --frames 1 -o {os.devnull}")
        if r.returncode == 0:
            kept.append(fmt)
        else:
            print(f"  format {fmt} excluded: "
                  f"{r.stderr.strip().splitlines()[-1][:80]}")
    return [f for f in FORMATS if f in kept]


def synth_clip(work, frames, pix_fmt="yuv420p"):
    """Deterministic coverage clip: testsrc2 has motion, edges, text and
    flat regions. Not a quality corpus -- a coverage input."""
    path = os.path.join(work, f"synth_{frames}_{pix_fmt}.y4m")
    if not os.path.exists(path):
        r = sh([FFMPEG, "-y", "-v", "error", "-f", "lavfi",
                "-i", f"testsrc2=size=192x144:rate=30",
                "-frames:v", str(frames), "-pix_fmt", pix_fmt,
                "-strict", "-1", path])
        if r.returncode != 0:
            sys.exit(f"synthetic clip generation failed: {r.stderr.strip()}")
    return path


def decode(dec_cmd, bs, tag, fails):
    """Run a decoder over the bitstream; return decoded frame count."""
    r = sh(dec_cmd)
    if r.returncode != 0:
        fails.append(f"{tag}-decode-exit-{r.returncode}")
        return -1
    err = [l for l in r.stderr.splitlines()
           if re.search(r"error|corrupt|invalid|co located", l, re.I)]
    if err:
        fails.append(f"{tag}-decode-errors({len(err)}):{err[0][:90]}")
    m = re.findall(r"frame=\s*(\d+)", r.stderr)
    return int(m[-1]) if m else -1


def run_cell(idx, args, clip, nframes, work, record, keep):
    out = os.path.join(work, f"cell{idx}.264")
    cmd = f"{YAH264} --input-y4m {clip} --frames {nframes} {args} -o {out}"
    fails = []

    # a "--pass 2" cell runs its own pass 1 first, into a per-cell stats file
    # (both encodes rerun for the determinism check, sequentially, so the
    # stats file cannot cross-contaminate)
    pass1 = None
    if "--pass 2" in args:
        stats = os.path.join(work, f"cell{idx}.stats")
        cmd += f" --stats {stats}"
        pass1 = cmd.replace("--pass 2", "--pass 1").replace(out, os.devnull)

    ru0 = resource.getrusage(resource.RUSAGE_CHILDREN)
    t0 = time.monotonic()
    if pass1:
        r1 = sh(pass1)
        if r1.returncode != 0:
            fails.append(f"pass1-exit-{r1.returncode}:{r1.stderr.strip()[:120]}")
    r = sh(cmd)
    wall = time.monotonic() - t0
    ru1 = resource.getrusage(resource.RUSAGE_CHILDREN)
    cpu = (ru1.ru_utime - ru0.ru_utime) + (ru1.ru_stime - ru0.ru_stime)

    if r.returncode != 0:
        fails.append(f"encode-exit-{r.returncode}:{r.stderr.strip()[:120]}")
    size = os.path.getsize(out) if os.path.exists(out) else 0
    if size == 0:
        fails.append("empty-output")

    md5 = kbps = psnr = None
    dec_frames = -1
    if not fails:
        md5 = hashlib.md5(open(out, "rb").read()).hexdigest()

        # determinism: same cell twice, byte-identical. Not asserted on the
        # hardware mode: Apple's encoder is not byte-stable run to run
        # (docs/videotoolbox-plan.md step 5 says so on its row); the rerun
        # still has to succeed.
        out2 = out + ".2"
        r2 = sh(cmd.replace(out, out2))
        hw_used = "encoder: VideoToolbox" in r.stderr
        if r2.returncode != 0:
            fails.append("rerun-encode-failed")
        elif not hw_used and hashlib.md5(open(out2, "rb").read()).hexdigest() != md5:
            fails.append("nondeterministic")
        if os.path.exists(out2):
            os.remove(out2)

        # the always-on real decoder
        dec_frames = decode([FFMPEG, "-v", "info", "-i", out,
                             "-f", "null", "-"], out, "ffmpeg", fails)
        if dec_frames != nframes:
            fails.append(f"frame-count {dec_frames}!={nframes}")
        if JM:
            jf = decode([JM, "-p", f"InputFile={out}", "-p",
                         "OutputFile=/dev/null"], out, "jm", fails)
            if jf >= 0 and jf != nframes:
                fails.append(f"jm-frame-count {jf}!={nframes}")

        # quality floor: catches "decodes fine, looks like garbage". The two
        # inputs are aligned by FRAME INDEX, not by timestamp: a raw .264 with
        # no VUI timing (the hardware's) decodes at a guessed rate and the
        # psnr filter would otherwise pair the wrong frames (18 dB on a
        # 35 dB stream, seen 2026-09-04).
        p = sh([FFMPEG, "-v", "info", "-i", out, "-i", clip,
                "-frames:v", str(nframes),
                "-lavfi", "[0:v]setpts=N/(30*TB)[a];[1:v]setpts=N/(30*TB)[b];[a][b]psnr",
                "-f", "null", "-"])
        m = re.search(r"average:(\d+\.?\d*)", p.stderr)
        if m:
            psnr = float(m.group(1))
            if psnr < 20.0:
                fails.append(f"psnr-floor {psnr}")
        else:
            fails.append("psnr-unreadable")

        # rate sanity, loose: coverage clips are tiny and ABR converges slowly
        kbps = size * 8 * 30.0 / nframes / 1000.0
        mrate = re.search(r"--bitrate (\d+)", args)
        if mrate and dec_frames == nframes:
            tgt = float(mrate.group(1))
            if abs(kbps - tgt) / tgt > 0.30:
                fails.append(f"rate-miss {kbps:.0f} vs {tgt:.0f} kbps")

    row = {"rev": REV, "when": int(time.time()), "seed": SEED, "cell": idx,
           "args": args, "clip": os.path.basename(clip),
           "ok": not fails, "fails": fails, "frames": dec_frames,
           "size": size, "kbps": round(kbps, 1) if kbps else None,
           "psnr": psnr, "wall": round(wall, 3), "cpu": round(cpu, 3),
           "cores": round(cpu / wall, 1) if wall > 0 else None}
    record.write(json.dumps(row) + "\n")

    status = "PASS" if not fails else "FAIL " + "; ".join(fails)
    print(f"  cell {idx:3d}  {status:<60s}  "
          f"[{args}] [{os.path.basename(clip)}]")
    if os.path.exists(out) and not keep:
        os.remove(out)
    return not fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tier", choices=["smoke", "full"], default="smoke")
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--products", type=int, default=None)
    ap.add_argument("--cell", type=int, default=None,
                    help="run only this cell index of the seeded run")
    ap.add_argument("--clip", default=None)
    ap.add_argument("--frames", type=int, default=None)
    ap.add_argument("--keep", action="store_true",
                    help="keep per-cell bitstreams in the work dir")
    a = ap.parse_args()

    global REV, SEED
    REV = git_rev()
    SEED = a.seed if a.seed is not None else int(time.time())
    nprod = a.products or (8 if a.tier == "smoke" else 50)
    nframes = a.frames or (60 if a.tier == "smoke" else 150)

    work = os.path.join(ROOT, "local", "regress")
    os.makedirs(work, exist_ok=True)

    # the format axis is drawn from the SAME rng stream as the flags, so a
    # (seed, cell) pair names one exact configuration, format included. The
    # draw is over the FULL format list and the pick is then mapped onto what
    # this build accepts, so cell numbering is stable across 8- and 10-bit
    # builds of the same seed.
    formats = probe_formats(work) if not a.clip else []
    rng = random.Random(SEED)
    cells = []
    for _ in range(nprod):
        args = " ".join(v for _, v in
                        [(n, rng.choice(alts)) for n, alts in AXES] if v)
        pick = rng.choice(FORMATS)
        if formats and pick not in formats:
            pick = formats[0]                   # the 4:2:0 workhorse
        cells.append((args, pick))

    print(f"regress: rev {REV}  seed {SEED}  {nprod} cells  "
          f"{nframes} frames  clip "
          f"{os.path.basename(a.clip) if a.clip else 'synthetic per-format'}  "
          f"jm {'yes' if JM else 'no'}")
    ok = True
    with open(os.path.join(work, "results.jsonl"), "a") as record:
        for i, (args, fmt) in enumerate(cells):
            if a.cell is not None and i != a.cell:
                continue
            clip = a.clip or synth_clip(work, max(nframes, 150), fmt)
            ok &= run_cell(i, args, clip, nframes, work, record, a.keep)
    print("regress: ALL PASS" if ok else "regress: FAILURES (replay with "
          f"--seed {SEED} --cell N)")
    sys.exit(0 if ok else 1)


main()
