# Streaming input for the parallel path

The parallel path reads its input on a thread of its own and never holds the
whole clip. Each GOP's bytes are written as soon as that GOP and every one
before it are done, so the output doesn't pile up either. Clip length no
longer limits what can be encoded. A two-hour 1080p title once needed 501 GiB
resident, and it now fits on an ordinary machine.

This page explains how that works and why it's built this way.
[options.md](options.md#memory-the-parallel-path-streams-through-a-bounded-window)
has the user-facing description and the current memory figures.

## Where it stands

- **File input** uses the seek reader. The frame count and the GOP table are
  known before the first read, and every Y4M frame is a fixed-size record one
  seek away, so the reader serves frames in the order the GOP workers consume
  them. It keeps each GOP in flight at most 16 frames ahead of its worker, so
  the store is about `(GOPs in flight + 1) x 16` frames, whatever the title's
  length.
- **Pipe input** has to be read in display order, so it uses a window of
  `(--threads + 1) x --keyint` frames, narrowed to read-ahead once the
  workers start. Above 1080p the store is also capped at an eighth of
  physical RAM.
- **The cut-aware split** (`--cut-split`, `--shot-crf`, and a `--plan` or
  `--qpfile` that forces a keyframe) reads a file twice. A scan pass hands
  each frame's luma to the streamed pre-scan and drops it, keeping a few
  numbers per frame, and then the normal streaming encode runs on the GOP
  table the scan produced. `--scan-out` and `--scan-in` save and reuse the
  scan, so one scan serves a whole CRF ladder. A pipe can't be read twice, so
  on a pipe these modes still hold the whole input, up to the memory limit.
- **Two-pass** needs the GOP split before pass 1 writes its per-GOP stats, so
  it needs a seekable input. Two-pass reads the input twice anyway.
- **The GOP workers** are now most of the memory. Each is a whole encoder,
  about 0.5 GiB at 1080p. `--max-memory MIB` sets a budget for the encode,
  half of physical RAM by default, and when the workers wouldn't fit, fewer
  GOPs run at once with a wider wavefront each.

Each of these paths writes the same bitstream as the whole-input read it
replaced.

## How it works

### A frame is freed as soon as the encoder takes it

When the encode call returns for a frame, the encoder has already copied the
picture into its lookahead ring, and nothing inside it keeps a pointer to the
caller's buffer. So a GOP worker can free each input frame right after
handing it over. Nothing needs a per-frame reference count, because a worker
owns its GOP's frame range outright.

Freeing per frame instead of per GOP keeps the resident set far below the
window. We measured it on 300 frames of sintel at `--keyint 25` and 18
threads, where 12 GOP workers run:

| retirement | frames held | window |
|---|---:|---:|
| per frame | 17 | 208 |
| whole GOP at once | 300 | 325 |

It also saves about 3% of wall time at 18 threads, which we didn't expect.
The whole-clip read had been causing about 250 MB of first-touch page faults
and fresh mappings while the encode wanted every core. Page reclaims fell from
41951 to 31592.

Peak memory on the 180-frame samsung clip:

| threads | whole GOP retired at once | per-frame retirement |
|---|---:|---:|
| 1 | 447.7 MB | 246.6 MB |
| 18 | 650.1 MB | 387 MB |

### The store never moves under a worker

Frame slots live in fixed segments of 512 frames instead of one growing array.
The reader grows the store while workers index into it, and reallocating one
array would move it out from under a worker mid-GOP. A segment is 12 KiB of
pointers, so segments stay until teardown. Even a two-hour title's worth is
4 MB.

### The window is a budget, and the worst case is still reachable

A worker that needs a frame the reader hasn't reached yet lets the reader run
past its window, so any window of one frame or more is correct and the output
never depends on the window size. `Y264_STREAM_WINDOW` overrides it, with a
floor of twice the keyframe interval. The cost is that the window is a budget
and not a bound, which is why we moved file input to the seek reader. Under
the old in-order reader, every worker started at once, the one given the
sixteenth GOP of a ten-minute title waited for a frame 3750 frames in, and the
store grew to everything in between.

On a pipe the worst case is `(--threads + 1) x --keyint` frames. At the
default keyint a two-hour 1080p title needs 5.8 GiB of window on 8 threads
and about 14 GiB, 4750 frames, on 18. It's
reached only when the split waits for the GOP count before any worker starts,
which happens on one thread on a pipe or with a forced split. The memory
refusal quotes this worst case instead of the usual read-ahead, because it
can still happen. The limit is half of physical RAM, overridable with
`Y264_MAX_INPUT_MB`. It's measured against physical RAM because macOS can
reclaim most of what it reports as inactive or purgeable on demand, and a
free-page check refuses encodes that would have run.

### Output is written in GOP order as it finishes

Holding every GOP's bytes to the end is small next to the input, about 9 GB
for a two-hour title at 10 Mbit/s, but it grows with the title the same way.
So the writer emits the lowest-numbered GOP that's finished and not yet
written. One consequence: a read error after the first GOP is out finds bytes
already written, and the exit code is what reports the failure. The
alternative, holding the whole stream so it can be withdrawn, is what this
design exists to avoid.

### The schedule doesn't need the whole input

Every scheduling decision on the parallel path depends on the frame count.
Without it the encoder would fall back to an even split of threads across
GOPs, which loses the frame-weighted split and the longest-first queue and
changes the ABR stream. Holding one GOP worker over 250 frames of sintel 720p
and sweeping its thread share gives two different ABR outputs, with the
boundary at 8 threads, and one output each for CQP and CRF.

**On a file, the length gives the count.** A Y4M frame is a fixed-size record
once the stream header is past. The reader reads the first `FRAME` header,
seeks back, and checks that the rest of the file divides exactly into frames.
The count is then known before any pixel data is read, and every scheduling
decision matches what a whole-input read would compute. Divisibility doesn't
prove every frame header is the same length, so the reader also checks each
header against the first and fails loudly instead of encoding a miscounted
clip.

**On a pipe, the dispatcher waits.** It holds the first dispatch until either
end of input or the `(--threads + 1)`th GOP boundary arrives. If the input
ends first, the count is exact and the full schedule applies. If the boundary
comes first, there are more GOPs than threads, which fixes one thread per GOP
with one GOP worker per thread. The longest-first queue computes the same
thing without the count: once the clip is longer than `threads x keyint`, each
GOP's share rounds to exactly one thread, and full GOPs tie on length, so the
order stays as is. The ABR split never sees a different thread share, and the
wait is bounded by the window, which is resident anyway.

The streamed and whole-input reads were compared bit for bit:

| arms | cells | result |
|---|---:|---|
| whole input, streamed file and streamed pipe | 96 | identical |
| whole input and streamed file | 24 | identical |
| streamed file and streamed pipe | 60 | identical |

The 96 cells are sintel 720p and bus CIF across CQP 26, CRF 22, ABR 2000 and
capped VBR, at 1, 2, 4 and 8 threads and keyint 10, 20 and 25. Those were
chosen so there are more GOPs than threads and the streamed split is really
exercised, not cut short by an early end of input. The other two sweeps cover
keyint 50 and 250 and go up to 18 threads. Since the bits don't move, no BD
round was needed.

### The cut-aware scan streams too

The pre-scan that finds the cuts runs the same per-frame step as the
whole-input scan over a small ring of lowres frames, and keeps one byte per
frame of keyframe map. A frame's decision needs the next frame, for the flash
guard, so it's settled one frame late, and the final flush settles the last
frame. The decisions match the whole-input scan frame for frame.

## What it measured

Peak RSS on 720p at `--threads 1 --keyint 50`:

| input frames | whole input resident | windowed |
|---|---:|---:|
| 180 | 503 MB | 378 MB |
| 450 | 930 MB | 384 MB |
| 900 | 1631 MB | 384 MB |
| 1800 | 3035 MB | 385 MB |

The windowed column is flat: memory per frame of clip falls from 1.559 MB to
0.004. With `Y264_STREAM_STAT=1` the encoder reports the store's high-water
mark. It read a full window of 100 frames at every length, so the cap holds by
construction.

An early version of the window still grew by 0.116 MB per frame, and the
growth followed the number of GOPs, not frames. The encoder's close was
leaving nine allocations behind, 5.67 MB per open and close. The whole-input
read had hidden it under a term 13 times larger, so a per-cycle leak is worth
checking for whenever a whole-input buffer goes away.

Streaming didn't buy throughput when everything fits in RAM. We ran N
single-threaded encodes of 450 frames at once, at `--keyint 50`, and timed
the wall seconds for all N, median of three:

| N | whole input | windowed |
|---|---:|---:|
| 1 | 9.00 | 9.16 |
| 4 | 9.61 | 9.81 |
| 8 | 11.13 | 11.08 |
| 18 | 15.02 | 15.17 |

The ratio stays between 0.98x and 1.01x, which is noise. Both arms fit on the
64 GiB machine, since 18 copies need 16.7 GB whole-input and 7.6 GB windowed.
Scaling from 1 to 18 encodes, yah264 keeps about 10.8 top-core equivalents of
18 busy in either arm, against 11.93 for x264. That gap didn't move with the
memory change, so resident size isn't what limits it.

Streaming helps a little once the work no longer fits in memory:

| 18 concurrent encodes of 1800 frames | memory wanted | wall, median |
|---|---:|---:|
| whole input | 54.6 GB | 64.08 s |
| windowed | 6.9 GB | 60.86 s |

That's a 1.05x gain. A single run of the same cell read 1.22x, which is why
the figure is a median.

The main gain was never throughput. It's that a long title encodes at all.

## What's left

Pipe input for the cut-aware split is the one path that still holds the
whole input, and it's refused once that reaches the memory limit.
`--scan-in` and `--scan-out` need a file too, so the advice is to encode from
a file.

The pipe window's worst case is large at high thread counts. Lowering
`--keyint` or `--threads`, or setting `Y264_STREAM_WINDOW`, keeps it down.
