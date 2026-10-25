#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""devknobs.py - refuse a release yah264 when a run sets a knob it would ignore.

A build configured with -Ddev_knobs=false reads only the short list of
CLI-carried knobs in src/common/env.h and ignores every other Y264_* knob, so
an arm that sets one encodes the default and reads as "no effect". A harness
that sets such a knob calls this first:

    python3 scripts/devknobs.py [--as <name>] <binary> [text ...]  # exit 2, saying why
    bdcompare.require_dev_knobs(binary, *texts)         # the same, in Python
    . scripts/devknobs.sh; y264_require_dev_knobs <binary> [text ...]

The texts are arm specs, env strings or command templates. The Y264_* names in
them, and in this process's environment (which the encoder inherits), count
only when they are encoder knobs (cli/knob_names.h, written by
scripts/knob_census.py) a release build doesn't read. Harness variables such
as Y264_CACHE, the CLI-carried knobs and the CLI's memory bounds never cause a
refusal.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_NAME = re.compile(r"(?<![A-Za-z0-9_])Y264_[A-Z0-9_]+")
_QUOTED = re.compile(r'"((?:Y|YAH)264_[A-Z0-9_]+)"')
_dev_only = None
_version = {}
_who = os.path.basename(sys.argv[0])


def dev_only():
    """The knob names a release build ignores."""
    global _dev_only
    if _dev_only is None:
        with open(os.path.join(ROOT, "cli", "knob_names.h")) as f:
            names = set(_QUOTED.findall(f.read()))
        with open(os.path.join(ROOT, "src", "common", "env.h")) as f:
            env = f.read()
        m = re.search(r"cli_backed\[\]\s*=\s*\{(.*?)\};", env, re.S)
        # the memory bounds the CLI reads itself in every build, which its
        # ignored-knob warning leaves out by name
        with open(os.path.join(ROOT, "cli", "yah264_cli.c")) as f:
            cli = re.findall(r'strcmp\(name, "(Y264_[A-Z0-9_]+)"\)', f.read())
        if not names or not m or not cli:
            sys.exit("devknobs: cannot read the knob lists in cli/knob_names.h, "
                     "src/common/env.h and cli/yah264_cli.c")
        _dev_only = names - set(_QUOTED.findall(m.group(1))) - set(cli)
    return _dev_only


def ignored(*texts, environ=True):
    """The release-ignored knobs named in texts (and in os.environ), sorted."""
    found = set()
    for t in texts:
        found.update(_NAME.findall(t or ""))
    if environ:
        found.update(k for k in os.environ if k.startswith("Y264_"))
    return sorted(found & dev_only())


def _version_of(binary):
    if binary not in _version:
        try:
            r = subprocess.run([binary, "--version"], capture_output=True, text=True)
        except OSError as e:
            print(f"{_who}: cannot run {binary}: {e.strerror}", file=sys.stderr)
            sys.exit(2)
        _version[binary] = r.stdout
    return _version[binary]


def is_yah264(binary):
    return _version_of(binary).startswith("yah264 ")


def is_dev_build(binary):
    return "(dev knobs)" in _version_of(binary)


def require(binary, *texts, environ=True):
    """Exit 2 when the texts or the environment set a knob `binary` ignores."""
    knobs = ignored(*texts, environ=environ)
    if knobs and not is_dev_build(binary):
        print(f"{_who}: {binary} was built without "
              f"-Ddev_knobs=true, so it ignores {', '.join(knobs)}, which this run sets; "
              "run meson configure <dir> -Ddev_knobs=true", file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    args = sys.argv[1:]
    if args[:1] == ["--as"] and len(args) > 1:
        _who, args = args[1], args[2:]
    if not args:
        sys.exit("usage: devknobs.py [--as <name>] <binary> [text ...]")
    require(args[0], *args[1:])
