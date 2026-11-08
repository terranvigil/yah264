#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""CRF or VBR ladders on a 10-bit HDR10 (PQ, BT.2020, limited range) Y4M,
yah264 against x264 High 10, scored with VMAF v1's HDR model and PSNR on the
10-bit code values, then BD-rate over the quality range both ladders cover.

  scripts/hdr_ladder.py SRC.y4m [--crf 18,22,26,30,34 [--max-crf 30] | --vbr K1,K2,...]
                        [--yah-lookahead N] [--x264-lookahead N]
                        [--encoders x264,yah264] [--threads 3] [--work DIR]
                        [--fresh]

Both encoders run at medium, with the stream tagged HDR10 (BT.2020 primaries
and matrix, PQ transfer, limited range). The source must be a progressive,
limited range 4:2:0 10-bit Y4M (C420p10) of any even size. The model follows
the source: 1d5h_2160 when its height is above 1080 (so 1440p and 3840x1600
score on it), 3d0h otherwise (1920x1080, a 1920x800 scope picture), and the
hfr variant of either at 50 fps and up. A source smaller than 1920x1080 in
both dimensions is scored at display size, as the project's VMAF v1 rule
says: the reference and the decode are both scaled up to fit 1920x1080
(bicubic, aspect kept) and CAMBI is told the coded size. PSNR is always
taken at the coded size. The decode and any scaled picture are streamed to
the scorer, so nothing frame-sized lands on disk; a step that fails or
stalls for 10 minutes stops the run with its own stderr. Every decode must
be clean and hold exactly the source's frame count, or the run stops. Rungs
must be distinct, and a metric whose quality doesn't rise with bitrate
across a ladder gets no BD figure. A rung line shows how many frames sit at
the VMAF model's clip bounds, and a ladder where more than 5% of any rung's
frames do gets no VMAF BD figure. Read the printed rungs before trusting a
BD figure; --max-crf leaves out the rungs above it, for a second read.

--vbr runs one rung per target K kbit/s instead of the CRF ladder, with
--bitrate K, --vbv-maxrate 1.5K, --vbv-bufsize 2K and --vbv-init 0.9 on
both encoders (0.9 is x264's own default; yah264 defaults to full, so
it is passed to both). Each rung prints its achieved rate against the
target, flagged when it misses the parity spec's +-10% tracking bar (the
spec judges that bar on real-title cells), and the verdict of
scripts/vbv_check.py on the stream from the same starting fill. The
leaky-bucket check only bites over a span much longer than the buffer: on a
clip of a few seconds the buffer starts nearly full and almost any stream
passes, so treat it as a sanity check there. The spec takes the VBR
targets from the reference's achieved rates at CRF 20/23/26/29/32 on the
same clip; get them with --encoders x264 --crf 20,23,26,29,32.

--threads defaults to 3, a share of the shared box; the spec runs its
ladders at 6 threads, so pass --threads 6 for a reading that matches it.

Both encoders run their preset's lookahead depth unless --yah-lookahead or
--x264-lookahead passes --rc-lookahead to that side. --x264-lookahead takes
0 to 250, and the depth printed for x264 is the one its stream's options SEI
records, which is what it ran with (0 when the SEI says mb-tree was off and
gives no depth). yah264 caps its window at 64 frames (read from its --help,
like its default, with a printed warning and 64 assumed for a build whose
help doesn't say), so a deeper --yah-lookahead is refused rather than
printed as run. CAMBI is the pooled mean of the banding feature the
HDR model computes in the same run.

Each rung's result is cached in the work directory under two keys. The
stream's key is the encoder binary and the non-system libraries it links,
its full command line and, for yah264, every Y264_* and YAH264_* variable
set, and the source's size, mtime, ctime, inode and sampled md5; a change to
any of them re-encodes. The score's key is the scorer chain: the vmaf and
ffmpeg binaries, their libraries and versions, the model file, the display
scaling, this script's own md5 and, for VBR, vbv_check.py; a change there
rescores the kept stream (printed "rescored") as long as its md5 is the one
recorded. A reused rung prints "cached". RESULT_VERSION versions the score
side of the cached JSON, so bumping it rescores without re-encoding. Each
binary runs by the path that was hashed, and is hashed again after every
rung it took part in; a run stops if one changed underneath it. --fresh
ignores the cache. A run holds a lock on its work directory, and a second
run on the same directory stops at once. SIGTERM, SIGHUP, SIGINT or SIGQUIT
kills every child and removes the run's scratch at once, and the run then
dies of that signal; a signal that was ignored when the run started (nohup)
stays ignored. ^Z and fg stop and resume the children with the run. YAH264,
X264, VMAF_HDR and ffmpeg must resolve to Mach-O or ELF executables, since
the key hashes the file itself; HDR_LADDER_ALLOW_SCRIPTS=1 lets a test
harness pass wrapper scripts, with no cached rung reused.

  YAH264    yah264 CLI (default build/cli/yah264)
  X264      x264 CLI (default x264)
  VMAF_HDR  a vmaf CLI built with the HDR models' colour options (checked
            at start: a build without them scores PQ as SDR and exits 0)
  VMAF_HDR_MODELS  the directory holding hdr_vmaf_v1.1.8[_hfr]/
"""
import signal
# until the full handlers below are in place no child exists, so ^C may end
# the run the plain way: by the signal, with no traceback from an import
if signal.getsignal(signal.SIGINT) is signal.default_int_handler:
    signal.signal(signal.SIGINT, signal.SIG_DFL)
import argparse, contextlib, fcntl, glob, hashlib, json, math, os, re, shutil, signal, subprocess, sys, \
    tempfile, threading, time

# SIGTERM, SIGHUP, SIGINT and SIGQUIT end the run at once: the handler ignores
# any further one, stops and SIGKILLs every live child and everything under
# it (found by parent pid, so a wrapper script's binary dies too), removes
# the registered scratch and then dies of the same signal, so a shell loop
# around the run sees it. It never raises, because a raise lands at an
# arbitrary bytecode, and inside the standard library that can leave a lock
# held or cut a cleanup short. Children stay in the run's process group, so
# a terminal's ^C, ^Z and fg reach them too, and a kill of the whole group
# takes them with it. The only window the handler can't see into is a child
# that exists but isn't yet registered; a signal there is held and acted on
# right after registering.
_CHILDREN = {}         # pid -> Child, while it hasn't been reaped
_SCRATCH = set()       # paths to remove on a signal
_HANDLED = []          # the signals the handler is installed for
_HOLD = [0]
_PENDING = []
_PROGRESS = [time.monotonic()]   # the stall timer's clock, reset on SIGCONT


def _child_pids(pid):
    """The direct children of pid, from the kernel's process table."""
    if sys.platform == "darwin":
        import ctypes
        lib = ctypes.CDLL("/usr/lib/libproc.dylib")
        buf = (ctypes.c_int * 4096)()
        n = lib.proc_listchildpids(pid, buf, ctypes.sizeof(buf))
        return [buf[i] for i in range(max(n, 0)) if buf[i] > 0]
    kids = []
    for d in os.listdir("/proc"):
        if d.isdigit():
            try:
                with open(f"/proc/{d}/stat") as f:
                    if int(f.read().rsplit(")", 1)[1].split()[1]) == pid:
                        kids.append(int(d))
            except (OSError, ValueError, IndexError):
                pass
    return kids


def _tree(roots):
    """roots and every process under them."""
    seen, todo = [], list(roots)
    while todo:
        p = todo.pop()
        if p in seen:
            continue
        seen.append(p)
        try:
            todo += _child_pids(p)
        except OSError:
            pass
    return seen


def _signal_tree(roots, sig):
    for p in _tree(roots):
        try:
            os.kill(p, sig)
        except OSError:
            pass


def _kill_tree(roots):
    # stop the tree first and walk it again, so nothing forks away between
    # the walk and the kill
    stopped = set()
    while True:
        new = [p for p in _tree(roots) if p not in stopped]
        if not new:
            break
        for p in new:
            try:
                os.kill(p, signal.SIGSTOP)
            except OSError:
                pass
            stopped.add(p)
    for p in stopped:
        try:
            os.kill(p, signal.SIGKILL)
        except OSError:
            pass


def _die(num):
    for sig in _HANDLED:
        signal.signal(sig, signal.SIG_IGN)
    _kill_tree(list(_CHILDREN))
    for path in list(_SCRATCH):
        try:
            if os.path.isdir(path) and not os.path.islink(path):
                shutil.rmtree(path, ignore_errors=True)
            else:
                os.remove(path)
        except OSError:
            pass
    try:
        os.write(2, f"hdr_ladder: stopped by signal {num}\n".encode())
    except OSError:
        pass
    # die of the signal itself, so a caller (a shell loop) sees which one
    signal.signal(num, signal.SIG_DFL)
    os.kill(os.getpid(), num)
    os._exit(128 + num)


def _on_signal(num, frame):
    if _HOLD[0]:
        _PENDING.append(num)
        return
    _die(num)


def _on_tstp(num, frame):
    # a ^Z sent to this pid alone stops the children as well, and fg (or
    # SIGCONT) resumes them
    _signal_tree(list(_CHILDREN), signal.SIGSTOP)
    # SIGSTOP, not SIGTSTP: the kernel drops a SIGTSTP aimed at an orphaned
    # process group, and a run started from a script often is one
    os.kill(os.getpid(), signal.SIGSTOP)
    _on_cont(signal.SIGCONT, frame)


def _on_cont(num, frame):
    # time spent stopped isn't a stall
    _PROGRESS[0] = time.monotonic()
    _signal_tree(list(_CHILDREN), signal.SIGCONT)


def _install_handlers():
    # a signal ignored at start (nohup, a background job) stays ignored,
    # here and in every child
    for sig in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT, signal.SIGQUIT):
        if signal.getsignal(sig) is not signal.SIG_IGN:
            _HANDLED.append(sig)
            signal.signal(sig, _on_signal)
    if signal.getsignal(signal.SIGTSTP) is not signal.SIG_IGN:
        signal.signal(signal.SIGTSTP, _on_tstp)
    signal.signal(signal.SIGCONT, _on_cont)


@contextlib.contextmanager
def held():
    _HOLD[0] += 1
    try:
        yield
    finally:
        _HOLD[0] -= 1
        if not _HOLD[0] and _PENDING:
            _die(_PENDING[0])


class Child(subprocess.Popen):
    """A Popen that leaves the registry once it has been reaped, so a later
    signal never aims at a pid that may have been reused."""

    def poll(self):
        rc = super().poll()
        if rc is not None:
            _CHILDREN.pop(self.pid, None)
        return rc

    def wait(self, timeout=None):
        rc = super().wait(timeout)
        _CHILDREN.pop(self.pid, None)
        return rc


def spawn_child(cmd, **kw):
    with held():
        p = Child(cmd, **kw)
        _CHILDREN[p.pid] = p
    return p


def kill_child(p):
    """SIGKILL a live child and everything under it."""
    if p.poll() is None:
        _kill_tree([p.pid])


def scratch(path):
    with held():
        _SCRATCH.add(path)
    return path


def unscratch(path):
    _SCRATCH.discard(path)


def capture(cmd, **kw):
    """Run cmd as a registered child and return its CompletedProcess."""
    p = spawn_child(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, **kw)
    try:
        so, se = p.communicate()
    finally:
        kill_child(p)
        p.wait()
    return subprocess.CompletedProcess(p.args, p.returncode, so, se)


def run(cmd, ok=(0,)):
    r = capture(["nice", "-n", "10"] + cmd)
    if r.returncode not in ok:
        sys.exit(f"hdr_ladder: failed: {' '.join(cmd)}\n{r.stderr[-1500:]}")
    return r


_install_handlers()
# line-buffered, so what was printed survives an exit that skips the flush
sys.stdout.reconfigure(line_buffering=True)


sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bdcompare import bd_rate, v1_scaled, V1_SCALE_VF          # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
YAH = os.environ.get("YAH264", os.path.join(ROOT, "build", "cli", "yah264"))
X264 = os.environ.get("X264", "x264")
VMAF = os.environ.get("VMAF_HDR")
FFMPEG = "ffmpeg"
MODELS = os.environ.get("VMAF_HDR_MODELS")
VBV_CHECK = os.path.join(ROOT, "scripts", "vbv_check.py")


# x264 takes up to 250 frames of lookahead
MAX_LOOKAHEAD = 250
# yah264's medium default and its cap, used only when its --help doesn't
# state them (it prints both on the --rc-lookahead line)
YAH_DEFAULT_LOOKAHEAD = 40
YAH_MAX_LOOKAHEAD = 64
VBV_INIT = 0.9
RATE_BAR = 10.0
# a rung with more of its frames than this at the VMAF model's clip bounds
# gets no VMAF BD; one clamped frame in a long rung is normal at CRF 18
CLAMP_SHARE = 0.05
# bump when the cached JSON's layout changes; a change to how a rung is
# measured is caught by the script's md5 in the key
RESULT_VERSION = 6
KNOB_PREFIXES = ("Y264_", "YAH264_")
LOCK = None


def y4m_info(path):
    with open(path, "rb") as f:
        hdr = f.readline().decode("latin-1")
    if not hdr.startswith("YUV4MPEG2 "):
        sys.exit(f"hdr_ladder: {path} is not a Y4M")
    toks = hdr.split()[1:]
    tok = {t[0]: t[1:] for t in toks if t[0] != "X"}
    ext = dict(t[1:].split("=", 1) for t in toks if t[0] == "X" and "=" in t)
    # the scorer is always told limited range and progressive frames
    if ext.get("COLORRANGE", "LIMITED").upper() != "LIMITED":
        sys.exit(f"hdr_ladder: {path} is XCOLORRANGE={ext['COLORRANGE']}; HDR10 is scored as limited range")
    if tok.get("I", "p") != "p":
        sys.exit(f"hdr_ladder: {path} is I{tok['I']}, not progressive")
    if tok.get("C") != "420p10":
        sys.exit(f"hdr_ladder: {path} is C{tok.get('C', '420')}, not C420p10")
    w, h = int(tok["W"]), int(tok["H"])
    if w % 2 or h % 2:
        sys.exit(f"hdr_ladder: {path} is {w}x{h}; 4:2:0 needs an even width and height")
    num, den = map(int, tok["F"].split(":"))
    return w, h, num / den


def model_path(w, h, fps):
    """The model follows the source: "above 1080p" means a height above 1080."""
    d = "hdr_vmaf_v1.1.8" + ("_hfr" if fps >= 50 else "")
    return os.path.join(MODELS, d, f"{d}_{'1d5h_2160' if h > 1080 else '3d0h'}.json")


# test harnesses stand wrappers in for the binaries; they may, with the
# cache read off, since a wrapper's hash says nothing about what it runs
ALLOW_ENV = "HDR_LADDER_ALLOW_SCRIPTS"
ALLOW_SCRIPTS = os.environ.get(ALLOW_ENV) == "1"
NATIVE_MAGIC = (b"\x7fELF", b"\xfe\xed\xfa\xce", b"\xfe\xed\xfa\xcf", b"\xce\xfa\xed\xfe",
                b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca",
                b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca")


def is_native(path):
    """A Mach-O (thin or universal) or ELF executable, by its magic bytes."""
    try:
        with open(path, "rb") as f:
            return f.read(4) in NATIVE_MAGIC
    except OSError:
        return False


def which(b):
    p = b if os.sep in b else shutil.which(b)
    return os.path.realpath(p) if p else None


def md5_of(path):
    m = hashlib.md5()
    with open(path, "rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            m.update(blk)
    return m.hexdigest()


def source_id(path):
    """Size, inode, mtime, ctime and the md5 of the first and last 8 MiB:
    hashing a whole 2160p excerpt on every run costs more than the check is
    worth, and a re-cut or recopy changes at least one of them (restoring the
    mtime still moves the ctime)."""
    st = os.stat(path)
    m = hashlib.md5()
    with open(path, "rb") as f:
        m.update(f.read(8 << 20))
        f.seek(max(0, st.st_size - (8 << 20)))
        m.update(f.read())
    return {"size": st.st_size, "ino": st.st_ino, "mtime_ns": st.st_mtime_ns,
            "ctime_ns": st.st_ctime_ns, "md5_ends": m.hexdigest()}


def linked_libs(path):
    """The non-system shared libraries a binary loads, followed through the
    libraries they load. Paths the loader resolves at run time (@rpath and
    friends) are kept by name only."""
    seen, todo = {}, [path]
    tool = ["otool", "-L"] if sys.platform == "darwin" else ["ldd"]
    while todo:
        p = todo.pop()
        try:
            r = capture(tool + [p])
        except OSError as e:
            sys.exit(f"hdr_ladder: can't list {p}'s libraries with {tool[0]}: {e}")
        if tool[0] == "ldd" and "not a dynamic executable" in r.stdout + r.stderr:
            continue
        if r.returncode != 0 or not r.stdout.strip():
            sys.exit(f"hdr_ladder: {tool[0]} failed on {p} (exit {r.returncode}); the cache "
                     f"key needs its libraries\n{r.stderr[-500:]}")
        if tool[0] == "otool":
            deps = [ln.split(" (")[0].strip() for ln in r.stdout.splitlines()[1:]]
        else:
            deps = [ln.split("=>")[-1].split(" (")[0].strip() for ln in r.stdout.splitlines()]
        for d in deps:
            if not d or d in seen or d == p or d.startswith(("/usr/lib/", "/System/", "/lib/", "/lib64/")):
                continue
            if os.path.isabs(d) and os.path.isfile(d):
                seen[d] = md5_of(os.path.realpath(d))
                todo.append(d)
            else:
                seen[d] = None
    return dict(sorted(seen.items()))


def tool_id(path, version_args=None):
    t = {"md5": md5_of(path), "libs": linked_libs(path)}
    if version_args:
        r = capture([path] + version_args)
        t["version"] = (r.stdout + r.stderr).strip()[:2000]
    return t


def yah_lookahead_limits(path):
    """yah264's default and maximum lookahead, as its --help states them."""
    r = capture([path, "--help"])
    text = r.stdout + r.stderr
    i = text.find("--rc-lookahead N")
    end = text.find("\n  --", i + 1) if i >= 0 else -1
    entry = re.sub(r"\s+", " ", text[i:end if end >= 0 else len(text)]) if i >= 0 else ""
    d = re.search(r"\(default (\d+)", entry)
    m = re.search(r"at most (\d+)", entry)
    if d and m:
        return int(d.group(1)), int(m.group(1))
    print(f"hdr_ladder: warning: {path}'s --help doesn't state its lookahead default and cap "
          f"(a build older than the one that prints them); assuming "
          f"{YAH_DEFAULT_LOOKAHEAD} and {YAH_MAX_LOOKAHEAD}")
    return YAH_DEFAULT_LOOKAHEAD, YAH_MAX_LOOKAHEAD


# a step that makes no progress for this long is killed and the rung fails;
# a frame normally passes in well under a second
STALL_SECONDS = 600
# the HDR models' SpEED feature prints these when it falls back on a flat or
# degenerate block; they are content-dependent and leave the score valid.
# Anything else vmaf says fails the rung
VMAF_BENIGN = re.compile(r"(libvmaf WARNING )?(est_params: covariance matrix is singular|"
                         r"compute_eigenvalues_tridiagonal: max iterations reached, "
                         r"possible non-convergence)$")


def score(out, src, w, h, threads, model_arg, scaled, tmp, vj):
    """Decode out once and stream it: each frame is checked and added to the
    PSNR sums here, then handed on to vmaf (through a scaler when the source
    is scored at display size). The reference reaches vmaf as the source file
    or, scaled, through a FIFO. No decode or scaled picture lands on disk.
    Returns (frames, PSNR-Y, PSNR-YUV)."""
    import numpy as np
    fs = w * h * 3 // 2
    procs, errs, killed = {}, {}, set()
    progress = _PROGRESS
    progress[0] = time.monotonic()
    stalled = []

    def err(name):
        errs[name] = open(os.path.join(tmp, name + ".err"), "w+")
        return errs[name]

    def nice(cmd):
        return ["nice", "-n", "10"] + cmd

    def errtext(name):
        errs[name].seek(0)
        return errs[name].read().strip()

    def kill(name):
        p = procs[name]
        if p.poll() is None:
            killed.add(name)
            kill_child(p)

    def spawn(name, cmd, **kw):
        procs[name] = spawn_child(cmd, **kw)

    stop = threading.Event()
    # the try opens before the first spawn, so a signal or a failed spawn at
    # any point still reaches the finally that kills what was started
    try:
        scale = ["-vf", V1_SCALE_VF, "-pix_fmt", "yuv420p10le", "-strict", "-1",
                 "-f", "yuv4mpegpipe"]
        ref = src
        if scaled:
            ref = os.path.join(tmp, "ref.y4m")
            os.mkfifo(ref)
            spawn("ref", nice([FFMPEG, "-v", "error", "-nostdin", "-i", src] + scale + ["-y", ref]),
                  stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=err("ref"))
        spawn("vmaf", nice([VMAF, "--reference", ref, "--distorted", "/dev/stdin",
                            "--threads", str(threads)] + vmaf_colour_args() +
                           ["--model", model_arg, "--json", "--output", vj]),
              stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=err("vmaf"))
        sink = procs["vmaf"].stdin
        if scaled:
            spawn("dscale", nice([FFMPEG, "-v", "error", "-nostdin", "-f", "yuv4mpegpipe",
                                  "-i", "pipe:0"] + scale + ["pipe:1"]),
                  stdin=subprocess.PIPE, stdout=procs["vmaf"].stdin, stderr=err("dscale"))
            procs["vmaf"].stdin.close()
            sink = procs["dscale"].stdin
        # -xerror and explode turn a damaged stream into an error; anything
        # ffmpeg says at -v error is treated the same, whatever its exit code
        spawn("dec", nice([FFMPEG, "-v", "error", "-nostdin", "-xerror", "-err_detect", "explode",
                           "-i", out, "-fps_mode", "passthrough", "-pix_fmt", "yuv420p10le",
                           "-strict", "-1", "-f", "yuv4mpegpipe", "pipe:1"]),
              stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=err("dec"))

        def watchdog():
            last = time.monotonic()
            while not stop.wait(0.2):
                # a long oversleep means the whole job was stopped, not stalled
                now = time.monotonic()
                if now - last > 5:
                    progress[0] = now
                last = now
                if scaled:
                    vm, rf = procs["vmaf"].poll(), procs["ref"].poll()
                    # either end of the reference FIFO can be left blocked in
                    # open() when the other exits first; opening the far end and
                    # closing it again lets the survivor see EOF or EPIPE
                    if vm is not None and rf is None:
                        try:
                            os.close(os.open(ref, os.O_RDONLY | os.O_NONBLOCK))
                        except OSError:
                            pass
                    if rf is not None and vm is None:
                        try:
                            os.close(os.open(ref, os.O_WRONLY | os.O_NONBLOCK))
                        except OSError:
                            pass
                if time.monotonic() - progress[0] > STALL_SECONDS:
                    stalled.append(True)
                    for k in list(procs):
                        kill(k)
                    return
        threading.Thread(target=watchdog, daemon=True).start()
        sse = np.zeros(3)
        n = 0
        problem = None       # what this side found wrong with the decode
        broken = False       # the scorer side stopped taking frames
        b = procs["dec"].stdout
        with open(src, "rb") as a:
            want, dhead = y4m_shape(a.readline()), b.readline()
            got = y4m_shape(dhead)
            if want != got:
                problem = f"{out} decodes as {got}, not the source's {want}"
            else:
                try:
                    sink.write(dhead)
                    while a.readline():
                        fl = b.readline()
                        if not fl:
                            problem = f"{out} is short: the source has more than {n} frames"
                            break
                        sb, db = a.read(2 * fs), b.read(2 * fs)
                        if len(db) != len(sb):
                            problem = f"{out} ends inside frame {n}"
                            break
                        s = np.frombuffer(sb, dtype=np.uint16).astype(np.int64)
                        d = np.frombuffer(db, dtype=np.uint16).astype(np.int64)
                        e = (s - d) ** 2
                        sse += [e[:w * h].sum(), e[w * h:w * h * 5 // 4].sum(),
                                e[w * h * 5 // 4:].sum()]
                        n += 1
                        sink.write(fl)
                        sink.write(db)
                        progress[0] = time.monotonic()
                    else:
                        if b.readline():
                            problem = f"{out} is long: it decodes more than the source's {n} frames"
                except BrokenPipeError:
                    broken = True
        if problem or broken:
            kill("dec")
        try:
            sink.close()
        except BrokenPipeError:
            broken = True
        rc = {}
        for k in ("dec", "dscale", "vmaf", "ref"):
            if k not in procs:
                continue
            # the watchdog's stall timer is the backstop for each wait too
            last = time.monotonic()
            while True:
                try:
                    rc[k] = procs[k].wait(timeout=1)
                    break
                except subprocess.TimeoutExpired:
                    now = time.monotonic()
                    if now - last > 5:
                        progress[0] = now
                    last = now
                    progress_age = now - progress[0]
                    if stalled or progress_age > STALL_SECONDS + 5:
                        kill(k)
        stop.set()
        if stalled:
            said = "".join(f"\n{k}: {errtext(k)[-1500:]}" for k in errs if errtext(k))
            sys.exit(f"hdr_ladder: scoring {out} made no progress for {STALL_SECONDS} s; "
                     f"every step was killed{said}")
        # report the first cause: the decode, then what this side found, then
        # the scorer and its scalers in the order a failure propagates
        dmsg = errtext("dec")
        if (rc["dec"] != 0 and "dec" not in killed) or dmsg:
            sys.exit(f"hdr_ladder: {out} decodes with errors (exit {rc['dec']}):\n{dmsg[-1500:]}")
        if problem:
            sys.exit(f"hdr_ladder: {problem}")
        said = {k: errtext(k) for k in ("vmaf", "dscale", "ref") if k in rc}
        said["vmaf"] = "\n".join(ln for ln in said["vmaf"].splitlines()
                                 if not VMAF_BENIGN.match(ln.strip())).strip()
        if broken and rc["vmaf"] == 0 and not said["vmaf"]:
            sys.exit(f"hdr_ladder: vmaf exited 0 without reading the whole decode of {out}")
        # every failing step is named, vmaf first: a scaler's broken pipe is
        # usually vmaf's failure seen from upstream, but not always
        failed = [f"the {k} step failed (exit {rc[k]}):\n{said[k][-1500:]}"
                  for k in ("vmaf", "dscale", "ref") if k in rc and (rc[k] != 0 or said[k])]
        if failed:
            sys.exit("hdr_ladder: " + "\n".join(failed))
    finally:
        stop.set()
        for k in procs:
            kill(k)
        for p in procs.values():
            p.wait()
        for f in errs.values():
            f.close()
    if n == 0:
        sys.exit("hdr_ladder: the decode has no frames")
    mse = sse / (np.array([w * h, w * h / 4, w * h / 4]) * n)
    p = [10 * math.log10(1023 * 1023 / m) if m > 0 else 100.0 for m in mse]   # lossless caps
    return n, p[0], (6 * p[0] + p[1] + p[2]) / 8


def vmaf_colour_args():
    col = []
    for side in ("ref", "dist"):
        col += [f"--color_range_{side}", "limited", f"--color_primaries_{side}", "bt2020",
                f"--color_trc_{side}", "pq", f"--color_matrix_{side}", "bt2020nc"]
    return col


def check_vmaf_build(path):
    """A vmaf without the HDR colour options warns on stderr and scores the
    PQ picture as SDR with exit 0, so the build is checked before any rung."""
    r = capture([path], stdin=subprocess.DEVNULL)
    text = r.stdout + r.stderr
    # the usage lists each option once for both sides, as --color_trc_ref/_dist
    missing = [f"--color_{o}_ref" for o in ("range", "primaries", "trc", "matrix")
               if f"--color_{o}_ref" not in text]
    if missing:
        sys.exit(f"hdr_ladder: {path} doesn't list {missing[0]} in its usage; VMAF_HDR must be "
                 "a vmaf built with the HDR models' colour options")


def y4m_shape(line):
    tok = {t[0]: t[1:] for t in line.decode("latin-1").split()[1:]}
    return tuple(tok.get(k) for k in "WHC")


def vbv_args(kbps):
    return round(kbps * 1.5), round(kbps * 2)


def lookahead_in_stream(path):
    """The lookahead depth the options SEI x264 writes into its stream
    records. With mb-tree off and no VBV the SEI gives no depth at all, and
    that is a depth of 0."""
    with open(path, "rb") as f:
        head = f.read(1 << 16)
    m = re.search(rb"rc_lookahead=(\d+)", head)
    if m:
        return int(m.group(1))
    if re.search(rb" mbtree=0", head):
        return 0
    sys.exit(f"hdr_ladder: {path} carries no options SEI with a lookahead depth")


def check_unchanged(paths):
    """Stop if a binary a rung ran was replaced while it ran."""
    for p, want in paths.items():
        if md5_of(p) != want:
            sys.exit(f"hdr_ladder: {p} changed during the run; its rungs can't be keyed")


def fmt(q):
    return str(int(q)) if q == int(q) else f"{q:g}"


def load_json(js):
    try:
        with open(js) as f:
            r = json.load(f)
    except (OSError, ValueError):
        return None
    return r if isinstance(r, dict) else None


def rung(enc, src, mode, q, work, threads, w, h, fps, enc_id, ids, lookahead, la_default,
         fresh, scaled):
    tag = f"{mode}{fmt(q)}" + (f"_la{lookahead}" if lookahead is not None else "")
    # two builds or thread counts in one work directory keep their own rungs
    tag += "_" + hashlib.md5(f"{enc_id['md5']} {threads}".encode()).hexdigest()[:8]
    if enc_id.get("env"):
        # a knob A/B keeps both arms' rungs side by side
        tag += "_env" + hashlib.md5(" ".join(enc_id["env"]).encode()).hexdigest()[:8]
    stem = os.path.join(work, f"{enc}_{tag}")
    js = stem + ".json"
    out, dec, vj = stem + ".264", stem + ".dec.y4m", stem + ".vmaf.json"
    part = stem + ".part.264"
    if mode == "crf":
        rc = ["--crf", fmt(q)]
    else:
        maxrate, bufsize = vbv_args(q)
        rc = ["--bitrate", fmt(q), "--vbv-maxrate", str(maxrate), "--vbv-bufsize", str(bufsize),
              "--vbv-init", str(VBV_INIT)]
    if lookahead is not None:
        rc += ["--rc-lookahead", str(lookahead)]
    # tag both streams HDR10, as a real HDR encode would be: an encoder that
    # reads the VUI then behaves as it would in the field
    vui = ["--colorprim", "bt2020", "--transfer", "smpte2084", "--colormatrix", "bt2020nc"]
    if enc == "yah264":
        cmd = [YAH, "--input-y4m", src] + rc + vui + ["--range", "limited",
                                                      "--threads", str(threads), "-o", part]
    else:
        cmd = [X264, "--preset", "medium", "--profile", "high10", "--output-depth", "10"] + rc + \
              vui + ["--range", "tv", "--threads", str(threads), "--log-level", "error",
                     "-o", part, src]
    # the stream is keyed on what makes it, the score on what reads it, so a
    # scorer change rescores the kept stream instead of encoding it again
    enc_key = {"encoder": enc_id, "cmd": cmd, "src": ids["src"]}
    score_key = {k: v for k, v in ids.items() if k != "src"}
    score_key.update(model=model_path(w, h, fps), display=V1_SCALE_VF if scaled else None,
                     version=RESULT_VERSION)
    old = None if fresh else load_json(js)
    if old and old.get("enc_key") == enc_key and old.get("score_key") == score_key:
        old["cached"] = True
        return old
    # a kept stream is reused only while its bytes are the ones the record names
    reuse = bool(old and old.get("enc_key") == enc_key and os.path.isfile(out)
                 and old.get("stream_md5") == md5_of(out))
    # intermediates any earlier version or a killed run may have left
    for p in [dec, stem + ".dec.disp.y4m", vj] + glob.glob(stem + ".score.*") + \
            ([] if reuse else [js, out, part]):
        if os.path.isdir(p):
            shutil.rmtree(p, ignore_errors=True)
        elif os.path.exists(p):
            os.remove(p)
    if reuse:
        # read again, so a corrected reader isn't masked by the old record
        ran = lookahead_in_stream(out) if enc == "x264" else \
            (lookahead if lookahead is not None else la_default)
    else:
        # the stream is written under a temporary name and must appear fresh
        # and non-empty, so an encoder that exits 0 without writing can't hand
        # an old stream to the scorer
        start = time.time_ns()
        scratch(part)
        run(cmd)
        if not os.path.exists(part) or os.path.getsize(part) == 0 or \
                os.stat(part).st_mtime_ns < start - 10**9:
            sys.exit(f"hdr_ladder: {enc} exited cleanly but wrote no stream: {' '.join(cmd)}")
        os.replace(part, out)
        unscratch(part)
        # read before the decode and scoring, so a misread costs one encode
        ran = lookahead_in_stream(out) if enc == "x264" else \
            (lookahead if lookahead is not None else la_default)
    stream_md5 = md5_of(out)
    model = "path=" + model_path(w, h, fps)
    if scaled:
        # scored at display size; CAMBI judges banding at the coded size
        model += f":cambi.enc_width={w}:cambi.enc_height={h}"
    with held():
        tmp = scratch(tempfile.mkdtemp(prefix=os.path.basename(stem) + ".score.", dir=work))
    scratch(vj)
    try:
        n, py, pyuv = score(out, src, w, h, threads, model, scaled, tmp, vj)
    except BaseException:
        # vmaf may have written a score for a decode that was cut short
        if os.path.exists(vj):
            os.remove(vj)
        raise
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
        unscratch(tmp)
    if not os.path.exists(vj) or os.path.getmtime(vj) < os.path.getmtime(out):
        sys.exit(f"hdr_ladder: vmaf exited cleanly but wrote no fresh {vj}")
    with open(vj) as f:
        vm = json.load(f)
    if len(vm["frames"]) != n:
        sys.exit("hdr_ladder: the VMAF frame count disagrees with the decode")
    pooled = vm["pooled_metrics"]
    cambi = [k for k in pooled if k.startswith("cambi")]
    if not cambi:
        sys.exit(f"hdr_ladder: {model_path(w, h, fps)} computes no CAMBI feature")
    frames_cambi = [f["metrics"][cambi[0]] for f in vm["frames"]]
    # the model clips each frame's score; a frame at a bound is a clamp, not
    # a measurement, and the pooled mean can hide many of them
    with open(model_path(w, h, fps)) as f:
        lo, hi = json.load(f)["model_dict"].get("score_clip", [None, None])
    clamped = sum(1 for f in vm["frames"]
                  if (lo is not None and f["metrics"]["vmaf"] <= lo) or
                  (hi is not None and f["metrics"]["vmaf"] >= hi))
    r = {"mode": mode, "q": q, "lookahead": lookahead, "lookahead_run": ran,
         "kbps": os.path.getsize(out) * 8 / (n / fps) / 1000,
         "vmaf": pooled["vmaf"]["mean"], "psnr_y": py, "psnr_yuv": pyuv,
         "cambi": pooled[cambi[0]]["mean"], "cambi_max": max(frames_cambi),
         "clamped": clamped, "frames": n,
         "enc_key": enc_key, "score_key": score_key, "stream": out, "stream_md5": stream_md5,
         "rescored": reuse}
    if mode == "vbr":
        maxrate, bufsize = vbv_args(q)
        v = run([sys.executable, VBV_CHECK, out, "--maxrate", str(maxrate),
                 "--bufsize", str(bufsize), "--init", str(VBV_INIT), "--y4m", src, "--quiet"],
                ok=(0, 1))
        r["rate_err"] = (r["kbps"] / q - 1) * 100
        verdict = v.stdout.strip().splitlines()[0] if v.stdout.strip() else ""
        # exit 1 is an underflow only when the checker says so; a crash exits 1 too
        if v.returncode == 1 and not verdict.startswith("UNDERFLOW "):
            sys.exit(f"hdr_ladder: vbv_check failed on {out}\n{v.stderr[-1500:]}")
        r["vbv"] = verdict
        r["vbv_ok"] = v.returncode == 0
    check_unchanged({cmd[0]: enc_id["md5"], VMAF: ids["vmaf"]["md5"], FFMPEG: ids["ffmpeg"]["md5"]})
    if md5_of(out) != stream_md5:
        sys.exit(f"hdr_ladder: {out} changed while it was scored")
    scratch(js + ".tmp")
    with open(js + ".tmp", "w") as f:
        json.dump(r, f)
    os.replace(js + ".tmp", js)
    unscratch(js + ".tmp")
    unscratch(vj)
    return r


def ladder_list(text, opt, conv):
    if text is None or not text.strip():
        sys.exit(f"hdr_ladder: {opt} needs a comma-separated list")
    try:
        qs = [conv(t) for t in text.split(",")]
    except ValueError as e:
        sys.exit(f"hdr_ladder: {opt} {text}: {e}")
    if len(set(qs)) != len(qs):
        sys.exit(f"hdr_ladder: {opt} {text} repeats a rung; a repeated point skews the BD fit")
    return sorted(qs)


def crf_value(t):
    q = float(t)
    if not 0 <= q <= 51:
        raise ValueError(f"CRF {t} is outside 0..51")
    # yah264 rounds --crf to tenths, so finer steps would label one stream as
    # two rungs, or run different values on the two sides
    if abs(q * 10 - round(q * 10)) > 1e-6:
        raise ValueError(f"CRF {t} is not a multiple of 0.1")
    q = round(q * 10) / 10
    q = int(q) if q == int(q) else q
    if float(fmt(q)) != q:
        raise ValueError(f"CRF {t} doesn't survive formatting for the command line")
    return q


def kbps_value(t):
    if not t.strip().isdigit() or int(t) <= 0:
        raise ValueError(f"target '{t}' is not a positive whole kbit/s")
    return int(t)


def lookahead_value(t):
    v = int(t)
    if not 0 <= v <= MAX_LOOKAHEAD:
        raise argparse.ArgumentTypeError(f"{t} is outside 0..{MAX_LOOKAHEAD}")
    return v


def strictly_rising(xs):
    return all(b > a for a, b in zip(xs, xs[1:]))


def main():
    global YAH, X264, VMAF, FFMPEG, LOCK
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src", help="the HDR10 source, a C420p10 Y4M")
    ap.add_argument("--crf", default=None,
                    help="CRF rungs, multiples of 0.1 (default 18,22,26,30,34)")
    ap.add_argument("--vbr", default=None,
                    help="VBR targets in whole kbit/s; replaces the CRF ladder")
    ap.add_argument("--max-crf", type=float, default=None,
                    help="leave out the CRF rungs above this")
    ap.add_argument("--yah-lookahead", type=lookahead_value, default=None,
                    help="yah264's --rc-lookahead (at most its 64-frame cap)")
    ap.add_argument("--x264-lookahead", type=lookahead_value, default=None,
                    help="x264's --rc-lookahead, 0 to 250")
    ap.add_argument("--encoders", default="x264,yah264",
                    help="x264, yah264 or both (the default); BD needs both")
    ap.add_argument("--threads", type=int, default=3,
                    help="threads per encode and per vmaf (default 3; the spec runs 6)")
    ap.add_argument("--work", default=None,
                    help="cache and scratch directory (default local/hdr_ladder/<source>)")
    ap.add_argument("--fresh", action="store_true", help="ignore cached rungs")
    a = ap.parse_args()
    if not VMAF or not MODELS:
        sys.exit("hdr_ladder: set VMAF_HDR and VMAF_HDR_MODELS")
    encs = a.encoders.split(",")
    if not encs or any(e not in ("x264", "yah264") for e in encs) or len(set(encs)) != len(encs):
        sys.exit("hdr_ladder: --encoders takes x264, yah264 or both")
    if a.threads < 1:
        sys.exit("hdr_ladder: --threads must be at least 1")
    if a.vbr is not None:
        if a.crf is not None or a.max_crf is not None:
            sys.exit("hdr_ladder: --vbr replaces the CRF ladder; drop --crf and --max-crf")
        mode, qs = "vbr", ladder_list(a.vbr, "--vbr", kbps_value)
    else:
        mode, qs = "crf", ladder_list(a.crf if a.crf is not None else "18,22,26,30,34",
                                      "--crf", crf_value)
        if a.max_crf is not None:
            qs = [q for q in qs if q <= a.max_crf]
    if mode == "crf" and "yah264" in encs and qs and qs[0] < 0.1:
        sys.exit(f"hdr_ladder: CRF {fmt(qs[0])} is below yah264's 0.1 minimum")
    left = f"{len(qs)} rung(s) left to run" + \
        (f" after --max-crf {fmt(a.max_crf)}" if mode == "crf" and a.max_crf is not None else "")
    if len(encs) == 2 and len(qs) < 2:
        sys.exit(f"hdr_ladder: {left}; a BD figure needs at least 2 per side")
    if not qs:
        sys.exit(f"hdr_ladder: {left}")
    la = {"yah264": a.yah_lookahead, "x264": a.x264_lookahead}
    if any(la[e] is not None for e in la if e not in encs):
        sys.exit("hdr_ladder: a lookahead was given for an encoder this run leaves out")
    w, h, fps = y4m_info(a.src)
    # everything a rung needs, checked before the first encode
    bins = {}
    for name, b in [(e, YAH if e == "yah264" else X264) for e in encs] + \
                   [("vmaf", VMAF), ("ffmpeg", "ffmpeg")]:
        p = which(b)
        if not p or not os.access(p, os.X_OK):
            sys.exit(f"hdr_ladder: no {name} binary at '{b}'")
        if not is_native(p):
            if not ALLOW_SCRIPTS:
                sys.exit(f"hdr_ladder: {name} at '{b}' resolves to {p}, which isn't a Mach-O or "
                         "ELF executable. The cache keys on the hashed file, so a wrapper script "
                         "would keep serving old numbers after the binary behind it changes; "
                         "point at the binary itself")
            print(f"hdr_ladder: warning: {name} at {p} is a script; "
                  f"{ALLOW_ENV}=1 is set, so no cached rung is reused")
            a.fresh = True
        bins[name] = p
    # every later run uses the path that is hashed here
    YAH, X264, VMAF, FFMPEG = (bins.get("yah264", YAH), bins.get("x264", X264),
                               bins["vmaf"], bins["ffmpeg"])
    check_vmaf_build(VMAF)
    if not os.path.isfile(model_path(w, h, fps)):
        sys.exit(f"hdr_ladder: no HDR model at {model_path(w, h, fps)}")
    la_default = {"x264": None, "yah264": None}
    if "yah264" in encs:
        la_default["yah264"], yah_max = yah_lookahead_limits(bins["yah264"])
        if la["yah264"] is not None and la["yah264"] > yah_max:
            sys.exit(f"hdr_ladder: --yah-lookahead {la['yah264']} is above yah264's "
                     f"{yah_max}-frame cap")
    work = a.work or os.path.join(ROOT, "local", "hdr_ladder",
                                  os.path.splitext(os.path.basename(a.src))[0])
    os.makedirs(work, exist_ok=True)
    # one run per work directory: two runs sharing it would score and cache
    # each other's streams
    LOCK = open(os.path.join(work, ".lock"), "w")
    try:
        fcntl.flock(LOCK, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        sys.exit(f"hdr_ladder: another run holds {work}; wait for it or pass another --work")
    knobs = sorted(f"{k}={v}" for k, v in os.environ.items() if k.startswith(KNOB_PREFIXES))
    enc_id = {e: tool_id(bins[e]) for e in encs}
    if "yah264" in enc_id:
        enc_id["yah264"]["env"] = knobs
    ids = {"src": source_id(a.src), "script_md5": md5_of(os.path.abspath(__file__)),
           "vmaf": tool_id(bins["vmaf"], ["--version"]),
           "ffmpeg": tool_id(bins["ffmpeg"], ["-version"]),
           "model_md5": md5_of(model_path(w, h, fps))}
    if mode == "vbr":
        ids["vbv_check_md5"] = md5_of(VBV_CHECK)
    print(f"{os.path.basename(a.src)} {w}x{h} {fps:.3f} fps, {mode.upper()}, medium, "
          f"--threads {a.threads}, {os.path.basename(model_path(w, h, fps))}"
          + (", VBV maxrate 1.5K buffer 2K init 0.9" if mode == "vbr" else ""))
    if knobs and "yah264" in encs:
        print("yah264 environment: " + " ".join(knobs))
    scaled = v1_scaled(w, h)
    if scaled:
        print(f"scored at display size: {w}x{h} scaled up to fit 1920x1080, CAMBI at {w}x{h}")
    stale = os.path.join(work, ".ref.disp.y4m")    # an earlier version's scaled reference
    if os.path.exists(stale):
        os.remove(stale)
    lad = {e: [rung(e, a.src, mode, q, work, a.threads, w, h, fps, enc_id[e], ids, la[e],
                    la_default[e], a.fresh, scaled)
               for q in qs] for e in encs}
    outside, underflow = [], []
    for e, rs in lad.items():
        for r in rs:
            line = (f"{e:7s} {mode}{fmt(r['q']):<6} {r['kbps']:9.0f} kbps  vmaf {r['vmaf']:6.2f}  "
                    f"psnr-y {r['psnr_y']:6.2f}  psnr-yuv {r['psnr_yuv']:6.2f}  "
                    f"cambi {r['cambi']:.3f} (max {r['cambi_max']:.3f})")
            if r["clamped"]:
                line += f"  clamped {r['clamped']}/{r['frames']}"
            if mode == "vbr":
                miss = abs(r["rate_err"]) > RATE_BAR
                if miss:
                    outside.append(f"{e} {fmt(r['q'])}")
                line += f"  rate {r['rate_err']:+.1f}%" + (f" OUTSIDE +-{RATE_BAR:g}%" if miss else "")
            print(line + ("  (cached)" if r.get("cached") else
                          "  (rescored)" if r.get("rescored") else ""))
            if mode == "vbr":
                if not r["vbv_ok"]:
                    underflow.append(f"{e} {fmt(r['q'])}")
                # the checker's line starts "STATUS STREAM: "; drop the
                # stream by its known path, which may itself hold ": "
                status, _, rest = r["vbv"].partition(" ")
                rest = rest[len(r["stream"]) + 2:] if rest.startswith(r["stream"] + ": ") else rest
                print(f"          vbv {status}: {rest}")
    print("lookahead as run: " + ", ".join(
        f"{e} " + "/".join(str(d) for d in sorted({r['lookahead_run'] for r in rs}))
        for e, rs in lad.items()))
    if mode == "vbr":
        n = sum(len(rs) for rs in lad.values())
        print(f"rate tracking, +-{RATE_BAR:g}% bar: " +
              (f"{len(outside)} of {n} rungs outside ({', '.join(outside)})" if outside
               else f"all {n} rungs within"))
        print("vbv: " + (f"{len(underflow)} of {n} rungs UNDERFLOW ({', '.join(underflow)})"
                         if underflow else f"all {n} rungs OK"))
    for e, rs in lad.items():
        kb = [r["kbps"] for r in rs]   # rs is in rung order
        rising = strictly_rising(kb) if mode == "vbr" else strictly_rising(kb[::-1])
        if not rising:
            print(f"warning: {e}'s bitrate doesn't move monotonically with the {mode.upper()} rungs")
    if len(lad) < 2:
        return
    b, t = sorted(lad["x264"], key=lambda r: r["kbps"]), sorted(lad["yah264"], key=lambda r: r["kbps"])
    lo, hi = max(b[0]["kbps"], t[0]["kbps"]), min(b[-1]["kbps"], t[-1]["kbps"])
    print(f"bitrate overlap: {lo:.0f}-{hi:.0f} kbps" if hi > lo else
          "bitrate overlap: none (BD-rate compares rates at equal quality, over the "
          "quality range printed with each figure)")
    for m in ("vmaf", "psnr_y", "psnr_yuv"):
        head = f"BD-rate {m:8s} yah264 vs x264 High 10: "
        if m == "vmaf":
            pinned = [f"{e} {mode}{fmt(r['q'])} ({r['clamped']}/{r['frames']})"
                      for e, rs in (("x264", b), ("yah264", t))
                      for r in rs if r["clamped"] > CLAMP_SHARE * r["frames"]]
            if pinned:
                print(head + f"none, more than {CLAMP_SHARE:.0%} of the frames sit at the "
                      "model's clip bounds on " + ", ".join(pinned) +
                      ", so those rungs aren't measurements; drop them from the ladder")
                continue
        bad = [e for e, rs in (("x264", b), ("yah264", t)) if not strictly_rising([r[m] for r in rs])]
        if bad:
            print(head + f"none, {m} doesn't rise with bitrate on {' and '.join(bad)}")
            continue
        qlo = max(b[0][m], t[0][m])
        qhi = min(b[-1][m], t[-1][m])
        bd = bd_rate([r["kbps"] for r in b], [r[m] for r in b], [r["kbps"] for r in t], [r[m] for r in t])
        print(head + (f"{bd:+.2f}% over {m} {qlo:.2f}-{qhi:.2f}" if bd is not None
                      else "no quality overlap"))


try:
    main()
except KeyboardInterrupt:
    # only before the handlers were in place: die of SIGINT, as above
    signal.signal(signal.SIGINT, signal.SIG_DFL)
    os.kill(os.getpid(), signal.SIGINT)
    sys.exit(128 + signal.SIGINT)
