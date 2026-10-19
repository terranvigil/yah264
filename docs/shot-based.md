# Shot-based encoding

Shot-based encoding gives each shot of a title its own settings. yah264 does
the part that belongs inside an encoder: it finds the shots, puts a keyframe on
every cut, sizes each shot's CRF against the whole title in one pass, and
exposes the hooks an outside search needs. The search itself, with its trial
encodes, resolution ladder and quality metric, belongs to an orchestrator that
runs above the encoder. The contract between the two is
[engine-interface.md](engine-interface.md), and this page doesn't repeat it.

## How it works

**The pre-scan.** The shot detector is the lookahead's own scene-cut decision
run ahead of the encode. It replays the same state machine, flash guard
included, so the keyframes it reports are exactly the ones the encode places.
It runs in parallel and codes nothing. From a file it's a pass of its own: it
reads each frame's luma, keeps a byte per frame and drops the picture, then the
encode rewinds and streams as usual, so memory stays at what a plain encode
uses. A pipe can't be read twice, so on a pipe the shot modes hold the input in
memory up to the limit and refuse past it. Plain encodes stream from a pipe
without that cost. [options.md](options.md) has the memory figures.

**The shot table.** For each shot the scan gives the first and last frame, the
mean and peak lowres intra cost, the mean inter cost and the ratio of the two.
`--shot-table` prints it as JSON on stderr. `--scan-out FILE` saves the scan
and `--scan-in FILE` reuses it. The scan depends on the frames and a few scan
settings, not on rate control, so one scan serves every CRF of a ladder. A
saved scan is refused unless the frame count, geometry, scan settings and a
hash of three frames' luma all match.

**`--cut-split`.** The threaded encoder already codes each GOP on its own
encoder instance. `--cut-split` moves those GOP boundaries onto the cuts, so
each shot is its own unit and every cut is a clean seek point. A shot longer
than the keyframe interval still splits at the interval. It changes the
stream, which is why it's opt-in, but it costs no compression: predicting
across a cut gains nothing.

**`--shot-crf`.** Turns on `--cut-split` and gives each shot its own CRF from
the shot table:

    crf_shot = crf + 6 * (1 - qcomp) * log2(C_shot / C_title)

C is the shot's mean lowres inter cost, and `C_title` is the frame-weighted
mean over the title. It uses qcomp 0.6 and an offset clamp of 4 QP either way,
and shots under 24 frames take their predecessor's offset. A shot spanning
several GOPs gives them all the same CRF. It applies under CRF only. Within a shot,
mb-tree and AQ work as usual; `--shot-crf` adds the across-shot term a flat CRF
can't. The plan depends only on the input and the parameters, so at one thread
the output is byte-identical run to run. Nothing is encoded twice and no trial
encode runs.

**The hooks.** `--plan` (zones: forced IDRs and per-range QP offsets),
`--qpfile` (per-frame type and absolute QP), `--frame-stats` (one JSON line per
coded frame), `--segment-out` (one file per GOP, each with its own parameter
sets) and `--gop-threads` (pins each GOP's frame-thread count). With the same
parameters and the same pinning, a shot encoded alone reproduces its bytes in
the full encode. That's guaranteed at one thread and best effort with more,
in line with the repeatability rule for threaded output.
`scripts/shot_determinism.sh` checks it (`T=1` fails on a mismatch; the
threaded default only warns).

## What an orchestrator does with them

The orchestrator is a separate Go project, codec-agnostic, meant to drive
yah264, yah265 and yaav1 through the same contract. A prototype has run
against yah264's hooks; the full tool isn't built yet. Its pipeline is five
stages, each a command with files in and out:

```
analyze  ->  probe  ->  score  ->  hull+select  ->  assemble (+package)
```

1. **analyze.** Ask the engine for the shot table
   (`--cut-split --shot-table --scan-out`) and write `shots.json`. It runs no
   scene detection of its own: the boundaries have to be the ones the engine
   cuts on, or the segments won't line up. A user plan can add forced
   boundaries such as chapter marks.
2. **probe.** Each grid cell is one full-title encode at a resolution and a
   CRF, with `--scan-in`, `--plan` putting an IDR on every shot start, a fixed
   `--gop-threads`, `--segment-out` and `--frame-stats`. Whole-title encodes
   beat per-shot jobs: the bytes are the same and the engine schedules shots
   across its GOP workers better than an outside queue would.
3. **score.** Decode each segment, upscale to the source size with Lanczos-3,
   and score VMAF-NEG per frame against the source. Bits come from the
   segment file.
4. **hull and select.** For each unit keep the upper convex hull of
   (bits, frames x quality). For a slope lambda each unit takes the point that
   maximizes `frames * quality - lambda * bits`; sweeping lambda traces the
   title's curve and the rungs are read off it.
5. **assemble.** Join the chosen segments in order. Each carries its own
   parameter sets, so the result is a valid stream.

The orchestrator owns the scaler, the metric, the hull, the selection, the
assembly, packaging and every trial encode. It never asks the engine to switch
resolution mid-encode, to score quality, or to change a default.

Two rules came out of the prototype. The unit of allocation is the engine's
GOP, not the shot, since a shot longer than the keyframe interval becomes
several GOPs; read the unit map back from `--frame-stats`, or set the keyframe
interval at or above the longest shot. And each unit's objective is weighted
by its frame count.

## What we chose and why

| choice | why |
|---|---|
| the encoder owns the QP axis; resolution and the ladder live above it | the lookahead already computes the costs; a ladder needs a scaler, a decode and a metric, and an encoder scoring itself with the metric it's judged on would be grading its own homework |
| `--shot-crf` and `--cut-split` opt-in | both move GOP boundaries and change the stream |
| shot CRF from the qcomp form at shot level, clamped at 4 QP | one pass, no trial encodes; -1.0% median BD-VMAF-NEG and -5.3% median BD-PSNR-Y against plain CRF on ten 1080p real titles |
| no demuxer in the library or CLI | real inputs are compressed mezzanines, so "mp4 support" means taking on libavformat and libavcodec; pipe from ffmpeg instead |
| shot modes read a file twice instead of buffering it | memory stays at a plain encode's; a whole-input buffer was 38 GB for ten minutes of 1080p |
| gates against our own flat CRF, plus a per-shot floor | equal CRF isn't equal quality across encoders, and a mode that wins the average by starving one shot is a visible regression |
| the plan, forces and stats share one input-frame index from zero | so the orchestrator never translates between coding order and display order |

Measured results for `--shot-crf` against plain CRF at the same setting, CRF
21 to 33:

| content | BD-rate, VMAF-NEG | BD-rate, PSNR-Y |
|---|---:|---:|
| ten real titles at 1080p (three 2-minute segments, seven 10-minute excerpts) | -1.0% median, 7 of 10 better | -5.3% median, all 10 better |
| three five-shot sequences (CIF, 720p, 1080p) | -1 to -4% | |

The gain is largest on the 2-minute segments and small on the 10-minute
excerpts. The prototype's measured hull at one resolution over five CRF points
read, in BD-VMAF-NEG:

| window | against flat CRF | against `--shot-crf` |
|---|---:|---:|
| the five-shot 720p sequence | -11.7% | -3.4% |
| a 3000-frame Big Buck Bunny window (36 shots, 38 GOPs) | -2.2% | -4.6% |

Joining one cell's segments reproduced that cell's encode byte for byte.

## What didn't work

| idea | why not |
|---|---|
| per-shot tool selection (classify shots as grainy, flat, high-motion or static and switch psy-trellis, mb-tree, decimation or AQ strength per class) | a per-shot oracle on long-form windows put a perfect selector at about 1%, which doesn't pay for the machinery; psy-trellis stays a manual `--tune grain` |
| per-shot resolution inside one H.264 stream | AVC has no reference-picture resampling, so a switch needs new parameter sets, a keyframe and a player that copes; segmented delivery makes it moot |
| an unweighted per-unit objective in the hull selection | units of unequal length get the wrong points; the Big Buck Bunny assembly came out 4.8% worse than flat CRF |

## Open questions

- `--shot-crf` sits near its 1% kill line on VMAF-NEG for real content: -1.0%
  median with 3 of 10 titles not better, while it clears on PSNR-Y on every
  title. If more long-form titles push the median under 1%, title-level CRF
  with mb-tree is enough on the metric we gate on, and the hooks are what's
  left worth shipping.
- The pre-scan's inter cost misreads animated pans: `--shot-crf` loses 2.6% to
  flat CRF on the Big Buck Bunny window. Measure before trusting it on
  animation.
- CRF works in whole QP steps, so shot offsets move bitrate in jumps of about
  12%. Fractional QP is a separate item.
- The measured hull with a resolution ladder (sparse grids of about three
  resolutions by four QPs at a fast preset, gated at within 1% BD of the
  exhaustive hull at half the encodes) hasn't run. A predicted hull from the
  shot table's features starts only if that stage shows trial encodes are the
  cost that matters.
- The prototype ran on two windows. The rest of the long-form corpus and the
  other two multi-shot sequences are still to run against the 3% bar over
  `--shot-crf`.

## Measuring it

- Every stage gate is BD-VMAF-NEG from
  `scripts/bdcompare.py --vmaf --no-cache` over five-point CRF sweeps,
  against our own flat CRF.
- The ten-clip board is single-shot clips, so a shot-based gain on it is zero
  by construction. `scripts/make_multishot.py` builds the five-shot sequences
  and `scripts/multishot_bd.py` scores them with the per-shot floor. The real
  test is the long-form corpus ([corpus-sources.md](corpus-sources.md)).
- `T=1 scripts/shot_determinism.sh` is the determinism gate. Without `T=1` it
  only reports.
- `Y264_CUT_SPLIT_STAT=1` prints the pre-scan's time and the IDRs it placed.
