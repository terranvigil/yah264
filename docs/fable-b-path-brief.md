# Brief: the variance of the B path

For a frontier-tier session. Written 2026-08-31, at the end of the round that
produced the evidence. Everything below is measured on this tree. Nothing is
argued from priors.

## The question

**Our B-frame path swings from a 13.5-point advantage to a 15.1-point loss
depending on content, and nobody knows why.** That spread is larger than any
other effect on the quality track. It is also invisible in the twelve-clip
corpus we have been tuning against.

If the broad form is too much for one session, take the narrow one: how often
do the co-located picture's references appear in the current slice's list 0?
That decides whether temporal direct is available at all, and availability
looks worth more per frame than the mode choice.

## The evidence

The table is BD-rate against x264 medium, in VMAF-NEG at CRF over 150-frame
windows. Points were chosen per clip to stay off saturation. Negative means we
spend fewer bits for the same quality.

| clip | with B-frames | `--bframes 0` | what B-frames contribute |
|---|--:|--:|--:|
| station2_1080p | -13.69% | -0.18% | **+13.5 for us** |
| sunflower_1080p | -8.39% | -0.08% | **+8.3 for us** |
| crowd_run_1080p | +1.76% | +3.79% | +2.0 |
| riverbed_1080p | +2.40% | +3.09% | +0.7 |
| pedestrian_1080p | -0.90% | -0.75% | -0.15 |
| **blue_sky_1080p** | **+14.17%** | **-0.94%** | **-15.1 against us** |

Two readings, both load-bearing:

**Without B-frames every 1080p clip sits between -0.75% and +3.79%.** The base
path is at parity. The entire 1080p story is the B path.

**The 720p / 1080p split that opened this** is 720p median -13.93% with 6 of 6
ahead, against 1080p median +0.43% with 3 of 6. These are the same twelve
clips. This encoder was never tuned against any of them. `docs/corpus-sources.md` has the
full table and the calibrated operating points.

Noted 2026-09-03: the figures above are the published control, CRF windows with
a point set chosen per clip. The rate-anchored solve in `docs/b-direct-mode.md`
lands within 1.3 points of it on all six. It is not the same number: blue_sky
reads +14.40% and the 1080p median +0.70%. Quote one instrument or the other,
and say which.

## What is already ruled out, with numbers

Do not re-walk these. All measured on blue_sky at matched achieved rate.

| candidate | reading |
|---|--:|
| seven allocation knobs | best is AC_GAIN=1.0 at -0.84%; aq-strength 0 is +23.15%; **mb-tree OFF is +5.66%, so mb-tree helps** |
| the fine-intra gate | -0.69%, and FINE_M 24 / 32 / always-on are identical because a 1.5x fine margin is vacuous against the 1.5x admission margin |
| multiref | `--ref 1` still reads +10.40% |
| the B skip exits | `Y264_B_SKIP_EXIT=0`, ie the full tournament, reads +0.39% |
| B partitioning | `Y264_B_RECT=1` +0.61%, `Y264_B_8X8=0` +0.11% |
| direct over-selection as a CAUSE | the share is 86.41% spatial against 86.74% temporal, ie unchanged, while BD moves 6.4 points |
| prediction error as a selector signal | coastguard predicts -0.6% better under temporal and codes +1.08% worse; samsung the reverse |

The P-frame census inversion is a red herring worth 0.7%. At matched rate we
code 7.50% intra in P frames against x264's 4.70%. Our intra split is 94%
I_16x16 against their 26%. It is striking and nearly worthless.

## The spec constraints that bound the design

**The spatial direct vector is normative.** Clause 8.4.1.2.2 derives it from
the neighbors and the co-located block, and the decoder derives the same one.
The encoder cannot improve it. Ours reads spec-shaped.

**Temporal direct is legal on only 11-24% of B frames**: blue_sky 3 of 28,
coastguard 3 of 28, samsung 6 of 25. Clause 8.4.1.2.3 makes it a requirement of
bitstream conformance that the picture referred to by `refIdxCol` be present in
RefPicList0, and `build_slice_prep` enforces that frame-wide.

**So `--direct temporal` collects -6.39% on blue_sky while engaging on about a
ninth of its B frames.** Whatever those frames are doing, they are doing a lot
of it. That points at the reference B frames in the pyramid, whose quality
propagates into every leaf that references them. That is the single most
suggestive unexplained number in this brief.

**Corrected 2026-09-03.** Both the 11-24% and the -6.39% are artifacts of
enforcing that requirement frame-wide, and the explanation above rests on them.
The clause applies where the derivation runs. A block whose reference does not
resolve costs its own macroblock the direct mode and nothing else. Gating per
macroblock costs about 1.7% of macroblocks and recovers 89% of the frames. See
"the guard was at the wrong granularity" in `docs/b-direct-mode.md`. A
slice-level legality guard (`Y264_TDIR_LEGAL`) then shipped on 2026-09-03. It
reaches only slices that would code temporal direct, so the spatial default is
untouched. Under the default B-pyramid it structurally sends 37 of 111 B slices
per 150 frames to spatial.

## State of the tree at handoff (2026-08-31, late)

Read this before touching a measurement. Everything here was established the
same day the brief was written. Several items would otherwise cost a round.

**The board you quote matters, and two of them disagree.** The published goal
figures come from **`scripts/ffboard.py`**, which runs both encoders as libraries
inside one ffmpeg process. `perf-comp-crf-set.sh` runs two CLIs with per-process
setup inside the measurement and reads **0.05-0.16 worse**. Comparing one
against the other shows a regression that is not there. That mistake cost most
of an evening.
Say which board produced any number.

**The tooling exists and here is where it lives.** ffmpeg with libyah264 at
`/tmp/ffmpeg-yah264/ffmpeg`, our library installed at `/tmp/y264inst`, x264 at
`/tmp/x264asm` and `/tmp/x264noasm`. Control worktrees are built at
`/private/tmp/y264base` (pre-session) and `/private/tmp/y264pre`
(pre-rename, so its binary is `next264`). `/tmp` is not durable, and
`docs/ffmpeg-integration-plan.md` has the rebuild recipe.

**A goal cannot be adjudicated on one day's draw.** Today ffboard read G1 1.05 /
G2 0.92 / G3 1.01 against a record of 0.95 / 0.85 / 0.96. Three builds
spanning the whole window read identically. The day-to-day spread reaches
~0.10, which is larger than goal 3's entire margin. Quote from repeat draws or attach
the spread.

**The corpus doubled and the boards did not move together.** Twelve HD clips
were fetched and calibrated (`docs/corpus-sources.md`), and ten have operating
points. `scripts/parity-clips.sh` was rebalanced to ten clips spanning
400-25000 kbps, with the old six kept as `CLIPS_LEGACY`. **`ffboard.py`
hardcodes its own copy of the six** and was deliberately left alone. The two
boards currently disagree on purpose as queue item 0b records. Do not reconcile them
inside a measurement round.

**On the speed side, RATE orders the table about four times as strongly as
resolution.** Under 3000 kbps we read 1.44x at one thread, and at 3000+ we read
1.17x. If any B-path arm is priced for wall, price it at both ends of that
range.

**Three traps were caught the same day. Each produced a confident wrong number
instead of an error.** The rename renamed the pure-C knob, so an old binary's
"pure-C" arm silently ran with NEON and faked a 0.73x (`NEXT264_NO_ASM` vs
`YAH264_NO_ASM`). zsh abandons an entire command when any
glob in it fails, which hid a build that existed. And an awk field offset
reported millions of skips from an 11,000-macroblock clip. What caught all
three was asking whether a number was POSSIBLE, not whether it was surprising.

## Instruments that exist

| instrument | answers |
|---|---|
| `Y264_DIRECT_SCORE=2` | both direct modes' skippability per B frame (the skippability vote). Also reports where temporal is not legal. md5-inert |
| `Y264_BPROF=1` / `BPROF2=1` | per-stage wall inside the B tournament, binned by final verdict |
| `scripts/b_census.py` | decoder-side B mode census: direct / L0 / L1 / Bi shares |
| `scripts/bd_at_rate.py` | BD at matched achieved rate, mandatory for anything that moves the CRF mapping |
| `Y264_TYPE_ORACLE` | replay x264's frame-type placement through us, so placement prices separately |
| `docs/instruments.md` | the rest, organized by the question they answer. **Read it before building a probe** |

## Owner-gated, so do not spend the session on them

Three things need the owner: promoting any new corpus clip (it re-medians every
published number), flipping `--direct temporal` (bare yah264 is deliberately
x264 medium, and x264 medium is spatial), and firing CI.

## What a good outcome looks like

A mechanism for the variance that predicts the sign on clips it was not derived
from, plus a named arm with a gate. The corpus rule is that anything fitted
trains on external video and tests on ours, once, at the end. The twelve-clip
gate corpus is test-only, permanently.

There is a second, entirely untouched thread if the first closes early.
Riverbed and crowd_run are 3-4% behind **without** B-frames, on water and
dense-crowd content. That is a base-path question. Nothing in this tree has
looked at it.

**Contradicted 2026-09-03.** The first screen of that thread, in
`docs/b-direct-mode.md`, reads the same `--bframes 0` control at +1.02% on
riverbed and **-2.20%** on crowd_run. We are ahead on crowd_run. The 3-4% came
from a band far below both clips' calibrated operating points. The thread is
still open. These two numbers are just not the reason to open it.

## One more piece of evidence, added at handoff

The `--bframes 0` probe covered all six 1080p clips, not only blue_sky.
It says the deficit is **not one mechanism**:

| clip | with B | no B | what B contributes |
|---|--:|--:|--:|
| station2 | -13.69% | -0.18% | **+13.5 for us** |
| sunflower | -8.39% | -0.08% | **+8.3 for us** |
| crowd_run | +1.76% | +3.79% | +2.0 |
| riverbed | +2.40% | +3.09% | +0.7 |
| pedestrian | -0.90% | -0.75% | -0.15 |
| **blue_sky** | **+14.17%** | **-0.94%** | **-15.1 against us** |

**Without B-frames every 1080p clip sits between -0.75% and +3.79%.** The base
path is at parity. The B path carries the entire spread, from +13.5 to
-15.1. blue_sky does not represent a 1080p problem. It is one end of a variance
problem whose other end is a large win. That is why this brief is about the
VARIANCE and not a mean deficit. It is also the strongest single argument that
one mechanism explains both ends.
