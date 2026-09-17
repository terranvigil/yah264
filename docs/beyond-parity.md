# Beyond parity

Every idea below comes from published work, and each section names its
sources. None of the mechanisms are ours. What is ours is where they run:
inside an H.264 encoder, in one encode pass, under a guarantee that a shot
encoded on its own is byte for byte the shot inside the whole film. That
guarantee is what lets a search-based method probe one shot and keep the
answer.

The document is ordered by status. Section 1 is what a user can run today,
with the measurement behind each claim. Section 2 is started and unfinished.
Section 3 is the reading list: ideas with evidence, none of them built here.
The staged plan behind the shot work is `shot-based-plan.md`, the
orchestrator's design is `orchestrator-design.md`, and ideas that were tried
and refused are in `ideas.md`.

## 1. Shipped beyond x264

### Per-shot CRF in one pass

`--shot-crf` gives every shot its own quality setting, computed from the
lookahead's downscaled costs through the standard qcomp-exponent curve at
shot granularity instead of per frame, clamped to four QP either way. No
shot is encoded twice. The costs are already in the pre-scan, so the file is
coded once, at the source resolution, and the offsets ride on a per-GOP `rc.rf`.

Netflix's dynamic optimizer is the reference for this direction, and it
works the other way around. It encodes every shot at many resolution and QP
points, scores each result, builds the rate-quality hull per shot, then picks
one point per shot with a constant-slope trellis. Netflix reports 17%
bitrate savings on VMAF against fixed-QP encoding and over 50% against
standard two-pass VBR. Netflix measured both on their own catalogue, and
both figures include resolution switching. Dozens of trial encodes per shot
buy them. What `--shot-crf` takes from the method is the direction of the
allocation: cheaper on hard shots, richer on easy ones, at no extra encoding
cost. It
cannot reach the slope-matched point or the resolution switch, because both
need the search.

Every reading below is BD-rate on VMAF-NEG, so lower is better.

| arm | anchor | reading |
|---|---|---:|
| `--shot-crf` | `--cut-split` alone, the three multi-shot sequences | -6 to -11% |
| `--shot-crf` | the default's across-shot term, same sequences | -1 to -4% |
| `--shot-crf` | flat CRF, a 3000-frame Big Buck Bunny window | **+2.6%** |
| measured hull, prototype | flat CRF, the five-shot 720p sequence | -11.7% |
| measured hull, prototype | flat CRF, the Big Buck Bunny window | -2.2% |

The Big Buck Bunny row is the one to read first. On that window the
pre-scan's inter cost misreads the animation's pans, the offsets go the wrong
way, and the shot arm loses ground against a flat encode. An earlier version
of this page said the shot flags keep a 1 to 4% lead, full stop. They keep it
on the three short multi-shot sequences, and they lose on the one long-form
window anybody has measured. What `--shot-crf` is worth over a feature-length
film is unmeasured. That measurement is queued, and this page will carry it
when it exists.

The three multi-shot sequences are concatenations of board clips, five shots
each, built by `scripts/make_multishot.py` and scored by
`scripts/multishot_bd.py`. site/results.md has the per-sequence table against
x264, and docs/what-shipped.md section 8 is the ledger entry.

### The shot table and `--cut-split`

`yah264_scan_idr_frames` pre-scans the input and returns exactly the IDRs the
real encode's lookahead would place, by replaying the same state machine with
the same flash guard. It is exact by construction and it runs in parallel.
It codes nothing. `--cut-split` puts an IDR on every cut it finds and moves
the GOP workers' boundaries there, so each shot is coded as its own unit and
every cut is a clean seek point. `--shot-table` prints the per-shot costs as JSON
on stderr: first and last frame, mean and peak lowres intra cost, mean inter
cost, ratio.

Both need the whole input at once. That means a file, and on a pipe they do
nothing. Long-form with cut-aware boundaries therefore wants two passes over
the input: a streaming analysis pass, then a streaming encode pass consuming
its plan. `shot-based-plan.md` says why.

### The engine interface and the guarantee under it

`engine-interface.md` is the contract an orchestrator can build on, written
so that yah265 and yaav1 can implement the same thing and one orchestration
layer can run all three. Four elements ship:

- the shot table
- a plan of zones over input-frame ranges. `--plan` takes forced IDRs and
  per-range QP offsets, and a zone's offset rides on top of whatever the
  rate control chose
- per-frame stats in coding order, no decode needed, through `--frame-stats`
- the stream also written one file per GOP, through `--segment-out`

The fourth element is the one nobody else offers. Encoding frames `[a, b)`
alone, with the same parameters and the same pinned frame-thread count,
reproduces the bytes the full encode wrote for that GOP. `--gop-threads K`
does the pinning, and the pinning is part of the promise. Without it a GOP
instance is sized from the whole-machine budget and the bytes move.
`scripts/shot_determinism.sh` is the test, and it reads 5 of 5 GOPs on the
CIF sequence and the same on the 720p one. We know of no
mainstream encoder that offers this, which is not the same as having checked
them all.

Why it matters: a search over one shot's operating points is only useful if
the winning probe is the stream. Here it is. So is partial re-encode, which
is what an NLE wants and what a fixed shot in a delivered title wants.

### The hardware backend

`--hw videotoolbox` encodes through the Mac's fixed-function H.264 engine,
with yah264's options mapped onto it and our scene-cut deciding its
keyframes. The stream is the hardware's, so none of this project's quality
claims apply to it. What is ours is the option mapping, the causal keyframe
forcing with no added latency, the warning block naming every option the
hardware ignores, and `--hw-strict` to turn that block into an error. The
mapping is one C table that the CLI's warnings read.

On the board clips at their ABR targets, the hardware uses 13 to 72 times
less CPU on the HD ones. It is faster in wall time there too, by up to 3.2x. On CIF
it is slower than we are, because opening the session is most of the run.
Quality is the trade: it comes in 2.3 to 9.5 VMAF-NEG points below our
encoder at the same bitrate on nine of the ten clips, and it is level on
foreman.

It is also not byte-stable run to run, and its quality moves with it.
riverbed read 83.0 on one run and 78.7 on the next at the same size. The full
row is in site/results.md and the build-out is in `videotoolbox-plan.md`.

## 2. In progress

### The orchestrator's hull stages

The measured hull, the trellis across shots and the ladder read off the
title's curve all live above the encoder, in a separate repository that reaches all
three codecs through the engine interface. A prototype already ran P0 and P1
against yah264's hooks: concatenating one cell's segments reproduced that
cell's encode byte for byte, and the measured hull at one resolution over
five CRF cells gave the two hull rows in the table above. Two lessons came
out of it. The unit is the engine's GOP. The per-unit objective has to be
weighted by frame count, because unweighted the Big Buck Bunny assembly read
+4.8% against flat.

Predicting the hull instead of measuring it is the stage after that, and the
literature says most of the gain survives the substitution. RCN-Hull predicts
hull membership as a resolution-by-QP matrix from the shot's frames with a
small Conv-GRU network, and loses 0.26% BD-rate against exhaustive ground
truth at 62% fewer trial encodes.

The ACM TOMM benchmark finds handcrafted features with ExtraTrees beating the
deep models, at around 88% accuracy. ATHENA's VCA project shows cheap
DCT-energy features predict encoding bitrate well enough to use at ingest
speed, at a Pearson correlation of 0.86 against 0.28 for classic Spatial
Information.

Our advantage over all of these is the lookahead, which already computes
downscaled intra and inter costs per frame. Those costs are more predictive
of coded bits than any feature computed without an encoder, and the shot
table hands them to the orchestrator so the search never runs its own
analysis pass.

### The live control plane

Every established encoder lets a caller swap the parameter set between
frames, and that is the whole of what a live service gets. Nothing says which
frame took the change. Nothing couples the change into the rate control's
learned state, so the target moves and the controller lurches as if it had
just opened. There is no way to say "code every second frame until the queue
drains", and nothing comes back except the packet sizes. Services that adapt
to bandwidth and backpressure build their control loop on guesswork around
that one call.

The proposal is a first-class control plane. Settings issue as per-frame
events (bitrate and VBV, CRF, QP bounds, frame rate or decimation, keyframe
request, speed preset, resolution at a keyframe, live zone offsets), apply at
a defined boundary, echo back in the frame stats, and couple into the rate
control through the same opening-refit mechanism that fixed our ABR start. In
the other direction the encoder reports queue depth, per-frame lateness and a
lookahead-derived bit budget, so the service can act before a buffer overruns.
A control log replays to byte-identical output. That makes a live session
reproducible offline.

The pieces it needs are already here: per-frame stats in coding order, zones,
the lookahead ring the opening is fitted on, and a deterministic pipeline. We
know of nobody shipping the coupled, echoed, replayable form. The same
contract on yah264, yah265 and yaav1 means a service writes its control loop
once and switches codec without touching it.

Status: planned. No code, no measurement and no date. `live-control-plan.md`
has the plan, the API sketch and the example app.

## 3. Candidates from the literature

Nothing in this section is built here. Each entry says what the published
work found and what the yah264 version would be.

### Film grain synthesis via the FGC SEI

The film grain characteristics SEI has been in H.264 Annex D since 2004,
SMPTE RDD-5 specifies the decoder synthesis procedure, and FFmpeg's H.264
decoder has parsed the SEI and applied grain since 2021. ITU-T H.Sup21 is the
current reference on film grain synthesis across codecs. AV1 made the
approach mainstream: denoise the source, code the clean frames, transmit
grain parameters, resynthesize at the decoder. Grain is the most expensive
content to code because it is high-entropy and unpredictable, so removing it
before coding and adding it back after is a large BD-rate win on film.

No mainstream open H.264 encoder writes this SEI. x264 does not.

**This one is a port.** The sibling yah265 already ships `--film-grain`: the
SEI, an estimator validated on synthetic grain, an optional denoise, and the
measurement band's FGC mode. It ships opt-in there. It is not the default
because a full-reference metric reads a correct synthesis as twice the error
of deleting the grain, which is a scoring problem. The yah264 version is
probably the same three pieces against a different syntax, and the denoiser
can share motion vectors with the lookahead. Output plays with
synthesized grain in FFmpeg-based players and degrades to clean video
everywhere else.

Sources: [FFmpeg cbs_h264 FGC SEI commit](https://github.com/FFmpeg/FFmpeg/commit/41d1dba4d281aafc4c67aa24ddb1798b25f1e27f),
[FFmpeg h274 film grain synthesis](https://patchwork.ffmpeg.org/comment/66172/),
[ITU-T H.Sup21 film grain synthesis](https://www.itu.int/rec/dologin_pub.asp?lang=e&id=T-REC-H.Sup21-202501-I!!PDF-E&type=items),
[AV1 film grain synthesis, Norkin and Birkbeck](https://norkin.org/pdf/DCC_2018_AV1_film_grain.pdf).

### Saliency-driven adaptive quantization

Variance AQ spends bits where blocking is visible. Saliency AQ spends them
where people look. PAVEN puts a saliency network on top of VVC and reports
over 7% bitrate reduction with no subjective quality loss. SJ-PVC combines
deep-learned saliency with a just-noticeable-distortion model on VVC and
reports 22.87% bitrate savings at equal subjective quality. Both numbers are
on VVC, and the mechanism transfers directly to H.264 MB-level QP, because
per-block QP offsets from a saliency map are per-block QP offsets.

The prototype path is cheap. Start with a heuristic proxy: centre weighting,
motion contrast against the global motion, skin tones, face boxes, layered on
variance AQ. Measure on VMAF-NEG and with eyes, then decide whether a small
neural saliency model earns its runtime. On Apple Silicon that model runs on
the ANE through Core ML without stealing CPU from the encode. Clamping QP
where distortion would stay below visibility is the natural second step, and
it is what pushed SJ-PVC past 20%.

What would be ours: the motion-contrast half of the proxy comes free from the
lookahead's motion field, so the first version costs no extra pass.

Sources: [PAVEN](https://www.sciencedirect.com/science/article/pii/S0952197625016665),
[SJ-PVC](https://www.sciencedirect.com/science/article/abs/pii/S0952197624019651).

### Learned rate control and coded-size prediction

Rate control quality is bounded by how well the encoder predicts coded bits
before coding them. Google shipped imitation-learned rate control for VP9 and
holds patents on ML coded-size estimation with feedback for production
encoders. The academic side shows CNN and transformer models predicting
per-CTU bits well enough to tighten VBV compliance and cut QP oscillation.

The near-term version here has no neural net in it. Replace scratch-CAVLC bit
measurement with a fitted cost model, then train a small predictor of frame
bits from lookahead features once collecting the data is easy. The features
are the ones the lookahead already has: intra cost, inter cost, MV entropy,
QP. A neural version drops into the same interface later.

Sources: [neural rate control via imitation learning](https://arxiv.org/pdf/2012.05339),
[ML coded-size estimation patent](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/11330263),
[rate control ML with feedback patent](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/12088823).

### ML mode and partition pruning

A small model predicts which partition shapes and modes are worth running RD
on, from features the encoder already has: SATD costs, neighbour modes and
the coherence of the MV field. The VVC literature reports 30-50% encode speedup at around
1% BD-rate cost for CNN partition-path prediction. H.264's mode space is
smaller so the ceiling is lower, but the same argument holds for the
expensive parts, which here are the P8x8 sub-mode search and I4 mode
enumeration.

What would be ours: decision trees or gradient-boosted models exported to
plain C arrays, so inference costs almost nothing and the build picks up no
dependency.

Source: [CNN partition prediction for VVC inter](https://arxiv.org/pdf/2310.13838).

### Encode-aware neural prefilter

Google's sandwiched compression brackets a standard codec with pre- and
post-networks trained through a differentiable codec proxy, and reports large
gains on out-of-scope content and metrics. The full sandwich needs a custom
player. The prefilter half does not. It is a network trained to transform
frames so that H.264 specifically codes them cheaper: soften what the DCT
hates, keep what survives quantization, suppress what AQ would overspend on.
Every existing player benefits. Train offline against a codec proxy plus VMAF
loss, then run it per frame on the ANE or GPU at encode time.

This is the highest-risk item on the page and the only one that needs a
training pipeline. Published results suggest meaningful BD-rate for grainy
and high-frequency content even in the pre-only configuration.

What would be ours: the codec proxy trained against this encoder, so the
filter is fitted to the quantizer and the mode decisions that will actually
see the frame.

Sources: [sandwiched compression paper](https://arxiv.org/abs/2402.05887),
[sandwiched video compression](https://arxiv.org/abs/2303.11473),
[reference code](https://github.com/google/sandwiched_compression).

### Reference-frame temporal filtering

SVT-AV1 motion-compensation-filters alt-ref frames before encoding, so
references are clean and residuals shrink. It is one of that encoder's larger
quality tools and it needs nothing from the decoder. The H.264 version:
before encoding a frame that will be referenced, blend it with
motion-compensated neighbours weighted by match quality. Encode-side only,
standard-compliant, biggest wins on noisy and grainy sources.

What would be ours: the motion search is the lookahead's, which runs anyway.
It also pairs with the FGC SEI item above, since the noise temporal filtering
removes is exactly what grain synthesis puts back.

Sources: [SVT-AV1 temporal filtering discussion](https://32blog.com/en/ffmpeg/ffmpeg-v8-svtav1-optimal-settings),
[SVT-AV1-PSY release notes](https://svt-av1-psy.com/releases/).

### Cross-rung analysis reuse for ABR ladders

When one invocation produces several resolutions of the same content, encode
the top rung first and seed the lower rungs from it: the scaled motion
field, the mode hints and the shot metadata. x264's analysis dump and load exists as a niche
two-invocation workflow. ATHENA's multi-rate encoding work reports large
speedups from reuse of this kind.

What would be ours: the ladder as a first-class single-invocation output with
the reuse internal, which the shot table already half provides. It composes
with per-shot hulls, because a segmented ladder is where per-shot resolution
decisions become usable at all.

Source: [VCA and per-title encoding overview](https://bitmovin.com/blog/video-complexity-analyzer-vca/).

### Per-shot content classification

Classify each shot from lookahead statistics (animation, film grain, screen
content, high motion) and switch tool settings per shot: psy strength,
deadzone, deblock offsets, B depth, AQ strength. The shot machinery makes the
plumbing nearly free, and it would replace the single global tune switch
other encoders make users pick by hand.

**Measured and parked.** This is `shot-based-plan.md`'s S3, and it closed. A
per-shot oracle on long-form windows put a *perfect* selector at about 1%,
which does not pay for the machinery. It reopens only if a tool with a
steeper content curve than the ones measured turns up.

### Smaller items

Temporal-layer-aware chroma QP offsets, from SVT-AV1's tune 3 work: bias
chroma QP by position in the B pyramid for more consistent quality across
frames. It is small and cheap, and it is measurable on top of the B pyramid
we already have.

Energy-aware encoding: a cycles-per-quality budget mode that picks per-shot
presets to hit a compute budget, following ATHENA's green-encoding line. It
would suit the M-series efficiency story. Low priority until the per-shot
machinery has a reason to exist beyond it.

Sources for the shot-based line as a whole:
[Netflix dynamic optimizer](https://netflixtechblog.com/dynamic-optimizer-a-perceptual-video-encoding-optimization-framework-e19f1e3a277f),
[RCN-Hull, convex hull prediction by recurrent learning](https://arxiv.org/abs/2206.04877),
[VCA project](https://github.com/cd-athena/VCA),
[Green VCA paper](https://arxiv.org/abs/2304.12384),
[convex hull prediction survey, ACM TOMM 2025](https://dl.acm.org/doi/10.1145/3723006).
