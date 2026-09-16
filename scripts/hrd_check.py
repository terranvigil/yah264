#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""hrd_check.py - Annex C hypothetical reference decoder checker.

Reads an Annex-B H.264 elementary stream, takes the CPB schedule OUT OF THE
STREAM (SPS VUI hrd_parameters, buffering_period and pic_timing SEI), simulates
the coded picture buffer exactly as Annex C specifies, and fails on underflow or
overflow.

    hrd_check.py STREAM.264 [--sched all] [--verbose] [--allow-no-hrd]

HOW THIS DIFFERS FROM scripts/vbv_check.py, WHICH ALREADY EXISTS. vbv_check is
told the bucket -- `--maxrate`, `--bufsize`, `--fps` -- and simulates the
ENCODER's leaky-bucket law with a full buffer at frame 0. It answers "did our
rate control respect the cap we gave it". This tool is told nothing: every
number comes from the stream, the removal times come from the pic_timing SEI
rather than from an assumed constant frame rate, and the initial fullness comes
from the buffering_period SEI. It answers "would a conforming decoder's buffer
survive this stream", which is the question a receiver asks and the one a
hardware decoder enforces. Both are wanted; neither replaces the other.

A STREAM WITH NO HRD IS NOT A PASS. yah264's VBV writes no hrd_parameters
today (`--nal-hrd` is wave 3 of the parity programme), so the honest answer for
those streams is "nothing to check", and it exits 3 to say exactly that. A
checker that returned 0 there would read as a green gate on every VBV stream in
the tree while checking nothing at all -- which is how a whole category of gate
quietly stops gating. Pass --allow-no-hrd when a caller wants that soft.

Exit codes: 0 clean, 1 a violation, 2 usage/parse error, 3 no HRD in the stream.

THE MODEL (C.1.2, C.3.2-C.3.4), for one SchedSelIdx of one HRD type:

    tc          = num_units_in_tick / time_scale                (VUI timing)
    tr,n(0)     = initial_cpb_removal_delay / 90000
    tr,n(n)     = tr,n(nb) + tc * cpb_removal_delay(n)          nb = last buffering period
    tai(0)      = 0
    tai(n)      = taf(n-1)                                      cbr_flag == 1
                = max(taf(n-1), tai,earliest(n))                cbr_flag == 0
    taf(n)      = tai(n) + b(n) / BitRate
    underflow  <=>  tr,n(n) < taf(n)                            (C.3.3: the picture is
                                                                 removed before it arrived)
    overflow   <=>  fullness at tr,n(n) > CpbSize               (C.3.2)

b(n) is the access unit's size in bits: for the NAL HRD every byte of the byte
stream belonging to the access unit, START CODES INCLUDED; for the VCL HRD the
VCL NAL units only, start codes excluded. Filler data (NAL type 12) belongs to
the access unit it FOLLOWS -- it is the mechanism CBR uses to make the rate, so
attributing it to the next picture would report an underflow the stream does not
have.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import h264_syntax as H                                        # noqa: E402

EPS = 1e-9


def _delivered(ai, af, bits, t):
    """Bits of one access unit that have entered the CPB by time t."""
    if t >= af - EPS:
        return bits
    if t <= ai:
        return 0.0
    return bits * (t - ai) / (af - ai) if af > ai else bits


def simulate(aus, sps, kind, idx, verbose=False):
    """One (hrd kind, SchedSelIdx) pass. Returns (violations, report lines)."""
    vui = sps["vui"]
    hrd = vui["nal_hrd"] if kind == "nal" else vui["vcl_hrd"]
    sched = hrd["sched"][idx]
    bitrate = float(sched["bit_rate"])
    cpbsize = float(sched["cpb_size"])
    cbr = sched["cbr"]
    timing = vui["timing"]
    if not timing or timing["time_scale"] == 0:
        return ([("NOTIME", 0, "HRD present but the VUI carries no timing_info; "
                                "removal times are undefined")], [])
    tc = timing["num_units_in_tick"] / float(timing["time_scale"])

    # Per-AU inputs, read out of the stream and nowhere else.
    recs = []
    for n, au in enumerate(aus):
        bp = None
        pt = None
        for t, payload in au["sei"]:
            if t == 0:
                try:
                    bp = H.parse_buffering_period(payload, sps)
                except ValueError:
                    bp = None
            elif t == 1:
                try:
                    pt = H.parse_pic_timing(payload, sps)
                except ValueError:
                    pt = None
        bits = (au["bytes"] if kind == "nal" else au["vcl_bytes"]) * 8.0
        recs.append({"bp": bp, "pt": pt, "bits": bits})

    if recs[0]["bp"] is None:
        return ([("NOBP", 0, "HRD present but the first access unit carries no "
                             "buffering_period SEI")], [])
    missing_pt = [n for n, r in enumerate(recs) if n > 0 and r["pt"] is None]
    if missing_pt:
        return ([("NOPT", missing_pt[0],
                  "HRD present but access unit %d carries no pic_timing SEI "
                  "(%d of %d missing)" % (missing_pt[0], len(missing_pt), len(recs)))], [])

    viols = []
    lines = []
    tr = [0.0] * len(recs)
    tai = [0.0] * len(recs)
    taf = [0.0] * len(recs)
    tr_nb = 0.0
    init_nb = None                        # the delay pair in force for tai,earliest
    for n, r in enumerate(recs):
        bp = r["bp"]
        if n == 0:
            d = bp[kind][idx]
            tr[0] = d["initial"] / 90000.0
            tr_nb = tr[0]
            init_nb = d
            tai[0] = 0.0
        else:
            tr[n] = tr_nb + tc * r["pt"]["cpb_removal_delay"]
            if bp is not None and bp[kind]:
                d = bp[kind][idx]
                tr_nb = tr[n]
                init_nb = d
                earliest = tr[n] - d["initial"] / 90000.0
            else:
                earliest = tr[n] - (init_nb["initial"] + init_nb["offset"]) / 90000.0
            tai[n] = taf[n - 1] if cbr else max(taf[n - 1], earliest)
        taf[n] = tai[n] + r["bits"] / bitrate

        if tr[n] < taf[n] - EPS:
            viols.append(("UNDERFLOW", n,
                          "removal at %.6fs but the last bit arrives at %.6fs "
                          "(short by %.0f bits)"
                          % (tr[n], taf[n], (taf[n] - tr[n]) * bitrate)))

    # Fullness at each removal instant (C.3.2), which is where it peaks.
    #
    # THE TWO DELIVERY SCHEDULES NEED TWO FORMULAS, and using one for both is
    # the mistake that makes this check vacuous. Under cbr_flag == 1 the channel
    # runs at BitRate for the whole stream -- that is what constant bit rate
    # means, and it is why CBR streams carry filler -- so what is in the buffer
    # at tr(n) is BitRate * tr(n) minus everything already removed. Strip the
    # filler out of a compliant CBR stream and that number walks past CpbSize,
    # which is the negative control in --self-test. Summing only the bits the
    # stream actually contains instead would have the buffer politely drain and
    # report the mutilated stream CLEAN.
    #
    # Under cbr_flag == 0 delivery PAUSES when the buffer is full (tai,earliest
    # is exactly that pause), so occupancy is the tail of access units that have
    # started arriving and not yet been removed, and overflow cannot occur by
    # construction. The number is still reported, because a peak sitting at
    # 100% of CpbSize says the stream has no headroom left.
    cum = [0.0] * (len(recs) + 1)
    for n, r in enumerate(recs):
        cum[n + 1] = cum[n] + r["bits"]
    worst_fill = 0.0
    worst_at = 0
    min_margin = None
    for n in range(len(recs)):
        t = tr[n]
        if cbr:
            fill = bitrate * t - cum[n]
        else:
            fill = 0.0
            k = n
            while k < len(recs) and tai[k] < t:
                fill += _delivered(tai[k], taf[k], recs[k]["bits"], t)
                k += 1
        if fill > worst_fill:
            worst_fill, worst_at = fill, n
        margin = tr[n] - taf[n]
        if min_margin is None or margin < min_margin:
            min_margin = margin
        if fill > cpbsize + EPS:
            viols.append(("OVERFLOW", n,
                          "buffer holds %.0f bits at removal, CpbSize is %.0f"
                          % (fill, cpbsize)))
        if verbose:
            lines.append("  au %4d  b=%8.0f  tai=%9.6f  taf=%9.6f  tr=%9.6f  fill=%9.0f"
                         % (n, recs[n]["bits"], tai[n], taf[n], tr[n], fill))

    lines.append("  %s hrd sched %d: BitRate %.0f bit/s  CpbSize %.0f bit  cbr=%d  tc=%.6fs"
                 % (kind.upper(), idx, bitrate, cpbsize, cbr, tc))
    lines.append("  peak fullness %.0f bit (%.1f%% of CpbSize) at au %d; "
                 "tightest arrival margin %.6fs"
                 % (worst_fill, 100.0 * worst_fill / cpbsize, worst_at, min_margin))
    return viols, lines


def _run(cmd):
    import subprocess
    return subprocess.run(cmd, stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode


def _check(path, extra=()):
    import subprocess
    cmd = [sys.executable, os.path.abspath(__file__), path, "--quiet"] + list(extra)
    return subprocess.run(cmd).returncode


def self_test(enc):
    """Fixtures, expectations and -- the part that matters -- NEGATIVE CONTROLS.

    A checker that has only ever been run on compliant streams has not been
    shown to be able to fail. Both mutations below are physical rather than
    arbitrary: strip a CBR stream's filler and its buffer overflows, which is
    what the filler was there to prevent; add a large filler NAL to one access
    unit and its own last bit arrives after its removal time.
    """
    import shutil
    import subprocess
    import tempfile

    for prog in ("ffmpeg", "x264"):
        if shutil.which(prog) is None:
            print("hrd_check --self-test: %s not found" % prog, file=sys.stderr)
            return 2
    if not os.path.isfile(enc):
        print("hrd_check --self-test: no yah264 at %s (--enc)" % enc, file=sys.stderr)
        return 2

    tmp = tempfile.mkdtemp(prefix="hrdself")
    try:
        src = os.path.join(tmp, "src.y4m")
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i",
                        "testsrc2=size=320x240:rate=25", "-frames:v", "50",
                        "-pix_fmt", "yuv420p", src], check=True)

        def x264(name, *args):
            out = os.path.join(tmp, name + ".264")
            _run(["x264"] + list(args) + ["-o", out, src])
            return out

        cbr = x264("cbr", "--nal-hrd", "cbr", "--vbv-maxrate", "800",
                   "--vbv-bufsize", "800", "--bitrate", "800", "--keyint", "25")
        vbr = x264("vbr", "--nal-hrd", "vbr", "--vbv-maxrate", "1000",
                   "--vbv-bufsize", "1000", "--bitrate", "600", "--keyint", "25")
        cbrb = x264("cbrb", "--nal-hrd", "cbr", "--vbv-maxrate", "1500",
                    "--vbv-bufsize", "1500", "--bitrate", "1500", "--bframes", "3",
                    "--b-pyramid", "normal", "--keyint", "10")

        # yah264's own VBV: a real stream with no HRD in it at all.
        yvbv = os.path.join(tmp, "yvbv.264")
        _run([enc, "--input-y4m", src, "--cabac", "--crf", "20", "--vbv-maxrate",
              "600", "--vbv-bufsize", "600", "--threads", "1", "-o", yvbv])

        data = H.read_stream(cbr)
        aus, _ = H.split_aus(data)

        nofill = os.path.join(tmp, "nofiller.264")
        with open(nofill, "wb") as f:
            for nl in H.split_nals(data):
                if nl["type"] != 12:
                    f.write(data[nl["start"]:nl["start"] + nl["size"]])

        bloat = os.path.join(tmp, "bloat.264")
        pad = b"\x00\x00\x00\x01\x0c" + b"\xff" * 100000 + b"\x80"
        with open(bloat, "wb") as f:
            for i, au in enumerate(aus):
                s = au["nals"][0]["start"]
                f.write(data[s:s + au["bytes"]])
                if i == 10:
                    f.write(pad)

        cases = [
            ("x264 --nal-hrd cbr",                   cbr,    (),     0),
            ("x264 --nal-hrd vbr",                   vbr,    (),     0),
            ("x264 --nal-hrd cbr, b-pyramid",        cbrb,   (),     0),
            ("x264 cbr, all SchedSelIdx",            cbr,    ("--sched", "all"), 0),
            ("yah264 VBV (no HRD in the stream)",    yvbv,   (),     3),
            ("yah264 VBV with --allow-no-hrd",       yvbv,   ("--allow-no-hrd",), 0),
            ("NEGATIVE: cbr with the filler removed", nofill, (),    1),
            ("NEGATIVE: cbr with an access unit bloated", bloat, (), 1),
        ]
        bad = 0
        for name, path, extra, want in cases:
            got = _check(path, extra)
            ok = got == want
            bad += 0 if ok else 1
            print("  %-44s want %d got %d  %s" % (name, want, got, "ok" if ok else "FAIL"))
        print("hrd_check --self-test: %s" % ("8/8 as expected" if not bad
                                             else "%d of %d wrong" % (bad, len(cases))))
        return 1 if bad else 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description="Annex C HRD/CPB compliance checker")
    ap.add_argument("stream", nargs="?")
    ap.add_argument("--self-test", action="store_true",
                    help="run the fixture battery, negative controls included")
    ap.add_argument("--enc", default=os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build", "cli", "yah264"),
        help="yah264 binary for --self-test")
    ap.add_argument("--sched", default="0",
                    help="SchedSelIdx to check, or 'all' (default 0)")
    ap.add_argument("--verbose", action="store_true", help="print every access unit")
    ap.add_argument("--allow-no-hrd", action="store_true",
                    help="exit 0 instead of 3 when the stream carries no HRD")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()

    if a.self_test:
        return self_test(a.enc)
    if not a.stream:
        ap.error("a stream is required (or --self-test)")

    try:
        data = H.read_stream(a.stream)
    except OSError as e:
        print("hrd_check: %s" % e, file=sys.stderr)
        return 2
    try:
        aus, _ = H.split_aus(data)
    except ValueError as e:
        print("hrd_check: parse error: %s" % e, file=sys.stderr)
        return 2
    if not aus:
        print("hrd_check: no access units in %s" % a.stream, file=sys.stderr)
        return 2
    sps = aus[0]["sps"]
    if sps is None:
        print("hrd_check: no SPS in %s" % a.stream, file=sys.stderr)
        return 2
    vui = sps.get("vui")
    kinds = []
    if vui:
        if vui.get("nal_hrd"):
            kinds.append("nal")
        if vui.get("vcl_hrd"):
            kinds.append("vcl")
    if not kinds:
        # Said out loud, always -- the exit code alone is too easy to swallow.
        # Under --quiet it goes to stderr so a caller reading stdout still gets
        # the message without it landing in the middle of a report.
        print("hrd_check: NO HRD -- %s carries no hrd_parameters in its SPS VUI, "
              "so nothing was checked (%d access units, %d bytes)"
              % (a.stream, len(aus), len(data)),
              file=sys.stderr if a.quiet else sys.stdout)
        return 0 if a.allow_no_hrd else 3

    fails = 0
    out = []
    for kind in kinds:
        hrd = vui["nal_hrd"] if kind == "nal" else vui["vcl_hrd"]
        idxs = range(hrd["cpb_cnt"]) if a.sched == "all" else [int(a.sched)]
        for idx in idxs:
            if idx >= hrd["cpb_cnt"]:
                print("hrd_check: SchedSelIdx %d but cpb_cnt is %d"
                      % (idx, hrd["cpb_cnt"]), file=sys.stderr)
                return 2
            viols, lines = simulate(aus, sps, kind, idx, a.verbose)
            out.extend(lines)
            for what, n, msg in viols:
                out.append("  FAIL %s %s sched %d au %d: %s" % (what, kind, idx, n, msg))
            fails += len(viols)

    if not a.quiet:
        print("hrd_check: %s -- %d access units, %d bytes, hrd: %s"
              % (a.stream, len(aus), len(data), "+".join(kinds)))
        for ln in out:
            print(ln)
        print("hrd_check: %s" % ("CLEAN" if fails == 0 else "%d violation(s)" % fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
