# Brief: the variance of the B path

For a frontier-tier session. Written 2026-08-31, at the end of the round that
produced the evidence. Everything below is measured on this tree; nothing is
argued from priors.

## The question

**Our B-frame path swings from a 13.5-point advantage to a 15.1-point loss
depending on content, and nobody knows why.** That spread is larger than any
other effect on the quality track, and it is invisible in the twelve-clip
corpus we have been tuning against.

The narrow form, if the broad one is too much for one session: how often the
co-located picture's references land in the current slice's list 0, since that
decides whether temporal direct is available at all, and availability looks
worth more per frame than the mode choice.

## The evidence

BD-rate against x264 medium, VMAF-NEG, CRF, 150-frame windows, points chosen
per clip to stay off saturation. Negative means we spend fewer bits for the
same quality.

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
ahead, against 1080p median +0.43% with 3 of 6. Same twelve clips, none of
which this encoder was ever tuned against. `docs/corpus-sources.md` has the
full table and the calibrated operating points.

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

The P-frame census inversion is a red herring worth 0.7%: at matched rate we
code 7.50% intra in P frames against x264's 4.70%, and our intra split is 94%
I_16x16 against their 26%. Striking, nearly worthless.

## The spec constraints that bound the design

**The spatial direct vector is normative.** Clause 8.4.1.2.2 derives it from
the neighbours and the co-located block, and the decoder derives the same one.
The encoder cannot improve it. Ours reads spec-shaped.

**Temporal direct is legal on only 11-24% of B frames** -- blue_sky 3 of 28,
coastguard 3 of 28, samsung 6 of 25. Clause 8.4.1.2.3 makes it a requirement of
bitstream conformance that the picture referred to by `refIdxCol` be present in
RefPicList0, and `build_slice_prep` enforces that frame-wide. x264 tests
something narrower, `fref[1][0]->i_poc_l0ref0 == fref[0][0]->i_poc`, which
holds for it because of how it builds its lists.

**So `--direct temporal` collects -6.39% on blue_sky while engaging on about a
ninth of its B frames.** Whatever those frames are doing, they are doing a lot
of it, which points at the reference B frames in the pyramid whose quality
propagates into every leaf that references them. That is the single most
suggestive unexplained number in this brief.

## Instruments that exist

| instrument | answers |
|---|---|
| `Y264_DIRECT_SCORE=2` | both direct modes' skippability per B frame, x264's own auto signal. Also reports where temporal is not legal. md5-inert |
| `Y264_BPROF=1` / `BPROF2=1` | per-stage wall inside the B tournament, binned by final verdict |
| `scripts/b_census.py` | decoder-side B mode census: direct / L0 / L1 / Bi shares |
| `scripts/bd_at_rate.py` | BD at matched achieved rate, mandatory for anything that moves the CRF mapping |
| `Y264_TYPE_ORACLE` | replay x264's frame-type placement through us, so placement prices separately |
| `docs/instruments.md` | the rest, organised by the question they answer. **Read it before building a probe** |

## Owner-gated, so do not spend the session on them

Promoting any new corpus clip (it re-medians every published number), flipping
`--direct temporal` (bare yah264 is deliberately x264 medium, and x264 medium
is spatial), firing CI.

## What a good outcome looks like

A mechanism for the variance that predicts the sign on clips it was not derived
from, plus a named arm with a gate. The corpus rule is that anything fitted
trains on external video and tests on ours, once, at the end. The twelve-clip
gate corpus is test-only, permanently.

A second, entirely untouched thread if the first closes early: riverbed and
crowd_run are 3-4% behind **without** B-frames, on water and dense-crowd
content. That is a base-path question and nothing in this tree has looked at it.
