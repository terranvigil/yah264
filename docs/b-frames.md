# B-frames

About three pictures in four are B, because adaptive placement fills every
run on the board clips at the default `--bframes 3`. This page covers B
placement, mode choice and skips. See [motion search and mode
decision](motion-and-modes.md) for the shared machinery.

Results vary with content most on the B path. At
`--bframes 0` every 1080p corpus clip is within a few percent of x264 medium.
With B-frames they range from well ahead to well behind. The largest single
cause is the direct-mode choice. That's why `--direct auto` is the
default.

Quality figures are BD-rate on VMAF v1. The rate-charge and B_8x8
SATD-folding rows are on VMAF-NEG (earlier metric).

## How it works

### Placement

With `--b-adapt 1` the lookahead decides frame by frame. A candidate stays a B
while the sum of its 8x8 lowres blocks' cheapest costs (inter from either
neighbor, or intra) is under 0.77 of its total intra cost. `--bframes`, the
key-frame interval and scene cuts are hard limits. `--b-adapt 0` codes a
fixed cadence.

With `--bframes 2` or more, `--b-pyramid normal` makes the middle B a
reference that the leaves on either side read.

`--b-bias` moves the 0.77 line linearly, to 1.0 at +100 and 0 at -100. It's a
dial for trading B-frames away. See [why it isn't a quality
setting](options.md#--b-bias-is-a-tuning-dial).

### The B macroblock at medium

At `--subme 8` and below, a B macroblock goes through these steps:

1. **Direct prediction.** The slice's direct mode, spatial or temporal,
   derives a vector pair. A macroblock that temporal refuses has no direct
   mode.
2. **Early B_Skip.** If the direct residual quantizes to nothing under a
   strict round-to-nearest probe (the P probe uses a deadzone), the
   macroblock skips before any search. An exact SSD screen
   (`Y264_BSKIP_SCREEN`) settles most answers without transforms.
3. **Searches.** List 0 searches all active references, counting reference
   bits. List 1 searches one. Bi reuses the list-0 winner's. Seeds are the
   spatial neighbors' vectors, the lowres pair vectors and, when derived, the
   direct vector.
4. **SATD phase.** The direct, L0, L1 and Bi 16x16 predictions are ranked by
   SATD plus vector rate. Modes within `best * 17/16 + 1`, or 18/16 with
   psy-rd, go to full RD, usually two to four RD trials. Bi and uni both stay
   in when they're close.
5. **Mid-tournament skip exit** (`Y264_B_SKIP_EXIT=1`). After the 16x16 RD
   trials, a non-reference B commits skip when skip still has the best cost
   and direct's SATD is within 33/32 of the best searched 16x16.
6. **B_8x8**, behind the quadrant gate below.
7. **Intra screen.** Intra gets an RD trial only when I_16x16 SATD is under
   0.75x the best inter SATD.
8. **Commit.** The winner is re-encoded with RDOQ (`--trellis 1`).

`--subme 9` and up skip these gates, screens and exits. Every 16x16 mode gets
RD, and 16x8 and 8x16 are searched.

### Direct mode and `--direct auto`

Because clause 8.4.1.2.2 fixes the spatial direct vector, the encoder only
chooses the slice's mode flag and whether each macroblock uses direct.

Temporal legality is checked per macroblock, since clause 8.4.1.2.3 requires
the co-located block's reference to be in list 0 at each derivation. A
failing macroblock loses direct for itself only, a median of about 1.7% of
macroblocks. A frame-wide check would have demoted almost nine frames in ten
to spatial.

Auto picks each slice's mode by vote. On a diagonal sample of one macroblock
in eight, the encoder derives both modes and counts how many macroblocks each
would make skippable under the strict probe. The counts fold into a running
score in coding order, decaying by 9/10 whenever the total passes the sampled
count. Each new B slice takes the higher score, deterministically at any thread
count.

The staircase (`Y264_STAIR_TDIR=1`) stays on with auto and drops direct
where the derived list-1 vector exceeds the list-1 searches' row bound.

`--direct temporal` adds a slice-level guard: temporal only when the
co-located picture's first list-0 picture is this slice's. Under the default
pyramid it sends 37 of 111 B slices per 150 frames to spatial. Auto leaves the
guard out because the vote already refuses temporal where the interval
mismatch hurts.

### B_8x8

B_8x8 splits the macroblock into four quadrants that pick direct, L0, L1 or
Bi. The 8x4, 4x8 and 4x4 sub-partitions aren't implemented. A quadrant is
searched from the 16x16 winner per list and held to its list-0 reference,
because a cold search over all references would make the mode unaffordable.

Three things keep B_8x8 cheap. First, the quadrant gate (`Y264_B8_QGATE=10`)
runs four `satd8x8` on the winning 16x16 prediction, because a split only
pays where the quadrants want different motion. The eight quadrant searches
run only when the worst quadrant holds more than `1 + n/16` of an even share:
`qmax * 4 * 16 > qsum * (16 + n)`.

Second, the quadrant sum is charged B_8x8's mb_type and sub_mb_type rate in
the SATD domain:

| part | bits |
|---|--:|
| the split itself | 9 |
| a direct quadrant | 1 |
| an L0 or L1 quadrant | 3 |
| a Bi quadrant | 5 |

Third, the RD trial runs only when the quadrant sum is within the
tournament's threshold. The search runs after the skip exit, so skips never
pay for it.

### 16x8 and 8x16

Medium searches no rectangular split by default. `Y264_B_RECT=1` tries the
orientation that splits the 16x16 winner's error most unevenly, seeded from
its vectors, on macroblocks that reach RD. It needs the b8x8 partitions,
which medium drops from 1080p up. `--subme 9` and up search both
orientations and run RD on all combinations.

## What we chose and why

| choice | why |
|---|---|
| `--direct auto` as the default | median 0.0% against spatial on 14 clips, with station2 -32%, stockholm -12%, in_to_tree -6%, sita -1.8%, bus -1.7% and shields -1.5%. The worst clip is sunflower at +3.3%, all of it on the two lowest rungs |
| Per-slice vote over a fixed mode | fixed temporal ranges from -14.3% to +12.1% against x264 medium depending on the clip, as the table below shows. The vote follows spatial on sunflower and pedestrian, and station2 still gains (-11.5% against spatial's -3.1%) |
| Per-macroblock temporal legality | the clause applies to each derivation. Enforcing it per frame wasted the mode on almost every frame |
| No slice-level guard under auto | adding it lost on every clip it touched, station2 worst |
| B_8x8 on, behind QGATE=10 | better on 8 of 13 clips on the CRF band. The board's quality leg rests on it |
| QGATE 10 over 6 | 6 buys more gain than the quality bar needs, for more wall. 16 is past the knee |
| The rate charge with the gate | on the earlier metric, without it the gate costs half a BD point at its mildest setting. With it the cost is three hundredths |
| B_8x8 searched after the skip exit | searching before the exit throws away 19-30% of all quadrant searches. Quality is the same either way |
| No 16x8/8x16 at medium by default | on VMAF v1 it helps every band clip but costs 13-21% more instructions |
| Mid-exit on non-reference B's only | errors in a reference B propagate. Against the full tournament it's within 0.02% on the CRF band except sintel, +0.51%. It's within 0.15% on the ABR band |
| Direct-competitive precondition on the exit | without it the exit skips too often at low ABR rates, and akiyo and park_joy lose |
| B intra margin 0.75x | against 1.5x, median 0.00% on the CRF band with sintel the worst clip at +1.28%, for 0.5-2.5% of wall at 12 threads. A tighter margin buys nothing more |

The quadrant gate settings, on the CRF band:

| QGATE | B_8x8 wall | BD-rate |
|---|--:|---|
| 6 | 2-4% | a further -0.35% median against 10, about 2.5x the total gain of 10 |
| 10 (shipped) | 1.1-1.3% of single-thread wall | -0.24% median against B_8x8 off, worst sintel +0.37% |
| 16 | | +0.26% median against 10. Every clip but akiyo and touchdown loses, and touchdown doesn't change |

On four of our clips x264 spends 4.6-10.6% of its wall on the same B
partitions. With the gate at 4, our cost was 0.8x to 1.4x of x264's.

Direct mode against x264 medium, rate-anchored at the corpus operating points
over 150 frames. Both encoders ran at three threads.

| clip | spatial | temporal | auto |
|---|--:|--:|--:|
| station2_1080p | -3.1% | -14.3% | -11.5% |
| blue_sky_1080p | +4.7% | -7.5% | -4.5% |
| pedestrian_1080p | -12.7% | -8.0% | -12.7% |
| riverbed_1080p | -11.2% | -11.2% | -11.2% |
| sunflower_1080p | -13.3% | +12.1% | -13.3% |

## What didn't work

| idea | why not |
|---|---|
| Cost-driven B placement (pairwise, path-cost at integer and half pel) | all three demoted too many B-frames and lost clearly. The lowres diamond starts from zero and overestimates the longest-distance leg |
| `--b-bias` below 0 | -50 removes most B-frames and costs 10-51% on four clips at the HD band's CRF 23-35 ladder ([options.md](options.md)) |
| Predict the better direct mode from prediction SSD | coastguard predicts better under temporal but codes worse. samsung does the reverse |
| Predict it from the direct share | the share barely moves, 86.4% against 86.7%, while BD moves by several points |
| Decide direct mode per shot in the lookahead, on lowres bi-prediction SATD | it predicted well in sample and not at all out of sample, where it picked the wrong mode on stockholm, shields and parkrun |
| The same lowres vote scored by distance from the lowres search result | no correlation with the better mode. Its spatial arm is biased toward exactly the content temporal suits |
| `--direct temporal` as the default | sunflower +12.1% against spatial's -13.3% |
| Use a scaled lowres cost as the skip exit's SATD precondition | the ratio's median ranges from 1.4 to 4.1 across clips. It catches 32-49% at 23-38% false positives |
| Commit B_Skip before motion search on a motion signal (lowres-static, mb-tree leaf, neighbor-skip) | every such gate also admits 10-95% of macroblocks whose searches then win by 8-25% SATD |
| Staged skip decision (neighbor admission, graded probe, commit after the ref-0 search) | built in full. It commits 6-16% of B macroblocks. The wall is a wash against a +3% bar |
| Propagation-guarded mid-exit for reference B's (`Y264_B_SKIP_EXIT=3`) | quality-neutral (worst akiyo +0.20%) but buys only 0.3-0.5% of wall |
| `--b-preme-skip` (skip before search when distortion is under the cheapest coded rate) | a lambda bound widens as rate falls. On sunflower it reads the same as the absolute form: both cost 0.85% against no early skip, 0.4 points on the bottom rung. The absolute form moves the damage between clips. Medium still runs mode 2 on pictures 1920 or more on the longer side |
| `--rd-surv-rank` (RD only the top-ranked survivors) | +0.63% HD-band median, pedestrian +1.78% |
| Folding B_8x8's SATD into the RD screen | screens out 16x16 candidates that go on to win RD. On the earlier metric it gives back over a third of the B_8x8 gain |

## Open questions

- The skip decision is settled. Nothing built reaches the 1.088x and 1.104x
  a perfect B skip after the ref-0 searches is worth on the low-rate cells.
  The remaining prize is a cheaper tournament.
- Past the gate, B_8x8 beats the running best in only 4.5-11% of RD trials.
  A better predictor than four `satd8x8` would save searches. Measure its
  precision on the real population first, since the same argument
  underdelivered for skips.
- The propagation guard reads `mbtree_off`, the combined AQ and propagation
  offset. A guard on `mbtree_off >= 0` is right for the wrong reason and will
  move if `--aq-strength` or offset centering changes.
- Whole-run placement is an unshipped, off-by-default arm with three cost
  corrections.
- Temporal direct is available only when the co-located picture's references
  are in list 0. Reference-list construction under the pyramid could raise
  that. We haven't costed it.

## Measuring it

The `Y264_*` variables need a `-Ddev_knobs=true` build.

- `Y264_DIRECT_WHY=1` prints each B slice's list 0, the co-located reference
  POCs (starred when unresolvable), a hash of the co-located field and the
  vote. `Y264_DIRECT_SCORE=2` logs both modes' skippable counts per frame.
  Neither changes the output.
- `Y264_B8_STAT=1` counts B_8x8 searches, RD trials, wins and skip-exit
  waste. `Y264_BPROF=1` bins B tournament wall by stage and verdict. Both use
  plain globals, so run them at `--threads 1`.
- `scripts/b_census.py` gives the decoder-side B mode census.
- Anchor 1080p comparisons to the [calibrated rate](corpus-sources.md) with
  `scripts/bd_at_rate.py`, or name the band. A shared CRF point set places
  clips of different difficulty differently. Several 1080p claims were band
  artifacts.
- Resampling doesn't validate a rare positive class. The per-shot direct
  selector passed leave-one-out on two positives and failed all three new
  ones.
- Run `scripts/determ_repeat.sh` and `scripts/stair_determ.sh` under load for
  anything touching B analysis order. Recon-match can't see run-to-run
  nondeterminism, since each run decodes to what it built.
- `temporal_direct` returns 0 mid-loop with its output partly written.
  `analyze_b_mb` zeros the struct first and seeds with the direct vector only
  on success. An uninitialized seed there made threaded output vary between
  runs without touching the recon, invisible to TSan and ASan.
- Clause 9.3.3.1.1.6 sets a B_Direct_8x8 neighbor's `refIdx` condTermFlag to
  0 under CABAC, whatever its derived reference. It only shows with `--ref`
  above 1 and mixed sub-types.
- `y264_me_mc_luma` writes a stride-16 destination. An 8x8 caller needs a
  16-wide buffer.
- Reproduce the B_8x8 arm:

  ```
  Y264_B8_STAT=1 build/cli/y264 --input-y4m tests/corpus/foreman_cif.y4m \
    --crf 27 --preset medium --threads 1 -o /dev/null
  BANDS=crf ARM='Y264_B8_QGATE=4' python3 scripts/run_band.py
  ```
