# Beyond parity

The ideas here come from published work. Each section names its sources.
What's ours is where they run: inside an H.264 encoder, in one pass. They also
run under a guarantee that a shot encoded on its own matches the same shot
inside the full encode. That guarantee lets a search probe one shot and keep the result.

The page runs from shipped, to in progress, to ideas we haven't built.
`shot-based-plan.md` has the staged plan for the shot work,
`orchestrator-design.md` the orchestrator's design, and `ideas.md` the ideas
we tried and dropped.

## Shipped

### Per-shot CRF in one pass

`--shot-crf` gives each shot its own quality setting: more bits for the hard
shots, fewer for the easy ones. Before encoding, yah264 reads the file once
and rates each shot from the lookahead's costs, so nothing is encoded twice.
Each shot stays within four QP of the setting you chose. Every cut gets
its own keyframe.

Against the same CRF without it, BD-rate (lower is better):

| content | VMAF-NEG | PSNR-Y |
|---|---:|---:|
| ten real titles, 2 to 10 minutes, at 1080p | -1.0% median, 7 of 10 better | -5.3% median, all 10 better |
| three short multi-shot sequences | -1 to -4% | |

The gain is largest on the 2-minute segments and small on the 10-minute
films, where a few titles come out slightly behind on VMAF-NEG. On PSNR-Y
every title improves. Against `--cut-split` alone, which adds the same
keyframes without the per-shot settings, the short sequences read -6 to -11%.

`--shot-crf` reads the file twice: once to rate the shots, keeping only a few
numbers per frame, and once to encode. Memory stays at what a normal encode
uses. `--scan-out` saves the first pass to a small file, and `--scan-in` reuses
it, so one scan serves every CRF of a ladder.

### Cut-aware keyframes and the shot table

The pre-scan finds exactly the keyframes the real encode's lookahead would
place, without coding anything. `--cut-split` puts a keyframe on every cut, so
each shot is coded as its own unit and every cut is a clean seek point.
`--shot-table` prints each shot's frame range and costs as JSON.

Both need the whole input, so they work on a file and do nothing on a pipe.
Cut-aware long-form encoding from a stream would need two passes: an analysis
pass, then an encode that reads its plan.

### The engine interface

`engine-interface.md` is the contract an orchestrator builds on, written so
yah265 and yaav1 can implement the same one. It covers the shot table, zones
with forced keyframes and QP offsets (`--plan`), per-frame forces
(`--qpfile`), per-frame stats without decoding (`--frame-stats`), and one
output file per GOP (`--segment-out`).

Under it sits the guarantee: encoding one GOP alone, with the same settings
and the same pinned thread count (`--gop-threads`), reproduces the bytes of
the full encode. `scripts/shot_determinism.sh` checks it. A search over one
shot is only useful if the winning trial is the final stream, and here it is.
So is redoing one shot later. We don't know of a mainstream encoder that
offers this, though we haven't checked them all.

### Denoising

Both are off by default. `docs/denoise.md` has the measurements.

- `--nr` is coefficient noise reduction. It's slightly better on 7 of 10
  standard clips and worse on none, at 1 to 2% more instructions.
- `--mcstf` filters each source frame against its motion-compensated
  neighbors before coding. It's the bigger tool on grain and sensor noise, up
  to 17% better on one noisy clip. It also loses on some clips and costs 10 to
  13% more instructions.

### The hardware backend

`--hw videotoolbox` encodes on the Mac's built-in H.264 engine, with our
options mapped onto it and our scene-cut choosing its keyframes. The stream is
the hardware's, so our quality claims don't apply to it. It names every option
the hardware ignores, and `--hw-strict` turns that warning into an error. The
results page has the comparison, and `videotoolbox-plan.md` the design.

## In progress

### The orchestrator's hull stages

This is the full per-shot method, after Netflix's dynamic optimizer: encode
every shot at many resolutions and quality levels, build each shot's
rate-quality curve, and pick one point per shot. Netflix reports 17% savings
against fixed-QP encoding and over 50% against two-pass VBR, including
resolution switching.

Measuring each shot's rate-quality hull, choosing across shots and reading a
ladder off the title's curve all happen above the encoder. That lives in a
separate repository that reaches all three codecs through the engine
interface. A prototype has run against yah264. Joining one cell's segments
reproduced that cell's encode byte for byte. Its measured hull covered one
resolution over five CRF points. It read -11.7% against flat CRF on the
five-shot 720p sequence and -2.2% on a long Big Buck Bunny window. The lessons were to work in the encoder's
GOPs and to weight each unit by its frame count. Unweighted, the Big Buck
Bunny assembly lost 4.8% against flat.

Predicting the hull instead of measuring it comes next. The literature
says most of the gain survives:

- RCN-Hull predicts which resolution and QP points sit on the hull from a
  shot's frames. It loses 0.26% against the exhaustive search with 62% fewer
  trial encodes.
- An ACM TOMM benchmark finds handcrafted features with ExtraTrees beat the
  deep models, at about 88% accuracy.
- ATHENA's VCA shows cheap DCT-energy features predict bitrate well enough to
  use at ingest speed.

Our advantage is the lookahead, which already computes per-frame intra and
inter costs. Those predict coded bits better than features computed without
an encoder. The shot table hands them over, so the search never runs its
own analysis.

### A live control plane

Today a live service can swap an encoder's settings between frames. That's
all. It doesn't learn which frame took the change, the rate control isn't told,
and nothing comes back but packet sizes.

The plan is a proper control plane. Bitrate, CRF, QP limits, frame rate,
keyframe requests, speed and resolution go in as per-frame events. Each one
applies at a defined frame, is echoed in the frame stats and feeds the rate
control. In the other direction the encoder reports queue depth, lateness and
a bit budget from the lookahead. A control log replays to identical output, so
a live session can be reproduced offline. The same contract on all three
codecs lets a service write its control loop once.

Nothing is built yet. `live-control-plan.md` has the plan, an API sketch and
the example app.

## Ideas we haven't built

### Film grain synthesis

The film grain SEI has been in the H.264 standard since 2004. FFmpeg's
decoder has applied it since 2021. AV1 made the approach mainstream: denoise
the source, code the clean frames, send grain parameters, and add the grain
back at the decoder. Grain is the most expensive content to code, so this saves a lot on
film. No mainstream open H.264 encoder writes this SEI.

This one is a port. yah265 already ships `--film-grain`, opt-in, with the SEI,
a grain estimator and an optional denoiser. It isn't the default there because
full-reference metrics score a correct synthesis as worse than deleting the
grain. In yah264 the denoiser could be `--mcstf`. Players without support
show the clean video.

Sources: [FFmpeg H.264 film grain SEI](https://github.com/FFmpeg/FFmpeg/commit/41d1dba4d281aafc4c67aa24ddb1798b25f1e27f),
[FFmpeg film grain synthesis](https://patchwork.ffmpeg.org/comment/66172/),
[ITU-T H.Sup21](https://www.itu.int/rec/dologin_pub.asp?lang=e&id=T-REC-H.Sup21-202501-I!!PDF-E&type=items),
[AV1 film grain synthesis](https://norkin.org/pdf/DCC_2018_AV1_film_grain.pdf).

### Saliency-driven quantization

Variance-based adaptive quantization spends bits where blocking shows.
Saliency spends them where people look. PAVEN reports over 7% savings on VVC
with no visible loss, and SJ-PVC, which adds a model of what the eye can't
notice, reports about 23%. Both are VVC results, but per-block QP offsets work
the same way in H.264.

The first version would be a cheap heuristic on top of today's AQ: center
weighting, motion against the global motion, skin tones and faces. The motion
part comes free from the lookahead. If it pays on VMAF-NEG and by eye, a small
saliency model could run on the Apple Neural Engine without taking CPU from
the encode.

Sources: [PAVEN](https://www.sciencedirect.com/science/article/pii/S0952197625016665),
[SJ-PVC](https://www.sciencedirect.com/science/article/abs/pii/S0952197624019651).

### Better coded-size prediction

Rate control is only as good as its guess of how many bits a frame will take.
Google shipped learned rate control for VP9, and research models predict
per-block bits well enough to tighten buffer compliance. The first step here
needs no neural network: a fitted cost model, then a small predictor of frame
bits from features the lookahead already has (intra cost, inter cost, motion
vector spread, QP).

Sources: [neural rate control via imitation learning](https://arxiv.org/pdf/2012.05339),
[ML coded-size estimation patent](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/11330263),
[rate control ML with feedback patent](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/12088823).

### Learned mode pruning

A small model could predict which partition shapes and modes are worth a full
trial, from costs the encoder already has. VVC work reports 30 to 50% faster
encodes for about 1% BD-rate. H.264 has fewer modes, so the ceiling is lower.
The costliest searches, P8x8 sub-modes and 4x4 intra modes, would still
benefit. Decision trees exported as plain C arrays would add almost no cost
and no dependency.

Source: [CNN partition prediction for VVC](https://arxiv.org/pdf/2310.13838).

### A prefilter trained for this encoder

Google's sandwiched compression wraps a standard codec in networks trained
through a model of the codec. The full version needs a custom player. The
prefilter alone doesn't. It reshapes frames so H.264 codes them more cheaply,
and every player benefits from that. Ours would be trained against this encoder's own
quantizer and mode decisions. It's the riskiest idea here and the only one
that needs a training pipeline.

Sources: [sandwiched compression](https://arxiv.org/abs/2402.05887),
[sandwiched video compression](https://arxiv.org/abs/2303.11473),
[reference code](https://github.com/google/sandwiched_compression).

### One analysis for a whole bitrate ladder

When one run produces several resolutions, encode the top rung first and seed
the lower ones from it: scaled motion, mode hints and the shot data. ATHENA's
multi-rate work reports large speedups from this. Ours would make the ladder a
single-run output, which also makes per-shot resolution choices usable.

Source: [VCA and per-title encoding](https://bitmovin.com/blog/video-complexity-analyzer-vca/).

### Smaller ideas

- **Chroma QP by B-pyramid level**, to keep quality steadier across frames.
  Small, cheap and easy to measure.
- **An energy budget mode** that picks per-shot presets to meet a compute
  budget. Low priority.

### Measured and dropped: per-shot tool settings

Classifying each shot (animation, grain, screen content, high motion) and
switching tool settings per shot looked natural with the shot machinery in
place. A per-shot test on long-form windows put even a perfect classifier at
about 1%, which doesn't pay for the machinery. It comes back only if a tool
with a much steeper content dependence turns up.

## Sources for the shot work

[Netflix dynamic optimizer](https://netflixtechblog.com/dynamic-optimizer-a-perceptual-video-encoding-optimization-framework-e19f1e3a277f),
[RCN-Hull](https://arxiv.org/abs/2206.04877),
[VCA project](https://github.com/cd-athena/VCA),
[Green VCA](https://arxiv.org/abs/2304.12384),
[convex hull prediction survey, ACM TOMM](https://dl.acm.org/doi/10.1145/3723006).
