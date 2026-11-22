# B-frames

B-frames hold most of the macroblocks in a yah264 stream. Because adaptive
placement fills every run on the board clips at the default `--bframes 3`,
about three pictures in four are B. This page covers where B-frames go, how a
B macroblock picks a mode and how the encoder decides that a B macroblock is
a skip. The modes are direct, the 16x16 list modes and B_8x8.
[Motion search and mode decision](motion-and-modes.md) covers motion search
and the shared RD machinery.

yah264's results vary more with content on the B path than anywhere else.
With `--bframes 0` every 1080p clip in the corpus sits within a few percent
of x264 medium, while with B-frames on the same clips spread from well ahead
to well behind. We made `--direct auto` the default because the direct-mode
choice is the largest single cause.

## How it works

### Placement

With `--b-adapt 1` the lookahead decides frame by frame. For each candidate,
each 8x8 lowres block takes the cheapest of three costs: inter against the
previous frame, inter against the next one, or intra. The candidate stays a B
while that sum is under 0.77 of its total intra cost. Otherwise it becomes an
anchor. The run limit (`--bframes`), the key-frame interval and scene cuts
are hard limits on top of that. `--b-adapt 0` codes a fixed cadence.

With `--bframes 2` or more, `--b-pyramid normal` codes the run as a pyramid.
The middle B is a reference picture that the leaves on either side read.

`--b-bias` moves the 0.77 line linearly. The line reaches 1.0 at +100 and
drops to 0 at -100. It's a dial for trading B-frames away. The
[options page](options.md#--b-bias-is-a-tuning-dial) explains why it isn't a
quality setting.

### The B macroblock at medium

At `--subme 8` and below, a B macroblock goes through these steps in order:

1. **Direct prediction.** The slice's direct mode, spatial or temporal,
   derives a vector pair. Temporal can refuse a macroblock, as the next
   section explains. That macroblock then has no direct mode.
2. **Early B_Skip.** A strict probe tests whether the direct residual
   quantizes to nothing. It rounds to nearest where the P probe uses a
   deadzone. When the residual vanishes, the macroblock is a skip before any
   search. An exact SSD screen in front of the probe (`Y264_BSKIP_SCREEN`)
   proves most answers without the transforms.
3. **Searches.** List 0 searches all active references and counts reference
   bits in the cost. List 1 searches one reference. Bi reuses the list-0
   winner's reference. Each list's search is seeded with the spatial
   neighbors' vectors and the lowres pair vectors. The direct vector joins
   them only when its derivation succeeded.
4. **SATD phase.** The direct, L0, L1 and Bi 16x16 predictions are built once
   and ranked by SATD plus vector rate. A mode survives to full RD when it's
   within `best * 17/16 + 1`, or 18/16 with psy-rd on. That usually means two
   to four RD trials. Bi and uni both stay in when they're close.
5. **Mid-tournament skip exit** (`Y264_B_SKIP_EXIT=1`). After the 16x16 RD
   trials, a non-reference B commits skip here when skip still has the best
   cost and direct's SATD is within 33/32 of the best searched 16x16. Nothing
   after this point runs.
6. **B_8x8**, behind the quadrant gate described below.
7. **Intra screen.** I_16x16 SATD must be under 0.75x the best inter SATD
   before intra gets an RD trial.
8. **Commit.** The winner is re-encoded with RDOQ (`--trellis 1`).

At `--subme 9` and up the full tournament runs, with none of the gates, screens
or exits above. Each 16x16 mode gets RD. The encoder also searches 16x8 and
8x16.

### Direct mode and `--direct auto`

The spatial direct vector is normative. Clause 8.4.1.2.2 derives it from the
neighbors and the co-located block. Because the decoder derives the same
vector, the encoder can't improve it. All the encoder chooses is the slice's
mode flag and whether a given macroblock uses direct at all.

Temporal legality is checked per macroblock. Clause 8.4.1.2.3 requires the
picture that a co-located block refers to to be in the current list 0. The
requirement holds where the derivation runs, not at the slice header. A
macroblock with an unresolvable co-located reference loses direct for itself
and nothing else. That costs a median of about 1.7% of macroblocks, where a
frame-wide check would have demoted almost nine frames in ten to spatial.

Auto chooses the mode for each slice by vote. On a diagonal sample of one
macroblock in eight, the encoder derives both direct modes, runs the strict
skip probe on each and counts how many macroblocks each mode would make
skippable. Those counts fold into a running score in coding order that decays
by 9/10 whenever the total passes the sampled macroblock count. Each new B
slice takes the mode with the higher score. The fold is deterministic at
every thread count.

The staircase stays on with auto (`Y264_STAIR_TDIR=1`). It holds the derived
list-1 vector to the same row bound as the list-1 searches. A macroblock
whose vector would exceed the bound drops direct.

`--direct temporal` fixes the mode to temporal and adds a slice-level guard.
The slice uses temporal only when the co-located picture's own first list-0
picture is this slice's first list-0 picture. Under the default pyramid the
guard sends 37 of 111 B slices per 150 frames to spatial. We left the guard
out of auto because the vote already refuses temporal where the interval
mismatch hurts.

### B_8x8

B_8x8 splits the macroblock into four independently predicted 8x8 quadrants.
Each quadrant picks B_Direct_8x8, B_L0_8x8, B_L1_8x8 or B_Bi_8x8. The 8x4,
4x8 and 4x4 sub-partitions aren't implemented. Each quadrant is searched from
the 16x16 winner per list and held to its list-0 reference, because a cold
search over all references would make the mode unaffordable.

Three things keep B_8x8 cheap. The quadrant gate (`Y264_B8_QGATE=10`) runs
four `satd8x8` on the winning 16x16 prediction to decide whether the eight
quadrant searches run at all. A split only pays where the quadrants want
different motion. The gate admits a macroblock when its worst quadrant holds
more than `1 + n/16` of an even share: `qmax * 4 * 16 > qsum * (16 + n)`.

The quadrant sum is then charged B_8x8's own mb_type and sub_mb_type rate in
the SATD domain. The split costs 9 bits. A direct quadrant costs 1, L0 and
L1 quadrants cost 3 and a Bi quadrant costs 5.

Last, B_8x8 faces the same RD screen as the other candidates. Its trial runs
only when the quadrant sum is within the tournament's threshold. Because the
search runs after the skip exit, macroblocks that leave as skips never pay
for it.

### 16x8 and 8x16

Medium searches no rectangular split by default. With `Y264_B_RECT=1`, a B
macroblock that reaches the RD stage tries the one orientation that splits
the 16x16 winner's error most unevenly. That search is seeded from the 16x16
vectors. The knob needs the b8x8 partitions, which medium's automatic set
leaves out from 1080p up. `--subme 9` and up search both orientations and run
RD on all combinations.

## What we chose and why

| choice | why |
|---|---|
| `--direct auto` as the default | median 0.0% against spatial on the 14-clip CRF band, with station2 -33%, stockholm -14%, in_to_tree -7%, shields -5% and bus -4%. The worst clip is sunflower at +3.6%, all of it on the two lowest rungs |
| Per-slice vote over a fixed mode | fixed temporal ranges from -26.5% to +18.5% against x264 medium depending on the clip, as the table below shows. Under the vote sunflower's loss is about +1%, and station2 still gains |
| Per-macroblock temporal legality | the clause applies to each derivation. Enforcing it per frame wasted the mode on almost every frame |
| No slice-level guard under auto | adding it lost on every clip it touched: station2 +9.7%, stockholm +3.6%, in_to_tree +2.5%, sunflower +1.7% |
| B_8x8 on, behind QGATE=10 | better on 10 of 12 clips on the CRF band for 1.1-1.3% of single-thread wall (median -0.33%, worst +0.02%). The board's quality leg rests on it |
| QGATE 10 over 6 | 6 costs 2-4% of wall where 10 costs 1.1-1.3%. It buys about twice the gain, which is more than the quality bar needs. 16 is past the knee, where foreman goes positive |
| The rate charge with the gate | without it the gate costs half a BD point at its mildest setting. With it the cost is three hundredths |
| B_8x8 searched after the skip exit | searching before the exit throws away 19-30% of all quadrant searches. Quality is the same either way |
| No 16x8/8x16 at medium by default | it helps every band clip but costs 13-21% more instructions |
| Mid-exit on non-reference B's only | errors in a reference B propagate. Byte-identical on the CRF band, worst +0.89% on the ABR band |
| Direct-competitive precondition on the exit | without it the exit skips too often at low ABR rates: akiyo +2.21%, park_joy +1.56% |
| B intra margin 0.75x | median +0.01% at matched rate, for 0.5-2.5% of wall at 12 threads. A tighter margin buys nothing more |

These modes are expensive in any encoder. On four of our clips, x264 spends
4.6-10.6% of its wall on the same B partition set. With the quadrant gate at
4, our cost comes to between 0.8x and 1.4x of x264's.

The table below compares fixed temporal with fixed spatial direct. Both are
BD-rate VMAF-NEG against x264 medium, rate-anchored over 150 frames.

| clip | spatial | temporal |
|---|--:|--:|
| station2_1080p | -12.5% | -26.5% |
| blue_sky_1080p | +16.5% | -3.0% |
| pedestrian_1080p | -0.4% | +5.0% |
| riverbed_1080p | +2.4% | +4.0% |
| sunflower_1080p | -7.7% | +18.5% |

## What didn't work

| idea | why not |
|---|---|
| Cost-driven B placement (pairwise, path-cost at integer and half pel) | all three demoted too many B-frames, +8-12% BD. The lowres diamond starts from zero and overestimates the longest-distance leg |
| `--b-bias` below 0 | -50 removes most B-frames and costs 11-54% |
| Predict the better direct mode from prediction SSD | coastguard predicts better under temporal but codes worse. samsung does the reverse |
| Predict it from the direct share | the share barely moves, 86.4% against 86.7%, while BD moves 6 points |
| Decide direct mode per shot in the lookahead, on lowres bi-prediction SATD | rho -0.76 in sample, then 0 for 3 out of sample: stockholm +9.0%, shields +9.5%, parkrun +4.2%. Out-of-sample rho was 0.000 |
| The same lowres vote scored by distance from the lowres search result | rho -0.06. Its spatial arm is biased toward exactly the content temporal suits |
| `--direct temporal` as the default | sunflower +18.5% against spatial's -7.7% |
| Use a scaled lowres cost as the skip exit's SATD precondition | the ratio's median ranges from 1.4 to 4.1 across clips. It catches 32-49% at 23-38% false positives |
| Commit B_Skip before motion search on a motion signal (lowres-static, mb-tree leaf, neighbor-skip) | every such gate also admits 10-95% of macroblocks whose searches then win by 8-25% SATD |
| Staged skip decision (neighbor admission, graded probe, commit after the ref-0 search) | built in full. It commits 6-16% of B macroblocks. The wall is a wash against a +3% bar |
| Propagation-guarded mid-exit for reference B's (`Y264_B_SKIP_EXIT=3`) | quality-neutral (worst +0.07%) but buys only 0.3-0.5% of wall |
| `--b-preme-skip` (skip before search when distortion is under the cheapest coded rate) | a lambda bound widens as rate falls, which lost 2.7 VMAF-NEG points on sunflower's bottom rung. The absolute form moves the damage between clips |
| `--rd-surv-rank` (RD only the top-ranked survivors) | +0.63% band median |
| Folding B_8x8's SATD into the RD screen | screens out 16x16 candidates that go on to win RD. The B_8x8 gain drops from -1.27% to -0.81% |

## Open questions

- The skip decision is settled. Nothing built reaches the 1.088x and 1.104x
  that a perfect B skip decision after the ref-0 searches is worth on the
  low-rate cells. The tournament's cost is still open. The remaining prize is
  making the tournament itself cheaper.
- Most B_8x8 quadrant searches lose. Past its gate, the RD trial beats the
  running best in 4.5-11% of trials. A gate that predicts "B_8x8 will win here"
  better than four `satd8x8` would save searches at equal quality. Measure the
  predictor's precision on the real population first, because the same argument
  underdelivered for the skip decision.
- The propagation guard reads the wrong quantity. `mbtree_off` is the
  combined AQ and propagation offset. It doesn't isolate the propagation term.
  A guard on `mbtree_off >= 0` is right for the wrong reason. It'll move if
  `--aq-strength` or offset centering changes.
- Whole-run placement exists as an off-by-default arm with three cost
  corrections. It hasn't shipped.
- Temporal direct's availability depends on how often the co-located
  picture's references fall in the current list 0. We haven't costed
  reference-list construction under the pyramid as a way to raise it.

## Measuring it

- `Y264_DIRECT_WHY=1` prints each B slice's list 0, the co-located reference
  POCs with a star on unresolvable ones, a hash of the co-located field and
  the auto vote. `Y264_DIRECT_SCORE=2` logs both modes' skippable counts per
  frame. Neither changes the output.
- `Y264_B8_STAT=1` counts B_8x8 searches, RD trials, wins and skip-exit
  waste. `Y264_BPROF=1` bins B tournament wall by stage and final verdict.
  Both keep their counts in plain globals, so run them at `--threads 1`.
- `scripts/b_census.py` gives the decoder-side B mode census.
- Anchor 1080p comparisons to the calibrated rate with
  `scripts/bd_at_rate.py`, or name the band. The page on
  [where the corpus comes from](corpus-sources.md) says where the calibrated
  points live. A shared CRF point set puts clips of different difficulty in
  different places. Several 1080p claims turned out to be band artifacts.
- Resampling doesn't validate a rare positive class. The per-shot direct
  selector passed leave-one-out on two positives and failed all three new
  ones.
- Run `scripts/determ_repeat.sh` and `scripts/stair_determ.sh` under load for
  anything touching B analysis order. Recon-match can't see run-to-run
  nondeterminism, because each run decodes to what it built.
- `temporal_direct` returns 0 from inside its four-block loop and leaves its
  output partly written. `analyze_b_mb` zeros the struct first and seeds the
  searches with the direct vector only on success. An uninitialized seed
  there made threaded output vary between runs without touching the recon.
  Neither TSan nor ASan sees it.
- Clause 9.3.3.1.1.6 sets a B_Direct_8x8 neighbor's `refIdx` condTermFlag to
  0 under CABAC, whatever its derived reference. The case only shows with
  `--ref` above 1 and mixed sub-types.
- `y264_me_mc_luma` writes a stride-16 destination. An 8x8 caller must give
  it a 16-wide buffer.
- Reproduce the B_8x8 arm:

  ```
  Y264_B8_STAT=1 build/cli/yah264 --input-y4m tests/corpus/foreman_cif.y4m \
    --crf 27 --preset medium --threads 1 -o /dev/null
  BANDS=crf ARM='Y264_B8_QGATE=4' python3 scripts/run_band.py
  ```
