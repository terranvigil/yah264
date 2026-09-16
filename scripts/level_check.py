#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""level_check.py - Annex A level-limit checker for an H.264 elementary stream.

Reads the SPS (and the HRD, when the stream carries one), works out the LOWEST
level in Table A-1 that the stream's own numbers fit, and asserts that the
declared level_idc is at least that -- i.e. that the encoder's automatic level
pick is conformant and is never exceeded.

    level_check.py STREAM.264 [--fps N[:D] | --y4m SRC.y4m] [--bitrate KBPS]
                              [--exact] [--strict-mv] [--quiet]

WHY A SEPARATE TOOL FROM THE ENCODER'S OWN PICK. The encoder chooses a level
from the parameters it was given; this reads the level back out of the bytes it
wrote, with an independently transcribed Table A-1, and compares. A checker that
shared the encoder's table would agree with it by construction and gate nothing.

THE LEGS, and what each one can honestly see from a bitstream:

  frame size   PicWidthInMbs x FrameHeightInMbs <= MaxFS, and each dimension
               within sqrt(8 x MaxFS) (A.3.1 d/e). Read from the SPS.
  MB rate      frame size x frame rate <= MaxMBPS. The frame rate comes from
               the VUI timing_info, or from --fps / --y4m when the stream
               carries none. There is deliberately no default frame rate: a
               guessed rate is wrong in the direction that PASSES.
  DPB          max_dec_frame_buffering <= Min(MaxDpbMbs / frame size, 16)
               (A.3.1 h). Falls back to max_num_ref_frames when the VUI carries
               no bitstream_restriction.
  bit rate     BitRate <= cpbBrNalFactor x MaxBR and CpbSize <= cpbBrNalFactor x
               MaxCPB, from the HRD when the stream has one. WITHOUT AN HRD
               THIS LEG IS NOT IN THE STREAM: pass --bitrate to state the rate
               the encoder was driven at, or the leg reports the measured
               average and is left out of the pick. Saying so is the point --
               a level check that silently drops its rate leg passes streams
               that a hardware decoder at that level would refuse.
  vertical MV  MaxVmvR for the declared level, against the bound the stream
               itself declares in the VUI (log2_max_mv_length_vertical). The
               ACTUAL motion vectors are not read: that needs a full slice
               decode, which is a decoder, not a gate. The declared bound is a
               real check -- x264 narrows it to the level, and a stream that
               advertises a wider range than its level allows is telling
               decoders to size for more than the level promises.

Exit codes: 0 clean, 1 a violation, 2 usage/parse error.
"""
import argparse
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import h264_syntax as H                                        # noqa: E402

# ITU-T H.264 Table A-1, transcribed from the specification.
# (label, level_idc, MaxMBPS, MaxFS, MaxDpbMbs, MaxBR, MaxCPB, MaxVmvR)
# MaxBR and MaxCPB are in units of cpbBrNalFactor bits/s and bits.
TABLE_A1 = [
    ("1",   10,     1485,     99,    396,     64,    175,   64),
    ("1b",  11,     1485,     99,    396,    128,    350,   64),
    ("1.1", 11,     3000,    396,    900,    192,    500,  128),
    ("1.2", 12,     6000,    396,   2376,    384,   1000,  128),
    ("1.3", 13,    11880,    396,   2376,    768,   2000,  128),
    ("2",   20,    11880,    396,   2376,   2000,   2000,  128),
    ("2.1", 21,    19800,    792,   4752,   4000,   4000,  256),
    ("2.2", 22,    20250,   1620,   8100,   4000,   4000,  256),
    ("3",   30,    40500,   1620,   8100,  10000,  10000,  256),
    ("3.1", 31,   108000,   3600,  18000,  14000,  14000,  512),
    ("3.2", 32,   216000,   5120,  20480,  20000,  20000,  512),
    ("4",   40,   245760,   8192,  32768,  20000,  25000,  512),
    ("4.1", 41,   245760,   8192,  32768,  50000,  62500,  512),
    ("4.2", 42,   522240,   8704,  34816,  50000,  62500,  512),
    ("5",   50,   589824,  22080, 110400, 135000, 135000,  512),
    ("5.1", 51,   983040,  36864, 184320, 240000, 240000,  512),
    ("5.2", 52,  2073600,  36864, 184320, 240000, 240000,  512),
    ("6",   60,  4177920, 139264, 696320, 240000, 240000,  512),
    ("6.1", 61,  8355840, 139264, 696320, 480000, 480000,  512),
    ("6.2", 62, 16711680, 139264, 696320, 800000, 800000,  512),
]

# Table A-1 footnote: cpbBrNalFactor / cpbBrVclFactor by profile.
BR_FACTOR = {
    66: (1200, 1000), 77: (1200, 1000), 88: (1200, 1000),
    100: (1500, 1250), 110: (3000, 2500),
    122: (4000, 3333), 244: (4000, 3333), 44: (4000, 3333),
}


def row_for(label):
    for r in TABLE_A1:
        if r[0] == label:
            return r
    return None


def declared_label(sps):
    """level_idc plus constraint_set3_flag is how level 1b is spelled."""
    lv = sps["level_idc"]
    cs3 = (sps["constraint_set"] >> 4) & 1
    if lv == 9:
        return "1b"
    if lv == 11 and cs3 and sps["profile_idc"] in (66, 77, 88):
        return "1b"
    for r in TABLE_A1:
        if r[1] == lv and r[0] != "1b":
            return r[0]
    return None


def pick_level(fs, w_mbs, h_mbs, mbps, dpb_frames, bitrate_bps, cpb_bits, nal_factor):
    """The lowest Table A-1 row that fits every leg it is given. Rate legs are
    skipped when the caller passes None -- see the module docstring."""
    import math
    for label, _idc, max_mbps, max_fs, max_dpb, max_br, max_cpb, _vmv in TABLE_A1:
        if label == "1b":
            continue                      # never auto-picked: it needs constraint_set3
        if fs > max_fs:
            continue
        side = int(math.sqrt(8 * max_fs))
        if w_mbs > side or h_mbs > side:
            continue
        if mbps is not None and mbps > max_mbps:
            continue
        if dpb_frames is not None and dpb_frames > min(max_dpb // fs, 16):
            continue
        if bitrate_bps is not None and bitrate_bps > max_br * nal_factor:
            continue
        if cpb_bits is not None and cpb_bits > max_cpb * nal_factor:
            continue
        return label
    return None


def _run(cmd):
    import subprocess
    return subprocess.run(cmd, stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode


def _check(path, extra=()):
    import subprocess
    cmd = [sys.executable, os.path.abspath(__file__), path, "--quiet"] + list(extra)
    return subprocess.run(cmd).returncode


def self_test(enc, corpus):
    """Synthetic geometries, a real corpus clip at its board rate, and a forced
    level below the conformant minimum as the negative control -- without which
    a green run only proves the tool can say CLEAN."""
    import shutil
    import subprocess
    import tempfile

    for prog in ("ffmpeg", "x264"):
        if shutil.which(prog) is None:
            print("level_check --self-test: %s not found" % prog, file=sys.stderr)
            return 2
    if not os.path.isfile(enc):
        print("level_check --self-test: no yah264 at %s (--enc)" % enc, file=sys.stderr)
        return 2

    tmp = tempfile.mkdtemp(prefix="lvlself")
    cases = []
    try:
        def lavfi(name, size, rate, frames):
            p = os.path.join(tmp, name + ".y4m")
            subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i",
                            "testsrc2=size=%s:rate=%s" % (size, rate),
                            "-frames:v", str(frames), "-pix_fmt", "yuv420p", p], check=True)
            return p

        cif = lavfi("cif", "352x288", 25, 30)
        hd = lavfi("hd", "1280x720", 30, 20)

        def y264(name, src, *args):
            out = os.path.join(tmp, name + ".264")
            _run([enc, "--input-y4m", src, "--threads", "1"] + list(args) + ["-o", out])
            return out

        # x264's own auto pick, with its HRD carrying the rate leg.
        xc = os.path.join(tmp, "x.264")
        _run(["x264", "--nal-hrd", "cbr", "--vbv-maxrate", "800", "--vbv-bufsize",
              "800", "--bitrate", "800", "-o", xc, cif])

        cases.append(("x264 cbr 352x288, level from its own HRD", xc,
                      ("--exact",), 0))
        cases.append(("yah264 auto level, 352x288 qp 26",
                      y264("cif_auto", cif, "--qp", "26"),
                      ("--y4m", cif, "--exact"), 0))
        cases.append(("yah264 auto level, 1280x720 qp 30",
                      y264("hd_auto", hd, "--qp", "30"),
                      ("--y4m", hd, "--exact"), 0))
        cases.append(("yah264 auto level, ABR with the rate leg supplied",
                      y264("cif_abr", cif, "--bitrate", "900"),
                      ("--y4m", cif, "--bitrate", "900", "--exact"), 0))
        cases.append(("NEGATIVE: --level 1.3 forced at 1280x720",
                      y264("hd_low", hd, "--qp", "30", "--level", "1.3"),
                      ("--y4m", hd,), 1))

        # A corpus clip at its board rate, when the corpus is fetched. The board
        # rates live in scripts/parity-clips.sh; foreman is the CIF anchor.
        for clip, rate in (("foreman_cif", 400), ("bus_cif", 400)):
            src = os.path.join(corpus, clip + ".y4m")
            if not os.path.isfile(src):
                continue
            cases.append(("corpus %s at the board rate %d kbit/s" % (clip, rate),
                          y264(clip, src, "--bitrate", str(rate)),
                          ("--y4m", src, "--bitrate", str(rate), "--exact"), 0))

        bad = 0
        for name, path, extra, want in cases:
            got = _check(path, extra)
            ok = got == want
            bad += 0 if ok else 1
            print("  %-52s want %d got %d  %s" % (name, want, got, "ok" if ok else "FAIL"))
        print("level_check --self-test: %s"
              % ("%d/%d as expected" % (len(cases), len(cases)) if not bad
                 else "%d of %d wrong" % (bad, len(cases))))
        return 1 if bad else 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description="Annex A level-limit checker")
    ap.add_argument("stream", nargs="?")
    ap.add_argument("--self-test", action="store_true",
                    help="run the fixture battery, negative control included")
    ap.add_argument("--enc", default=os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build", "cli", "yah264"),
        help="yah264 binary for --self-test")
    ap.add_argument("--corpus", default=os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tests", "corpus"),
        help="corpus directory for --self-test")
    ap.add_argument("--fps", help="frame rate as N or N:D, when the stream has no VUI timing")
    ap.add_argument("--y4m", help="read the frame rate from this source clip's Y4M header")
    ap.add_argument("--bitrate", type=float,
                    help="the rate (kbit/s) the encoder was driven at, for the rate leg "
                         "of a stream with no HRD")
    ap.add_argument("--exact", action="store_true",
                    help="require the declared level to EQUAL the pick, not merely reach it")
    ap.add_argument("--strict-mv", action="store_true",
                    help="fail when the VUI's declared vertical MV bound exceeds MaxVmvR")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()

    if a.self_test:
        return self_test(a.enc, a.corpus)
    if not a.stream:
        ap.error("a stream is required (or --self-test)")

    try:
        data = H.read_stream(a.stream)
        aus, _ = H.split_aus(data)
    except (OSError, ValueError) as e:
        print("level_check: %s" % e, file=sys.stderr)
        return 2
    if not aus or aus[0]["sps"] is None:
        print("level_check: no SPS in %s" % a.stream, file=sys.stderr)
        return 2
    sps = aus[0]["sps"]
    vui = sps.get("vui") or {}

    fs = sps["frame_size_mbs"]
    w_mbs = sps["pic_width_in_mbs"]
    h_mbs = sps["frame_height_in_mbs"]

    # --- frame rate -------------------------------------------------------
    fps = None
    fps_src = None
    t = vui.get("timing")
    if t and t["num_units_in_tick"]:
        # time_scale counts FIELD ticks; a frame is two of them (E.2.1, and
        # DeltaTfiDivisor in C.2.3). 1/50 with time_scale 50 is 25 fps.
        fps = t["time_scale"] / (2.0 * t["num_units_in_tick"])
        fps_src = "VUI timing_info"
    if a.y4m:
        try:
            with open(a.y4m, "rb") as f:
                hdr = f.readline().decode("ascii", "replace")
            for tok in hdr.split():
                if tok.startswith("F"):
                    n, d = tok[1:].split(":")
                    fps = float(n) / float(d)
                    fps_src = "Y4M header of %s" % os.path.basename(a.y4m)
        except (OSError, ValueError) as e:
            print("level_check: --y4m: %s" % e, file=sys.stderr)
            return 2
    if a.fps:
        n, _, d = a.fps.partition(":")
        fps = float(n) / float(d or 1)
        fps_src = "--fps"

    # --- DPB --------------------------------------------------------------
    bsr = vui.get("bitstream_restriction")
    if bsr:
        dpb_frames = bsr["max_dec_frame_buffering"]
        dpb_src = "VUI max_dec_frame_buffering"
    else:
        dpb_frames = sps["max_num_ref_frames"]
        dpb_src = "SPS max_num_ref_frames (no VUI bitstream_restriction)"

    # --- rate -------------------------------------------------------------
    prof = sps["profile_idc"]
    nal_factor, vcl_factor = BR_FACTOR.get(prof, (1200, 1000))
    hrd = vui.get("nal_hrd") or vui.get("vcl_hrd")
    is_vcl_hrd = vui.get("nal_hrd") is None and vui.get("vcl_hrd") is not None
    factor = vcl_factor if is_vcl_hrd else nal_factor
    bitrate_bps = cpb_bits = None
    rate_src = None
    if hrd:
        bitrate_bps = max(s["bit_rate"] for s in hrd["sched"])
        cpb_bits = max(s["cpb_size"] for s in hrd["sched"])
        rate_src = "%s HRD" % ("VCL" if is_vcl_hrd else "NAL")
    elif a.bitrate:
        bitrate_bps = a.bitrate * 1000.0
        rate_src = "--bitrate"

    measured = None
    if fps:
        measured = len(data) * 8.0 * fps / len(aus)

    # --- the pick ---------------------------------------------------------
    mbps = fs * fps if fps else None
    pick = pick_level(fs, w_mbs, h_mbs, mbps, dpb_frames, bitrate_bps, cpb_bits, factor)
    decl = declared_label(sps)

    lines = []
    fails = []
    lines.append("  profile_idc %d  level_idc %d (%s)  %dx%d mbs (%d MBs)  chroma_format %d"
                 % (prof, sps["level_idc"], decl or "?", w_mbs, h_mbs, fs,
                    sps["chroma_format_idc"]))
    if fps:
        lines.append("  frame rate %.4f fps (%s) -> %.0f MB/s over %d access units"
                     % (fps, fps_src, mbps, len(aus)))
    else:
        lines.append("  frame rate UNKNOWN (no VUI timing, no --fps/--y4m): "
                     "the MB/s leg is NOT checked")
    lines.append("  DPB %d frames (%s)" % (dpb_frames, dpb_src))
    if rate_src:
        lines.append("  rate leg from %s: BitRate %.0f bit/s%s"
                     % (rate_src, bitrate_bps,
                        ", CpbSize %.0f bit" % cpb_bits if cpb_bits else ""))
    else:
        lines.append("  rate leg NOT CHECKED (no HRD in the stream, no --bitrate)%s"
                     % ("; measured average %.0f kbit/s" % (measured / 1000.0)
                        if measured else ""))

    if decl is None:
        fails.append("level_idc %d is not a Table A-1 level" % sps["level_idc"])
    if pick is None:
        fails.append("no level in Table A-1 fits this stream")

    if decl and pick:
        r_decl = row_for(decl)
        r_pick = row_for(pick)
        lines.append("  lowest conformant level: %s; declared: %s" % (pick, decl))
        if r_decl[1] < r_pick[1] or (r_decl[1] == r_pick[1] and decl == "1b" and pick != "1b"):
            fails.append("declared level %s is BELOW the lowest conformant level %s"
                         % (decl, pick))
        elif a.exact and decl != pick:
            fails.append("declared level %s is not the auto pick %s" % (decl, pick))

        # every leg, against the DECLARED level -- "never exceeded"
        _, _, max_mbps, max_fs, max_dpb, max_br, max_cpb, max_vmv = r_decl
        side = int(math.sqrt(8 * max_fs))
        if fs > max_fs:
            fails.append("frame size %d MBs exceeds MaxFS %d for level %s" % (fs, max_fs, decl))
        if w_mbs > side or h_mbs > side:
            fails.append("frame %dx%d MBs exceeds the sqrt(8*MaxFS)=%d side bound for level %s"
                         % (w_mbs, h_mbs, side, decl))
        if mbps is not None and mbps > max_mbps:
            fails.append("%.0f MB/s exceeds MaxMBPS %d for level %s" % (mbps, max_mbps, decl))
        if dpb_frames > min(max_dpb // fs, 16):
            fails.append("DPB %d frames exceeds Min(MaxDpbMbs/FrameSize,16)=%d for level %s"
                         % (dpb_frames, min(max_dpb // fs, 16), decl))
        if bitrate_bps is not None and bitrate_bps > max_br * factor:
            fails.append("BitRate %.0f bit/s exceeds MaxBR %d x %d = %d for level %s"
                         % (bitrate_bps, max_br, factor, max_br * factor, decl))
        if cpb_bits is not None and cpb_bits > max_cpb * factor:
            fails.append("CpbSize %.0f bit exceeds MaxCPB %d x %d = %d for level %s"
                         % (cpb_bits, max_cpb, factor, max_cpb * factor, decl))

        # vertical MV: the bound the stream declares about itself
        if bsr:
            declared_vmv = (1 << bsr["log2_max_mv_length_vertical"]) / 4.0
            note = ("  vertical MV: level %s allows +-%d luma samples; the VUI declares "
                    "+-%.0f" % (decl, max_vmv, declared_vmv))
            if declared_vmv > max_vmv:
                note += "  <-- WIDER THAN THE LEVEL"
                if a.strict_mv:
                    fails.append("VUI log2_max_mv_length_vertical implies +-%.0f luma "
                                 "samples, above MaxVmvR %d for level %s"
                                 % (declared_vmv, max_vmv, decl))
            lines.append(note)
        else:
            lines.append("  vertical MV: level %s allows +-%d luma samples; the stream "
                         "declares no bound (no VUI bitstream_restriction)" % (decl, max_vmv))

    if not a.quiet:
        print("level_check: %s -- %d access units, %d bytes" % (a.stream, len(aus), len(data)))
        for ln in lines:
            print(ln)
        for f in fails:
            print("  FAIL %s" % f)
        print("level_check: %s" % ("CLEAN" if not fails else "%d violation(s)" % len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
