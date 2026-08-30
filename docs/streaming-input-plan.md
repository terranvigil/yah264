# Streaming input for the parallel path

`encode_threaded` reads the input on its own thread through a bounded window
instead of slurping the clip, and writes each GOP as it finishes instead of
buffering the whole output. Clip length is not the ceiling.

## The window

A frame is dead as soon as `yah264_encoder_encode` returns for it: the call
pads the picture into the lookahead ring slot (or into `e->plane` with the
window off) before it returns, and nothing in the encoder keeps a pointer into
the caller's picture. So a worker waits per frame and retires per frame, and the
window is not sized by slices at all. What stays resident is read-ahead,
`(g + 1) x 16` frames, and that is a budget rather than a bound: a worker blocked
on an unread frame opens a valve that lets the reader past the window, so any
window >= 1 is correct and `Y264_STREAM_WINDOW=4` is byte-identical.
`Y264_STREAM_WINDOW` overrides the budget outright.

The worst case a pathological schedule can still reach is a GOP retiring as a
unit:

    window = (--threads + 1) x --keyint frames

At the default keyint of 250 that is 2000 frames on 8 workers and 4750 on 18. A
two-hour 1080p title goes from 501 GiB to 5.8 or 13.7 GiB, and stops depending on
clip length at all. `--keyint` scales it linearly. The memory refusal quotes this
worst-case figure rather than the read-ahead budget, because the valve means it
is still reachable.

Slots live in fixed 512-frame segments, not one `realloc`'d array. The reader
grows the store while workers index it, and a `realloc` would move the array out
from under a worker mid-GOP. Segments are 12 KiB of pointers each, so they are
never freed until teardown: even a two-hour title's worth is 4 MB.

A GOP worker owns `[start, end)` and frees those frames when it publishes, which
is the simplest ownership rule available and needs no per-frame refcount. The
encoder is closed before the free, so nothing inside it still points at them.

The output streams the same way. Holding every GOP's bytes to the join is small
next to the input (9 GB for a two-hour title at 10 Mbit/s) but grows without
bound in exactly the same way, so the writer takes the lowest un-emitted
completed GOP instead. One consequence worth naming: a read error past the first
published GOP finds bytes already written, and the exit code is what says so.
Holding the whole stream to be able to withdraw it is the alternative, and it is
the thing this design exists to stop.

## Knowing `n` without reading the input

A streaming reader that does not know the total frame count `n` loses the
frame-weighted greedy (`uneven`) split and the longest-first queue (`queued`),
falls back to `Y264_GOP_EVEN=1`, and therefore **moves ABR bits**: the drain
split engages at `k=8` and the hash changes there. Forcing one GOP worker over
250 frames of sintel_720p and sweeping its thread share gives two ABR hashes
with the boundary exactly at 8, against one hash for CQP and CRF.

Streaming does not imply the even split, though. **A Y4M frame is a fixed-size
record once the stream header is past**, so for a seekable input the file
*length* is the frame count. Read the first `FRAME` header, seek back, check that
the remainder divides by header+payload exactly. `n` is then known before a byte
of pixel data is read, every scheduling decision is the one the whole-input read
computed, and the only thing that changes is when the pixels arrive.
Divisibility is not a proof that every header is six bytes, so the reader also
checks each header against the first and fails loudly instead of encoding a
miscounted clip.

A pipe has no length, and the answer there is still not the even split. Hold the
dispatch until either EOF or the `(--threads + 1)`'th GOP boundary arrives:

- **EOF first.** `n` is exact. Full schedule, including both heuristics.
- **The boundary first.** `n_gops > nthreads`, which fixes `g` to `nthreads` and
  `k = 1`. The longest-first queue would compute the same thing without needing
  `n`: its per-GOP share `ceil(len * nthreads / n)` is exactly 1 for every GOP
  once `n > nthreads * keyint`, and its order is the identity because every full
  GOP ties on length and the short tail sorts last.

So the `k` that ABR's drain split keys on never changes, and the wait is bounded
by the window, which is resident anyway. Measured, three-way:

| Arms | Cells | Result |
| --- | --- | --- |
| main == streamed file == streamed pipe | 96 | identical |
| main == streamed file | 24 | identical |
| streamed file == streamed pipe | 60 | identical |

The 96 cells are sintel_720p and bus_cif across CQP 26, CRF 22, ABR 2000 and
capped-VBR, at 1/2/4/8 threads and keyint 10/20/25, chosen so `n_gops >
nthreads` and the streamed split is genuinely live, not short-circuited by an
early EOF. The other two sweeps cover keyint 50 and 250 and run to 18 threads.
The bits do not move, so no BD round is needed.

## What still reads the whole input

**The cut-aware split.** `yah264_scan_idr_frames` takes `luma[0..n)` and needs
every frame at once, and the boundaries it produces are the dispatcher's input,
so there is nothing to dispatch until it has run. It keeps the whole-input read
and the refusal that goes with it. That is a deliberate trade: `Y264_CUT_SPLIT`
is off by default and buys 17.4% of samsung_720p's t18 wall when it is on, and
downgrading it to arithmetic boundaries to fit the window would spend that to buy
memory nobody asked to save on that path.

It is incrementally computable when someone wants it. `sc_down_task` and
`sc_cost_task` are per-frame, `sc_exact_pcost` compares frame *i* against *i-1*,
and the replay of `la_finalize`'s state machine walks forward carrying
`since_idr`. Keeping one previous lowres frame is enough. A GOP's end is then
known at the next cut or at `keyint`, whichever comes first, which is exactly
when the dispatcher needs it. The pre-scan also holds its own whole-clip arrays
of half-res luma plus an int32 per macroblock, which measured +242 MiB over 1253
frames of 720p, so making it incremental retires that too.

**Two-pass.** Both halves of the stats round-trip are indexed by the GOP split,
so pass 1 needs the split before it writes its per-GOP files. It needs a seekable
input, which is not a real restriction: two-pass reads the input twice by
definition, and a pipe cannot.

## What it measured

Peak RSS on 720p at `--threads 1 --keyint 50`, whole-input against windowed:

| Input frames | Whole-input | Windowed |
| --- | --- | --- |
| 180 | 503 MB | 378 MB |
| 450 | 930 MB | 384 MB |
| 900 | 1631 MB | 384 MB |
| 1800 | 3035 MB | 385 MB |

Flat. 1.559 MB per frame of clip becomes 0.004. `Y264_STREAM_STAT=1` reports the
window's high-water mark and it reads 100 frames of 100 at every length, so the
store is capped by construction, not by luck.

Per-frame retirement takes the resident set well under the window budget.
Measured on 300 frames of sintel at `--keyint 25` and 18 threads, where 12 GOP
workers run, `Y264_STREAM_STAT` reports 17 frames resident of a 208-frame window,
against 300 of 325 when a GOP retired as a unit. Peak RSS on samsung 180f is
246.6 MB at t1 (from 447.7) and 387 MB at t18 (from 650.1).

**Trap: a per-encoder-cycle leak hides under a whole-input read.** A first cut of
the window still crept at 0.116 MB/frame, and the residue scaled with the GOP
count rather than the frame count, which is to say with the number of times a
worker opens and closes an encoder: at a fixed 200-frame window and 1800 input
frames, 18 GOPs cost 623 MB, 36 cost 711 and 72 cost 894.
`yah264_encoder_close` was leaving nine allocations behind, three of them the
buffered B source planes, and `leaks --atExit` put it at 5.67 MB a cycle. The
whole-input read had been burying it under a term 13x larger.

## What it did not buy

Aggregate throughput under real concurrency, N single-threaded encodes of 450
frames at `--keyint 50`, medians of 3, wall seconds for all N:

| N | Whole-input | Windowed | x264 |
| --- | --- | --- | --- |
| 1 | 9.00 | 9.16 | 13.71 |
| 4 | 9.61 | 9.81 | 14.27 |
| 8 | 11.13 | 11.08 | 15.64 |
| 18 | 15.02 | 15.17 | 20.69 |

That is 0.98x to 1.01x, which is nothing. Both arms fit in RAM at this size (18
copies is 16.7 GB whole-input against 7.6 GB windowed on a 64 GiB box), and the
machine-capacity deficit does not move with them: scaling 1 to 18, x264 keeps
11.93 top-core equivalents of 18 and yah264 keeps 10.79 whole-input and 10.87
windowed. So the deficit is not resident-size-bound: peak RSS is not the hot set.
Do not expect the memory arm to pay for itself here.

Push the working set past the box and it does turn up, modestly. At 18 concurrent
1800-frame encodes, whole-input wants 54.6 GB and windowed wants 6.9: 64.08 s
against 60.86, a 1.053x. x264 takes 81.46. A single run of that cell read 1.217x,
which is why it is quoted from medians.

The headline is not throughput. It is that the encode runs at all: a two-hour
1080p title needed 501 GiB and now needs 5.8 on 8 workers.

Per-frame retirement also buys about 3% of t18 wall, which the memory arm did not
predict. The whole-clip read was 250 MB of first-touch page faults and fresh
mmaps running alongside an encode that wants every core; page reclaims fall from
41951 to 31592.
