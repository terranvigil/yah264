#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: BSD-2-Clause
"""Audit every lazy env-static gate in src/ for the two ways they go wrong.

The encoder resolves its ~200 Y264_* knobs through lazy function-local
statics:

    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_FOO"); v = e ? atoi(e) : 1; }

Under GOP-parallel opens these first-touch on whichever worker gets there
first. The races are benign same-value init, but a nonzero TSan floor hides
the next real report -- the 4:4:4 MC border overflow (08-29) sat behind four
of them. Two distinct failures:

  TRAP  the resolved value still satisfies the guard, so the gate re-runs
        getenv and REWRITES its static on EVERY call. A perpetual writer;
        no warm can quiet it. (Y264_WF_CAPK resolved "unset" to -2 under a
        `v < 0` guard and did exactly this.)
  COLD  latches correctly, but nothing resolves it on the main thread before
        the pool exists, so first touch races.

Warmed means reachable from warm_lr_statics, ntp_pool_create or
ntp_bg_create. A warm can only reach a gate through a CALLABLE function: a
static resolved inline inside a hot function is unreachable by construction
and has to be hoisted into an accessor first.

    python3 scripts/env_gate_audit.py            # cold + trap gates
    python3 scripts/env_gate_audit.py --all      # every gate, with verdicts
    python3 scripts/env_gate_audit.py --json out.json

Exits nonzero if any TRAP is found. Run it after adding a knob.
"""
import re, sys, json, pathlib, subprocess

ROOT = pathlib.Path(__file__).resolve().parent.parent
WARM_ROOTS = ["warm_lr_statics", "ntp_pool_create", "ntp_bg_create"]

# functions whose getenv reads into a local each call: no shared static, so
# no race, and warming them would be meaningless.
NOT_A_GATE = {
    "auto_threads", "la_depth_for", "wf_width_for", "la_chain_prop",
    "rc_set_qp", "yah264_encoder_open", "yah264_encoder_close",
    "cpu_detect_once", "mbt_oracle_init", "dpbp_open", "skor_init",
    "mb_log_open", "blate_open", "trf_open", "tr_select",
    "crf_aqabs_forced", "blk8_intra_dispatch", "warm_lr_statics",
}

FUNC   = re.compile(r"^(?:static\s+)?[A-Za-z_][\w \t*]*?\b(\w+)\s*\([^;]*?\)\s*$", re.M)
KNOB   = re.compile(r'getenv\s*\(\s*"([^"]+)"')
GUARD  = re.compile(r"if\s*\(\s*(\w+)(?:\[\d+\])?\s*(<|<=|==|!=)\s*(-?\d+)\s*\)")
TERN   = re.compile(r"(\w+)(?:\[\d+\])?\s*=\s*\w+\s*\?\s*(.+?)\s*:\s*(.+?)\s*;", re.S)
ARRDEF = re.compile(r"\bint\s+(\w+)\[\d+\]\s*=\s*\{([^}]*)\}")
CALL   = re.compile(r"\b(\w+)\s*\(")


def functions(text):
    """(name, line, body) for every function defined at column 0.

    Brace-counted, so a one-line body is missed -- see the `{` check. That is
    deliberate: every gate in this tree is written multi-line.
    """
    lines = text.split("\n")
    i = 0
    while i < len(lines):
        m = FUNC.match(lines[i])
        if m and i + 1 < len(lines) and lines[i + 1].strip() == "{":
            depth, j, started = 0, i + 1, False
            while j < len(lines):
                depth += lines[j].count("{") - lines[j].count("}")
                if lines[j].count("{"):
                    started = True
                if started and depth <= 0:
                    break
                j += 1
            yield m.group(1), i + 1, "\n".join(lines[i:j + 1])
            i = j + 1
            continue
        # one-liner: `void f(void) { ... }`
        if m is None and "{" in lines[i] and "}" in lines[i] and "getenv" not in lines[i]:
            m2 = re.match(r"^(?:static\s+)?[\w ]+?\b(\w+)\s*\([^;]*?\)\s*\{", lines[i])
            if m2:
                yield m2.group(1), i + 1, lines[i]
        i += 1


def as_int(s):
    try:
        return int(s.strip().rstrip(";").strip())
    except ValueError:
        return None


def satisfies(val, op, sent):
    return {"<": val < sent, "<=": val <= sent,
            "==": val == sent, "!=": val != sent}.get(op)


def main():
    files = sorted(subprocess.run(["grep", "-rl", "getenv", str(ROOT / "src")],
                                  capture_output=True, text=True).stdout.split())
    rows, allfuncs = [], {}
    for f in files:
        text = pathlib.Path(f).read_text()
        rel = str(pathlib.Path(f).relative_to(ROOT))
        for name, line, body in functions(text):
            allfuncs.setdefault(name, []).append(body)
            knobs = KNOB.findall(body)
            if not knobs or name in NOT_A_GATE:
                continue
            guards = GUARD.findall(body)
            if not guards:
                continue                      # per-call read, not a latch
            gname, gop, gsent = guards[0]
            gsent = int(gsent)

            unset = None
            for tn, _set, tunset in TERN.findall(body):
                if tn == gname:
                    unset = as_int(tunset)
                    break
            if unset is None:
                # the resolve branch's default array, e.g. int p[3] = { 75, .. }
                stripped = re.sub(r"static\s+[^;]*;", "", body)
                for _an, vals in ARRDEF.findall(stripped):
                    unset = as_int(vals.split(",")[0])
                    if unset is not None:
                        break

            if unset is None:
                v, why = "latches?", f"guard `{gname} {gop} {gsent}`, unset value not statically readable"
            elif satisfies(unset, gop, gsent):
                v, why = "TRAP", (f"unset resolves {gname}={unset}, which still satisfies "
                                  f"`{gname} {gop} {gsent}` -> rewrites its static every call")
            else:
                v, why = "latches", f"unset {gname}={unset} clears `{gname} {gop} {gsent}`"
            rows.append(dict(file=rel, line=line, name=name, knobs=knobs,
                             verdict=v, why=why))

    warmed, frontier = set(), list(WARM_ROOTS)
    while frontier:
        fn = frontier.pop()
        if fn in warmed:
            continue
        warmed.add(fn)
        for b in allfuncs.get(fn, []):
            frontier += [c for c in CALL.findall(b) if c in allfuncs and c not in warmed]
    for r in rows:
        r["warm"] = r["name"] in warmed

    if "--json" in sys.argv:
        json.dump(rows, open(sys.argv[sys.argv.index("--json") + 1], "w"), indent=1)

    show_all = "--all" in sys.argv
    traps = [r for r in rows if r["verdict"] == "TRAP"]
    cold = [r for r in rows if not r["warm"] and r["verdict"] != "TRAP"]

    print(f"{len(rows)} lazy env-static gates; {len(traps)} TRAP, {len(cold)} COLD, "
          f"{sum(r['warm'] for r in rows)} warmed")
    for label, group in (("TRAP -- perpetual writer, no warm can quiet it", traps),
                         ("COLD -- latches, but first touch races", cold),
                         ("all gates", rows if show_all else [])):
        if not group:
            continue
        print(f"\n=== {label} ===")
        for r in sorted(group, key=lambda r: (r["file"], r["line"])):
            mark = "" if r["warm"] else "  [cold]"
            print(f"  {r['file']}:{r['line']}  {r['name']}  {','.join(r['knobs'])}{mark}")
            if r["verdict"] == "TRAP":
                print(f"      {r['why']}")
    return 1 if traps else 0


if __name__ == "__main__":
    sys.exit(main())
