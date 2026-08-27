#!/usr/bin/env python3
# Copyright (c) 2026, the next264 authors
# SPDX-License-Identifier: BSD-2-Clause
"""knob_census.py - the N264_* knob census (docs/knobs.md), generated + checked.

The 2026-08-20 census round's standing instrument. Two modes:

  python3 scripts/knob_census.py            # regenerate docs/knobs.md
  python3 scripts/knob_census.py --check    # CI mode: exit 1 if the tree and
                                            # the catalog disagree, or a
                                            # default-comment contradiction
                                            # appears

WHAT IT ENFORCES
- Every env knob read in src/ + cli/ appears in docs/knobs.md (no dark knobs:
  a knob nobody can find is a knob the next session rebuilds).
- No catalog entry survives its reader (no ghost entries).
- No STALE DEFAULT COMMENT: a comment within 40 lines above a reader claiming
  "default OFF/0" while the parsed default is nonzero (or the reverse). This
  exact staleness baited a wasted rebuild once (the phase-B kernel, 08-18)
  and misdescribed two knobs after the 08-20 flip; it is the one comment
  class worth policing mechanically.

TIERS (heuristic, then hand-corrected in the OVERRIDES table below):
  default    - resolves nonzero/nonempty with no env: a shipped default; its
               escape is the env
  instrument - measurement/probe/oracle/dump machinery (name pattern or
               listed in docs/instruments.md)
  arm        - default-off env-gated feature or kept refuted arm (the flip-
               first lesson: these have re-pricing value; they stay)
"""
import os, re, sys, collections

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "docs", "knobs.md")
SRC_DIRS = ["src", "cli"]
INSTR_PAT = re.compile(
    r"(STAT|PROF|LOG|TRACE|DUMP|LED|CENSUS|PROBE|ORACLE|_REC$|_PLAY$|UNSAFE|"
    r"ETSTAT|SPLIT$|NOISE|DBG)")
GETENV = re.compile(r'getenv\("(N264_[A-Z0-9_]+)"\)')
# the common default idioms:  v = e ? atoi(e) : D;   and the guarded form
# v = e ? (atoi(e) ? 1 : 0) : D;  -- in both, D is the value after the LAST
# colon on the statement, so take the final numeric-after-colon on the line.
DEFVAL = re.compile(r':\s*(-?[0-9.]+)\s*;')
# "was default off" / "old default" are HISTORY, not claims about today
OFFCLAIM = re.compile(r'(?<!was )(?<!old )[Dd]efault(?:s)?\s+(?:is\s+|stays\s+)?'
                      r'(OFF|off|0(?![.\d])|ON|on)\b')

# hand-tier corrections where the heuristic is wrong; extend as needed
OVERRIDES = {
    "N264_MBT_REC": "instrument", "N264_MBT_PLAY": "instrument",
    "N264_REFENC_CACHE": "instrument",
    # both exist so a thread-count sweep never needs a rebuild (encoder.c
    # ~3339): a pin and a ceiling over the resolved auto budget, not a
    # default-off feature waiting to be re-priced
    "N264_AUTO_THREADS": "instrument", "N264_AUTO_THREADS_MAX": "instrument",
}

# knobs whose nearest default-claim is correct but beyond the detector:
#   SUBPEL         -1 is a "follow the preset" sentinel; the comment's
#                  "default 0 (square)" describes the resolved fallback
#   ABR_CGUARD     conditional default (ON iff the rf model is armed)
#   GPU_RANGE      the flagged "Default OFF" belongs to N264_SATDX4's block
#                  directly above (comment adjacency, not staleness)
#   PART_IMPORTANT sub-knob of the parked PART_EARLYTERM=2 mode; its numeric
#                  default is that mode's tuning constant, inert otherwise
#   PART_SLACK_X4  same block: the "PARKED, default OFF" verdict comment
#                  describes that mode, not this tuning constant (default 4)
CLAIM_OK = {"N264_SUBPEL", "N264_ABR_CGUARD", "N264_GPU_RANGE",
            "N264_PART_IMPORTANT", "N264_PART_SLACK_X4"}


def scan():
    knobs = {}
    for d in SRC_DIRS:
        for base, _, files in os.walk(os.path.join(ROOT, d)):
            if ".claude" in base:
                continue
            for f in files:
                if not f.endswith((".c", ".h", ".m")):
                    continue
                path = os.path.join(base, f)
                rel = os.path.relpath(path, ROOT)
                lines = open(path, errors="replace").read().splitlines()
                for i, ln in enumerate(lines):
                    m = GETENV.search(ln)
                    if not m:
                        continue
                    name = m.group(1)
                    dms = DEFVAL.findall(ln)
                    if not dms and i + 1 < len(lines):
                        dms = DEFVAL.findall(lines[i + 1])
                    default = dms[-1] if dms else "?"
                    # nearest default-claim comment above (stale check). The
                    # scan stops at the previous knob's getenv AND at any
                    # comment line naming a DIFFERENT knob, so a neighbour's
                    # "default OFF" is never charged to this one.
                    claim = None
                    for j in range(i - 1, max(-1, i - 40), -1):
                        others = [n for n in GETENV.findall(lines[j])
                                  if n != name]
                        named = re.findall(r"N264_[A-Z0-9_]+", lines[j])
                        if others or (named and name not in named):
                            break            # another knob's territory
                        cm = OFFCLAIM.search(lines[j])
                        if cm:
                            claim = (cm.group(1).upper(), j + 1)
                            break
                    e = knobs.setdefault(name, {"sites": [], "default": default,
                                                "claim": claim})
                    e["sites"].append(f"{rel}:{i+1}")
                    if e["default"] == "?" and default != "?":
                        e["default"] = default
    return knobs


def tier_of(name, default, instr_doc):
    if name in OVERRIDES:
        return OVERRIDES[name]
    if INSTR_PAT.search(name) or name in instr_doc:
        return "instrument"
    try:
        if float(default) != 0:
            return "default"
    except ValueError:
        pass
    return "arm"


def stale_claims(knobs):
    bad = []
    for name, e in sorted(knobs.items()):
        if name in CLAIM_OK or not e["claim"] or e["default"] == "?":
            continue
        claim, line = e["claim"]
        try:
            dv = float(e["default"])
        except ValueError:
            continue
        if claim in ("OFF", "0") and dv != 0:
            bad.append(f"{name}: comment near {e['sites'][0]} (line {line}) "
                       f"claims default {claim}, code default is {e['default']}")
        if claim == "ON" and dv == 0:
            bad.append(f"{name}: comment near {e['sites'][0]} (line {line}) "
                       f"claims default ON, code default is 0")
    return bad


def generate(knobs, instr_doc):
    tiers = collections.defaultdict(list)
    for name, e in sorted(knobs.items()):
        tiers[tier_of(name, e["default"], instr_doc)].append((name, e))
    with open(OUT, "w") as f:
        f.write("# The N264_* knob census (generated -- edit "
                "scripts/knob_census.py OVERRIDES, not this file)\n\n"
                "Regenerate: `python3 scripts/knob_census.py`. CI check: "
                "`--check`.\n\nTier meanings: **default** = shipped behaviour, "
                "the env is its escape; **instrument** = measurement machinery "
                "(catalogued in docs/instruments.md when it has produced a "
                "result); **arm** = default-off gate or kept refuted arm -- "
                "kept deliberately, see docs/archive/goal3-flip-first.md for why "
                "refuted arms retain re-pricing value.\n")
        for tier in ("default", "instrument", "arm"):
            f.write(f"\n## {tier} ({len(tiers[tier])})\n\n")
            f.write("| knob | default | reader |\n|---|---|---|\n")
            for name, e in tiers[tier]:
                f.write(f"| `{name}` | {e['default']} | {e['sites'][0]} |\n")
    print(f"wrote {OUT}: " + ", ".join(
        f"{t} {len(tiers[t])}" for t in ("default", "instrument", "arm")))


def main():
    instr_doc = set()
    ipath = os.path.join(ROOT, "docs", "instruments.md")
    if os.path.exists(ipath):
        instr_doc = set(re.findall(r"N264_[A-Z0-9_]+", open(ipath).read()))
    knobs = scan()
    bad = stale_claims(knobs)
    if "--check" in sys.argv:
        rc = 0
        if os.path.exists(OUT):
            cat = set(re.findall(r"`(N264_[A-Z0-9_]+)`", open(OUT).read()))
            dark = sorted(set(knobs) - cat)
            ghost = sorted(cat - set(knobs))
            if dark:
                print("UNCATALOGUED (regenerate docs/knobs.md):", *dark); rc = 1
            if ghost:
                print("CATALOG GHOSTS (reader removed):", *ghost); rc = 1
        else:
            print("docs/knobs.md missing"); rc = 1
        for b in bad:
            print("STALE DEFAULT COMMENT:", b); rc = 1
        sys.exit(rc)
    generate(knobs, instr_doc)
    for b in bad:
        print("STALE DEFAULT COMMENT:", b)


if __name__ == "__main__":
    main()
