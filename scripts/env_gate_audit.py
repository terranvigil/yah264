#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Audit every lazy env-static gate in src/ for the two ways they go wrong.

The encoder resolves its ~200 Y264_* knobs through lazy function-local
statics:

    static int v = -1;
    if (v < 0) { const char *e = y264_env("Y264_FOO"); v = e ? atoi(e) : 1; }

Under GOP-parallel opens these first-touch on whichever worker gets there
first. The races are benign same-value init, but a nonzero TSan floor hides
the next real report -- the 4:4:4 MC border overflow (08-29) sat behind four
of them. Two distinct failures:

  TRAP  the resolved value still satisfies the guard, so the gate re-runs
        the env read and REWRITES its static on EVERY call. A perpetual writer;
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

# functions whose env reads land in a local each call: no shared static, so
# no race, and warming them would be meaningless.
NOT_A_GATE = {
    "auto_threads", "la_depth_for", "wf_width_for", "la_chain_prop",
    "yah264_encoder_open", "yah264_encoder_close",
    "cpu_detect_once", "mbt_oracle_init", "dpbp_open", "skor_init",
    "mb_log_open", "blate_open", "trf_open", "tr_select",
    "blk8_intra_dispatch", "warm_lr_statics", "compute_mbtree",
    "encoder_open_sw", "stair_drain", "rcr_open_env",
}

# the env read: y264_env (src/common/env.h), or a bare getenv
ENVREAD = r"(?:y264_env|getenv)"
KNOB   = re.compile(ENVREAD + r'\s*\(\s*"([^"]+)"')
GUARD  = re.compile(r"if\s*\(\s*(\w+)(?:\[\d+\])?\s*(<|<=|==|!=)\s*(-?\d+)\s*\)")
TERN   = re.compile(r"(\w+)(?:\[\d+\])?\s*=\s*\w+\s*\?\s*(.+?)\s*:\s*(.+?)\s*;", re.S)
ARRDEF = re.compile(r"\bint\s+(\w+)\[\d+\]\s*=\s*\{([^}]*)\}")
CALL   = re.compile(r"\b(\w+)\s*\(")
IDENT  = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
# words that open a statement, never a function definition
KEYWORDS = {"if", "else", "while", "for", "do", "switch", "case", "return",
            "sizeof", "typedef", "struct", "union", "enum", "goto", "default"}
# words that take parentheses in a declaration without naming it
DECL_WORDS = {"__attribute__", "__declspec", "_Alignas", "alignas", "__typeof__",
              "typeof", "_Atomic"}


def strip_comments(text):
    """The text with every comment blanked to spaces, newlines kept, so line
    numbers and columns survive. String and character literals are kept."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def functions(text):
    """(name, line, body, signature) for every function defined at column 0.

    The signature may wrap: lines are joined until its parentheses balance.
    The body's opening brace may sit on the signature's last line or on its
    own line, and a whole definition may sit on one line. Comments are
    blanked first, so a trailing comment never hides a signature. A line that
    opens with a statement keyword is never a signature.
    """
    lines = strip_comments(text).split("\n")
    i = 0
    while i < len(lines):
        ln = lines[i]
        m = re.match(r"[A-Za-z_]\w*", ln)
        if not m or m.group(0) in KEYWORDS or "(" not in ln or \
                ("=" in ln and ln.index("=") < ln.index("(")):
            i += 1
            continue
        # join the signature until its parentheses balance
        sig, j, depth, seen, after = "", i, 0, False, None
        while j < len(lines) and j < i + 12:
            for c, ch in enumerate(lines[j]):
                if ch == "(":
                    depth += 1; seen = True
                elif ch == ")":
                    depth -= 1
                    if seen and depth == 0:
                        sig += lines[j][:c + 1]
                        after = lines[j][c + 1:]
                        break
            if after is not None:
                break
            sig += lines[j] + "\n"
            j += 1
        if after is None:
            i += 1
            continue
        if after.lstrip().startswith("("):    # a function-pointer declarator
            i += 1
            continue
        if ";" in after.split("{")[0]:
            i = j + 1                         # a prototype or a declaration
            continue
        k = j
        if "{" not in after:
            k = j + 1
            while k < len(lines) and not lines[k].strip():
                k += 1
            if k >= len(lines) or not lines[k].lstrip().startswith("{"):
                i += 1
                continue
        # brace-count the body from the definition's first line
        depth, e, started = 0, i, False
        while e < len(lines):
            seg = lines[e] if e != i else lines[e]
            depth += seg.count("{") - seg.count("}")
            if seg.count("{"):
                started = True
            if started and depth <= 0 and e >= k:
                break
            e += 1
        names = [x for x in IDENT.findall(sig) if x not in DECL_WORDS and x not in KEYWORDS]
        if names:
            yield names[0], i + 1, "\n".join(lines[i:e + 1]), sig
        i = e + 1


def as_int(s):
    try:
        return int(s.strip().rstrip(";").strip())
    except ValueError:
        return None


def satisfies(val, op, sent):
    return {"<": val < sent, "<=": val <= sent,
            "==": val == sent, "!=": val != sent}.get(op)


def params_of(sig):
    """The parameter names of a joined signature, in order."""
    inner = sig[sig.index("(") + 1:sig.rindex(")")]
    out = []
    for part in inner.split(","):
        ids = re.findall(r"[A-Za-z_]\w*", part.split("[")[0])
        if ids and ids[-1] != "void":
            out.append(ids[-1])
    return out


def find_readers(defs):
    """Every function that reads the environment by name: y264_env and getenv,
    and any function that hands one of its own parameters to a reader
    (env_int, rc_tunable, env_f). Maps the name to the index of the argument
    a caller's unset value sits in (the second), or None for the base two."""
    readers = {"y264_env": None, "getenv": None}
    changed = True
    while changed:
        changed = False
        for name, _line, body, sig, _rel in defs:
            if name in readers:
                continue
            ps = params_of(sig)
            for r in list(readers):
                m = re.search(r"\b" + r + r"\s*\(\s*(\w+)\s*[,)]", body)
                if m and m.group(1) in ps:
                    readers[name] = 1 if len(ps) > 1 else None
                    changed = True
                    break
    return readers


def split_args(text):
    """Top-level comma split of a call's argument text (up to its ')')."""
    out, depth, cur = [], 0, ""
    for ch in text:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            if depth == 0:
                break
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip()); cur = ""
            continue
        cur += ch
    out.append(cur.strip())
    return out


def audit(sources):
    """sources: {relative path: text}. Returns (gate rows, {function: [bodies]})."""
    defs = []
    for rel, text in sorted(sources.items()):
        for name, line, body, sig in functions(text):
            defs.append((name, line, body, sig, rel))
    readers = find_readers(defs)
    rx_read = re.compile(r"\b(" + "|".join(map(re.escape, readers)) + r')\s*\(\s*"([^"]+)"')
    rows, allfuncs = [], {}
    for name, line, body, sig, rel in defs:
        allfuncs.setdefault(name, []).append(body)
        if name in readers and readers[name] is not None:
            continue                          # the helper itself, not a gate
        reads = rx_read.findall(body)
        knobs = [k for _r, k in reads]
        if not knobs or name in NOT_A_GATE:
            continue
        guards = GUARD.findall(body)
        if not guards:
            continue                          # per-call read, not a latch
        gname, gop, gsent = guards[0]
        gsent = int(gsent)

        unset = None
        for tn, _set, tunset in TERN.findall(body):
            if tn == gname:
                unset = as_int(tunset)
                break
        if unset is None:
            # a helper's default: v = env_int("Y264_X", D)
            for r, idx in readers.items():
                if idx is None:
                    continue
                m = re.search(r"\b" + re.escape(gname) + r"(?:\[[^\]]*\])?\s*=\s*(?:\(\w+\)\s*)?"
                              + r + r"\s*\(", body)
                if m:
                    args = split_args(body[m.end():])
                    if len(args) > idx:
                        unset = as_int(args[idx])
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

    return rows, allfuncs


def main():
    files = sorted(str(p) for p in (ROOT / "src").rglob("*.[ch]"))
    sources = {str(pathlib.Path(f).relative_to(ROOT)): pathlib.Path(f).read_text() for f in files}
    rows, _allfuncs = audit(sources)

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
