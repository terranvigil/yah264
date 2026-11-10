#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Audit every lazy env-static gate in src/ for the two ways they go wrong.

The encoder resolves many of its Y264_* knobs (docs/knobs.md counts them)
through lazy statics:

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
    python3 scripts/env_gate_audit.py --self-test   # every gate shape, synthetic

Exits nonzero if any TRAP is found. Run it after adding a knob.
"""
import re, sys, json, pathlib, subprocess

ROOT = pathlib.Path(__file__).resolve().parent.parent
WARM_ROOTS = ["warm_lr_statics", "ntp_pool_create", "ntp_bg_create"]


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
    return {"<": val < sent, "<=": val <= sent, ">": val > sent, ">=": val >= sent,
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


def balanced(text, start):
    """Index just past the bracket that closes the one at text[start]."""
    pairs = {"(": ")", "{": "}", "[": "]"}
    want, depth = pairs[text[start]], 0
    for i in range(start, len(text)):
        if text[i] == text[start]:
            depth += 1
        elif text[i] == want:
            depth -= 1
            if depth == 0:
                return i + 1
    return len(text)


def num(tok, macros):
    """An integer constant, a same-file #define of one, or None."""
    tok = tok.strip()
    for _ in range(4):
        m = re.fullmatch(r"\(\s*(.+?)\s*\)", tok)
        if m:
            tok = m.group(1)
            continue
        if tok in macros:
            tok = macros[tok].strip()
            continue
        break
    v = as_int(tok)
    if v is None:
        try:
            v = float(tok)
        except ValueError:
            return None
    return v


CMP = re.compile(r"(?<![\w.>])([A-Za-z_]\w*)(?:\[[^\]]*\])?\s*(<=|>=|==|!=|<|>)\s*(\(?-?\w+\)?)")
NOT = re.compile(r"!\s*([A-Za-z_]\w*)\b(?!\s*[(\[.])")
NEG = {"<": ">=", "<=": ">", ">": "<=", ">=": "<", "==": "!=", "!=": "=="}


def unset_of(var, region, readers, macros):
    """The value `var` holds after `region` runs with the knob unset: the
    first assignment to it, read as its ternary's unset branch, a helper's
    default argument, or a constant."""
    m = re.search(r"\b" + re.escape(var) + r"(?:\[[^\]]*\])?\s*=(?!=)\s*([^;]+);", region)
    if not m:
        return None
    rhs = m.group(1).strip()
    rhs = re.sub(r"^\(\s*\w+\s*\)\s*", "", rhs)          # a leading cast
    depth = 0
    for i, ch in enumerate(rhs):                         # top-level ternary
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        elif ch == ":" and depth == 0 and "?" in rhs[:i]:
            return num(rhs[i + 1:], macros)
    for r, idx in readers.items():
        mm = re.match(re.escape(r) + r"\s*\(", rhs)
        if mm and idx is not None:
            args = split_args(rhs[mm.end():])
            return num(args[idx], macros) if len(args) > idx else None
    return num(rhs, macros)


def declared(decl):
    """The names a declaration's text (between 'static' and ';') declares."""
    out = []
    for part in split_args(decl + ")"):
        part = part.split("=")[0].split("[")[0]
        ids = re.findall(r"[A-Za-z_]\w*", part)
        if ids:
            out.append(ids[-1])
    return out


def file_statics(text):
    """Names declared at file scope as static or atomic."""
    out = set()
    text = strip_comments(text)
    for m in re.finditer(r"^(?:static|_Atomic|atomic_\w+)\b", text, re.M):
        d = decl_at(text, m.end())
        if "(" not in d.split("=")[0]:         # a variable, not a function
            out.update(declared(d))
    return out


def decl_at(text, start):
    """The declaration text from `start` to its ';', a brace initialiser
    (`= { -1, -1 }`) skipped whole."""
    i, depth = start, 0
    while i < len(text):
        ch = text[i]
        if ch in "({[":
            depth += 1
        elif ch in ")}]":
            depth -= 1
        elif ch == ";" and depth <= 0:
            return text[start:i]
        i += 1
    return text[start:]


def body_statics(body):
    """Names a body declares static, plus locals loaded from a static atomic."""
    out = set()
    inner = body[body.find("{") + 1:]
    for m in re.finditer(r"\bstatic\b", inner):
        out.update(declared(decl_at(inner, m.end())))
    return out


def find_guard(body, rx_read, readers, macros, statics):
    """(var, op, sentinel, unset) of the latch guarding the body's first env
    read, or None when the read is not behind one. A guard is an `if` whose
    condition compares a variable (`v < 0`, `v == UNSET`, `!done`, either
    side of a compound) and whose statement holds the read, or an early
    `if (...) return` ahead of it, read negated."""
    rd = rx_read.search(body)
    if not rd:
        return None
    statics = set(statics) | body_statics(body)
    for m in re.finditer(r"\b(\w+)\s*=\s*atomic_load\w*\s*\(\s*&\s*(\w+)", body):
        if m.group(2) in statics:
            statics.add(m.group(1))           # a local copy of a static atomic
    pos, best = rd.start(), None
    for m in re.finditer(r"\bif\s*\(", body):
        if m.start() >= pos:
            break
        c0 = m.end() - 1
        c1 = balanced(body, c0)
        cond = body[c0 + 1:c1 - 1]
        rest = body[c1:]
        st = len(rest) - len(rest.lstrip())
        if rest.lstrip().startswith("{"):
            s1 = c1 + balanced(rest, st)
        else:
            s1 = c1 + rest.find(";") + 1
        early = rest.lstrip().startswith("return")
        if not (c1 <= pos < s1) and not (early and s1 <= pos):
            continue
        cmps = [(v, op, t) for v, op, t in CMP.findall(cond)]
        cmps += [(v, "==", "0") for v in NOT.findall(cond)]
        cmps = [c for c in cmps if c[0] in statics]     # a latch keeps its value
        if not cmps:
            continue
        var, op, tok = cmps[0]
        if early:
            op = NEG[op]
        region = body[c1:s1] if not early else body[s1:]
        best = (var, op, num(tok, macros), unset_of(var, region, readers, macros))
    return best


def audit(sources):
    """sources: {relative path: text}. Returns (gate rows, {function: [bodies]})."""
    defs = []
    for rel, text in sorted(sources.items()):
        for name, line, body, sig in functions(text):
            defs.append((name, line, body, sig, rel))
    readers = find_readers(defs)
    fstatics = {rel: file_statics(text) for rel, text in sources.items()}
    macros = {rel: {m.group(1): m.group(2) for m in
                    re.finditer(r"^[ \t]*#[ \t]*define[ \t]+(\w+)[ \t]+(.+?)[ \t]*$", text, re.M)}
              for rel, text in sources.items()}
    rx_read = re.compile(r"\b(" + "|".join(map(re.escape, readers)) + r')\s*\(\s*"([^"]+)"')
    rows, allfuncs = [], {}
    for name, line, body, sig, rel in defs:
        allfuncs.setdefault(name, []).append(body)
        if name in readers and readers[name] is not None:
            continue                          # the helper itself, not a gate
        reads = rx_read.findall(body)
        knobs = [k for _r, k in reads]
        if not knobs:
            continue
        g = find_guard(body, rx_read, readers, macros.get(rel, {}), fstatics[rel])
        if g is None:
            continue                          # per-call read, not a latch
        gname, gop, gsent, unset = g
        if unset is None:
            # the resolve branch's default array, e.g. int p[3] = { 75, .. }
            stripped = re.sub(r"static\s+[^;]*;", "", body)
            for _an, vals in ARRDEF.findall(stripped):
                unset = as_int(vals.split(",")[0])
                if unset is not None:
                    break

        if unset is None or gsent is None:
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


# --self-test: one case per gate shape the audit has to read. Each knob maps
# to (verdict, warmed); a knob left out (SYN_LOCAL) must not read as a gate.
SELF_TEST_SRC = r"""
#include <stdlib.h>
#include "env.h"
static int env_int(const char *name, int dflt)
{
    const char *e = y264_env(name);
    return e ? atoi(e) : dflt;
}
/* 1: gate through a helper, never warmed */
static int g_helper(void) { static int v = -1; if (v < 0) v = env_int("SYN_HELPER", 0); return v; }

/* 2: macro sentinel, never warmed */
#define UNSET (-1000)
static int g_macro(void)
{
    static int v = UNSET;
    if (v == UNSET) { const char *s = y264_env("SYN_MACRO"); v = s ? atoi(s) : 0; }
    return v;
}

/* 3: trailing comment on the signature, never warmed */
static int g_comment(void)          /* warmed somewhere */
{
    static int v = -1;
    if (v < 0) { const char *s = y264_env("SYN_COMMENT"); v = s ? atoi(s) : 0; }
    return v;
}

/* 4: boolean latch, never warmed */
static int g_bool(void)
{
    static int done, v;
    if (!done) { const char *s = y264_env("SYN_BOOL"); v = s ? atoi(s) : 0; done = 1; }
    return v;
}

/* 5: multi-line signature, never warmed */
static int g_wrapped(int a,
                     int b)
{
    static int v = -1;
    if (v < 0) { const char *s = y264_env("SYN_WRAP"); v = s ? atoi(s) : 0; }
    return v + a + b;
}

/* 6: brace on the signature line, never warmed */
static int g_knr(void) {
    static int v = -1;
    if (v < 0) { const char *s = y264_env("SYN_KNR"); v = s ? atoi(s) : 0; }
    return v;
}

/* 7: TRAP through the helper: unset resolves -2 under v < 0 */
static int g_trap_helper(void)
{
    static int v = -1;
    if (v < 0) v = env_int("SYN_TRAPH", -2);
    return v;
}

/* 8: compound guard, never warmed */
static int g_compound(int on)
{
    static int v = -1;
    if (v < 0 && on) { const char *s = y264_env("SYN_COMPOUND"); v = s ? atoi(s) : 0; }
    return v;
}

/* 9: a cold gate, called only from a one-line if in a hot function */
static int g_cold_inif(void)
{
    static int v = -1;
    if (v < 0) { const char *s = y264_env("SYN_INIF"); v = s ? atoi(s) : 0; }
    return v;
}
static void hot_worker(int *x,
                       int y)
{
    if (*x > 0) { if (g_cold_inif()) x[1]++; }
}

/* 10: inline static in a hot function, written on one indented line */
static void hot_inline(int *x)
{
    static int v = -1;
    if (v < 0) { const char *s = y264_env("SYN_INLINE"); v = s ? atoi(s) : 0; }
    x[0] += v;
}

/* 12: a guard on a local is a per-call read, not a gate */
static int g_local(int t)
{
    int x = t;
    if (x < 0) { const char *s = y264_env("SYN_LOCAL"); x = s ? atoi(s) : 0; }
    return x;
}

/* 13: a static atomic read through a local copy, never warmed */
static int g_atomic(void)
{
    static _Atomic int v = -1;
    int x = atomic_load_explicit(&v, memory_order_relaxed);
    if (x < 0) {
        const char *e = y264_env("SYN_ATOMIC");
        x = e ? atoi(e) : 0;
        atomic_store_explicit(&v, x, memory_order_relaxed);
    }
    return x;
}

/* 14: an array latch with a brace initialiser, never warmed */
static double g_arr(int which)
{
    static double v[3] = { -1.0, -1.0, -1.0 };
    if (v[0] < 0) {
        const char *s = y264_env("SYN_ARR");
        v[0] = s ? atof(s) : 6.0; v[1] = 1.0; v[2] = 2.0;
    }
    return v[which];
}

/* 11: early return ahead of the read, warmed */
static int g_early_n = -2;
static void g_early(void)
{
    if (g_early_n != -2) return;
    g_early_n = -1;
    const char *p = y264_env("SYN_EARLY");
    if (p) g_early_n = atoi(p);
}

static void warm_lr_statics(void)
{
    int n = 0;
    if (n == 0) { n = 1; }
    g_early();
}
"""
SELF_TEST_WANT = {
    "SYN_HELPER": ("latches", False), "SYN_MACRO": ("latches", False),
    "SYN_COMMENT": ("latches", False), "SYN_BOOL": ("latches", False),
    "SYN_WRAP": ("latches", False), "SYN_KNR": ("latches", False),
    "SYN_TRAPH": ("TRAP", False), "SYN_COMPOUND": ("latches", False),
    "SYN_INIF": ("latches", False), "SYN_INLINE": ("latches", False),
    "SYN_EARLY": ("latches", True), "SYN_ATOMIC": ("latches", False),
    "SYN_ARR": ("latches", False),
}


def self_test():
    rows, funcs = audit({"src/synth.c": SELF_TEST_SRC})
    got = {}
    for r in rows:
        for k in r["knobs"]:
            got[k] = (r["verdict"], r["warm"])
    bad = []
    for k, want in SELF_TEST_WANT.items():
        if got.get(k) != want:
            bad.append(f"{k}: want {want}, got {got.get(k)}")
    for k in set(got) - set(SELF_TEST_WANT):
        bad.append(f"{k}: not a gate, read as {got[k]}")
    fake = sorted(set(funcs) & KEYWORDS)
    if fake:
        bad.append(f"statement keywords read as functions: {fake}")
    for b in bad:
        print("env_gate_audit --self-test: FAIL " + b)
    print(f"env_gate_audit --self-test: {len(SELF_TEST_WANT) - len(bad)}/{len(SELF_TEST_WANT)} "
          f"cases {'PASS' if not bad else 'FAIL'}")
    return 1 if bad else 0


def main():
    if "--self-test" in sys.argv:
        return self_test()
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
