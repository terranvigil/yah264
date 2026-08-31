# Animation and anime: the plan

What x264 and the wider industry do for animation, which of it survives contact
with our own measurements, and the ranked arms. Companion to
`docs/animation-content.md` (why the 1.34x same-preset number is not a defect).

Provenance: this plan was researched under the CONTRIBUTING.md constraint. No
x264/x265/ffmpeg source code was read, opened, or quoted. Inputs were x264's
`--fullhelp` text, published papers and blog posts, community writeups, and our
own measurements. Every mechanism below is stated in terms of the coding tool
and the content property, not another codebase's implementation.

All new numbers in this doc: self-A/B `bdcompare.py --vmaf --no-cache`,
BD-rate VMAF-NEG, 180 frames, CRF 30,34,38,42 (desaturated reads at 34-46 where
marked), arm vs the shipped default. Negative = the arm is better. Wall numbers
are interleaved medians (5-7 rounds) at t12 to /dev/null.

## 1. Content model: what animation is as a signal

| property | 2D hand-drawn anime | 3D CGI (bbb/sintel) | coding tool implicated |
|---|---|---|---|
| large flat fills | dominant | partial (sintel yes, bbb less) | skip/DC modes cheap; blocking and banding fully visible (no noise masking); variance AQ mis-allocates |
| hard 1-2px line art | dominant | soft edges, motion blur | ringing from aggressive texture-preserving RD; intra edge prediction |
| noise floor | none (digital) or film grain (cel era) | render grain often ADDED, partly to mask gradient banding | psy energy retention helps only when there is energy to retain |
| gradients in dark scenes | common | common | banding, the class's #1 failure mode; 8-bit rounding with nothing to dither it |
| temporal cadence | on 2s/3s: 30-60% held frames; static cel backgrounds reappear after occlusion | rendered every frame, motion-blurred; bbb has ZERO near-duplicates (min consecutive luma diff 15.3, median 20.7); sintel 12.7% exact repeats | frame skip, B depth, DPB reach |
| scene cuts | hard and frequent | softer | b-adapt cut handling (our hard-cut defect was fixed 2026-07-19) |
| text/overlays | credits, signs, subs | little | intra on sharp edges; niche |

The important split is the NOISE FLOOR, not the medium. Digitally clean
cel-style content wants blocking/ringing/banding protected and has no texture
to preserve. Grainy content (film-era anime, CGI with render grain) is the
opposite, and the community's most-flagged mistake is applying "anime settings"
to grainy sources: the grain was masking the banding.

Both our animation clips are 3D CGI. bbb (sustained motion, textured fur and
foliage, no held frames) is the natural-video end of the animation class;
sintel (flat dark cel-like shading, 12.7% held frames) is the closest thing we
have to the anime signal. Hand-drawn anime sits past sintel on every axis.

## 2. What the industry does (sources and confidence)

x264's published animation tune (`--fullhelp`): bframes +2, deblock 1:1,
psy-rd 0.4, aq-strength 0.6, refs doubled. The stated mechanisms (doom9
"x264-Settings for Anime" thread, MeGUI settings wiki; HIGH confidence on the
reasoning, the constants are eyeballed): redundancy rewards deep B/DPB;
blocking is naked on flat fills so deblock harder; psy energy preservation
manufactures ringing on line art and has no grain to save, so weaker.

**Three of those four constants are measured DEAD on our encoder** (see 3).
The mechanisms above are still good; the constants do not transfer, because our
default psy-rd (2.0), AQ (0.4 variance) and RD depth already sit elsewhere.

Fansub/BDrip practice (silentaperture mdbook guide, kokomins x265 anime guide;
HIGH confidence it represents practice, mixed mechanism/lore):
- bframes 8 measured worth ~3-5% on anime, marginal gain past 6-8 under 1%
  except near-static content (kokomins, measured).
- AQ with a dark-scene bias, strength ~0.6-0.7, named at banding in dark flats
  (mechanism stated).
- psy-rd tiered by noise floor: ~1.0 clean digital, 1.5-2.0 grainy (mechanism
  stated; values taste).
- ref 16, merange 64, no-dct-decimate: lore momentum, no published marginal
  gains (cargo cult; and our --ref 6/9 measured +0.65/+0.30 here).
- 10-bit encoding of 8-bit sources ("Hi10P"): higher internal precision in
  prediction/MC preserves gradient steps that 8-bit rounding bands, usually
  SAVING bits at matched quality (Ateme/Larbier whitepaper ~5-20%; doom9: the
  win is mostly on animation). Codec-agnostic mechanism; the H.264-specific
  part was only the ecosystem price (no hardware decode for Hi10P, and the
  fansub scene paid it anyway, which says how big the banding win looked).
  HIGH on mechanism, MED on magnitude.

Streaming industry: Netflix's founding per-title example is animation (My
Little Pony 1080p at 1.5 Mbps vs a 5.8 Mbps fixed ladder), i.e. their
animation answer is RATE ALLOCATION, not codec tools; then shot-based encoding
(HIGH, measured at catalog scale). They built CAMBI, a banding-specific
no-reference metric, precisely because VMAF misses banding on flat/dark
content (HIGH). Bilibili published a neural pre-filter, codec-agnostic (MED).
HEVC screen-content tools (intra block copy, palette) name animation in the
literature but do not exist in H.264 and never shipped in anime pipelines.

## 3. Our measurements (this session, 2026-08-26)

x264 tune-constant transfer (coordinator's runs, bbb): psy-rd 0.4 **+1.10%**,
psy-rd 1.0 **+0.69%**, ref 6 **+0.65%**, ref 9 **+0.30%**. All dead. Our
shipped `--tune animation` reads bbb **-3.69%** but sintel **+4.47%**.

Per-element decomposition of our own tune (new):

| element | bbb_720p | sintel_720p | verdict |
|---|---|---|---|
| psy-trellis 0.5 | +0.57% | +0.00% (byte-identical) | dead; the shipped flat-gate psy lattice already exceeds 0.5 on sintel and 0.5 hurts bbb |
| aq-strength 0.6 | +1.00% | +5.93% | dead, hurts both |
| bframes 5 | -3.81% | -1.14% | the only live element |
| bframes 7 | -6.80% (desat 34-46) | -1.01% | better; wall 1.00x bbb, 0.945x sintel |
| aq-strength 0.2 | +2.23% | -3.73% | AQ preference splits the class |
| aq-strength 0 | +6.81% | -6.86% | symmetric split, 13.7 points apart |

So the coordinator's question has a clean answer: the tune is NOT content-luck
and "animation" is not an incoherent class. It is three constants pulling in
different directions. bframes is class-coherent (helps both, and helps sintel's
wall). psy-trellis is superseded by a shipped, better-targeted feature. AQ is
the axis the class genuinely splits on: flat cel-like content (sintel, and
hand-drawn anime beyond it) wants variance AQ weak-to-off, textured CGI (bbb)
wants the default. bframes 7 on natural clips: foreman -3.04, samsung +6.20,
bus +2.34, so it stays tune/selector material, not a default.

(Oddity, unexplained: bframes 5 costs 11.9% wall on bbb while bframes 7 costs
zero, reproducible across 12 interleaved rounds. Something in the auto
la_buf = bframes+1 or mini-GOP geometry. One BPROF/NTP_PROF look if bframes 5
is ever the pick; bframes 7 sidesteps it.)

Duplicate frames (the owner's suspicion): **refuted, in the good direction.**
bbb has zero near-duplicates, so nothing was on that table for the measured
1.34x. A synthetic on-2s bbb (fps=15,fps=30: 450 frames, 225 exact repeats)
moves us 1.677s -> 1.374s (-18%) and x264-asm only 1.249 -> 1.203 (-3.7%);
the wall ratio improves 1.343 -> 1.142. We exploit repeats BETTER than x264,
and we emit fewer bytes on both clips at the same CRF. Hand-drawn anime, with
its held frames, should therefore SHRINK the speed gap relative to bbb, which
is the worst case (sustained unique motion).

Where the animation slowdown actually lives (BPROF t1, crf 26): on bbb, 54% of
B MBs whose final verdict is SKIP escape the early skip-accept and run the
full tournament (526k of 966k; ~70% of the 4.17s B tournament is billed to
eventual-skips). On samsung the escape rate is 19%. P side: 56% vs 28%. The
per-MB skip-eval cost is the same on both clips; the difference is purely the
escape rate. Animation's high skip share multiplies our known skip-verdict
cost (the goal-3 operating-point mechanism).

## 4. Ranked arms (expected value / cost to measure)

Ordered; the first three are the ones to actually run first.

**A. Re-cut `--tune animation` to what measures (quality, free).**
Drop psy_trellis 0.5 and aq 0.6, make the tune bframes +4 (7 total).
Measured: bbb -6.80%, sintel -1.01%, wall 1.00x/0.945x. One CLI hunk.
Confirming gate: `bdcompare` both animation clips at 30,34,38,42 AND 34-46
(saturation), `bd_at_rate.py` since bframes moves achieved rate, wall
interleaved t12, `determ_repeat.sh` loaded + conformance as always. Ships as a
tune, so corpus neutrality is not required, but run foreman/samsung/bus for
the record (done once: -3.04/+6.20/+2.34).

**B. Flat-gated skip-accept (speed, env-knobs only to measure).**
The named mechanism for the 1.34x: eventual-skip MBs running the full
tournament at 2.8x the natural-video escape rate. The corpus-wide rescue space
is EXHAUSTED (memory: all three refused 08-20, rows are the trade's price; do
not re-run those arms corpus-wide). The unmeasured shape is CONTENT-GATED:
engage the stronger skip exits only where the flat-MB share is high, the same
classifier the shipped psy lattice already computes per frame.
Measure before building: sweep `Y264_B_SKIP_EXIT=3` (passed E1 already),
`Y264_B_SKIP_EXIT_SSD`, `Y264_P_SKIP_EXIT=1`, `Y264_MIDSKIP=1` on bbb+sintel:
wall interleaved t12 + BD both clips + `run_band.py BANDS=crf` neutrality.
If the ungated knobs read as a wall win with a BD price ON ANIMATION TOO, the
gated version is dead, stop. If animation absorbs them free (its skips are
truer), the gate is worth the small wiring. Ceiling from BPROF: ~half the
B+P tournament wall on bbb is eventual-skip overhead; even a third of it is
~5-8% clip wall. Bands/BD are load-immune; wall runs need the spinner check.

**C. Corpus: add hand-drawn 2D clips (prerequisite for the class's name).**
Both current animation clips are 3D CGI; every anime-specific claim is
unsupported until a 2D clip exists. Candidates, licenses verified by search
(TEST-ONLY forever, per the gate-corpus rule):
- **Sita Sings the Blues** (CC0, archive.org): flat 2D fills, hard edges, held
  frames. High-bitrate master, not lossless; fine for BD self-A/B. Cheapest.
- **Sol Levante** (Netflix + Production I.G, CC BY 4.0, opencontent.netflix.com):
  real hand-drawn 4K anime, 16-bit masters on public S3. Needs tone-mapping to
  SDR 4:2:0. The only true anime master with a clean license.
- **Hero** (Blender Studio 2018, CC-BY): Grease Pencil 2D line art, digitally
  clean; .blends downloadable, can re-render lossless.
- Grainy-cel class if ever wanted: Fleischer Superman shorts (US PD), which is
  the "film-era anime" signal (grain + cels).
Cost: a fetch row + `parity-clip-calib.sh` point + CLASSES tag. Re-run arms A
and B on the new clip before shipping either as "animation" behavior.

**D. Flat-gated AQ (quality, modest implementation).**
The measured 13.7-point aq-strength split between bbb and sintel, plus the
community's dark-flat-banding mechanism, plus hand-drawn anime sitting past
sintel on flatness. Shape: scale aq_strength down (0.4 -> ~0.1-0) when the
frame's flat-MB share is high, reusing the psy-lattice classifier; or hand the
choice to the M4 per-content selector, where animation is now the strongest
per-content signal yet observed. Measure first with the existing knob:
`--aq-strength` sweep is already done per clip; the arm's real gate is the
GATED form on bbb (must hold ~0.00) and sintel (should collect several of the
-6.86). Then `band_at_rate.py` corpus + deep band. Risk: AQ interacts with
mb-tree consumption (jointly adapted, do not re-sweep those axes); change only
the strength input the gate already owns.

**E. Deblock offsets (quality at low rates, ~1 day implementation).**
The one element of x264's tune we cannot test: no --deblock knob, slice header
hardcodes 0:0 (encoder.c:2236), deblock.c:211 already notes the spec offset
slot (Clip3(0,51, qPav + offset), spec 8.7.2.2). Blocking on flat fills is
real and unmasked on animation, and offsets are spec-defined slice-header
fields (conformance gate covers them). Implement the knob, sweep -1:-1..2:2 on
both animation clips at the DEEP band (blocking is a starved-rate artifact;
`BANDS=deep band_at_rate.py`), plus CIF natural clips for no-harm. Two
cautions: tune-constant transfer has failed three times this week, so sweep,
do not copy 1:1; and deblock is our worst SIMD family (9.3x vs x264), so
stronger filtering buys wall cost on our slowest kernels; price wall at t12.

**F. Banding instrumentation + the 10-bit arm (differentiator, research).**
CAMBI runs locally today (`vmaf --feature cambi`, verified). Step 1 is free:
score our existing animation encodes vs x264 at the deep band for CAMBI beside
VMAF-NEG; if we band less at matched rate that is a publishable
differentiator, if we band more it names the next quality arm (VMAF cannot see
it either way, which is why Netflix built the metric). Step 2, the Hi10P
question: our 10-bit is a compile-time build (-Dbit_depth=10, verified the
8-bit build refuses 10-bit input); encode 8-bit-upconverted animation in the
10-bit build, decode, CAMBI + BD vs the 8-bit build at matched rate. Mechanism
is codec-agnostic and well-attested; H.264 Hi10P has no hardware decode, so
this ships as a measurement/story and an option, not a default.

**G. Content-adaptive preset floor (speed, owner-adjacent).**
docs/animation-content.md already shows our veryfast beats x264 medium on
animation quality at 1.07x wall. The generalization is the per-content
selector (M4) choosing the operating point from the lookahead's content class.
Not animation-specific work, but animation is its best-measured customer now;
this is where the "1.34x" conversation actually closes, and it is
differentiator design (Fable-budget territory), not a constant to sweep.

## 5. What NOT to do

- Do not lower psy-rd toward x264's 0.4, and do not double refs. Both measured
  worse here (psy-rd 0.4 +1.10, ref 6 +0.65). Re-test psy-rd only if a 2D
  clip lands AND shows ringing; measure, never copy the constant.
- Do not build VFR/dedup or any duplicate-frame machinery. Measured: we
  already beat x264 on duplicate exploitation (-18% wall on a 50%-dup clip vs
  their -3.7%). Community dedup practice is telecine correctness, not bitrate;
  duplicates are near-free in-codec.
- Do not trade the -29.76% BD lead for wall. Any speed arm on this content
  gates on BD-neutral both animation clips, and the deep band, before its wall
  number is allowed to argue. The preset ladder (arm G) is the sanctioned way
  to spend quality for speed here.
- Do not re-run the refused corpus-wide ME/skip rescues as-is (memory:
  exhausted, three refusals). Only the content-gated form (arm B) is new.
- Do not chase HEVC screen-content tools (intra block copy, palette): not in
  H.264, cannot ship in a conforming bitstream.
- Do not gate any banding claim on VMAF alone; that is what CAMBI is for.
- Cargo cult to ignore: ref 16, merange 64, --no-dct-decimate, "bframes 16",
  x265 grain-tune analogies. No published marginal gains, and our ref sweep
  already read positive-BD.
- Do not ship any animation arm measured on one clip. This week's tune would
  have shipped as a -3.69% win from bbb alone and cost sintel +4.47.

## 6. Measurement discipline for this track

Both animation clips on every arm, always; add the 2D clip the day it lands.
bbb saturates above VMAF-NEG ~94 at CRF 30, so quote the 34-46 read beside the
standard points. bbb_720p is not in the standing band ladders (CLIPS_CALIB,
not CLIPS; adding it re-medians the published numbers and is an owner call),
so animation gates run through bdcompare/bd_at_rate directly for now. BD and
bands are load-immune; wall numbers follow the interleaved-median rules and
the spinner check like everything else.

---

# Executed 2026-08-26

## Item 1, re-cut `--tune animation`: SHIPPED

bbb_720p **-9.99%**, sintel_720p **+0.15%**, against the old tune's -3.69% and
+4.47%. The clip it was hurting is neutral and the clip it helped nearly tripled.
Default path byte-identical, `make test` 9/9. Commit 21d7709.

## Item 2, flat-gated skip-accept via existing knobs: CLOSED EMPTY

Every existing skip-exit knob, wall and BD measured together on both animation
clips, `--threads 1`, crf 31 for wall and points 30/34/38/42 for BD:

| arm | bbb wall | bbb BD | sintel wall | sintel BD |
|---|--:|--:|--:|--:|
| `Y264_B_SKIP_EXIT=3` | 0.990x | -0.04% | 0.995x | +0.27% |
| `Y264_P_SKIP_EXIT=1` | 1.011x | +0.94% | 1.016x | +17.77% |
| `Y264_P_SKIP_EXIT=2` | 0.964x | +5.71% | 1.000x | +32.15% |

The P-side exits are worse than refused, they are actively expensive: at crf 34
on sintel `P_SKIP_EXIT=2` emits **13.6% MORE bits** than the default for the same
rate factor (arm verified live, md5s differ). They exit on a cost comparison that
misjudges flat content, so the skip they take is one the tournament would have
priced better. `B_SKIP_EXIT=3` is free on bbb and costs a little on sintel while
buying 1% of wall, which is not worth a default.

**The mechanism is real but these knobs do not address it.** 54% of eventual-skip
B macroblocks on bbb escape early skip-accept and run the full tournament,
against 19% on samsung, and that is where animation's extra wall lives. Capturing
it needs a gate that recognises flat content BEFORE deciding, not a cheaper exit
after the fact. That is implementation work with a real design question in it,
not a sweep, and it should be costed against the fact that we are already
-29.76% BD-rate ahead here: any version that spends bits is a bad trade.

Ceiling estimate of 5-8% clip wall is unretired. Nothing measured it; the knobs
that exist reach 3.6% at +5.71% BD.

---

# Executed 2026-08-30

## Arm B, re-run at THREADS=12 and completed: the content gate is DEAD

The 08-26 pass above measured three of the four named knobs at `--threads 1`.
This pass closes the arm: it adds the two never-measured cells
(`Y264_B_SKIP_EXIT=2`, with and without a widened `Y264_B_SKIP_EXIT_SSD`, and
`Y264_MIDSKIP=1`) and prices every arm at the threaded operating point the
plan actually asks for.

**Wall, `bench/wall_ab.py`, interleaved t12, medians of 11, both animation
clips at the plan's crf 31** (new `bbbA` / `sintelA` cells; bbb_720p 300
frames, sintel_720p 144 -- note `bench/` is gitignored, so those two cells are
local to this box and a fresh clone re-adds them). Read every number NET OF THE CONTROL: the bbbA cell
carries a persistent base-vs-ctrl bias of +1.17% (it held at +1.52% over 5
runs and +1.17% over 11), which is larger than most of the arms.

| arm | bbb raw | bbb net of ctrl | sintel raw | sintel net of ctrl |
|---|--:|--:|--:|--:|
| ctrl (duplicate base) | +1.17% | -- | -0.04% | -- |
| `Y264_B_SKIP_EXIT=3` | +1.52% | **+0.35%** | +0.32% | **+0.36%** |
| `Y264_MIDSKIP=1` | +1.30% | **+0.13%** | +1.39% | **+1.43%** |
| `Y264_P_SKIP_EXIT=1` | +2.36% | **+1.19%** | -0.04% | **0.00%** |

`Y264_B_SKIP_EXIT=2` is **inert on bbb** -- md5 identical to the default over
the whole cell, with and without `B_SKIP_EXIT_SSD=1024` -- so its +1.2%/+2.1%
raw readings were the cell's noise. It moves output on sintel and buys +0.38%
there, which is nothing.

**BD, `bdcompare.py --vmaf --no-cache`, self-A/B, same binary and args as the
wall cells.** `Y264_MIDSKIP=1` was the one arm never measured on animation:

| arm | clip | points | BD-rate (VMAF-NEG) |
|---|---|---|--:|
| `Y264_MIDSKIP=1` | bbb_720p | 30,34,38,42 | +2.01% (saturation flagged) |
| `Y264_MIDSKIP=1` | bbb_720p | 34,38,42,46 | **+2.75%** |
| `Y264_MIDSKIP=1` | sintel_720p | 30,34,38,42 | **+13.03%** |

**The kill criterion this arm was given fires.** The plan said: if the ungated
knobs read as a wall win with a BD price ON ANIMATION TOO, the gated version is
dead, stop. Every knob that buys any wall costs BD on both animation clips --
MIDSKIP +2.75/+13.03, P_SKIP_EXIT=1 +0.94/+17.77 (08-26) -- and the one arm
that is nearly free on quality, `B_SKIP_EXIT=3`, buys 0.35% of threaded wall.
There is no cell where animation absorbs a stronger skip exit for free, so the
flat-content classifier that would gate them has nothing to gate. **Arm B is
closed. Do not re-sweep these four knobs on animation.**

**The new fact is the threaded denominator.** At t1 `B_SKIP_EXIT=3` read 0.990x
on bbb (1.0%); at t12, net of control, it reads 0.35%. The tournament work
these exits delete is work the wavefront was already overlapping, so pricing
this family at t1 -- which is how the 08-26 table was taken -- roughly triples
what it looks worth. Any future skip-side arm gets a t12 number before it gets
a design.

**Band neutrality was deliberately NOT run.** `run_band.py BANDS=crf` gates
corpus damage for something that would ship; nothing here survives to ship, and
the corpus-wide rescue space for these same knobs was already refused on 08-20.

**The ceiling stays unretired and the mechanism stays real.** BPROF still puts
~5-8% of clip wall in eventual-skip tournament overhead; the knobs that exist
reach 1.2% of t12 wall at a BD price. What is now also known is that the gate
was never the missing piece -- the exits themselves misprice flat content, on
both animation clips, which is why every one of them pays in bits. A cheaper
tournament for eventual-skip MBs remains the live target, and it needs a
different instrument than these four knobs.
