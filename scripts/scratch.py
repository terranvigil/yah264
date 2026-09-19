# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# scratch.py -- per-run scratch directories that go away again, for the python
# harnesses. The twin of scripts/scratch.sh; both read the prefix namespace
# out of scripts/scratch-prefixes.txt so there is one list, not two.
#
#     import scratch
#     work = scratch.mkdtemp("bdq")
#
# The directory is removed when the process ends: a normal return, an
# exception, a Ctrl-C, a SIGTERM from a killed agent. KEEP_SCRATCH=1 keeps it
# and prints the path instead, for a cell that failed and has to be looked at.
#
# PER-RUN SCRATCH ONLY. A cache meant to survive the run goes under
# cache_dir(), which is a named directory outside $TMPDIR -- macOS sweeps
# $TMPDIR, and a board that lost its build to that sweep cost a day.

import atexit
import os
import shutil
import signal
import sys
import tempfile

_HERE = os.path.dirname(os.path.abspath(__file__))
_PREFIXES = os.path.join(_HERE, "scratch-prefixes.txt")

_dirs = []
_armed = False


def namespace():
    """The live scratch prefix, read from the shared list."""
    try:
        with open(_PREFIXES) as f:
            for line in f:
                parts = line.split()
                if len(parts) == 2 and parts[0] == "ns":
                    return parts[1]
    except OSError:
        pass
    return "yah264-"


def cleanup():
    keep = os.environ.get("KEEP_SCRATCH", "0") == "1"
    while _dirs:
        d = _dirs.pop()
        if keep:
            if os.path.isdir(d):
                sys.stderr.write("KEEP_SCRATCH: kept %s\n" % d)
        else:
            shutil.rmtree(d, ignore_errors=True)


def release(d):
    """Drop one scratch directory early, for a loop that makes one per item."""
    if d in _dirs:
        _dirs.remove(d)
    if os.environ.get("KEEP_SCRATCH", "0") == "1":
        if os.path.isdir(d):
            sys.stderr.write("KEEP_SCRATCH: kept %s\n" % d)
        return
    shutil.rmtree(d, ignore_errors=True)


def _on_signal(signum, _frame):
    cleanup()
    try:
        signal.signal(signum, signal.SIG_DFL)
        os.kill(os.getpid(), signum)
    except Exception:
        os._exit(128 + signum)


def _arm():
    global _armed
    if _armed:
        return
    _armed = True
    atexit.register(cleanup)
    # SIGINT already unwinds through atexit via KeyboardInterrupt, but not out
    # of a worker pool that swallows it, and SIGTERM does not unwind at all.
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        try:
            signal.signal(sig, _on_signal)
        except (ValueError, OSError):
            pass                       # not the main thread; atexit still runs


def mkdtemp(tag):
    """A scratch directory named $TMPDIR/yah264-<tag>.XXXXXXXX, removed at exit."""
    d = tempfile.mkdtemp(prefix="%s%s." % (namespace(), tag))
    _dirs.append(d)
    _arm()
    return d


def cache_dir(name):
    """A NAMED, non-temp home for something that must outlive the run."""
    root = os.environ.get("Y264_CACHE",
                          os.path.join(os.path.expanduser("~"), ".cache", "yah264"))
    p = os.path.join(root, name)
    os.makedirs(p, exist_ok=True)
    return p


def df_guard(need_gb=20, what=None):
    """Refuse to start on a nearly-full temp volume.

    The disk-full symptom is not an ENOSPC message: it is a harness cell
    reading an empty reconstruction file and reporting a content failure.
    """
    d = os.environ.get("TMPDIR", "/tmp")
    try:
        st = os.statvfs(d)
    except OSError:
        return                                   # unreadable: do not block
    free_gb = st.f_bavail * st.f_frsize / (1 << 30)
    if free_gb < need_gb:
        name = what or os.path.basename(sys.argv[0])
        sys.exit("%s: only %.1f GB free on %s, need %d GB.\n"
                 "  Run 'make clean-scratch' to drop stale harness scratch, "
                 "then retry." % (name, free_gb, d, need_gb))
