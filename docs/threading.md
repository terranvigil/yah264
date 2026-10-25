# Threading

yah264 has two levels of parallelism. Inside one encoder instance the library
runs a macroblock-row wavefront and lets consecutive frames of a GOP overlap on
it. Across instances the command-line tool runs GOP-parallel workers, each a
whole encoder coding its own closed GOP. The library owns what one instance
does with a thread budget. The caller owns how a budget is split across
instances.

The site page [threading.html](threading.html) walks through the wavefront and
the auto rule with diagrams. This doc is the developer's version: the
mechanism, the defaults and the dead ends.

## How it works

### How auto picks the thread count

`param.threads` is the budget for one encoder instance: 0 asks the library to
decide, 1 is serial, N means up to N. `frame_threads`, the explicit wavefront
width, wins when it's set, because the CLI's GOP workers each carry their own
share. The resolved width is

```
min(request or auto, yah264_frame_thread_cap(width, height))
auto = min(online cores, 16)
```

The core count comes from `y264_machine_threads()` in `src/common/cpu.c`. On
Apple Silicon it enumerates `hw.nperflevels` and sums every level, with no
assumption about which tier is the performance one. Elsewhere it's
`sysconf(_SC_NPROCESSORS_ONLN)`. The ceiling of 16 applies to auto only. An
explicit request passes through, still clamped by the picture.

`yah264_frame_thread_cap` depends on the picture alone. One frame's diagonal
retires in `2(R-1)+C` macroblock-times (R rows, C columns), and k frames
overlapping `lag` rows apart retire in `2(R-1)+C+(k-1)*2*lag`, so the cap is
`k*R*C` over that, floored at `Y264_MT_POOL_MIN` (2). A frame is narrow when its
one-frame cap is under 16. Narrow frames are sized for k=2 (an anchor and one
frame row-gated onto it), wider ones for k=1. `frame_thread_cap_k` in
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
`-threads N` passes straight through and omitting it gets auto. The CLI's
`--threads` defaults to the same auto.

### The macroblock-row wavefront

Each macroblock needs its left, top, top-left and top-right neighbors finished.
Row r may code macroblock x once row r-1 has finished x+2, so the work advances
as a diagonal. The schedule is a pure function of frame geometry. Threads race
to execute cells, never to decide what a cell reads.

Analysis and reconstruction run on the wavefront. Everything with a raster
total order lives in one serial entropy pass per frame that trails the rows:
the CABAC engine and its adaptive contexts, the CAVLC bitstream and skip run,
and the `mb_qp_delta` chain. That pass consumes committed per-macroblock
records and never feeds analysis. Two things make the split hold:

- **RD bit estimates use a row-private context.** CABAC RD and RDOQ can't read
  the live engine state, which encodes the whole frame's raster history. Each
  row starts from a snapshot published at macroblock (1, r-1) and advances it
  over its own committed decisions, WPP-style. It's about 460 bytes, just the
  contexts a 4:2:0 macroblock can reach (4:4:4 keeps the full range).
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
opaque tasks, never codec state. It has no progress getters on purpose: a task
that could read how far a racing row has got would depend on scheduling, so
that value isn't observable. A pool is built only when the resolved width
exceeds 1, so `--threads 1` is the serial path.

### Frames overlapping inside a GOP

The reference staircase, always on, lets a frame start before
its reference has finished. Its rows are row-gated on the reference at least 4
macroblock rows behind (`Y264_STAIR_LAG`), and its vertical motion vectors into
that reference are clamped so they never touch rows the reference hasn't
published. At the floor lag the clamp is +40 pixels. On taller frames at
narrower pools the lag, and with it the clamp, widens (`stair_lag_for`). The
staircase also lets a mini-GOP's non-reference B frames read a still-running
reference B, for runs of 2 or 3 B frames, and keeps up to 3 bursts in flight
(`Y264_STAIR_K`). It needs B-pyramid, stays off under VBV and some direct
modes, and engages only when the instance has a pool. [b-frames.md](b-frames.md)
and [rate-control.md](rate-control.md) cover how it meets those.

### GOP-parallel workers

The CLI splits `--threads N` into g GOP workers times k wavefront threads and
prints the split:

```
yah264: encoded 250 frame(s) in 2 GOP(s) on 2 GOP-worker(s) x 4 frame-thread(s)
```

- **g = min(N, GOP count)** and **k = ceil(N / g)**, with each worker's share
  capped at the picture's wavefront cap. GOPs come first because they're the
  more efficient level, with no ramp triangles and no serial entropy tail. The
  wavefront fills the cores the GOPs can't.
- **One GOP per worker, GOPs of unequal length** (a trailing partial GOP, or a
  cut-aware split): the g x k budget goes out greedily to the worker with the
  worst frames-per-thread, skipping workers already at the cap.
- **More GOPs than workers:** the queue hands GOPs out longest first, and each
  GOP gets `ceil(len * N / frames)` threads. That share finishes the longest
  GOP inside the throughput bound, so one long shot can't become the wall while
  the other cores idle.
- **Memory:** each worker is a whole encoder, about 0.5 GiB at 1080p and 1.4 to
  1.7 GiB at 2160p at one frame thread. `--max-memory` (default half of
  physical RAM) caps the total. When the workers wouldn't fit, fewer run, each
  with a wider wavefront, and the CLI says so on stderr.

A GOP's bits depend on its thread share, so the split is part of the output.
The split is a function of the GOP table alone, and output is written in GOP
order however the queue ran.

`--open-gop` and `--intra-refresh` have no closed GOPs to split at, so they run
one instance end to end and `--threads` goes to its wavefront. `--dump-recon`
forces the fully serial path and warns when an explicit `--threads` above 1
can't be honored. [options.md](options.md) lists the other cases.

### Streaming input and its memory window

The parallel path reads input on a thread of its own and never holds the whole
clip, so title length doesn't limit what can be encoded.

- **Workers free each frame as soon as the encoder takes it.** The encoder
  copies the picture into its lookahead ring and keeps no pointer to the
  caller's buffer, and a worker owns its GOP's frame range outright, so nothing
  needs reference counting.
- **File input uses the seek reader.** A Y4M frame is a fixed-size record, so
  the reader takes the frame count from the file length and checks every frame
  header against the first, failing loudly on a mismatch. The GOP table and the
  whole schedule are known before the first pixel read. The reader keeps each
  GOP in flight at most 16 frames ahead of its worker, so the store is about `(GOPs in flight + 1) x 16`
  frames whatever the length: 272 frames (0.6 GiB) on a ten-minute 1920x818
  title at default threads.
- **Pipe input** has to be read in display order. The window is
  `(--threads + 1) x --keyint` frames, narrowed to read-ahead once workers
  start. Early dispatch, on above one thread, hands GOPs out
  as the reader reaches them. Above 1080p the store is capped at an eighth of
  physical RAM.
- **Frame slots live in fixed 512-frame segments**, never one growing array,
  because the reader grows the store while workers index into it.
- **Output is written in GOP order as GOPs finish.** A read error after the
  first GOP is out leaves bytes on disk, and the exit code reports the failure.
- **The cut-aware split** (`--cut-split`, `--shot-crf`, or a `--plan` or
  `--qpfile` that forces a keyframe) reads a file twice. A scan pass keeps one
  byte per frame and drops the pixels, then the normal streaming encode runs on
  the GOP table it found. `--scan-out` and `--scan-in` reuse a scan. A pipe
  can't be read twice, so these modes hold a piped input whole, up to the
  memory limit (half of physical RAM, `Y264_MAX_INPUT_MB`), and refuse past it.

Every streamed path writes the same bitstream a whole-input read would: 96
cells across CQP, CRF, ABR and capped VBR at 1 to 8 threads, plus sweeps up to
18 threads, all byte-identical.

### Repeatability

Two runs of one configuration at `--threads 1` must be byte-identical. That's
a hard gate. Above one thread, byte-identical output isn't required across runs
or across thread counts, and checks on it warn without failing. Threaded output
differs from serial by design: at `--threads 1` the encoder engages a
single-thread quality mode (`stq` in `src/encoder/mbcode.h`) that turns off
early-termination speed trades in motion search and partition choice. A
threaded encode usually repeats at a fixed thread count, but nothing promises
it, so an optimization that breaks that can still ship. The correctness gate
stays hard at every thread count: the reconstruction matches the reference
decoders.

## What we chose and why

| choice | why |
|---|---|
| auto ceiling of 16 | Free on an 18-core Apple Silicon box at 1080p (1.01 s against 1.00 s uncapped), and occupancy fell from 13.9 cores to 12.1. The last cores were spinning, and on asymmetric chips they land on efficiency cores and lengthen the critical path |
| auto counts every perf level | All 18 cores beat 12 at 1080p on both y4m input and in-process mp4 decode |
| auto resolved in the library | A caller that sets only `threads = 0` gets a threaded encode without computing core counts itself, and one exported number keeps the CLI, ffmpeg and library callers from drifting apart |
| GOP parallelism stays in the caller | Inside one handle it needs whole-GOP input buffering, split rate control and output reordering. On 1080p one instance with a wide wavefront beat a GOP split on wall and spent 4.6% fewer bits, because the split forced keyframes |
| k=2 wavefront cap on narrow frames | At CIF it reads 12 workers against 7 for k=1, and measured +7.9% foreman, +3.7% bus, +3.1% stefan |
| entropy as a trailing serial pass | It's the only raster-ordered state, and nothing downstream of analysis needs bits, so the next frame's analysis overlaps it |
| `predict_prev_qp` pricing at every width | Output can't depend on where rows split, and it measured +0.00% BD against the true chain |
| `--threads 1` quality mode | A serial encode has the speed margin. Turning the trades off costs about 6% wall for 0.4% fewer bits |
| greedy split keeps the g x k total | Trimming the oversubscribed total down to N read 0.89x on an 8-GOP clip. Wavefront ramp gaps let a sleeping worker free its core |
| per-frame input retirement | 17 frames held against 300 for whole-GOP retirement (300 frames, 18 threads), and about 3% faster from fewer page faults |
| seek reader for files | The store is bounded by construction. The in-order reader let a starving worker drag the reader far ahead: 6.6 GiB of store on a ten-minute title against 0.6 |

## What didn't work

| idea | why not |
|---|---|
| auto = performance cores only | Lost to all cores on both input types. The reading that favored it was I/O-bound on an external drive |
| more workers than the cap | 18 threads at CIF was 6 to 11% slower than 8. Extra workers contend for a diagonal that can't feed them |
| k=3 frame overlap in the cap | 16 workers at CIF, measured worse than 12 |
| staircase lag below 4 rows | Reads unwritten rows, so the output is unsound, and it was worth only 1.01x on CIF |
| a minimum per-GOP thread share | Swept 1 to 6 at 18 threads. Any floor takes threads from the long GOP and gives them to short ones that can't use them |
| true-chain `mb_qp_delta` pricing | Makes output depend on the row split under mb-tree, AQ or CRF |
| frame-parallel model with in-flight RC | Rate control that plans against a peer's predicted size depends on thread count by construction |
| holding the whole input | 1.56 MB resident per frame of 720p clip; a two-hour 1080p title needed 501 GiB. The input memory limit is never raised; the input is chunked instead |
| waiting on a pipe for the GOP count | A serial read of the whole window before any GOP starts. Early dispatch is the default above one thread; one thread still waits |

## Open questions

- **Does the CLI's GOP-first split still earn its keep?** On 1080p one instance
  with a wide wavefront beats it on wall and bits, and forcing g from 1 to 4 at
  CIF read flat. The CLI still routes every planned encode through GOP workers.
  Long titles with many more GOPs than threads may be where GOPs win, and
  nobody has measured that against one instance.
- **Row-level VBV.** Not built. If it is, its QP moves must come from the
  analysis pass's committed estimates in fixed row order, never from actual
  bits mid-frame.

## Measuring it

- **Print cores next to wall.** An encode stuck on one core looks like a slow
  encoder, with correct output and no error. A harness that always passes
  `-threads` never exercises the path real invocations take, so test the
  default too.
- **`Y264_GOP_FORCE_G` and `Y264_GOP_FORCE_K` change the bytes.** A GOP's bits
  depend on its share, so a forced split is a different stream, not just a
  timing arm.
- **`/usr/bin/time -l` gives peak footprint.** When a whole-input buffer goes
  away, check for a per-open leak it had been hiding.
- **Run determinism checks on a loaded box** with spinners, and gate the
  non-default modes too (`ARGS=` on `scripts/determ_repeat.sh`). An idle box
  runs the threaded path the same way every time. Count distinct md5s instead
  of reading pass/fail. [instruments.md](instruments.md) has the recipe.
- **Time against local disk.** A source on an external drive makes I/O the
  limit and can turn a thread-count reading upside down.
- Daytime speed work is judged by instruction counts and CPU-seconds
  (`scripts/instr-ratio.sh`). `scripts/stress_threads.py` and
  `scripts/tsan_catch.sh` are the hang and race gates.
