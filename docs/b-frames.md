# B-frames

B-frames carry most of the macroblocks in a yah264 stream. At the default
`--bframes 3` the adaptive placement fills every run on the board clips, so
about three pictures in four are B. This page covers
where B-frames go, how a B macroblock picks its mode (direct, the 16x16
lists, B_8x8), and how the encoder decides a B macroblock is a skip. Motion
search and the shared RD machinery are in
[motion-and-modes.md](motion-and-modes.md).

The B path is also where yah264's results vary most by content. With
`--bframes 0` every 1080p clip in the corpus sits within a few percent of
x264 medium, while with B-frames on the same clips spread from well ahead to
well behind. The direct-mode choice is the largest single cause, which is why
`--direct auto` is the default.

## How it works

### Placement

`--b-adapt 1` decides per frame in the lookahead. For each candidate, every
8x8 lowres block takes the cheapest of three costs: inter against the
previous frame, inter against the next one, and intra. The candidate stays a
B while that sum is under 0.77 of its total intra cost.
Otherwise it becomes an anchor. The run limit (`--bframes`), the key-frame
interval and scene cuts are hard limits on top. `--b-adapt 0` codes a fixed
cadence.

With `--bframes 2` or more the run is coded as a pyramid
(`--b-pyramid normal`): the middle B is a reference picture, and the leaves
on either side read it.

`--b-bias` moves the 0.77 line linearly, to 1.0 at +100 and 0 at -100. It's a
dial for trading B-frames away, not a quality setting. See
[options.md](options.md#--b-bias-is-a-tuning-dial).

### The B macroblock at medium

In order, at `--subme 8` and below:

1. **Direct prediction.** The slice's direct mode (spatial or temporal)
   derives a vector pair. Temporal can refuse per macroblock (see below), and
   then this macroblock has no direct mode.
2. **Early B_Skip.** A strict probe, round-to-nearest rather than the P
   probe's deadzone, tests whether the direct residual quantizes to nothing.
   If so, the macroblock is a skip before any search. An exact SSD screen in
   front of the probe (`Y264_BSKIP_SCREEN`) proves most answers without the
   transforms.
3. **Searches.** List 0 searches every active reference, with reference bits
   in the cost. List 1 searches one reference. Bi reuses the list-0 winner's
   reference. Each list is seeded with the spatial neighbors' vectors, the
   direct vector (only when the derivation succeeded) and the lowres pair
   vectors.
4. **SATD phase.** Direct, L0, L1 and Bi 16x16 are built once and ranked by
   SATD plus vector rate. Every mode within `best * 17/16 + 1` survives to
   full RD (18/16 with psy-rd on). That's usually two to four RD trials, and
   Bi and uni stay together when they're close.
5. **Mid-tournament skip exit** (`Y264_B_SKIP_EXIT=1`). After the 16x16 RD
   trials, if skip still has the best cost and direct's SATD is within 33/32
   of the best searched 16x16, a non-reference B commits skip here. Nothing
   after this point runs.
6. **B_8x8**, gated (below).
7. **Intra screen.** I_16x16 SATD must be under 0.75x the best inter SATD
   before intra gets an RD trial.
8. **Commit.** The winner is re-encoded with RDOQ (`--trellis 1`).

At `--subme 9` and up the tournament runs in full: every 16x16 mode gets RD,
16x8 and 8x16 are searched, and none of the gates above apply.

### Direct mode and `--direct auto`

The spatial direct vector is normative. Clause 8.4.1.2.2 derives it from the
neighbors and the co-located block, and the decoder derives the same one, so
the encoder can't improve it. Its choices are the slice's mode flag and
whether a given macroblock uses direct at all.

**Temporal legality is checked per macroblock.** Clause 8.4.1.2.3 requires
the picture a co-located block refers to to be in the current list 0. That
binds where the derivation runs, not at the slice header. A macroblock with
an unresolvable co-located reference loses direct for itself and nothing
else. That costs a median of about 1.7% of macroblocks. A frame-wide check
would have demoted almost nine frames in ten to spatial.

**The per-slice vote.** On one macroblock in eight (a diagonal sample), the
encoder derives both direct modes and runs the strict skip probe on each. It
counts how many macroblocks each mode would make skippable. Counts are folded
into a running score in coding order, decayed by 9/10 whenever the total
passes the sampled macroblock count, and each new B slice takes the mode with
the higher score. The fold is deterministic at every thread count. The
staircase stays on with auto (`Y264_STAIR_TDIR=1`): the derived list-1 vector
is held to the same row bound as the list-1 searches, and a macroblock whose
vector would exceed it drops direct.

`--direct temporal` pins temporal and adds a slice-level guard: temporal is
used only when the co-located picture's own first list-0 picture is this
slice's first list-0 picture. Under the default pyramid that sends 37 of 111
B slices per 150 frames to spatial. Auto doesn't use the guard, because the
vote already refuses temporal where the interval mismatch hurts.

### B_8x8

Four independently predicted 8x8 quadrants, each picking B_Direct_8x8,
B_L0_8x8, B_L1_8x8 or B_Bi_8x8. The 8x4, 4x8 and 4x4 sub-partitions aren't
implemented. Each quadrant is searched from the 16x16 winner per list and
pinned to its list-0 reference. A cold search over every reference would
make the mode unaffordable.

Three things keep it cheap:

- **The quadrant gate** (`Y264_B8_QGATE=10`). Four `satd8x8` on the winning
  16x16 prediction decide whether the eight quadrant searches run. A split
  only pays where the quadrants want different motion, so the gate admits
  when the worst quadrant holds more than `1 + n/16` of an even share:
  `qmax * 4 * 16 > qsum * (16 + n)`.
- **The rate charge.** The quadrant sum is charged B_8x8's own mb_type and
  sub_mb_type rate in the SATD domain: 9 bits for the split, and 1, 3, 3 or 5
  for a direct, L0, L1 or Bi quadrant.
- **The same RD screen** as every other candidate. The trial runs only when
  the quadrant sum is within the tournament's threshold.

The search runs after the skip exit, so macroblocks that leave as skips never
pay for it.

### 16x8 and 8x16

Medium searches no rectangular split by default. With `Y264_B_RECT=1`, each B
macroblock that reaches the RD stage tries the one orientation along which the
16x16 winner's error splits most unevenly, seeded from the 16x16 vectors. It
needs the b8x8 partitions, which medium's automatic set leaves out from 1080p
up. `--subme 9` and up search both orientations and RD every combination.

## What we chose and why

| choice | why |
|---|---|
| `--direct auto` as the default | 14-clip CRF band median 0.0% against spatial: station2 -33%, stockholm -14%, in_to_tree -7%, shields -5%, bus -4%. The worst clip is sunflower at +3.6%, all of it on the two lowest rungs |
| Per-slice vote over a pinned mode | pinned temporal swings from -26.5% to +18.5% against x264 medium depending on the clip (table below). Under the vote sunflower's loss is about +1%, and station2 still gains |
| Per-macroblock temporal legality | the clause binds per derivation. Per-frame enforcement wasted the mode on almost every frame |
| No slice-level guard under auto | adding it lost on every clip it touched: station2 +9.7%, stockholm +3.6%, in_to_tree +2.5%, sunflower +1.7% |
| B_8x8 on, gated at QGATE=10 | 10 of 12 clips better on the CRF band (median -0.33%, worst +0.02%) for 1.1-1.3% of single-thread wall. It carries the board's quality leg |
| QGATE 10 over 6 | 6 costs 2-4% of wall against 10's 1.1-1.3%, for about twice the gain, and it overshoots what the quality bar needs. 16 is past the knee (foreman goes positive) |
| The rate charge with the gate | without it the gate costs half a BD point at its mildest setting. With it, three hundredths |
| B_8x8 searched after the skip exit | searched before it, 19-30% of all quadrant searches are thrown away by the exit. Quality is the same either way |
| No 16x8/8x16 at medium by default | it helps every band clip but costs 13-21% more instructions |
| Mid-exit on non-reference B's only | reference B errors propagate. Byte-identical on the CRF band, worst +0.89% on the ABR band |
| Direct-competitive precondition on the exit | without it the exit over-skips at low ABR rates (akiyo +2.21%, park_joy +1.56%) |
| B intra margin 0.75x | median +0.01% at matched rate, for 0.5-2.5% of wall at 12 threads. Tighter buys nothing more |

Pinned temporal against pinned spatial, BD-rate VMAF-NEG against x264 medium,
rate-anchored, 150 frames:

| clip | spatial | temporal |
|---|--:|--:|
| station2_1080p | -12.5% | -26.5% |
| blue_sky_1080p | +16.5% | -3.0% |
| pedestrian_1080p | -0.4% | +5.0% |
| riverbed_1080p | +2.4% | +4.0% |
| sunflower_1080p | -7.7% | +18.5% |

These modes are expensive in any encoder. x264 pays 4.6-10.6% of its wall
for the same B partition set on four of our clips, and with the quadrant gate
at 4 our price lands between 0.8x and 1.4x of that.

## What didn't work

| idea | why not |
|---|---|
| Cost-driven B placement (pairwise, path-cost at integer and half pel) | all three over-demoted, +8-12% BD. The zero-start lowres diamond overestimates the longest-distance leg |
| `--b-bias` below 0 | -50 removes most B-frames and costs 11-54% |
| Predict the better direct mode from prediction SSD | coastguard predicts better under temporal and codes worse; samsung the reverse |
| Predict it from the direct share | the share barely moves (86.4% vs 86.7%) while BD moves 6 points |
| Decide direct mode per shot in the lookahead, on lowres bi-prediction SATD | in-sample rho -0.76, then 0 for 3 out of sample (stockholm +9.0%, shields +9.5%, parkrun +4.2%). Out-of-sample rho was 0.000 |
| The same lowres vote scored by distance from the lowres search result | rho -0.06. Its spatial arm is biased toward exactly the content temporal suits |
| Pinned `--direct temporal` as the default | sunflower +18.5% against spatial's -7.7% |
| Use a scaled lowres cost as the skip exit's SATD precondition | the ratio's median swings 1.4 to 4.1 across clips. Catches 32-49% at 23-38% false positives |
| Commit B_Skip before motion search on a motion signal (lowres-static, mb-tree leaf, neighbor-skip) | every such gate also admits 10-95% of macroblocks whose searches then win by 8-25% SATD |
| Staged skip decision (neighbor admission, graded probe, commit after the ref-0 search) | built whole. Commits 6-16% of B macroblocks and the wall is a wash, against a +3% bar |
| Propagation-guarded mid-exit for reference B's (`Y264_B_SKIP_EXIT=3`) | quality-neutral (worst +0.07%) but buys only 0.3-0.5% of wall |
| `--b-preme-skip` (skip before search when distortion is under the cheapest coded rate) | a lambda bound widens as rate falls: 2.7 VMAF-NEG points lost on sunflower's bottom rung. The absolute form moves the damage between clips |
| `--rd-surv-rank` (RD only the top-ranked survivors) | +0.63% band median |
| Folding B_8x8's SATD into the RD screen | screens out 16x16 candidates that go on to win RD. The B_8x8 gain drops from -1.27% to -0.81% |

## Open questions

- **The skip decision is closed. The tournament cost isn't.** A perfect B
  skip decision after the ref-0 searches is worth 1.088x and 1.104x on the
  low-rate cells. That's a ceiling nothing built reaches. The remaining prize
  is making the tournament itself cheaper.
- **B_8x8's population.** At its gate the RD trial beats the running best in
  4.5-11% of trials, so most quadrant searches lose. A gate that predicts
  "B_8x8 will win here" better than four `satd8x8` would cut searches at
  equal quality. Measure the predictor's precision on the real population
  first. The same argument for the skip decision underdelivered.
- **The propagation guard's currency.** `mbtree_off` is the combined AQ and
  propagation offset, not the propagation term alone. A guard on
  `mbtree_off >= 0` is right for the wrong reason, and it'll move if
  `--aq-strength` or offset centering changes.
- **Whole-run placement** is an off-by-default arm with
  three cost corrections. It hasn't shipped.
- Temporal direct's availability depends on how often the co-located
  picture's references land in the current list 0. Reference-list
  construction under the pyramid hasn't been costed as a lever.

## Measuring it

- `Y264_DIRECT_WHY=1` prints each B slice's list 0, the co-located reference
  POCs (starring unresolvable ones), a hash of the co-located field and the
  auto vote. `Y264_DIRECT_SCORE=2` logs both modes' skippable counts per
  frame. Both are output-inert.
- `Y264_B8_STAT=1` counts B_8x8 searches, RD trials, wins and skip-exit
  waste. `Y264_BPROF=1` bins B tournament wall by stage and final verdict.
  Both are plain globals: run them at `--threads 1`.
- `scripts/b_census.py` gives the decoder-side B mode census.
- Anchor 1080p comparisons to the calibrated rate
  (`scripts/bd_at_rate.py`, `docs/corpus-sources.md`) or name the band. A
  shared CRF point set puts clips of different difficulty in different
  places, and several 1080p claims turned out to be band artifacts.
- With a rare positive class, resampling isn't validation. The per-shot
  direct selector passed leave-one-out on two positives and failed all three
  new ones.
- Run `scripts/determ_repeat.sh` and `scripts/stair_determ.sh` under load for
  anything touching B analysis order. Recon-match can't see run-to-run
  nondeterminism, because each run decodes to what it built.
- `temporal_direct` returns 0 from inside its four-block loop and leaves its
  output partly written. `analyze_b_mb` zeros the struct first and seeds the
  searches with the direct vector only on success. An uninitialized seed
  there made threaded output vary between runs without touching the recon,
  and neither TSan nor ASan sees it.
- Under CABAC, a B_Direct_8x8 neighbor's `refIdx` condTermFlag is 0 whatever
  its derived reference (9.3.3.1.1.6). It only shows with `--ref` above 1 and
  mixed sub-types.
- `y264_me_mc_luma` writes a stride-16 destination. An 8x8 caller must give
  it a 16-wide buffer.
- Reproduce the B_8x8 arm:

  ```
  Y264_B8_STAT=1 build/cli/yah264 --input-y4m tests/corpus/foreman_cif.y4m \
    --crf 27 --preset medium --threads 1 -o /dev/null
  BANDS=crf ARM='Y264_B8_QGATE=4' python3 scripts/run_band.py
  ```
