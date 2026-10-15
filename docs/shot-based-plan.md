# Shot-based encoding

Shot-based encoding gives each shot of a title its own settings. This page
covers what yah264 does for it, which parts belong in the encoder and which
belong above it, and the stages still to build. The orchestrator that runs the
search above the encoder has its own design in
[orchestrator-design.md](orchestrator-design.md). The contract between the two
is [engine-interface.md](engine-interface.md).

## Where it stands

The encoder's part is done. yah264 finds the shots, puts a keyframe on every
cut (`--cut-split`), gives each shot its own CRF in one pass (`--shot-crf`),
and exposes the hooks an external search needs: the shot table, a plan of
keyframes and QP offsets, per-frame forces, per-frame stats, one output file
per GOP, and the guarantee that a shot encoded alone matches the same shot
inside the full encode.

Against plain CRF at the same setting, over CRF 21 to 33, `--shot-crf` reads:

| content | BD-rate, VMAF-NEG | BD-rate, PSNR-Y |
|---|---:|---:|
| ten real titles at 1080p: three 2-minute segments, seven 10-minute excerpts | -1.0% median, 7 of 10 better | -5.3% median, all 10 better |
| three short multi-shot sequences | -1 to -4% | |

The gain is largest on the 2-minute segments. On the 10-minute excerpts it's
small, and a few titles come out slightly behind on VMAF-NEG.

The per-shot tool-selection stage was measured and closed. The two stages
left, a measured hull with a resolution ladder and then a predicted hull,
belong to the orchestrator, which will live in its own repository and has
not been built past a prototype.

## Decisions

**The encoder owns the QP axis. The orchestrator owns resolution and the
ladder.** Finding shots, rating them and sharing quality between them at one
resolution is work the encoder does better than any wrapper, because its
lookahead already computes the costs. A resolution ladder needs a scaler, a
decode and a metric scored against the source. Those live outside the encoder,
and an encoder that scored its own output with the metric it's judged on would
be grading its own homework. Distribution across machines, stitching,
manifests and containers belong to the orchestrator too.

That makes yah264's job to be the best engine under an orchestration layer,
which is a narrower goal than a dynamic optimizer inside the encoder. We think
it's the right one. [plan.md](plan.md) names the same goal as hull-assist
mode.

**Per-shot resolution inside one H.264 stream is out.** AVC has no
reference-picture resampling, so a resolution change needs new parameter sets
and a keyframe, and players that accept them. Segmented delivery makes the
question moot anyway, because each rung is its own stream. A one-run ladder
with shared analysis would still save compute, but never quality.

**No demuxer in yah264, in the library or the CLI.** Real shot-based inputs
are compressed mezzanines, so "mp4 support" really means decoding support,
which means taking on libavformat and libavcodec. Other encoders draw the same
line. SVT-AV1 takes Y4M and raw input, x264 builds its libav input as an
option, and av1an does all its IO through ffmpeg or VapourSynth. People pipe
from ffmpeg:

    ffmpeg -i src.mp4 -f yuv4mpegpipe - | yah264 --input-y4m - ...

Orchestrators use the CLI per job or the library API. Output is Annex-B, and
`ffmpeg -c copy` puts it in mp4. CMAF and fMP4 segment output can come later
as a product feature.

**Shot modes want a file.** The cut-aware split has to know every cut before
it can schedule the first GOP. From a file it reads the input twice: a scan
pass that keeps a few numbers per frame and drops the pictures, then a normal
streaming encode. Memory stays at what a plain encode uses. A pipe can't be
read twice, so on a pipe the shot modes hold the whole input, up to the memory
limit. Plain encodes stream from a pipe without that cost.
[options.md](options.md) has the memory figures.

**Gates are measured against our own flat CRF.** Every stage gate is
BD-VMAF-NEG from `scripts/bdcompare.py --vmaf --no-cache` over five-point
sweeps. Equal CRF doesn't mean equal quality across encoders, so comparing our
shot mode against another encoder's CRF would measure the scales instead of
the mode ([rc-mode-matrix.md](rc-mode-matrix.md)). Each multi-shot gate also
reports a per-shot floor, the worst single shot's VMAF change against the
flat encode, because a mode that wins the average by starving one shot is a
regression viewers will see.

## How it works

The shot detector is the lookahead's own scene-cut decision run ahead of the
encode. It replays the same state machine, flash guard included, so the
keyframes it reports are exactly the ones the real encode would place. It runs
in parallel and codes nothing. Its output is a shot table: each shot's first
and last frame, its mean and peak lowres intra cost, its mean inter cost, and
the ratio of the two. `--shot-table` prints it as JSON on stderr.

`--scan-out FILE` saves the scan and `--scan-in FILE` reuses it. The scan
depends on the frames and a handful of settings and not on the rate control,
so one scan serves every CRF of a ladder. A saved scan is refused unless the
frame count, geometry, scan settings and a hash of three frames' luma all
match the encode.

The threaded encoder already codes each GOP on its own encoder instance.
`--cut-split` moves those GOP boundaries onto the cuts, so each shot is coded
as its own unit and every cut is a clean seek point. It costs no compression,
since predicting across a cut gains nothing.

`--shot-crf` turns on `--cut-split` and gives each shot its own CRF from the
shot table, using the standard qcomp form at shot level instead of per frame:

    crf_shot = crf + 6 * (1 - qcomp) * log2(C_shot / C_title)

C is the shot's mean lowres inter cost, and qcomp is 0.6. The offset is
clamped to four QP either way, and shots shorter than 24 frames take their
predecessor's offset. A shot that spans several GOPs gives all of them the
same CRF. The plan depends only on the input and the parameters, so the
output repeats at a fixed thread count just as the cut split does. Within a
shot mb-tree works as usual. `--shot-crf` supplies the across-shot term that a
flat CRF can't.

CRF works in whole QP steps, so the offsets move bitrate in jumps of about
12%. At a four-QP clamp that's acceptable. Fractional QP is a separate item.

Nothing is encoded twice and no trial encode runs. That's the whole
difference from Netflix's dynamic optimizer, whose savings come from a search
over trial encodes at many resolutions and QPs. The two share only the
direction of the allocation, and the stages that need the search are S4 and
after.

## What's done

### S0: a corpus that can see shots

The calibrated board is ten single-shot clips, so a shot-based gain on it is
zero by construction. The multi-shot set joins those clips into three
sequences of five shots each, at CIF, 720p and 1080p, with known cut positions
and targets in the VMAF 88 to 94 band. `scripts/make_multishot.py` builds
them and `scripts/multishot_bd.py` scores them, per-shot floor included. Five
six-second shots aren't a title, so the real test is the long-form corpus
([corpus-sources.md](corpus-sources.md)), which is where the ten-title result
above comes from.

### S1: the shot table

The pre-scan used to compute every frame's lowres costs and throw them away.
It now returns them as the shot table, which `--shot-table` prints and the
library hands over. Both `--cut-split` and `--shot-table` stay opt-in, because
moving GOP boundaries changes the bitstream. The gate was that the table
matches the keyframes the encode actually places. It does by construction,
since the scan replays the same arithmetic.

### S2: per-shot CRF

Shipped opt-in as `--shot-crf`. Its gate was 2% BD-VMAF-NEG on the multi-shot
set with no shot below the floor, killed below 1%. It cleared that against
`--cut-split` alone on all three sequences.

Comparing against x264 then exposed a gap in our flat CRF. Our flat CRF
trailed x264 on the multi-shot sequences while leading on the same clips taken
one at a time, so the shot mode was catching up rather than pulling ahead. The
cause was the frame-mean part of adaptive quantization, which ran at the
within-frame strength of 0.4 instead of full strength and was missing from
non-reference B frames. The fix puts it on every frame's base QP at full
strength, and it ships on by default for every input path.

BD-VMAF-NEG on the three sequences:

| arm | against | CIF | 720p | 1080p |
|---|---|---:|---:|---:|
| `--shot-crf` | `--cut-split` alone | -10.8% | -7.6% | -5.9% |
| the frame-mean fix, no pre-scan | the old default | -8.2% | -4.1% | -6.2% |

The fix cost single-shot clips +0.3% at the median. Over the new default,
`--shot-crf` keeps 1 to 4% on the short sequences, and all of that comes from
sizing each shot against the whole title. On the ten real titles it's the
result in the table at the top.

### S3: per-shot tool selection, closed

The idea was to classify shots as grainy, flat, high-motion or static and
switch tools per class: psy-trellis strength on grain, mb-tree strength on
static content, decimation thresholds, AQ strength. Several measured wins were
parked waiting for such a switch, and psy-trellis still ships as the manual
`--tune grain` for that reason. A per-shot oracle on long-form windows put a
perfect selector at about 1%. No real classifier beats a perfect one, and 1%
doesn't pay for the machinery. It reopens only if a tool with a much steeper
dependence on content turns up.

### S4: hooks for an orchestrator

The hooks are what make yah264 a good engine under av1an-class tools.
[engine-interface.md](engine-interface.md) has the full contract, written so
yah265 and yaav1 can implement the same one.

- The shot table, from the CLI as JSON and from the library as a struct, so an
  orchestrator can seed its hull prediction without its own analysis pass.
- `--plan`, with `yah264_encoder_set_zones` in the library: forced keyframes
  and per-range QP offsets in one encode. It's what other encoders call zones,
  driven from a file.
- `--qpfile`: per-frame type and QP forces, for replaying decisions already
  made.
- `--frame-stats`: one JSON line per coded frame in coding order, with
  `yah264_encoder_frame_stats` in the library.
- `--segment-out`: one file per GOP, each with its own parameter sets.
- `--gop-threads` pins each GOP's frame-thread count. With the same
  parameters and the same pinning, a shot encoded alone reproduces its bytes
  in the full encode. `scripts/shot_determinism.sh` checks it and reads 5 of
  5 GOPs identical on the CIF sequence. The guarantee
  is hard at one thread and best effort with more, in line with the
  repeatability rule for threaded output.

A prototype orchestrator ran against these hooks. It measured each shot's
rate-quality hull over five CRF points at one resolution and read, in
BD-VMAF-NEG:

| window | against flat CRF | against `--shot-crf` |
|---|---:|---:|
| the five-shot 720p sequence | -11.7% | -3.4% |
| a 3000-frame Big Buck Bunny window | -2.2% | -4.6% |

`--shot-crf` lost to flat CRF by 2.6% on that Big Buck Bunny window, because
the pre-scan's inter cost misreads the animation's pans. That's why the hull's
margin over it is larger there. Joining one cell's segments reproduced that
cell's encode byte for byte. The prototype taught two things the orchestrator
design now builds in: the unit is the engine's GOP, and each unit's objective
is weighted by its frame count. Unweighted, the Big Buck Bunny assembly lost
4.8% against flat.

## What's left

### S5: a measured hull and ladder

This runs in the orchestrator, not here. It encodes sparse per-shot grids,
around three resolutions by four QPs at a fast preset, interpolates each
shot's curve, builds per-shot hulls, picks across shots at a common slope, and
runs final encodes only at the chosen points. It brings the project's first
output-quality scaler, Lanczos-3, and the decode, upscale and VMAF method the
published work uses. A ladder tool comes into this repository only if shared
analysis turns out to be a compute win worth owning.

The gate is within 1% BD of the exhaustive hull at half the encodes or fewer,
plus an ffmpeg packaging check that rungs switch cleanly at shot-aligned
segments. A rung that promises a delivery cap needs capped VBR to hold its
buffer across GOP joins, which it now does
([rate-control.md](rate-control.md)).

### S6: a predicted hull

This one may never start. It would predict each shot's hull from the shot
table's features with a gradient-boosted tree model trained on S5's measured
hulls, shipped as plain C arrays. Because a convexity check runs after
encoding, a wrong guess costs one extra encode instead of a broken ladder. The
target comes from RCN-Hull, which got within about 1% BD of the measured hull
with 60% fewer encodes, and the TOMM benchmark says handcrafted features are
enough to get there.

S6 starts only once S5 has produced the training data, and only if S5 shows
that trial encodes are the cost that matters.

## What would change this plan

- `--shot-crf` sits right at its kill line on real content: -1.0% median on
  VMAF-NEG, with 3 of 10 titles not better. It clears on PSNR-Y on every title.
  If more long-form titles push the VMAF-NEG median under 1%, title-level CRF
  with mb-tree is enough on the metric we gate on, and what's left is S4: the
  best engine under an orchestrator, which is still worth shipping.
- If S5 shows a sparse measured hull is already cheap, as it may be on short
  shots at fast presets, S6 saves compute nobody is paying for and shouldn't
  be built.
- If packaging forces re-encodes, through server-side ad insertion points or
  strict segment ladders, that erase what a one-run ladder saves, the ladder
  stays outside the encoder for good and the encoder keeps only S4.

## What others ship

**Netflix's dynamic optimizer** is the reference system, and it's an
orchestrator. It splits at shots, trial-encodes each shot at many resolution
and QP points, scores everything with VMAF, picks one point per shot at a
constant slope across shots, and stitches. On VMAF Netflix reports 17%
savings against fixed-QP encoding and over 50% against two-pass VBR. Against a
fixed x264 ladder it's roughly 28%. Every figure is on their own catalog and
includes resolution switching and dozens of encodes per shot, so they're a
ceiling for any one-pass scheme.

Netflix's history holds the sharpest lesson. Encoding each
shot as its own distributed job hurt. Each shot paid about 20 frames of
rate-control warmup, short shots paid 4 to 8% in keyframe overhead, and
roughly 900 tasks per hour of content swamped their messaging layer. The next generation grouped shots into three-minute chunks, so shots
stay the unit of quality decisions and chunks the unit of work. yah264's GOP
workers are the same arrangement inside one machine.

**av1an** is the open-source per-scene orchestrator most people run: scene
splitting, chunk-parallel encodes, a target-quality mode that probes CRFs per
chunk to hit a VMAF score, and support for x264, x265, SVT-AV1 and rav1e. It's
free and it works, which is why the useful thing for yah264 to be is a good
engine under it rather than a rival to it.

**Encoder-side pieces** exist everywhere. x264 has `--zones` for manual
per-range ratefactor overrides, and CRF with qcomp and mb-tree is already
constant-slope allocation at frame granularity. x265 has `--zonefile` and
`--scenecut-aware-qp`. SVT-AV1 has `--enable-variance-boost`,
`--luminance-qp-bias` and a per-frame QP file, which av1an's target quality
drives. AWS MediaConvert's QVBR is a shipped in-encoder content-adaptive
mode, and Bitmovin and Mux sell per-title encoding as a service. Mux's
instant per-title predicts the ladder from features with no trial encodes.

**Research** has moved from searching the hull to predicting it. RCN-Hull
predicts hull membership from a shot's content and loses 0.26% BD-rate with
62% fewer encodes. The ACM TOMM benchmark of Telili et al., on 300 UHD shots
across AVC, HEVC and VVC, finds handcrafted features with ExtraTrees beat the
deep models at around 88% accuracy, for about 1.8% quality loss against the
exhaustive search. VCA's DCT-energy features are cheap enough
to run at ingest speed, and ARTEMIS applies the idea to live streaming with no
trial encodes at all.

So per-shot QP inside an encoder isn't new. It's zones done automatically, and
CRF already allocates at a finer grain than shots. What we haven't seen
shipped elsewhere is the combination yah264 has: shot-level allocation in one
pass with a measured BD gate, and a determinism guarantee that lets a search
probe one shot and keep the result. We haven't checked every encoder.

## Sources

- Netflix: [Dynamic Optimizer](https://netflixtechblog.com/dynamic-optimizer-a-perceptual-video-encoding-optimization-framework-e19f1e3a277f),
  [Optimized shot-based encodes](https://netflixtechblog.com/optimized-shot-based-encodes-now-streaming-4b9464204830)
- [RCN-Hull, IEEE TIP 2024](https://arxiv.org/abs/2206.04877); [Telili et al., ACM TOMM 2025 hull-prediction benchmark](https://dl.acm.org/doi/full/10.1145/3723006);
  [VCA](https://github.com/cd-athena/VCA); [ARTEMIS, NSDI 2024](https://www.usenix.org/system/files/nsdi24-tashtarian.pdf)
- [av1an target quality](https://rust-av.github.io/Av1an/Features/TargetQuality.html);
  [SVT-AV1 parameters (variance boost, luminance-qp-bias)](https://github.com/psy-ex/svt-av1-psy/blob/master/Docs/Parameters.md);
  the x265 CLI documentation for `--zonefile` and `--scenecut-aware-qp`; x264 `--zones`
- [Mux instant per-title](https://www.mux.com/blog/instant-per-title-encoding);
  [Bitmovin split-and-stitch](https://bitmovin.com/blog/split-and-stitch-encoding/)
