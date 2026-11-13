#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# dispatch_probe.py -- does each SIMD tier's code actually RUN when its cap
# says it should (x86-64 builds, docs/simd.md).
#
# checkasm calls every kernel by its own name, so it says a kernel is right,
# never that the encoder reaches it. A dispatch guard that drops a tier, a
# table entry left on C, or a call to the wrong tier leaves checkasm green and
# every tier encoding byte-identical to C. This probe watches the calls.
#
# It relinks the CLI from the build's own objects with `ld --wrap` on every
# kernel the tier archives define. Each wrapper bumps a counter and jumps to
# the kernel, so nothing else about the binary changes. Then it encodes a few
# short clips made from --source (4:2:0 at an odd macroblock width, 4:2:2,
# 4:4:4, at each depth asked for) in a few configurations, under each cap,
# and requires, per depth:
#   none     no kernel is called (the cap takes);
#   each cap no kernel above the cap is called, and for every kernel that is
#            called at all, under any cap, the highest tier of it compiled at
#            or under this cap is the one called, and no other tier of it is;
#   coverage every kernel the build has is reached from outside the tier
#            objects and called under some cap and configuration, unless
#            ALLOWED says why it can't be.
# A kernel's name without its tier suffix ties its tiers together.
#
# usage: dispatch_probe.py --build DIR --source Y4M [--depths "8 10"]
#            [--tiers "none sse4 avx2"] [--wrapper "CMD ..."] [--work DIR]
# --source is an 8-bit 4:2:0 CIF clip (the corpus foreman_cif). --wrapper runs
# the probe binary under, e.g., an explicit qemu-x86_64.
# Exit 0 when everything passes, 1 on a failure, 2 on a usage error.
import argparse, os, re, shlex, subprocess, sys

RANK = {"none": 0, "sse4": 1, "avx2": 2, "avx512": 3}
MEMBER = re.compile(r"([a-z0-9]+)_(sse4|avx2|avx512)(_10)?\.c\.o$")
KERNEL = re.compile(r"^(.*)_(sse4|avx2|avx512)(_10)?$")
KNOBS = ("Y264_SIMD_FORCE", "YAH264_NO_ASM", "Y264_ASM_OFF")
NM = os.environ.get("NM", "nm")
# nm -A: "archive:member:value type name", the value absent on U lines
NMLINE = re.compile(r"^([^:]+):([^:]+):\s*(?:[0-9a-f]+\s+)?(\S)\s+(\S+)$")
# kernels no encode can reach, by name without tier or depth suffix. checkasm
# still tests each of them; these are the encoder paths that never call them
ALLOWED = {
    "y264_bilin4_row": "the lowres bilinear row has no caller left",
    "y264_deblock_luma_h4": "compiled out: the whole-edge kernel takes every edge first",
    "y264_deblock_luma_v4": "compiled out: the whole-edge kernel takes every edge first",
    "y264_ssd4x4_map16": "its one caller, a static SSD map in mbcode.c, has no caller",
    "y264_hpel_hrow": "the band form takes every band; its scratch always fits",
    "y264_hpel_outrow": "the band form takes every band; its scratch always fits",
    "y264_intra8x8": "no caller: intra 8x8 goes through intra8x8_edge and from_edge",
    "y264_sa8d_8x8": "a pixel-table entry the encoder never reads",
    "y264_hadamard_ac_8x8": "a pixel-table entry the encoder never reads",
    "y264_texture_ac4_16x16": "a pixel-table entry the encoder never reads",
}
CLIPS = (("420w336", ["-vf", "crop=336:288:0:0"], "yuv420p"),
         ("422", [], "yuv422p"),
         ("444", [], "yuv444p"))
CONFIGS = (("slow", ["--preset", "slow"]),
           ("rdo", ["--preset", "placebo", "--psy-rd", "1.0", "--psy-trellis", "1.0"]))


def die(msg, rc=2):
    print(f"dispatch: {msg}")
    sys.exit(rc)


def run(cmd, **kw):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, **kw)


def nm(args):
    r = run([NM] + args)
    if r.returncode:
        die(f"{NM} {' '.join(args)} failed: {r.stdout.strip()[:200]}")
    return [m for m in map(NMLINE.match, r.stdout.splitlines()) if m]


def kernels(build):
    """{symbol: (depth, tier, family, base)} from the tier archives' global code"""
    out = {}
    for depth, prefix in (("8", "libyah264_x86_"), ("10", "libyah264_10_x86_")):
        for tier in ("sse4", "avx2", "avx512"):
            a = os.path.join(build, prefix + tier + ".a")
            if not os.path.exists(a):
                continue
            for p in nm(["-A", "-g", "--defined-only", a]):
                m = MEMBER.search(p.group(2))
                k = KERNEL.match(p.group(4))
                if p.group(3) == "T" and m and k:
                    out[p.group(4)] = (depth, m.group(2), m.group(1), k.group(1))
    return out


def expected(src, depth, tier):
    d = os.path.join(src, "src", "dsp", "x86")
    suffix = f"_{tier}_10.c" if depth == "10" else f"_{tier}.c"
    return {f[: -len(suffix)] for f in os.listdir(d)
            if f.endswith(suffix) and (depth == "10" or not f.endswith("_10.c"))}


def referenced(build, ks):
    """the kernels some object outside the tier archives refers to"""
    seen = set()
    for lib in ("libyah264.a", "libyah264_10.a"):
        for p in nm(["-A", "-u", os.path.join(build, lib)]):
            if p.group(4) in ks and not MEMBER.search(p.group(2)):
                seen.add(p.group(4))
    return seen


def link(build, ks, work):
    names = sorted(ks)
    with open(os.path.join(work, "wrap.s"), "w") as f:
        f.write("\t.text\n")
        for i, s in enumerate(names):
            f.write(f"\t.globl __wrap_{s}\n\t.type __wrap_{s}, @function\n__wrap_{s}:\n"
                    f"\tlock incq y264dp_n+{8 * i}(%rip)\n\tjmp __real_{s}\n")
        f.write('\t.section .note.GNU-stack,"",@progbits\n')
    with open(os.path.join(work, "count.c"), "w") as f:
        f.write("#include <stdio.h>\n#include <stdlib.h>\n")
        f.write(f"unsigned long long y264dp_n[{len(names)}];\n")
        f.write(f"static const char *const nm[{len(names)}] = {{\n")
        f.write("".join(f'  "{s}",\n' for s in names))
        f.write("};\nstatic void dump(void)\n{\n"
                '    const char *p = getenv("Y264_DISPATCH_OUT");\n'
                '    FILE *f = p ? fopen(p, "w") : NULL;\n'
                "    if (!f) return;\n"
                f"    for (int i = 0; i < {len(names)}; i++)\n"
                '        if (y264dp_n[i]) fprintf(f, "%s %llu\\n", nm[i], y264dp_n[i]);\n'
                "    fclose(f);\n}\n"
                "__attribute__((constructor)) static void init(void) { atexit(dump); }\n")
    with open(os.path.join(work, "wrap.rsp"), "w") as f:
        f.write("".join(f"-Wl,--wrap={s}\n" for s in names))
    cli = os.path.join(build, "cli", "yah264.p", "yah264_cli.c.o")
    if not os.path.exists(cli):
        die(f"no {cli}: build the CLI first")
    exe = os.path.join(work, "yah264-probe")
    cmd = shlex.split(os.environ.get("CC", "cc")) + [
        "-o", exe, cli, os.path.join(work, "wrap.s"), os.path.join(work, "count.c"),
        "@" + os.path.join(work, "wrap.rsp"),
        "-Wl,--start-group", os.path.join(build, "libyah264.a"),
        os.path.join(build, "libyah264_10.a"), "-lm", "-Wl,--end-group", "-pthread"]
    r = run(cmd)
    if r.returncode:
        die(f"link failed: {r.stdout.strip()[:600]}")
    return exe


def make_clips(source, depths, work):
    out = []
    for d in depths:
        for tag, vf, fmt in CLIPS:
            fmt = fmt + ("10le" if d == "10" else "")
            path = os.path.join(work, f"{tag}-{d}.y4m")
            if not os.path.exists(path):
                r = run(["ffmpeg", "-v", "error", "-y", "-i", source, "-frames:v", "3", *vf,
                         "-pix_fmt", fmt, "-strict", "-1", "-f", "yuv4mpegpipe", path + ".part"])
                if r.returncode:
                    die(f"ffmpeg could not make {path}: {r.stdout.strip()[:200]}")
                os.replace(path + ".part", path)
            out.append((d, tag, path))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--source", required=True)
    ap.add_argument("--src", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap.add_argument("--depths", default="8 10")
    ap.add_argument("--tiers", default="none sse4 avx2")
    ap.add_argument("--wrapper", default="")
    ap.add_argument("--work")
    a = ap.parse_args()
    tiers = list(dict.fromkeys(a.tiers.split()))
    depths = a.depths.split()
    if any(t not in RANK for t in tiers) or any(d not in ("8", "10") for d in depths):
        die(f"bad --tiers '{a.tiers}' or --depths '{a.depths}'")
    if not os.path.exists(a.source):
        die(f"no source clip {a.source}")
    work = a.work or os.path.join(a.build, "dispatch-probe")
    os.makedirs(work, exist_ok=True)
    ks = {s: k for s, k in kernels(a.build).items() if k[0] in depths}
    refs = referenced(a.build, ks)
    exe = link(a.build, ks, work)
    clips = make_clips(a.source, depths, work)
    wrap = shlex.split(a.wrapper)
    env = {k: v for k, v in os.environ.items() if k not in KNOBS}

    calls = {}          # (depth, cap) -> {symbol: count}, over every clip and config
    bad = []
    for d, tag, clip in clips:
        for t in tiers:
            for cname, copts in CONFIGS:
                out = os.path.join(work, f"calls-{d}-{tag}-{cname}-{t}.txt")
                if os.path.exists(out):
                    os.remove(out)
                e = dict(env, Y264_SIMD_FORCE=t, Y264_DISPATCH_OUT=out)
                r = run(wrap + [exe, "--input-y4m", clip, "--frames", "3", "--threads", "1",
                                *copts, "-o", os.devnull], env=e)
                if r.returncode or not os.path.exists(out):
                    bad.append(f"{d}-bit cap {t} {tag} {cname}: the encode failed "
                               f"(rc {r.returncode}): {r.stdout.strip()[-160:]}")
                    continue
                c = calls.setdefault((d, t), {})
                for line in open(out):
                    s, n = line.split()
                    if s in ks:
                        c[s] = c.get(s, 0) + int(n)

    for d in depths:
        mine = {s: k for s, k in ks.items() if k[0] == d}
        bases = {}
        for s, k in mine.items():
            bases.setdefault(k[3], {})[k[1]] = s
        called = {k[3] for t in tiers for s, k in mine.items() if calls.get((d, t), {}).get(s)}
        for t in tiers:
            why = []
            c = calls.get((d, t), {})
            have = {k[2] for k in mine.values() if k[1] == t}
            for f in sorted(expected(a.src, d, t) - have if t != "none" else ()):
                why.append(f"{f} has {t} sources but no {t} kernels in the build")
            above = sorted(s for s in c if RANK[mine[s][1]] > RANK[t])
            if above:
                why.append("above the cap: " + " ".join(above))
            for b in sorted(called):
                ok = [tt for tt in bases[b] if RANK[tt] <= RANK[t]]
                if not ok:
                    continue
                want = max(ok, key=RANK.get)
                if not c.get(bases[b][want]):
                    why.append(f"{bases[b][want]} not called")
                wrong = [bases[b][tt] for tt in ok if tt != want and c.get(bases[b][tt])]
                if wrong:
                    why.append(f"{' '.join(wrong)} called instead of {bases[b][want]}")
            if why:
                bad.append(f"{d}-bit cap {t}: " + "; ".join(why))
            else:
                print(f"dispatch {d}-bit cap {t}: PASS ({sum(c.values())} kernel calls)")
        for s, k in sorted(mine.items()):
            if k[3] in ALLOWED or k[1] not in tiers:
                continue
            if s not in refs:
                bad.append(f"{d}-bit: {s} is referenced only from the tier objects")
            elif not any(calls.get((d, t), {}).get(s) for t in tiers):
                bad.append(f"{d}-bit: {s} was never called; extend the configs or ALLOWED")

    for line in bad:
        print(f"dispatch FAIL {line}")
    if not bad:
        n = sum(k[3] not in ALLOWED and k[1] in tiers for k in ks.values())
        print(f"dispatch: all {n} kernels of the tiers asked for are reached, each at its "
              f"own tier under each cap; {len(ks) - n} others allowed or not asked for")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
