#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# dispatch_probe.py -- does each SIMD tier's code actually RUN when its cap
# says it should (x86-64 builds, docs/simd.md).
#
# checkasm calls every kernel by its own name, so it says a kernel is right,
# never that the encoder reaches it. A dispatch guard that drops a tier, or a
# Y264_SIMD_FORCE name that maps to the wrong mask, leaves checkasm green and
# every tier encoding byte-identical to C -- the identity legs then compare C
# with C and pass. This probe watches the calls instead.
#
# It relinks the CLI from the build's own objects with `ld --wrap` on every
# kernel the tier archives define, at both depths. Each wrapper bumps a counter
# and jumps to the kernel, so nothing else about the binary changes. Then it
# encodes a clip per depth under each cap and requires:
#   none   no kernel of that depth is called at all (the cap takes);
#   <tier> every family that has source files at that tier and depth has
#          kernels in the build AND has at least one of them called, and no
#          kernel of a higher tier is called.
# A family is the kernel file's stem: src/dsp/x86/<family>_<tier>[_10].c.
#
# usage: dispatch_probe.py --build DIR [--clip8 Y4M] [--clip10 Y4M]
#            [--tiers "none sse4 avx2"] [--frames N] [--wrapper "CMD ..."]
# --wrapper runs the probe binary under, e.g., an explicit qemu-x86_64.
# Exit 0 when every depth and cap passes, 1 on a failure, 2 on a usage error.
import argparse, os, re, shlex, subprocess, sys

RANK = {"none": 0, "sse4": 1, "avx2": 2, "avx512": 3}
MEMBER = re.compile(r"([a-z0-9]+)_(sse4|avx2|avx512)(_10)?\.c\.o$")
KNOBS = ("Y264_SIMD_FORCE", "YAH264_NO_ASM", "Y264_ASM_OFF")
NM = os.environ.get("NM", "nm")
# nm -A: "archive:member:value type name", the value absent on U lines
NMLINE = re.compile(r"^([^:]+):([^:]+):\s*(?:[0-9a-f]+\s+)?(\S)\s+(\S+)$")


def die(msg, rc=2):
    print(f"dispatch: {msg}")
    sys.exit(rc)


def run(cmd, **kw):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, **kw)


def kernels(build):
    """{symbol: (depth, tier, family)} from the tier archives' global code"""
    out = {}
    for depth, prefix in (("8", "libyah264_x86_"), ("10", "libyah264_10_x86_")):
        for tier in ("sse4", "avx2", "avx512"):
            a = os.path.join(build, prefix + tier + ".a")
            if not os.path.exists(a):
                continue
            r = run([NM, "-A", "-g", "--defined-only", a])
            if r.returncode:
                die(f"nm failed on {a}: {r.stdout.strip()[:200]}")
            for line in r.stdout.splitlines():
                p = NMLINE.match(line)
                if not p or p.group(3) != "T":
                    continue
                m = MEMBER.search(p.group(2))
                if m:
                    out[p.group(4)] = (depth, m.group(2), m.group(1))
    return out


def expected(src, depth, tier):
    d = os.path.join(src, "src", "dsp", "x86")
    suffix = f"_{tier}_10.c" if depth == "10" else f"_{tier}.c"
    fams = set()
    for f in os.listdir(d):
        if f.endswith(suffix) and (depth == "10" or not f.endswith("_10.c")):
            fams.add(f[: -len(suffix)])
    return fams


def references(build, ks):
    """distinct kernels each depth's library reaches from outside the tier objects"""
    seen = {}
    for depth, lib in (("8", "libyah264.a"), ("10", "libyah264_10.a")):
        r = run([NM, "-A", "-u", os.path.join(build, lib)])
        n = {}
        for line in r.stdout.splitlines():
            p = NMLINE.match(line)
            if not p or p.group(4) not in ks or MEMBER.search(p.group(2)):
                continue
            k = ks[p.group(4)]
            if k[0] == depth:
                n.setdefault(k[1], set()).add(p.group(4))
        seen[depth] = {t: len(v) for t, v in n.items()}
    return seen


def link(build, ks, work):
    os.makedirs(work, exist_ok=True)
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
    cmd = [os.environ.get("CC", "cc"), "-o", exe, cli,
           os.path.join(work, "wrap.s"), os.path.join(work, "count.c"),
           "@" + os.path.join(work, "wrap.rsp"),
           "-Wl,--start-group", os.path.join(build, "libyah264.a"),
           os.path.join(build, "libyah264_10.a"), "-lm", "-Wl,--end-group", "-pthread"]
    r = run(cmd)
    if r.returncode:
        die(f"link failed: {r.stdout.strip()[:600]}")
    return exe


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--src", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap.add_argument("--clip8")
    ap.add_argument("--clip10")
    ap.add_argument("--tiers", default="none sse4 avx2")
    ap.add_argument("--frames", default="3")
    ap.add_argument("--wrapper", default="")
    ap.add_argument("--work")
    a = ap.parse_args()
    tiers = a.tiers.split()
    for t in tiers:
        if t not in RANK:
            die(f"unknown tier {t}")
    clips = [(d, c) for d, c in (("8", a.clip8), ("10", a.clip10)) if c]
    if not clips:
        die("give --clip8 and/or --clip10")
    for d, c in clips:
        if not os.path.exists(c):
            die(f"no clip {c}")
    ks = kernels(a.build)
    work = a.work or os.path.join(a.build, "dispatch-probe")
    refs = references(a.build, ks)
    for d, _ in clips:
        print(f"dispatch {d}-bit: kernels reached from outside the tier objects: " +
              (", ".join(f"{t} {n}" for t, n in sorted(refs.get(d, {}).items(),
                                                        key=lambda x: RANK[x[0]])) or "none"))
    exe = link(a.build, ks, work)
    wrap = shlex.split(a.wrapper)
    env = {k: v for k, v in os.environ.items() if k not in KNOBS}
    bad = 0
    for d, clip in clips:
        for t in tiers:
            why = []
            want = expected(a.src, d, t) if t != "none" else set()
            have = {k[2] for k in ks.values() if k[0] == d and k[1] == t}
            for f in sorted(want - have):
                why.append(f"{f} has {t} sources but no {t} kernels in the build")
            out = os.path.join(work, f"calls-{d}-{t}.txt")
            if os.path.exists(out):
                os.remove(out)
            e = dict(env, Y264_SIMD_FORCE=t, Y264_DISPATCH_OUT=out)
            r = run(wrap + [exe, "--input-y4m", clip, "--frames", a.frames, "--threads", "1",
                            "--preset", "slow", "-o", os.devnull], env=e)
            if r.returncode or not os.path.exists(out):
                why.append(f"the encode failed (rc {r.returncode}): {r.stdout.strip()[-200:]}")
            calls = {}
            if os.path.exists(out):
                for line in open(out):
                    s, n = line.split()
                    if s in ks and ks[s][0] == d:
                        k = ks[s]
                        calls[(k[1], k[2])] = calls.get((k[1], k[2]), 0) + int(n)
            higher = sorted({f"{tt}:{f}" for (tt, f) in calls if RANK[tt] > RANK[t]})
            if higher:
                why.append("kernels above the cap ran: " + " ".join(higher))
            for f in sorted(want & have):
                if not calls.get((t, f)):
                    why.append(f"no {t} {f} kernel ran")
            ran = " ".join(f"{f} {calls.get((t, f), 0)}" for f in sorted(want))
            if why:
                bad += 1
                print(f"dispatch {d}-bit cap {t}: FAIL: " + "; ".join(why))
            else:
                print(f"dispatch {d}-bit cap {t}: PASS" +
                      (f" ({t} calls: {ran})" if ran else " (no kernel called)"))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
