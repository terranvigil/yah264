# yah264 options reference

Every option the encoder accepts, what it defaults to, and how it interacts with
the others. Read against `cli/yah264_cli.c`, `include/yah264.h` and
`src/encoder/params.c`. Where this file and the code disagree, the code wins and
this file is the bug.

Rate control has its own guide: [rate-control.md](rate-control.md). Read that
one before picking `--crf` or `--bitrate`; this file only says what the flags
are, not which to reach for.

## Contents

- [Invoking it](#invoking-it)
- [What the bare default is](#what-the-bare-default-is)
- [The option table](#the-option-table)
- [Presets](#presets)
- [Tunes](#tunes)
- [Which options change the bitstream shape](#which-options-change-the-bitstream-shape)
- [Threading, and what it does to your output](#threading-and-what-it-does-to-your-output)
- [x264 compatibility](#x264-compatibility)
- [Calling it from C](#calling-it-from-c)
- [Environment variables](#environment-variables)
- [Rough edges](#rough-edges)

## Invoking it

```sh
yah264 --input-y4m <in.y4m|-> [-o <out.264|->] [options]
```

Input is Y4M and nothing else. There is no raw-YUV mode, no container demuxer,
and no `--input-res` or `--fps` override: width, height, frame rate and chroma
format all come from the Y4M header. Pipe from ffmpeg for anything else:

```sh
ffmpeg -i input.mp4 -f yuv4mpegpipe - | yah264 --input-y4m - --crf 23 -o out.264
```

Output is an Annex-B elementary stream. `-` means stdin/stdout for either side.

The Y4M `C` tag is parsed for `420`, `422` and `444`, plus their `p10`/`p12`
high-depth forms. **The input's bit depth must match the build's**: an 8-bit
binary refuses a 10-bit file rather than converting it, and tells you to rebuild
with `meson setup build -Dbit_depth=10`. One binary is one bit depth.

## What the bare default is

`yah264 --input-y4m in.y4m -o out.264` with no other flags is deliberately
`x264 --preset medium`'s tool-set, so the two are comparable without an argument
list on either side. That means preset medium, CABAC, `--ref 3`, `--bframes 3`,
8x8 transform on, adaptive B on, `--rc-lookahead 40`, `--keyint 250`.

It also means **constant QP at 26**, because no rate-control flag was given.
That is the one part of the default that is not an x264 match: x264's bare
default is CRF 23. If you want constant quality you have to ask for it.

`--psy-rd` defaults to 2.0, this encoder's own swept value rather than an x264
match, and `--psy-trellis` to 0.0, which is x264 medium's.

## The option table

Defaults marked "preset" are set by the preset ladder and the bare default is
medium's row; see [Presets](#presets).

**A value outside an option's domain is refused, naming the domain.** A
non-numeric value and an out-of-range one both exit 2 with a message like
`yah264: --ref expects 1..2147483647 (got '0')`. Parsing is lenient about the
*form* of the number, so `--bitrate 800.0` works because that is what
`str(float)` produces, but a non-integral value for an integer option is refused
rather than truncated.

Enum-valued options refuse an unknown name the same way, so a typo and x264's
unimplemented `--direct none` are both rejected rather than silently read
as something else.

### Input and output

| Option | Argument | Default | What it does |
| --- | --- | --- | --- |
| `--input-y4m` | path or `-` | required | Y4M input. No other input format exists. |
| `-o`, `--output` | path or `-` | `-` (stdout) | Annex-B output. |
| `--frames` | N | 0 = all | Stop after N input frames. |
| `--dump-recon` | path | off | Write the encoder's own reconstruction as Y4M, in display order. **Forces the single-threaded path** (see below), and an explicit `--threads` above 1 is warned about rather than dropped. |
| `--version` | | | Print version, exit. |
| `-h`, `--help` | | | Print usage, exit. |

### Rate control

Covered properly in [rate-control.md](rate-control.md). The flags:

| Option | Argument | Default | What it does |
| --- | --- | --- | --- |
| `--qp` | 0..51 | 26 | Constant QP. This is the mode you get if you name no other. |
| `--bitrate` | kbit/s | off | Single-pass ABR average bitrate. |
| `--crf` | ~0..51 | off | Constant rate factor. Accepts a decimal but see the staircase note in the RC guide. |
| `--vbv-maxrate` | kbit/s | 0 = off | VBV peak rate. Needs `--vbv-bufsize` too; either alone does nothing. |
| `--vbv-bufsize` | kbit | 0 = off | VBV buffer size. |
| `--pass` | 1 or 2 | off | Two-pass: 1 writes stats, 2 reads them. Pair with `--bitrate`. |
| `--stats` | path | `yah264.stats` | Two-pass statistics file. |
| `--aq-strength` | float | 0.4 rate-controlled, 0.0 at CQP | Variance adaptive quantisation. 0 disables. |
| `--abr-model` | `default`, `rf` | `default` | ABR bit allocation. `rf` (spelled `x264` in older scripts, still accepted) spends a given bitrate markedly better and hits it less reliably; see below. |
| `--rc-lookahead` | frames | preset (40 at medium) | mb-tree propagation window. 0 turns the window off. |

### `--abr-model rf`, and why it is not the default

`default` is the shipped allocator. `rf` selects the rate-factor one: a self-normalising
rate factor for P frames, with I and B anchored to the running non-B QP track.
It is opt-in because the two things an ABR encoder does, spending a bitrate well
and hitting it exactly, come apart here: it is better at the first and worse at
the second.

**Spending it.** At 1200 kbit/s on samsung_720p the default's I frames are 3.4x
smaller than x264's and its B frames 2.7x larger; `--abr-model rf` puts both
within a few percent of x264's own sizes. Across the 12-clip band that is a
median **-5.65% BD-rate**, 9 clips better (samsung -26.6%, akiyo -49.2%), 3
worse (ducks +5.8%).

**Hitting it.** On content whose complexity signal collapses, a near-black
opening being the usual way, the rate factor runs away and this model has no
swing limit to absorb it: sintel reads **+24.5% over target** at 900 frames.
Elsewhere the error is a warm-up transient that decays with length (ducks
+11.4% at 180 frames, +4.6% at 480).

So: choose it when quality per bit matters more than landing the target
exactly, and do not choose it for a hard bitrate budget on unknown content.

**`--qp`, `--bitrate` and `--crf` each name a mode, and the last one on the
command line wins**, which is x264's rule. A warning on stderr names what the
loser still does:

```
yah264: warning: --crf 23 is dropped -- --bitrate 5000 came later and selects ABR (last rate-control flag wins, as in x264)
yah264: warning: --qp 26 still sets only the base QP -- --crf 26 came later and selects CRF (last rate-control flag wins, as in x264)
```

The two messages differ because `--qp` is not thrown away: `rc.qp` seeds the
encoder's base QP whatever the mode. `--crf` and `--bitrate` really are
discarded.

So `--bitrate X --crf Y` is CRF, and `--crf Y --qp Z` and `--bitrate X --qp Z`
are constant QP. Every one of them prints the warning above, so the bits move
only where you are told they moved. Repeating the *same* flag is not a clash and
says nothing.

`--pass` is not in the contest. It is a mode plus a stats round-trip whose
target is `--bitrate`; two-pass CRF is not implemented, so a `--crf` passed
alongside it is dropped and says so.

`--aq-strength`'s default is a two-way branch: **0.4** under CRF/ABR/two-pass,
0.0 under constant QP. An explicit value, including an explicit `0`, always
wins. 0.4 is this encoder's own measured value, not x264 medium's 1.0: a
seven-clip VMAF-NEG sweep put the optimum near 0.3, and the shipped default sits
just above it. Why it lands so far below x264's is a known open question about
how this AQ differs from x264's.

### GOP structure

| Option | Argument | Default | What it does |
| --- | --- | --- | --- |
| `--keyint` | N | 250 | Maximum frames between IDRs. |
| `--min-keyint` | N | 0 = auto (`keyint/10`) | A scene cut closer than this to the last keyframe is not promoted to an IDR. Clamped internally to `[1, keyint/2+1]`. |
| `--scenecut` | N | 40 | Adaptive-keyframe aggressiveness; higher inserts more. `0` or any negative turns it off. |
| `--no-scenecut` | | | Same as `--scenecut 0`. Only `--keyint` places IDRs. |
| `--bframes` | N | preset (3) | Consecutive B frames between anchors. 0 disables B entirely. |
| `--b-adapt` | N | 1 | Adaptive B placement. 0 codes a fixed cadence. |
| `--direct` | `auto`\|`spatial`\|`temporal` | `auto` | B direct MV derivation; `auto` lets each B slice pick by the running skippability score (since 2026-09-03). |
| `--ref` | N | preset (3) | P-frame list-0 reference count, clamped to 16. |

`--bframes 2` or higher enables the B-pyramid, which changes `max_num_ref_frames`
and therefore the auto-selected level.

**`--bframes 0` also turns mb-tree off unless the lookahead window is on.**
mb-tree runs when `bframes > 0`, or when `rc-lookahead > 0` and the IPPP path is
enabled (it is by default). So `--bframes 0 --rc-lookahead 0` is a genuinely
different rate-control workload from the default, not just fewer frame types.

### Coding tools

| Option | Argument | Default | What it does |
| --- | --- | --- | --- |
| `--preset` | name | `medium` | Speed/quality ladder. See below. |
| `--tune` | name | off | Content tuning. See below. |
| `--cabac` / `--cavlc` | | CABAC (preset) | Entropy coder. `ultrafast` sets CAVLC. |
| `--transform-8x8` / `--no-transform-8x8` | | on (preset) | 8x8 transform and 8x8 intra. On means High profile. |
| `--cqm` | `flat`\|`jvt` | `flat` | Quantisation matrices. `jvt` writes scaling lists into the SPS and forces High profile. |
| `--me` | `dia`\|`hex`\|`umh` | auto from preset | Motion search. Auto is hex at medium and faster, UMH at slow and above. |
| `--psy-rd` | float | 2.0 | Psychovisual RD strength. 0 disables. |
| `--psy-trellis` | float | 0.0 | Psy-trellis strength. Around 1.0 for grain. |
| `--trellis` | 0..2 | 1 | RDOQ placement, x264's scale: 0 off, 1 the committed macroblock only, 2 every mode decision. |
| `--subme` | 1..11 | preset (7 at medium) | Subpel/RD analysis level, x264's scale. See below. |
| `--subpel` | 0..2 | preset (2 at medium) | Refinement *pattern*: 0 square, 1 diamond, 2 capped diamond. No x264 equivalent. |
| `--merange` | pels | 16 | UMH search radius, x264's `--merange`. **Only UMH reads it**; `dia` and `hex` ignore it, so it does nothing at medium. |
| `--qcomp` | 0..1 | 0.6 | Rate-curve compression, x264's `--qcomp`. See the divergence below. |
| `--deadzone-inter` | 0..32 | 21 | Inter luma quantisation deadzone, x264's flag and x264's value. |
| `--deadzone-intra` | 0..32 | 11 | Intra luma quantisation deadzone. |

Each of those six also has an `Y264_*` variable, **which still works and still
wins**: the flag sets the variable the encoder reads, and if the environment
disagrees with the flag the environment takes it and says so on stderr. That
keeps sweep scripts that override a binary's arguments from the environment
working, without an env var quietly beating an explicit flag.

Three of them do not mean quite what the x264 flag of the same name means:

- **`--subme` also picks the search method.** With no `--me`, this encoder gates
 hex against UMH on subme (below 8 hex, 8 and above UMH), so `--subme 8` changes
 the algorithm and not just the effort. x264 keeps the two independent. Pass
 `--me` to pin it. `--subme 0` is refused rather than obeyed: it is x264's
 *fastest* mode and this library's "unset", which is the slowest (10), so
 accepting it would do the opposite of what an x264 user meant.
- **`--qcomp` reaches the ABR curve and the mb-tree strength derived from it, not
 the CRF or two-pass curves**, which carry their own (`Y264_TP_QCOMP`). x264's
 applies to all of them.
- **The deadzone flags are not a no-op at their own defaults.** Passing either
 swaps the exact shipped expression, intra `step/3` and inter `step/6`, for the
 1/64 approximation, so `--deadzone-inter 21 --deadzone-intra 11` measures
 +0.23% on foreman rather than 0. It also disables the NEON quant path. The
 value is inverted on the way in exactly as x264 inverts it
 (x264 inverts the deadzone on the way in, so its internal bias is `32 - flag`), so the numbers port.

**`--trellis` takes all three of x264's levels**, and it defaults to 1 as x264
does: 0 is RDOQ off everywhere, plain deadzone; 1 quantises the trials with the
deadzone and re-encodes only the winner with RDOQ; 2 runs RDOQ in every mode
decision. `Y264_TRELLIS_COMMIT=0` is the separate escape that puts RDOQ back in
every trial at level 1. There is no `--deblock`, `--weightp`,
`--slices`, `--open-gop` or `--interlaced`. Deblocking is always on with default
offsets; weighted prediction is signalled in the PPS unconditionally.

### Stream metadata

| Option | Argument | Default | What it does |
| --- | --- | --- | --- |
| `--sar` | `W:H` | unspecified (square) | Sample aspect ratio. Accepts `16:11` or `16/11`. |
| `--level` | e.g. `3.1` or `31` | auto | Force an H.264 level. Range 1.0 to 6.2. |
| `--no-sei` | | SEI on | Suppress the x264-style settings SEI. |

There is **no `--profile` flag**. The profile is derived and cannot be
constrained downward:

| Condition | profile_idc |
| --- | --- |
| CAVLC, no B frames, no 8x8 transform | 66 (Baseline) |
| CABAC or B frames | 77 (Main) |
| 8x8 transform on, or `--cqm jvt` | 100 (High) |
| build bit depth > 8 | 110 (High 10) |
| 4:2:2 input | 122 (High 4:2:2) |
| 4:4:4 input | 244 (High 4:4:4) |

To get Baseline you have to spell out `--cavlc --bframes 0 --no-transform-8x8`
yourself.

`--level` below the computed conformant minimum is accepted, but prints a
warning that the stream may be non-conformant. It does not clamp the encode to
fit the level you asked for. The auto level is derived from frame size, frame
rate and DPB size per Annex A.

### Threading

| Option | Argument | Default | What it does |
| --- | --- | --- | --- |
| `--threads` | N | 0 = auto (online cores) | Total thread budget, split across GOP workers and in-frame wavefront threads. |
| `--sync-lookahead` | frames | auto | Input frames buffered so the lookahead runs ahead of the encode on its own thread. `0` or negative disables. |

`--sync-lookahead` costs exactly that many frames of latency and changes no
bits. Auto resolves to `bframes+1` once the frame-thread pool is wide enough to
run a lookahead chain against, and 0 otherwise. The CLI prints the resolved lead
and its millisecond cost at your frame rate on stderr. `--tune zerolatency` sets
it off.

There is no `--frame-threads` flag; the in-frame wavefront share is derived from
`--threads` by the budget split.

## Presets

The ladder sets five things. Everything else is preset-independent.

| Preset | subme | subpel | ref | rc-lookahead | cabac | 8x8 | bframes |
| --- | --- | --- | --- | --- | --- | --- | --- |
| ultrafast | 1 | 2 | 1 | 0 | off | off | 0 |
| superfast | 1 | 2 | 1 | 0 | on | on | 3 |
| veryfast | 2 | 2 | 1 | 10 | on | on | 3 |
| faster | 4 | 2 | 2 | 20 | on | on | 3 |
| fast | 6 | 2 | 2 | 30 | on | on | 3 |
| **medium** | 7 | 2 | 3 | 40 | on | on | 3 |
| slow | 8 | -1 | 5 | 50 | on | on | 3 |
| slower | 9 | -1 | 8 | 60 | on | on | 3 |
| veryslow | 10 | -1 | 16 | 60 | on | on | 3 |
| placebo | 11 | -1 | 16 | 60 | on | on | 3 |

subpel 2 is a capped diamond (x264's subme-7 shape); -1 is an 8-neighbour square
iterated to convergence. subme at or below 8 uses the fast SATD partition path,
9 and above does full RD per partition.

The preset also gates motion search when `--me` is not given: subme below 8 runs
hex, 8 and above runs UMH. So `--preset slow` changes the search algorithm, not
just its effort.

Explicit flags override the preset regardless of order on the command line:
`--ref`, `--bframes`, `--rc-lookahead`, `--cabac`/`--cavlc`,
`--transform-8x8`/`--no-transform-8x8`, `--me`.

An unknown preset name is an error, not a warning.

## Tunes

`--tune` sets content-adaptive defaults. Explicit `--psy-rd` and
`--psy-trellis` override whatever the tune chose.

| Tune | Effect |
| --- | --- |
| `grain` | psy-trellis 1.0, psy-rd 1.5. The 1.5 was a measured VMAF-NEG win on heavy grain against a 1.0 default; the default has since moved to 2.0, so the tune now lowers psy-rd and has not been re-measured against it. |
| `film` | psy-trellis 0.5. psy-rd left at the 2.0 default; the film value has not been BD-measured for want of a film clip. |
| `animation` | psy-trellis 0.5, aq-strength 0.6, and bframes +2 (capped at 8). |
| `psnr` | psy-rd 0, psy-trellis 0, aq-strength 0. |
| `ssim` | psy-rd 0, psy-trellis 0, AQ kept. |
| `zerolatency` | bframes 0, rc-lookahead 0, sync-lookahead off. |

`zerolatency` and `animation` only apply their frame-type changes if you did not
set `--bframes` yourself. An unknown tune name is an error.

## Which options change the bitstream shape

Worth knowing before you A/B anything:

- `--cqm flat` writes no scaling matrix at all, so it is byte-identical to a
 decoder using the default flat-16.
- `--sync-lookahead` never changes a bit. It is pure latency-for-throughput.
- `--threads` **can** change bits. See the next section.
- `--no-sei` changes the stream but not the pictures.

## Threading, and what it does to your output

`--threads N` is a budget, not a worker count. The CLI splits it into `g` GOP
workers times `k` in-frame row-wavefront threads, sizing each GOP's share by the
frames it actually owns, and prints what it chose:

```
yah264: encoded 250 frame(s) in 2 GOP(s) on 2 GOP-worker(s) x 4 frame-thread(s)
```

**The determinism guarantee is: same input, same config, same thread count gives
the same output, bit for bit.** Output may differ across *different* thread
counts. That is deliberate, and x264 does not offer the stronger
thread-count-invariant guarantee either. The mechanism is that the in-frame
wavefront prices predecessor context slightly differently from the serial path,
so `k=1` and `k>=2` can differ (all `k>=2` agree with each other). If you need
reproducibility across machines with different core counts, pin `--threads`.

`--dump-recon` forces the fully serial path, because the recon stream has to be
a single continuous self-consistent encode. Two-pass falls back to it as well in
the cases below. **A `--threads N` greater than 1 that cannot be honoured says
so on stderr and names the condition** rather than silently encoding on one
thread:

```
yah264: warning: --threads 8 cannot be honoured, encoding serially: --dump-recon needs one continuous self-consistent recon stream, which per-GOP encoders cannot produce
```

It fires only on an explicit `--threads` above 1. Leaving it unset means "auto,
every core", which is not a request, and warning on it would fire on all 252
`--dump-recon` runs of the conformance gate.

**4:2:2 and 4:4:4 thread.** The encoder codes all three chroma formats through
I/P/B on both entropy coders, and the GOP-parallel reader's frame store reads
the subsampling off the Y4M `C` tag, the same as the serial path and the recon
dumper.

Two-pass runs on the parallel path in both passes, splitting the stats file
along GOP boundaries. Pass 2 needs a stats file that a *threaded* pass 1 wrote:
one from a serial pass 1 has no GOP markers to split on, so pass 2 quietly stays
serial for it. Pass 2 also refuses outright, with a message, if the stats file
describes a different GOP split than the current run computed, which is what
happens if you change `--frames` or `--keyint` between passes.

### Memory: the parallel path streams through a bounded window

The parallel path reads the input on its own thread and keeps only a window of
it resident. A GOP worker owns its frames and frees them when it publishes, so
what stays in memory is however many GOPs are in flight plus one GOP of
read-ahead: `(--threads + 1) x --keyint` frames. Clip length is not the ceiling.
The window is, and you set it with the two flags you were already setting.

Measured on 720p at `--threads 1 --keyint 50`, peak RSS windowed against holding
the whole input resident:

| Input frames | Whole input resident | Windowed |
| --- | --- | --- |
| 180 | 503 MB | 378 MB |
| 450 | 930 MB | 384 MB |
| 900 | 1631 MB | 384 MB |
| 1800 | 3035 MB | 385 MB |

The windowed column is flat: 1.559 MB per frame of clip becomes 0.004. Set
`Y264_STREAM_STAT=1` and the encoder reports what the window actually held,
which is 100 frames of 100 at every one of those lengths.

The window is generous, though, and it is worth sizing before you are surprised
by it. On one thread at the default keyint it is 500 frames, so 720p settles
around 975 MB where x264 sits at 167 flat; at 18 threads it is 4750 frames, 14
GiB at 1080p. Lower `--keyint`, lower `--threads`, or set `Y264_STREAM_WINDOW`.

Two things still read the whole input. `Y264_CUT_SPLIT=1` pre-scans for scene
cuts, and since the boundaries it finds are what the dispatcher schedules on,
there is nothing to dispatch until the scan has run, so it keeps the whole-input
read and a whole-clip memory ceiling. Two-pass needs the GOP split before pass 1 writes its
per-GOP stats, so it needs a seekable input, and it has to read the input twice
anyway.

Frame counts come from the file length rather than from reading frames. A Y4M
frame is a fixed-size record once the stream header is past, so yah264 reads
the first `FRAME` header, seeks back, and checks that the remainder divides
exactly. That matters for more than the memory guard: every scheduling decision
on this path is a function of the frame count, so knowing it up front keeps the
schedule, and the bitstream, identical to what the whole-input read produced. A
pipe has no length. It gets the same schedule anyway, by waiting for either EOF
or the `--threads + 1`'th GOP boundary before it dispatches, whichever comes
first; past that point the frame count no longer changes the answer.

The memory refusal measures the window rather than the clip, so it fires on a
machine too small for the parallelism you asked for rather than on a clip too
long for the box. The limit is half of **physical** RAM, overridden by
`Y264_MAX_INPUT_MB` in MiB. Physical rather than free: on macOS most of what
`vm_stat` calls inactive, speculative or purgeable is reclaimable on demand, so
a free-page gate refuses encodes that would have run and answers differently
between two runs of the same command.

```
yah264: this encode needs 41.7 GiB of memory and the limit is 32.0 GiB
yah264: the threaded path streams, but its window is (--threads + 1) x --keyint = 14250 frame(s); lower either, or set Y264_STREAM_WINDOW
yah264: limit 32.0 GiB (50% of 64.0 GiB physical), 2.97 MiB resident per 1920x1080 frame, so 11046 frame(s) fit
yah264: encode a segment with --frames, split the input, or raise Y264_MAX_INPUT_MB
```

A raw 4:2:0 8-bit frame is `w*h*1.5` bytes; 4:2:2 is `w*h*2` and 4:4:4 is
`w*h*3`, and the guard computes from the real geometry. Riding above the window
without scaling with it is a fixed per-worker encoder cost, around 330 MiB for
one 1080p worker and another 50 for a second.

## x264 compatibility

Options that match x264 in **name and semantics**, so a command line ports
across unchanged:

`--preset`, `--tune` (the six names listed above), `--bitrate`, `--qp`,
`--keyint`, `--min-keyint`, `--no-scenecut`, `--bframes`, `--b-adapt`, `--ref`,
`--cabac`/`--cavlc`, `--no-transform-8x8`, `--psy-rd`, `--psy-trellis`,
`--aq-strength`, `--rc-lookahead`, `--sync-lookahead`, `--vbv-maxrate`,
`--vbv-bufsize`, `--pass`, `--stats`, `--threads`, `--frames`, `--sar`,
`--level`, `--me`, `--direct`, `--cqm`, `-o`.

Options that differ, and how:

| Option | The difference |
| --- | --- |
| `--crf` | **The number is not comparable to x264's.** Same CRF value has measured a size difference from -54.6% to +45.3% against x264 across the corpus. Do not port a CRF setting across. See [rate-control.md](rate-control.md). |
| `--crf` | Fractional values are accepted but largely inert; the quantiser rounds to an integer QP. |
| `--crf 0` | x264's lossless. Not implemented here, and refused rather than accepted, because `rc.rf = 0` means "CRF unarmed" in the param struct. |
| `--direct` | x264's `none` is not implemented and is refused. `auto` (the default, as in x264) picks per slice by skippability; `spatial` and `temporal` behave the same as x264's. |
| `--qp` | x264 forces mb-tree and AQ off at constant QP. yah264 forces AQ off *at the CLI* but leaves mb-tree running. `--qp 26` is not the same workload on both encoders. |
| `--scenecut` | On the CLI, `--scenecut 0` means off, same as x264. In the C API, `param.scenecut = 0` means **default (40)** and off is spelled with a negative. Assign `YAH264_SCENECUT_OFF`; see [Calling it from C](#calling-it-from-c). |
| `--sync-lookahead` | Same 0-means-off spelling on the CLI, same negative-means-off idiom in the API. Assign `YAH264_SYNC_LOOKAHEAD_OFF`. |
| `--input-y4m` | x264 sniffs input format; this takes Y4M explicitly and only. |
| bare default | x264 defaults to CRF 23; yah264 defaults to QP 26. |
| `--cqm` | x264 takes `flat`/`jvt` plus custom file forms; only `flat` and `jvt` here. |
| `--subme` | Also selects the search method here: with no `--me`, below 8 is hex and 8 or above is UMH. x264 keeps effort and method independent. `--subme 0` is refused; see the coding-tools section. |
| `--merange` | Only UMH reads it. x264's applies to hex and esa too. |
| `--qcomp` | Reaches the ABR curve and mb-tree strength; the CRF and two-pass curves carry their own. x264's applies to all. |
| `--deadzone-inter`/`-intra` | Same values, same inversion, but passing either also swaps the exact shipped quant expression for its 1/64 approximation, so x264's own defaults measure +0.23% rather than 0. |

x264 options with **no equivalent at all**: `--profile`,
`--deblock`, `--weightp`, `--slices`, `--open-gop`, `--interlaced`, `--tune
fastdecode`, `--qpmin`/`--qpmax`/`--qpstep`, `--ipratio`/`--pbratio`,
`--vbv-init`, `--nal-hrd`, `--muxer`/`--demuxer`, `--fps`, `--input-res`.

The absence of `--nal-hrd` matters for delivery: **yah264 writes no HRD
parameters into the SPS**, even when VBV is active. See the guarantee discussion
in [rate-control.md](rate-control.md).

## Calling it from C

Everything above describes the CLI, which is x264-compatible where it claims to
be. The C API in `include/yah264.h` matches x264's field values, with three
deliberate divergences: `scenecut`, `sync_lookahead` and `subme`, all of them
the zero-as-unset convention rather than a numbering choice. Read this before
you port anything.

None of it applies if you use the CLI. The CLI maps each flag onto the values
below for you.

**Zero means the default, not off.** `yah264_param_t` fills itself
from `yah264_param_default`, and twenty of its fields treat zero as "unset,
pick the default". That convention is fine until a field's off switch is a value
x264 spells as zero. Two fields are in that position:

| Field | x264 spells off as | Zero here means | Off here is |
| --- | --- | --- | --- |
| `scenecut` | `--scenecut 0` | the default, **40**, which is x264's own aggressiveness | `YAH264_SCENECUT_OFF` |
| `sync_lookahead` | `--sync-lookahead 0` | auto, a lead of `bframes+1` | `YAH264_SYNC_LOOKAHEAD_OFF` |

So `param.scenecut = 0` does not disable adaptive keyframes. It requests them at
full strength. Nothing errors, the encode succeeds, and you find out from the
keyframe count and the file size. Use the constants:

```c
yah264_param_t param;
yah264_param_default(&param);
param.scenecut = YAH264_SCENECUT_OFF; /* NOT 0 */
```

Both constants are `-1`, and any negative value means off.

The convention is deliberate for these two. Nineteen other fields depend on
zero-as-unset, and making two of them read zero differently would swap a
divergence you can document for an inconsistency you cannot.

### The enumerated fields carry x264's values

Porting a constant gets you the tool it names. What yah264 does not implement
is refused by `yah264_encoder_open` rather than narrowed to the nearest thing
available, so there is no value that quietly encodes something else.

| Field | x264 | yah264 |
| --- | --- | --- |
| `rc.method` | CQP 0, CRF 1, ABR 2 | x264's, plus 2-pass at **100** |
| `rc.rf` | `float`, `23.0` | **`double`**, `23.0` |
| `me_method` | DIA 0, HEX 1, UMH 2 | x264's, plus auto at **-1** |
| `direct` | NONE 0, SPATIAL 1, TEMPORAL 2, AUTO 3 | the same values; NONE refused, AUTO is the default |
| `csp` | I400 1, I420 2, I422 6, I444 12 | x264's **I420 2, I422 6, I444 12** |
| `subme` | 0 = fastest | 0 = the library default 10, the slowest; see below |

`YAH264_ABI_VERSION` is 1. Assert on it if you want a build-time tripwire, and
`#ifndef` it if you also build against headers that predate it.

#### Where matching x264 exactly is not possible

Four places, each deliberate rather than a half-match:

- **`YAH264_RC_2PASS` is 100.** x264 has no 2-pass rc method; it spells 2-pass
 as ABR plus `b_stat_read`/`b_stat_write`. 3 is exactly where x264 would put a
 fourth method, so 2-pass sits where a future `X264_RC_*` cannot collide with
 it. With `_2PASS` and a `stats` path, leaving `rc.pass` at 0 runs pass 1, which
 opens the stats file for writing and **truncates it**.
- **`YAH264_ME_AUTO` is -1.** x264's `me_method` is always explicit, so every
 non-negative seat belongs to `X264_ME_*` (which already uses 0..4). Negative
 is also how this struct spells auto for `subpel` and `sync_lookahead`. The
 consequence worth knowing: **auto is not 0**, so a param struct that skips
 `yah264_param_default` is asking for dia. `param_default` writes `direct` and
 `me_method` explicitly for exactly this reason.
- **`csp` takes x264's values but not its encoding.** `X264_CSP_*` are bitflags
 with a mask (`X264_CSP_MASK`) and modifiers (`_VFLIP`, `_HIGH_DEPTH`) layered
 on top. yah264 implements none of that, because bit depth here is compile-time
 (`Y264_BIT_DEPTH`), so `X264_CSP_I420 | X264_CSP_HIGH_DEPTH` is not 4:2:0 to
 us, it is an unknown value and open fails. Pass one constant, do not mask.
 Everything in the gaps (I400, NV12/NV21, YV12/YV16, YUYV/UYVY/V210, YV24, the
 RGB family) is likewise refused rather than approximated.
- **`rc.rf` is a `double`, not x264's `float`.** Assigning `f_rf_constant` to a
 double is exact, so porting works. `0` leaves CRF unarmed rather than meaning
 x264's lossless `--crf 0`, which is the zero-as-unset convention above rather
 than a scale problem. The CLI rounds `--crf` to tenths; ask through the API for
 finer.

#### `subme` follows x264's scale except at zero

The scale is **not inverted and not offset**. It runs the same direction as
x264's subpel level (higher = slower, more RD) and the tiers line up. The
only disagreement is at zero: `0` here is the library default **10**, the
slowest setting, where x264's `0` is a real mode and its fastest. That is the
zero-as-unset convention shared with nineteen other fields, and renumbering one
of them would swap a divergence you can document for an inconsistency you
cannot. Porting `0` for speed maximises effort instead. **Ask for `1`.**

### Fields that are genuinely fine

Everything else either agrees with x264 at zero or has no x264 equivalent.
`rc.qp`, `bframes`, `ref`, `keyint`, `aq_strength`, `psy_rd`, `psy_trellis`,
`cabac`, `transform8x8`, `sar_*` and the `vbv_*` pair all read zero the way an
x264 user expects; `keyint_min` 0 is auto in both. Three notes that are not
traps but will still surprise:

- **`param.threads` is a budget for one encoder instance**, not a width: 0 asks
 the library to pick (the machine's thread count, capped at 16), 1 is serial and
 N means up to N. A library caller who sets `threads = 8` gets a wavefront of 8.
 `frame_threads` is still the explicit low-level width, and it wins when set,
 which is what lets the CLI's GOP workers carry their own share.
- **`param.annexb` is read by nothing at all.** Annex-B is the only output mode.
- **`badapt` 2 and `cqm` 2** are silently narrowed rather than rejected:
 `X264_B_ADAPT_TRELLIS` becomes fast b-adapt, `X264_CQM_CUSTOM` becomes JVT.

## Environment variables

The encoder reads **over 300 distinct `Y264_*` environment variables** in C
code; [knobs.md](knobs.md) is generated from the source and carries the count.
Almost none of them are features. They are how this project ships an experiment
without a rebuild, and the great majority are measurement scaffolding that
happens to be reachable from your shell.

They are tiered below by whether you have any business setting them. A rule of
thumb: if it is not in the first table, the answer is no.

Shared semantics worth knowing before you set any of them:

- **Most are read once and cached** in a lazy static, warmed on the main thread
 at encoder open. Changing one mid-process does nothing. Three re-read per
 call: `Y264_MBTREE_MVLAMBDA`, `Y264_ABR_QCOMP` on the serial path, and
 `Y264_DBG_CPLX`.
- **Some are presence-only**, so setting them to `0` still turns them *on*:
 `YAH264_NO_ASM`, `Y264_RC_TRACE`, `Y264_TP_DBG`, `Y264_MBTREE_DBG`,
 `Y264_MBT_PRE_DBG`, `Y264_DBG_CPLX`. `Y264_WF_THREADS` is half-presence-only:
 its mere presence disables the thread-cap clamp, separately from its value.
- **Some test one literal character.** `Y264_ME_LAMBDA` and `Y264_WF_PREDQP`
 only act on exactly `0`; `Y264_VITERBI` acts on anything that is not `0`;
 `Y264_NO_SCENECUT` only acts on exactly `1`, so `=2` does nothing.
- **Some are scaled.** `Y264_TP_BEXP`, `Y264_TP_IPF` and `Y264_TP_PBF` are
 percentages divided by 100. The four `Y264_NTP_SPIN*` are microseconds.

### Tier 1: worth knowing about

Escape hatches, and knobs with no CLI equivalent.

| Variable | Default | What it is for |
| --- | --- | --- |
| `YAH264_NO_ASM` | asm on | Force every scalar C path. Presence-only, so `=0` also disables asm. |
| `Y264_SUBPEL` | preset | The subpel pattern: 0 square, 1 diamond, 2 capped diamond. Promoted to `--subpel`, and still overrides it. |
| `Y264_UMH_RANGE` | 16 | UMH search radius in integer pels. Promoted to `--merange`, and still overrides it. |
| `Y264_NO_UMH` | unset | Overrides `--me` and the preset gate entirely. 1 forces hex, 0 forces UMH. |
| `Y264_ABR_QCOMP` | 0.6 | The ABR rate curve's compression, and the mb-tree strength derived from it. Promoted to `--qcomp`, and still overrides it. |
| `Y264_DZ_INTRA` / `Y264_DZ_INTER` | unset | Quantiser rounding bias in 1/64 units, the encoder's own scale, **not** x264's flag value, which is `32` minus this. Promoted to `--deadzone-intra`/`--deadzone-inter`, which do the inversion; both still override. |
| `Y264_AQ_MODE` | 2 | 1 is log2-variance AQ, 2 and above is x264 aq-mode 2. No CLI flag. |
| `Y264_AQ_DARK` | 0 (off) | Dark-region AQ bias, around 0.5 to 1.0. |
| `Y264_TP_PLAN` | **1 (on)** | The two-pass offline allocator. 0 selects the ranking allocator instead, which is far worse. |
| `Y264_2PASS_MT` | on | 0 forces two-pass onto the serial path, reproducing the serial output exactly. |
| `Y264_MBTREE_OFF` | 0 (off) | Skip mb-tree entirely, which is x264's own CQP policy. Set it when comparing `--qp` runs against x264. Changes bits. |
| `Y264_CUT_SPLIT` | 0 (off) | Split GOP workers on real scene cuts instead of arithmetic boundaries. Worth up to 17% of wall on multi-shot clips. Bitstream unchanged. Its pre-scan needs every frame at once, so it turns streaming off and brings back the whole-clip ceiling, plus a second whole-clip array on top (~+18%). |
| `Y264_MAX_INPUT_MB` | 50% of physical RAM | How much memory the input window may take, in MiB. It refuses rather than being OOM-killed. Raise it if you know the box can take it; lower it to test the refusal. |
| `Y264_STREAM_WINDOW` | `(--threads + 1) x --keyint` | Input frames held resident, overriding the default window. Below `2 x --keyint` the reader could block before a whole GOP is dispatchable, so that is the floor. Scheduling-only: the bitstream does not move with it. |
| `Y264_STREAM_STAT` | 0 (off) | Report the window's high-water mark at the end of the encode: frames actually resident, and compressed bytes published but not yet written. |
| `Y264_WF_THREADS` | from `--threads` | Sets the wavefront width directly **and bypasses the critical-path cap**. The only way to probe above the knee. |
| `Y264_LA_BUF` | auto = `bframes+1` (4 at medium) | Overrides `--sync-lookahead`, **in both directions**: the auto default resolves a lead of `bframes+1` whenever there is a pool, so `=1` and `=0` REDUCE it and cost 1-6% of wall. Costs latency, and changes no bits **while the decoupled chain is on**, which is whenever the knob has any effect; forced together with `Y264_LA_THREAD=0` it changes the bitstream deterministically. |
| `Y264_NTP_SPIN` | 25 (us) | Thread-pool spin budget before sleeping. Worth tuning on unusual core counts. |
| `Y264_CRF_CPLX` | 1 (on) | Behaviour-matched CRF content adaptation, on by default. Narrows the equal-CRF spread against x264 from 100 points to 41, and improves 9 of 11 corpus clips at the tuned anchor, worst +1.54%. 0 turns it off. |
| `Y264_CRF_FPS` | follows `CRF_CPLX`, so on | Frame-duration term, so CRF N is the same operating point at 24 and 50 fps. A correctness fix rather than a tuning one. |
| `Y264_AQ_CHROMA` | 0 (off) | Sums chroma into the AQ energy, as x264 does when it measures a macroblock's energy. Faithful, and measured neutral: it costs 9% of the mb-tree bucket and buys +0.35..-0.29% BD. Off even under `CRF_CPLX`. |

Two you should know exist so you never set them:

| Variable | Why not |
| --- | --- |
| `Y264_WF_PREDQP=0` | Escapes to the true raster QP chain, which makes analysis **non-deterministic across thread counts**. The default of 1 is what holds the determinism guarantee up. |
| `Y264_UNSAFE_NO_REFBWAIT`, `Y264_UNSAFE_NO_PREVPWAIT` | Deliberately racy. They drop synchronisation waits to measure a ceiling. The output is not trustworthy. |
| `Y264_UNSAFE_NO_EMIT`, `Y264_UNSAFE_NO_NAL` | Deliberately broken. They delete the entropy emit and the NAL assembly to price the emission path. The bitstream is invalid; the reconstruction is unaffected, which is the point. |

### Tier 2: calibration knobs

These exist so a constant can be swept against x264 without a rebuild. They have
defaults that were measured, and moving one moves quality in ways that have
usually already been tested and rejected. Listed so you know what you are
looking at if you find one in a script, not as a tuning surface.

**Motion and mode decision:** `Y264_HPEL_SAD`, `Y264_HPEL_THRESH`,
`Y264_ME_SMALL_NOUMH`, `Y264_ME_LAMBDA`, `Y264_TEMPORAL_SEED`,
`Y264_RICH_SEEDS`, `Y264_LR_SEED`, `Y264_B_SEEDS`, `Y264_LR_ME`, `Y264_TR_PRE`,
`Y264_TRELLIS_COMMIT`, `Y264_VITERBI`, `Y264_RDOQ_SEED64`, `Y264_PSY_TRELLIS`,
`Y264_CABAC_RD`, `Y264_EST_CTX`, `Y264_DCTDEC`, `Y264_DCTDEC_T4`,
`Y264_DCTDEC_T8`, `Y264_QPELRD`, `Y264_QPELRD_HYST`, `Y264_QPELRD_LUMA`,
`Y264_INTRA_SKIP`, `Y264_INTRA_FINE_M`, `Y264_INTRA_SCREEN`,
`Y264_INTRA_ADMIT_M`, `Y264_INTRA_SCREEN_PURE`, `Y264_P_RECT`,
`Y264_PART_EARLYTERM`, `Y264_PART_THRESH`, `Y264_PART_IMPORTANT`,
`Y264_PART_HETERO`, `Y264_B_THRESH`, `Y264_B_RECT`, `Y264_BPO`,
`Y264_PROBE_TRELLIS`, `Y264_RD_ADMIT`, `Y264_RD_ADMIT_MARGIN`, `Y264_MIDSKIP`,
`Y264_MIDSKIP_MARGIN`, `Y264_ADME`.

**AQ and rate control:** `Y264_AQ2_BIAS`, `Y264_AQ_BOOST`, `Y264_AQ_OCTILE`,
`Y264_AQ_ANCHOR`, `Y264_CRF_CL`, `Y264_CRF_AQABS`, `Y264_CRF_PB0`,
`Y264_CRF_PED`, `Y264_CRF_PBSCALE`, `Y264_CRF_BASE`, `Y264_CRF_SLOPE`,
`Y264_CRF_CAP`, `Y264_CRF_CL_SHIFT`, `Y264_RCP_GAIN`, `Y264_RCP_WARM`,
`Y264_RCP_QPD`, `Y264_VBV_RHI`, `Y264_VBV_QPD`, `Y264_VBV_CJUMP`,
`Y264_TP_DIFFLIM`, `Y264_TP_CORR`, `Y264_TP_RESOLVE`, `Y264_TP_BEXP`,
`Y264_TP_IPF`, `Y264_TP_PBF`, `Y264_TP_CPLXBLUR`, `Y264_TP_QBLUR`,
`Y264_TP_CWARM`.

**mb-tree and lookahead:** `Y264_MBTREE_WHOLEBUF`, `Y264_MBTREE_IPPP`,
`Y264_MBTREE_STRENGTH`, `Y264_MBTREE_BFIX`, `Y264_MBTREE_MVLAMBDA`,
`Y264_MBTREE_CENTER`, `Y264_MBTREE_PROP_INVQ`, `Y264_MBTREE_BOTHLIST`,
`Y264_MBTREE_ADAPT`, `Y264_MBTREE_AINT`, `Y264_MBTREE_ASLOPE`,
`Y264_MBTREE_ALO`, `Y264_MBTREE_AHI`, `Y264_LOWRES_COH`, `Y264_LR_INTRA_NEIGHBOUR`,
`Y264_LR_REUSE`, `Y264_LA_THREAD`, `Y264_LA_INLINE`, `Y264_LA_POOL_MIN`.

**Threading and scheduling** (all byte-identical unless noted): `Y264_W2`,
`Y264_FPIPE`, `Y264_STAIR`, `Y264_STAIR_DEPTH`, `Y264_STAIR_WIDE`,
`Y264_STAIR_EVICTPOOL`, `Y264_RC_PIPE`, `Y264_RC_PIPE_VBV`, `Y264_HPEL`,
`Y264_GOP_EVEN`, `Y264_NTP_SPIN_ROW`, `Y264_NTP_SPIN_JOIN`,
`Y264_NTP_SPIN_IDLE`, `Y264_F1`, `Y264_F3`, `Y264_F3C`.

`Y264_MBTREE_CENTER` deserves a warning: it has two read sites with *different*
defaults, so its effective default depends on which mb-tree path is live.

`Y264_VBV_BOUND` (default **0**) belongs to both of the lists above and to
neither cleanly, which is why it is called out here. It bounds every frame
against its measured coded size instead of only the instance's first, and it
reaches the concurrent routes by disengaging the stair and fpipe, so it is a
rate-control change *and* a scheduling one. It only does anything under a VBV
cap with no bitrate target and no pass-1 record (capped VBR and CQP+VBV); every
other mode is byte-identical with it on. On: `scripts/cvbr_compliance.sh` goes
from 34/36 to 36/36 at both windows, quality is neutral-to-positive in the mode,
and wall clock is ~1.5-1.6x.

### Tier 3: measurement scaffolding, not features

Profiling counters, trace dumps, deliberately-racy ceiling probes, and gates on
experiments that were measured and rejected. Setting any of these gets you
diagnostics or a known-worse encode. Several change bits.

`Y264_THREAD_PROF`, `Y264_NTP_PROF`, `Y264_NTP_STATS`, `Y264_NTP_PARK`,
`Y264_STAIR_STAT`, `Y264_VBV_STAT`, `Y264_RCP_DBG`, `Y264_RC_TRACE`,
`Y264_TP_DBG`, `Y264_MBTREE_DBG`, `Y264_MBT_PRE_DBG`, `Y264_DBG_CPLX`,
`Y264_LA_STAT`, `Y264_ME_STATS`, `Y264_MB_LOG`, `Y264_SKIP_ORACLE`,
`Y264_EST_CHECK`, `Y264_CUT_SPLIT_STAT`, `Y264_ADME_LOG`,
`Y264_PROBE_DEADZONE`, `Y264_HEX_ORACLE`, `Y264_STAIR_BDEPTH`,
`Y264_STAIR_MULTIHOP`, `Y264_STAIR_WIDE_REF`, `Y264_STAIR_REFBGATE`,
`Y264_STAIR_REFBEARLY`, `Y264_STAIR_LEAFRUN`, `Y264_STAIR_LAG_FORCE`,
`Y264_RCP_LAG`, `Y264_RCP_LAG_NOWIDE`, `Y264_VBV_FORCE`,
`Y264_UNSAFE_NO_REFBWAIT`, `Y264_UNSAFE_NO_PREVPWAIT`, `Y264_UNSAFE_NO_EMIT`,
`Y264_UNSAFE_NO_NAL`, `Y264_NO_SCENECUT`,
`Y264_MBT_PRE`, `Y264_DPB_POOL`, `Y264_GOP_FORCE_G`, `Y264_GOP_FORCE_K`.

Note `Y264_NO_SCENECUT` is **not** a user-facing way to disable scene cuts; it
is a diagnostic that isolates a threading barrier. Use `--no-scenecut`.

`Y264_MB_LOG` and `Y264_SKIP_ORACLE` take a **path**, not a boolean.

### Not read by the encoder

These appear in scripts and tests only, never in C: `Y264_CRF`,
`Y264_CRF_CACHE`, `Y264_REFENC_CACHE`, `Y264_STRESS_ABR`, `Y264_TRELLIS_PRINT`,
`YAH264_ARGS`, `YAH264_DESC`, `YAH264_ENC`, `YAH264_CONF_*`, and
`YAH264_VMAF_MODEL` (used by `scripts/vmaf.sh` and `scripts/bdcompare.py`).

Some names that look like environment variables are compile-time macros and
cannot be set from a shell: `Y264_STAIR_K`, `Y264_STAIR_HOPS`,
`Y264_MT_POOL_MIN`, `Y264_STAIR_LAG`, `Y264_LA_CAP_MAX`, `Y264_DPB_POOL_MAX`,
`Y264_BIT_DEPTH`.

### Comments that lie about their defaults

The comments beside the readers in `src/encoder/encoder.c` now agree with the
code for all four gates this section used to list. What is still stale is the
struct-field comments in `src/encoder/encoder.h`, which describe an era when
these were off. Trust the reader, or the census in
[knobs.md](knobs.md), and not the header:

| Variable | Header comment claims | Code does |
| --- | --- | --- |
| `Y264_RC_PIPE` | default off | defaults to **1** |
| `Y264_RC_PIPE_VBV` | default off | defaults to **1** |
| `Y264_STAIR` | default off | defaults to **1** |
| `Y264_W2` | default off | **on whenever there is a thread pool** |
| `Y264_ABR_EARLY` | default 0, a probe | defaults to **2**, the shipped drain split |

## Rough edges

Things that will bite, listed because a reference that only lists what works is
not a reference.

- A value-taking flag as the final argument (`yah264 ... --qp`) is reported as
 an unknown argument rather than a missing value.
- `--threads` does nothing under `--dump-recon`, or under the two-pass fallbacks
 above. It warns when you asked for more than one.
- The parallel path's window is `(--threads + 1) x --keyint` frames, so a high
 thread count at the default keyint asks for a lot of memory on a short clip's
 behalf: 18 threads at `--keyint 250` is 4750 frames, 14 GiB at 1080p. Clip
 length does not matter, but that product does.
- `--level` warns rather than enforces.
