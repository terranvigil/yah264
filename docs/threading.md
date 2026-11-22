# Threading

yah264 has two levels of parallelism. Inside one encoder instance the library
runs a macroblock-row wavefront and lets consecutive frames of a GOP overlap on
it. Across instances the command-line tool runs GOP-parallel workers. Each is
a whole encoder coding its own closed GOP. The library decides what one
instance does with a thread budget. How a budget is split across instances is
up to the caller.

The site's [threading page](threading.html) walks through the wavefront and
the auto rule with diagrams. This doc is the developer's version, covering the
mechanism, the defaults and the dead ends.

## How it works

### How auto picks the thread count

`param.threads` is the budget for one encoder instance: 0 asks the library to
decide, 1 is serial, N means up to N. `frame_threads` sets the wavefront width
explicitly and wins when it's set, because each of the CLI's GOP workers has
its own share. The resolved width is

```
min(request or auto, yah264_frame_thread_cap(width, height))
auto = min(online cores, 16)
```

The core count comes from `y264_machine_threads()` in `src/common/cpu.c`. On
Apple Silicon it enumerates `hw.nperflevels` and sums all the levels without
assuming which one is the performance tier. Elsewhere it's
`sysconf(_SC_NPROCESSORS_ONLN)`. The ceiling of 16 applies to auto only. An
explicit request passes through, although the picture still clamps it.

`yah264_frame_thread_cap` depends on the picture alone. For a frame of R
macroblock rows and C columns, one frame's diagonal retires in `2(R-1)+C`
macroblock-times. With k frames overlapping `lag` rows apart, the group retires
in `2(R-1)+C+(k-1)*2*lag`. The cap is `k*R*C` over that, floored at
`Y264_MT_POOL_MIN` (2). A frame is narrow when its one-frame cap is under 16.
Narrow frames are sized for k=2, an anchor plus one frame whose rows wait on
the anchor's. Wider frames are sized for k=1. `frame_thread_cap_k` in
`src/encoder/encoder.c` has the formula.

| resolution | cap | 4 | 8 | 12 | 16 | 32 | 64 cores |
|---|--:|--:|--:|--:|--:|--:|--:|
| QCIF 176x144 | 5 | 4 | 5 | 5 | 5 | 5 | 5 |
| CIF 352x288 | 12 | 4 | 8 | 12 | 12 | 12 | 12 |
| 720p | 21 | 4 | 8 | 12 | 16 | 16 | 16 |
| 1080p | 32 | 4 | 8 | 12 | 16 | 16 | 16 |
| 4K | 63 | 4 | 8 | 12 | 16 | 16 | 16 |

`yah264_threads_auto()` is exported so a caller splitting work across instances
sizes its budget from the same number the library uses. Through ffmpeg,
`-threads N` passes straight through. Leaving it out gets auto, which is also
the default for the CLI's `--threads`.

### The macroblock-row wavefront

Each macroblock needs its left, top, top-left and top-right neighbors finished.
Row r may code macroblock x once row r-1 has finished x+2. The work therefore
advances as a diagonal. The schedule is a pure function of frame geometry.
Threads race to execute cells, never to decide what a cell reads.

Analysis and reconstruction run on the wavefront. Everything with a raster
total order lives in one serial entropy pass per frame that trails the rows:
the CABAC engine and its adaptive contexts, the CAVLC bitstream and skip run,
and the `mb_qp_delta` chain. That pass consumes committed per-macroblock
records and never feeds analysis. Two things make the split hold:

- **RD bit estimates use a row-private context.** CABAC RD and RDOQ can't read
  the live engine state because it encodes the whole frame's raster history.
  Each row starts from a snapshot published at macroblock (1, r-1) and
  advances it over its own committed decisions, WPP-style. The snapshot is
  about 460 bytes and holds just the contexts a 4:2:0 macroblock can reach.
  At 4:4:4 it keeps the full range.
- **`mb_qp_delta` pricing uses the QP map's raster predecessor**
  (`predict_prev_qp`) instead of the true entropy chain, at every thread count
  including one. The true chain depends on which predecessor actually coded a
  delta, so under mb-tree, AQ or CRF it makes output depend on where rows
  split.

Lookahead, frame typing, scene cuts and mb-tree run serially in input order on
lowres data and hand the wavefront finished per-macroblock maps. Rate control
updates once per frame in coding order. Nothing reads a peer's in-flight size
estimate.

The pool is `ntp_` in `src/common/threadpool.c`. It sees geometry, counters and
opaque tasks, never codec state. The pool has no progress getters on purpose.
A task that could read how far a racing row has got would depend on
scheduling, so the pool doesn't expose that value. A pool is built only when
the resolved width exceeds 1, so `--threads 1` is the serial path.

### Frames overlapping inside a GOP

The reference staircase is always on and lets a frame start before its
reference has finished. The frame's rows wait on the reference and stay at
least 4 macroblock rows behind it (`Y264_STAIR_LAG`). Its vertical motion
vectors into that reference are clamped so they never touch rows the
reference hasn't published. At the floor lag the clamp is +40 pixels. On
taller frames at narrower pools the lag widens and takes the clamp with it
(`stair_lag_for`). For runs of 2 or 3 B frames, the staircase also lets a
mini-GOP's non-reference B frames read a still-running reference B. It keeps
up to 3 bursts in flight (`Y264_STAIR_K`). The staircase needs B-pyramid and
stays off under VBV and some direct modes. It engages only when the instance
has a pool. The [B-frames](b-frames.md) and [rate control](rate-control.md)
docs cover how it meets those conditions.

### GOP-parallel workers

The CLI splits `--threads N` into g GOP workers times k wavefront threads and
prints the split:

```
yah264: encoded 250 frame(s) in 2 GOP(s) on 2 GOP-worker(s) x 4 frame-thread(s)
```

The split sets `g = min(N, GOP count)` and `k = ceil(N / g)`, and caps each
worker's share at the picture's wavefront cap. GOPs come first because they're
the more efficient level: they have no ramp triangles and no serial entropy
tail. The wavefront fills the cores the GOPs can't. Two cases change how the
threads go out:

- One GOP per worker, with GOPs of unequal length (a trailing partial GOP, or
  a cut-aware split). The g x k budget goes out greedily to the worker with
  the worst frames-per-thread. Workers already at the cap are skipped.
- More GOPs than workers. The queue hands GOPs out longest first. Each GOP
  gets `ceil(len * N / frames)` threads. That share finishes the longest GOP
  inside the throughput bound, so one long shot can't become the wall while
  the other cores idle.

Each worker is a whole encoder. At one frame thread a worker takes about
0.5 GiB at 1080p and 1.4 to 1.7 GiB at 2160p. `--max-memory` (default half of
physical RAM) caps the total. When the workers wouldn't fit, fewer of them run
with a wider wavefront each. The CLI says so on stderr.

Because a GOP's bits depend on its thread share, the split is part of the
output. Only the GOP table decides the split. Output is written in GOP
order however the queue ran.

`--open-gop` and `--intra-refresh` have no closed GOPs to split at. They run
one instance end to end and give `--threads` to its wavefront. `--dump-recon`
forces the fully serial path and warns when an explicit `--threads` above 1
can't be honored. The [options reference](options.md) lists the other cases.

### Streaming input and its memory window

The parallel path reads input on its own thread and never holds the whole
clip, so title length doesn't limit what can be encoded.

Workers free each frame as soon as the encoder takes it. The encoder copies
the picture into its lookahead ring and keeps no pointer to the caller's
buffer. Since a worker owns its GOP's frame range outright, nothing needs
reference counting.

File input uses the seek reader. A Y4M frame is a fixed-size record, so the
reader takes the frame count from the file length. It checks every frame
header against the first and fails loudly on a mismatch. The GOP table is
known before the first read. The reader reads in display order until the
schedule is published, then reads GOPs as the schedule hands them to workers.
It keeps each GOP in flight at most 16 frames ahead of its worker. The store
is therefore about `(GOPs in flight + 1) x 16` frames whatever the length:
272 frames (0.6 GiB) on a ten-minute 1920x818 title at default threads.

Pipe input has to be read in display order. Its window is
`(--threads + 1) x --keyint` frames, narrowed to read-ahead once workers
start. Early dispatch is on above one thread and hands GOPs out as the reader
reaches them. Above 1080p the store is capped at an eighth of physical RAM.

Frame slots live in fixed 512-frame segments, never one growing array,
because the reader grows the store while workers index into it.

Output is written in GOP order as GOPs finish. A read error after the first
GOP is out leaves bytes on disk. The exit code reports the failure.

The cut-aware split (`--cut-split`, `--shot-crf`, or a `--plan` or `--qpfile`
that forces a keyframe) reads a file twice. A scan pass keeps one byte per
frame and drops the pixels. The normal streaming encode then runs on the GOP
table the scan found. `--scan-out` and `--scan-in` reuse a scan. A pipe can't
be read twice, so these modes hold a piped input whole up to the memory limit
(half of physical RAM, `Y264_MAX_INPUT_MB`) and refuse past it.

Every streamed path writes the same bitstream a whole-input read would. Across
CQP, CRF, ABR and capped VBR at 1 to 8 threads, all 96 cells match byte for
byte. So do sweeps up to 18 threads.

### Repeatability

Two runs of one configuration at `--threads 1` must be byte-identical. That's
a hard gate. Above one thread, output doesn't have to be byte-identical across
runs or thread counts. Checks on it warn without failing. Threaded output
differs from serial by design. At `--threads 1` the encoder engages a
single-thread quality mode (`stq` in `src/encoder/mbcode.h`) that turns off
early-termination speed trades in motion search and partition choice. A
threaded encode usually repeats at a fixed thread count. Since nothing promises
it, an optimization that breaks the repeat can still ship. The correctness gate
stays hard at every thread count: the reconstruction matches the reference
decoders.

## What we chose and why

| choice | why |
|---|---|
| auto ceiling of 16 | Free on an 18-core Apple Silicon box at 1080p (1.01 s against 1.00 s uncapped). Occupancy fell from 13.9 cores to 12.1. The last cores were spinning. On asymmetric chips they run on efficiency cores and lengthen the critical path |
| auto counts every perf level | All 18 cores beat 12 at 1080p on both y4m input and in-process mp4 decode |
| auto resolved in the library | A caller that sets only `threads = 0` gets a threaded encode without computing core counts itself. One exported number keeps the CLI, ffmpeg and library callers from drifting apart |
| GOP parallelism stays in the caller | Inside one handle it needs whole-GOP input buffering, split rate control and output reordering. On 1080p one instance with a wide wavefront beat a GOP split on wall and spent 4.6% fewer bits, because the split forced keyframes |
| k=2 wavefront cap on narrow frames | At CIF it reads 12 workers against 7 for k=1. It measured +7.9% on foreman, +3.7% on bus and +3.1% on stefan |
| entropy as a trailing serial pass | It's the only raster-ordered state. Because nothing downstream of analysis needs bits, the next frame's analysis overlaps it |
| `predict_prev_qp` pricing at every width | Output can't depend on where rows split. Against the true chain it measured +0.00% BD |
| `--threads 1` quality mode | A serial encode has the speed margin. Turning the trades off costs about 6% wall for 0.4% fewer bits |
| greedy split keeps the g x k total | Trimming the oversubscribed total down to N read 0.89x on an 8-GOP clip. Wavefront ramp gaps let a sleeping worker free its core |
| per-frame input retirement | Holds 17 frames against 300 for whole-GOP retirement on 300 frames at 18 threads. It's about 3% faster from fewer page faults |
| seek reader for files | The store is bounded by construction. The in-order reader let a starving worker drag the reader far ahead: 6.6 GiB of store on a ten-minute title against 0.6 |

## What didn't work

| idea | why not |
|---|---|
| auto = performance cores only | Lost to all cores on both input types. The reading that favored it was I/O-bound on an external drive |
| more workers than the cap | 18 threads at CIF was 6 to 11% slower than 8. Extra workers contend for a diagonal that can't feed them |
| k=3 frame overlap in the cap | 16 workers at CIF, measured worse than 12 |
| staircase lag below 4 rows | Reads unwritten rows, so the output is unsound. It was worth only 1.01x on CIF |
| a minimum per-GOP thread share | Swept 1 to 6 at 18 threads. Any floor takes threads from the long GOP and gives them to short ones that can't use them |
| true-chain `mb_qp_delta` pricing | Makes output depend on the row split under mb-tree, AQ or CRF |
| frame-parallel model with in-flight RC | Rate control that plans against a peer's predicted size depends on thread count by construction |
| holding the whole input | 1.56 MB resident per frame of 720p clip. A two-hour 1080p title needed 501 GiB. The input memory limit is never raised. The input is chunked instead |
| waiting on a pipe for the GOP count | A serial read of the whole window before any GOP starts. Early dispatch is the default above one thread. One thread still waits |

## Open questions

- **Does the CLI's GOP-first split still earn its keep?** On 1080p one instance
  with a wide wavefront beats it on wall and bits. Forcing g from 1 to 4 at CIF
  read flat. The CLI still routes every planned encode through GOP workers.
  GOPs may win on long titles with many more GOPs than threads. Nobody has
  measured that against one instance.
- **Row-level VBV.** Not built. If it is, its QP moves must come from the
  analysis pass's committed estimates in fixed row order, never from actual
  bits mid-frame.

## Measuring it

- **Print cores next to wall.** An encode stuck on one core looks like a slow
  encoder. It gives correct output with no error. A harness that always passes
  `-threads` never exercises the path real invocations take, so test the
  default too.
- **`Y264_GOP_FORCE_G` and `Y264_GOP_FORCE_K` change the bytes.** A GOP's bits
  depend on its share, so a forced split is a different stream, not just a
  timing arm.
- **`/usr/bin/time -l` gives peak footprint.** When a whole-input buffer goes
  away, check for a per-open leak it had been hiding.
- **Run determinism checks on a loaded box** with spinners. Gate the
  non-default modes too (`ARGS=` on `scripts/determ_repeat.sh`). An idle box
  runs the threaded path the same way every time. Count distinct md5s instead
  of reading pass/fail. The [instruments catalog](instruments.md) has the
  recipe.
- **Time against local disk.** A source on an external drive makes I/O the
  limit and can turn a thread-count reading upside down.
- Daytime speed work is judged by instruction counts and CPU-seconds
  (`scripts/instr-ratio.sh`). `scripts/stress_threads.py` and
  `scripts/tsan_catch.sh` are the hang and race gates.
