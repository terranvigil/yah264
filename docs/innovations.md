# Innovations under consideration

Research notes on the differentiating features worth building into yah264, with
evidence and sources. Each section covers what the idea is, why it should win,
what the published results say, and a sketch of how it would land here. Shot-based
encoding has its own plan in `shot-based-plan.md`.

## 1. Shot-based encoding, single pass

Detect shots in the lookahead, analyze each shot's complexity, and pick encoding
parameters per shot instead of per title. The full treatment is Netflix's
dynamic optimizer: encode every shot at many resolution and QP points, build the
rate-quality convex hull per shot, then pick one point per shot with a
constant-slope trellis so the whole title sits on its optimal rate-quality
curve. Netflix reports 17% bitrate savings versus fixed QP on VMAF and over 50%
versus standard two-pass VBR, but it needs dozens of trial encodes per shot and
lives in an orchestrator above the encoder.

The research since then shows most of the gain survives without the exhaustive
search. The RCN-Hull paper (Paul, Norkin, Bovik, IEEE TIP 2024) predicts the
hull as a binary resolution-by-QP membership matrix from the shot's frames with
a small Conv-GRU network: 62% fewer trial encodes, 53.8% less wall time, and the
predicted hulls lose only 0.26% BD-rate against exhaustive ground truth. The
same paper benchmarks two cheaper routes that matter for us: a fast-preset proxy
encode whose hull approximates the slow-preset hull, and handcrafted features
with a classical model (weaker but no neural runtime). ATHENA's VCA project
shows cheap DCT-energy features predict encoding bitrate well (Pearson 0.86
versus 0.28 for classic Spatial Information) at nearly 300 fps for UHD on eight
threads, and their OPTE work applies this to live per-title encoding.

Inside an encoder we hold an advantage over all of these: the lookahead already
computes downscaled intra and inter costs per frame, which are more predictive
of actual coded bits than any content feature computed without an encoder. The
plan is per-shot analysis driving per-shot QP first (safe in any stream), then
predicted-hull ladder output (per-shot resolution decisions for segmented
streaming). Details in `shot-based-plan.md`.

**Status (2026-09-05): the first two stages shipped, and they found a bug in
the flat path.** S1 promotes the pre-scan to a shot table (`--shot-table`,
`--cut-split` puts an IDR on every cut). S2 is per-shot CRF (`--shot-crf`):
-10.8 / -7.6 / -5.9% BD-VMAF-NEG against cut-split alone on the three
multi-shot sequences (CIF / 720p / 1080p concatenations of the board clips),
with the worst shot's quality rising at every point. The control that
mattered was x264: on those sequences our flat CRF trailed it by 13.4 / 4.9
/ 7.3% while leading by 6-14% on the same clips singly, so per-shot CRF was
catching up, not pulling ahead. The cause was the AQ frame-mean term running
at the 0.4 within-frame strength (x264: 1.0) with two thirds of the frames
carrying none at all; fixed on the CRF base QP for every path, streaming
included (`Y264_AQ_DC`): -8.2 / -4.1 / -6.2% against the old default, x264
gap +4.4 / +0.3 / +0.7, single-shot band median +0.30%. The shot flags keep
a 1-4% lead over that because they see the whole file. Next: S3, per-shot
tool choices, gated against the new default. The measuring instruments are
`scripts/multishot_bd.py` and `scripts/make_multishot.py`.

**How this differs from Netflix's dynamic optimizer.** Netflix encodes every
shot many times, at several resolutions and quality points, measures each
result, builds the rate-quality hull per shot and then picks one point per
shot so the whole title sits at one quality-per-bit slope; it is a search
run by an orchestrator above the encoder, at dozens of encodes per shot.
What shipped here is a single pass with no trial encodes: the shot's cost
comes from the lookahead's downscaled analysis, the offset comes from a
closed-form curve (x264's rate equation at shot granularity), and every shot
is coded once at the source resolution. It gets the direction of Netflix's
allocation (cheaper on hard shots, richer on easy ones) at zero extra
encoding cost, and it cannot get the parts that need the search: the exact
slope-matched point per shot, and resolution switching. Those are the
hull-assist and ladder stages of `shot-based-plan.md`, still planned.

Sources: [Netflix dynamic optimizer](https://netflixtechblog.com/dynamic-optimizer-a-perceptual-video-encoding-optimization-framework-e19f1e3a277f),
[RCN-Hull, convex hull prediction by recurrent learning](https://arxiv.org/abs/2206.04877),
[VCA project](https://github.com/cd-athena/VCA),
[Green VCA paper](https://arxiv.org/abs/2304.12384),
[convex hull prediction survey, ACM TOMM 2025](https://dl.acm.org/doi/10.1145/3723006).

## 2. Film grain synthesis via the FGC SEI

The film grain characteristics SEI has been in H.264 Annex D since 2004, SMPTE
RDD-5 specifies the decoder synthesis procedure, and FFmpeg's H.264 decoder has
parsed the SEI and applied grain since 2021 (see the cbs_h264 FGC commit and the
h274 synthesis routine). ITU-T H.Sup21 (January 2025) is the current reference
on film grain synthesis technology across codecs. AV1 made this approach
mainstream: denoise the source, code the clean frames, transmit grain
parameters, resynthesize at the decoder. Grain is the most expensive content to
code because it is high-entropy and unpredictable; removing it before coding and
adding it back after is a large BD-rate win on film content.

No mainstream open H.264 encoder writes this SEI. x264 does not. The pieces are:
a temporal denoiser, a grain parameter estimator (fit the frequency-filtering
model of RDD-5, or the AR model, to the noise layer between source and
denoised), and the SEI writer. Output plays with synthesized grain in
FFmpeg-based players and degrades gracefully (clean video) everywhere else.
Pairs with reference-frame temporal filtering below, and the denoiser can share
motion vectors with the lookahead.

Sources: [FFmpeg cbs_h264 FGC SEI commit](https://github.com/FFmpeg/FFmpeg/commit/41d1dba4d281aafc4c67aa24ddb1798b25f1e27f),
[FFmpeg h274 film grain synthesis](https://patchwork.ffmpeg.org/comment/66172/),
[ITU-T H.Sup21 film grain synthesis](https://www.itu.int/rec/dologin_pub.asp?lang=e&id=T-REC-H.Sup21-202501-I!!PDF-E&type=items),
[AV1 film grain synthesis, Norkin and Birkbeck](https://norkin.org/pdf/DCC_2018_AV1_film_grain.pdf).

## 3. Saliency-driven adaptive quantization

Variance AQ spends bits where blocking is visible. Saliency AQ goes further:
spend bits where people look. PAVEN (2025) uses a saliency network on top of VVC
and reports over 7% bitrate reduction with no subjective quality loss. SJ-PVC
(2025) combines deep-learned saliency with a just-noticeable-distortion model on
VVC and reports 22.87% bitrate savings at equal subjective quality. Those
numbers are on VVC; the mechanism (per-block QP offsets from a saliency map)
transfers directly to H.264 MB-level QP.

The prototype path is cheap: start with a heuristic saliency proxy (center
weighting, motion contrast against the global motion, skin tones, face boxes)
layered on variance AQ, measure with VMAF-NEG and eyeballs, then decide if a
small neural saliency model earns its runtime. On Apple Silicon the model can
run on the ANE via Core ML without stealing CPU from the encode. The JND side
(clamp QP where distortion would stay below visibility) is a natural second
step and is what pushed SJ-PVC past 20%.

Sources: [PAVEN](https://www.sciencedirect.com/science/article/pii/S0952197625016665),
[SJ-PVC](https://www.sciencedirect.com/science/article/abs/pii/S0952197624019651).

## 4. Learned rate control and coded-size prediction

Rate control quality is bounded by how well the encoder predicts coded bits
before coding. Google shipped imitation-learned rate control for VP9 and holds
patents on ML coded-size estimation with feedback for production encoders; the
academic side shows CNN and transformer models predicting per-CTU bits well
enough to tighten VBV compliance and cut QP oscillation. For yah264 the
near-term version is not a neural net: replace scratch-CAVLC bit measurement
with a fitted cost model, then train a small predictor of frame bits from
lookahead features (intra cost, inter cost, MV entropy, QP) once data collection
is easy. The neural version is a drop-in upgrade to the same interface.

Sources: [neural rate control via imitation learning](https://arxiv.org/pdf/2012.05339),
[ML coded-size estimation patent](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/11330263),
[rate control ML with feedback patent](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/12088823).

## 5. ML mode and partition pruning

A small model predicts which partition shapes and modes are worth running RD on,
from features the encoder already has (SATD costs, neighbor modes, MV field
coherence). The VVC literature reports 30-50% encode speedup at around 1%
BD-rate cost for CNN partition-path prediction; H.264's mode space is smaller so
the ceiling is lower, but the same holds for the expensive parts (P8x8 sub-mode
search, I4 mode enumeration once those exist). Decision-tree or gradient-boosted
models exported to plain C arrays keep the inference cost near zero and the
build dependency-free.

Source: [CNN partition prediction for VVC inter](https://arxiv.org/pdf/2310.13838).

## 6. Encode-aware neural prefilter

Google's sandwiched compression brackets a standard codec with pre- and
post-networks trained through a differentiable codec proxy, and reports large
gains especially for out-of-scope content and metrics. The full sandwich needs a
custom player, but the prefilter half is decoder-agnostic: a network trained to
transform frames so that H.264 specifically codes them cheaper (soften what the
DCT hates, keep what survives quantization, suppress what AQ would overspend
on). Every existing player benefits. Train offline against a codec proxy plus
VMAF loss, run per-frame on the ANE or GPU at encode time. This is the highest
risk item here and the only one needing a training pipeline, but published
results suggest meaningful BD-rate for grainy and high-frequency content even
in the pre-only configuration.

Sources: [sandwiched compression paper](https://arxiv.org/abs/2402.05887),
[sandwiched video compression](https://arxiv.org/abs/2303.11473),
[reference code](https://github.com/google/sandwiched_compression).

## 7. Reference-frame temporal filtering

SVT-AV1 motion-compensation-filters alt-ref frames before encoding so
references are clean and residuals shrink; it is one of that encoder's larger
quality tools and needs nothing from the decoder. H.264 port: before encoding a
frame that will be referenced (I frames, P frames at low temporal layers), blend
it with motion-compensated neighbors weighted by match quality. Encode-side
only, standard-compliant, biggest wins on noisy and grainy sources, and shares
motion search with the lookahead. Combined with the FGC SEI item, the noise
that temporal filtering removes is exactly what grain synthesis puts back.

Sources: [SVT-AV1 temporal filtering discussion](https://32blog.com/en/ffmpeg/ffmpeg-v8-svtav1-optimal-settings),
[SVT-AV1-PSY release notes](https://svt-av1-psy.com/releases/).

## 8. Cross-rung analysis reuse for ABR ladders

When one invocation produces several resolutions of the same content, encode the
top rung first and seed the lower rungs with its scaled motion field, mode
hints, and shot metadata. x264's analysis dump/load exists but is a niche
two-invocation workflow; making the ladder a first-class single-invocation
output with internal reuse is the differentiator. Composes with per-shot hulls
(section 1): the ladder mode is where per-shot resolution decisions become
usable, since segmented streaming lets resolution change at segment boundaries.
ATHENA's multi-rate encoding work reports large speedups from exactly this kind
of reuse.

Source: [VCA and per-title encoding overview](https://bitmovin.com/blog/video-complexity-analyzer-vca/).

## 9. Per-shot content classification

Classify each shot from lookahead statistics (animation, film grain, screen
content, sports/high motion) and switch tool settings per shot: psy strength,
deadzone, deblock offsets, B depth, AQ strength. The shot machinery from
section 1 makes this nearly free, and it replaces the single global "tune"
switch other encoders make users pick by hand. Low risk, mostly evaluation
work to find per-class settings that beat the global default.

## 10. Live control: a runtime control plane for broadcast and live services

Every established encoder lets a caller swap the parameter set between
frames, and that is the whole of what a live service gets: no word on which
frame took the change, no coupling into the rate control's learned state (the
target moves and the controller lurches as if it had just opened), no way to
say "code every second frame until the queue drains", and nothing coming back
except the packet sizes. Services that adapt to bandwidth and backpressure
build their control loop on guesswork around that call.

The proposal is a first-class control plane: settings issued as per-frame
events (bitrate and VBV, CRF, QP bounds, frame rate or decimation, keyframe
request, speed preset, resolution at a keyframe, live zone offsets), applied
at a defined boundary, echoed back in the frame stats, and coupled into the
rate control through the same opening-refit mechanism that fixed our ABR
start. In the other direction the encoder reports queue depth, per-frame
lateness and a lookahead-derived bit budget, so the service can act before a
buffer overruns rather than after. A control log replays to byte-identical
output, which makes a live session reproducible offline.

Why it is ours to win: the pieces exist here already (per-frame stats in
coding order, zones, the lookahead ring the opening is fitted on, a
deterministic pipeline), and nobody ships the coupled, echoed, replayable
form. The same contract on yah264, yah265 and yaav1 means a service writes
its control loop once and switches codec without touching it. The plan, the
API shape and the example app (a camera or pipe encoder feeding a simulated
network whose bandwidth trace drives the controls) are in
`live-control-plan.md`.

## 11. Research: keep finding the features nobody else has

A standing item rather than a feature: a periodic research pass whose only
job is to find more entries for this document. Read what the streaming
services publish (Netflix, Meta, YouTube, Twitch engineering posts and
papers), the codec conferences (PCS, ICIP, DCC, the AOM and MPEG research
tracks), the open encoders' changelogs and issue trackers, and the
commercial encoders' feature lists, and ask of each: what is a need no
encoder answers, what does an orchestrator or a service have to build around
the encoder today that the encoder could do better from inside, and what
does our architecture (the lookahead ring, the deterministic GOP pipeline,
the per-frame stats channel, three codecs on one interface) make cheap that
is expensive elsewhere. Brainstorm from the user's side too: a broadcaster,
a game streamer, a surveillance vendor, a phone camera, an archive. Each
candidate gets a section here with the evidence, why it should win, and a
sketch of how it lands, or a line in the ideas backlog's refused list with
the reason. The live control plane above came out of exactly this question.

## 12. Smaller items

Temporal-layer-aware chroma QP offsets, from SVT-AV1's tune 3 work: bias chroma
QP by position in the B pyramid for more consistent quality across frames.
Small, cheap, and measurable on top of the B pyramid.

Energy-aware encoding: a cycles-per-quality budget mode that picks per-shot
presets to hit a compute budget, following ATHENA's green-encoding line of
work. Interesting for the M-series efficiency story; low priority until the
per-shot machinery exists.
