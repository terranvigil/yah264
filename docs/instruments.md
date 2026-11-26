# Instruments by the question they answer

These are the reusable measurement tools that have produced a result. Check
here before you build a profiler, oracle or A/B harness: it's probably in the
tree already under a name nobody remembered. Feature knobs live in
[knobs.md](knobs.md), which `scripts/knob_census.py` generates with every
`Y264_*` env var's reader, default and tier.

## 1. Where does the time go?

### Profilers by stage

- `Y264_BPROF=1`: per-stage wall in the B and P macroblock tournaments, binned
  by final verdict, with macroblocks-touched counts. Prints `BPROF` and `PPROF`
  at exit. t1 only, inert by default, md5-identical.
- `Y264_BPROF2=1`: splits one B RD trial into encode, dist and bits, with the
  tr-pre share. It sizes the RD-trial arm.
- `Y264_MBT_SPLIT=1`: mb-tree cost by stage (invq, bind, phaseA, phaseB,
  finish) plus source and memo-miss counts. About 95% of the reference B's cost
  is phase A.
- `Y264_MBT_MISSWHY=1|2`, with `Y264_MBT_SPLIT=1`: why a phase A source had no
  pair legs to reuse, splitting the `nobleg` bucket into anchor, untyped,
  null-leg and no-have. `=2` prints one line per miss (POC, ring offset,
  settled bound, the stored pair's anchor POCs, the pair the walk wanted) and
  is t1 only. The misses are window-frontier leaves with no future anchor
  bracket yet (`want <poc>/INT_MIN`), not a keying defect. md5-identical.
- `Y264_STAGE_PROF=1` in a `-Dc_args=-DY264_STAGE_PROF` build: per-stage ms in
  the macroblock loop and deblock. **t1 only.** With more threads it silently
  writes a zero-byte bitstream and exits 0. Shipped code is unaffected.
- `-Dc_args=-DY264_IPROF`: exact retired instructions by stage, as `IPROF`
  lines at exit. Use it over a time profile for any "how many instructions"
  question: a time sample charges memory stalls to whatever was stalling, which
  inflated sunflower's per-trial psy term about 6x.
  - Build: `meson setup build-ip --buildtype=release -Ddev_knobs=true -Dc_args=-DY264_IPROF`.
    The normal build compiles it out.
  - Stages: integer ME, sub-pel, RD trial, RD trial bits, commit re-encode,
    intra, entropy emit, deblock, lookahead, mb-tree, the rest of the
    macroblock decision, hpel plus borders.
  - It reads the thread's own counter (`thread_selfcounts`, no root) per scope
    and subtracts the roughly 1,690 instructions each read costs. Works at any
    `--threads`. Stage totals reach 97% of an unperturbed run.
- `Y264_THREAD_PROF=1`: per-stage buckets across the encode. **These buckets
  have lied before**, so corroborate before acting.
- `Y264_THREAD_PROF=1` plus `Y264_NTP_PROF=1`: adds a pool-idle column per
  driver stage, which separates a stage that idles the machine from one that
  overlaps the wavefront.
- `Y264_NTP_PROF=1`, the idle split: busy, midrow, gate, ramp, tail and nojob
  per worker, against the pool's empty clock.
- `-Dc_args=-DY264_OP_LEDGER` (own build dir): counts real operations (SATD
  calls by site, early exits, skips, intra admits) as `Y264LED` lines. Use it
  when a sampled profile is suspect.

### The GOP-parallel schedule

`Y264_GOP_TRACE=1` (CLI) is the only instrument that sees the CLI's outer
layer. It prints one stderr line per GOP, then the reader's EOF, the dispatch
time and plan, and process totals. Output is unchanged.

| field | meaning |
|---|---|
| worker, `k` | the worker and its frame-thread share |
| pulled, start, end | times on the encode's clock |
| `tcpu` | the worker thread's CPU seconds |
| `pcpu`, `pinstr` | process CPU seconds and retired instructions over the GOP |

- At one frame thread `tcpu` is the whole GOP. `pcpu` and `pinstr` belong to
  one GOP only under `Y264_GOP_FORCE_G=1`, the only form that prices k > 1.
- `Y264_GOP_FORCE_G=1 Y264_GOP_FORCE_K=1` prices every GOP at one frame
  thread, the input a schedule model needs. Use `pinstr` there, with CPU
  seconds as the cross-check: the 12 slower cores run at about 0.75 the speed
  of the 6 fast ones.
- On 1080p, k=2 costs about +2.5% instructions over k=1, and k=3 the same as
  k=2.

### Instruction counts against the reference

`scripts/instr-ratio.sh N` prints the instruction-count ratio against x264
beside the wall ratio. It reads `/usr/bin/time -l` on macOS and
`perf stat -x, -e instructions,cycles,task-clock` on Linux.

- **Linux needs readable perf.** At a stock `kernel.perf_event_paranoid` of 2
  or 4 the row prints `NO-COUNTER`. Fix it with
  `sysctl kernel.perf_event_paranoid=1` or a privileged container.
- Never read it under QEMU or Rosetta, whose counts measure the emulator. The
  x86 column is a rented-metal reading.

### Memory and stalls

- `scripts/alloc_census.sh OUT -- CMD` (`tools/alloc_census/`): which call
  sites hold or churn memory. Every allocation of 256 KiB or more, by its two
  innermost frames and size class, with count, peak GiB live and GiB
  allocated. macOS dyld interposing, no rebuild. Peak "live" is virtual, so
  read it beside `/usr/bin/time -l`'s peak footprint.
- `sample <pid> 15 1 -file F` then `scripts/sample_phases.py F`: CPU by encoder
  stage at any thread count, the threaded twin of BPROF. Blocked waits are
  dropped, yield spins kept, and kernel time lands on the user frame that
  trapped (that's how compressor cost shows up). `--rules` loads another
  binary's rules, kept in local notes. `--bin NAME=PATH` adds an nm fallback.
- `xctrace record --template 'CPU Counters' --attach <pid>` then
  `scripts/cc_split.py`: cycle-weighted useful, processing, delivery and
  discarded issue-slot shares. **Attach, never `--launch`**, which hangs here.
  Attach in steady state, since later threads are missed.
- **The memory-pressure check**: compare `/usr/bin/time -l`'s peak footprint
  with max RSS, page reclaims and sys time, and watch `vm_stat` Decompressions
  during the run. Past about 20-30 GiB resident the box compresses. Check this
  before reading any 2160p CPI.

### SIMD ablation

- `Y264_ASM_OFF=<class>` sends one dispatch class to its C fallback. Its wall
  delta is what that class buys.
- `Y264_SIMD_FORCE=none|sse4|avx2|avx512` caps the whole binary at one ISA
  tier, so one binary can show what the SSE4.2 build would do on an AVX2 box.
  `YAH264_NO_ASM=1` is the control, and `none` is its twin. The x86 names are
  inert on aarch64.

Because neither moves a byte, both are identity-cmp axes. Meson's `-Dsimd=` is the
build-time twin.

### Motion search, B modes and direct

- `Y264_ME_ETSTAT=1`: what an integer-search early-out would delete and cost.
  Post-seed probes are bucketed by the best seed's cost per pixel, with how
  often and how far hex, grid or diamond then moved the MV. Read the MV
  columns, not the gain, which doesn't predict BD. t1, md5-exact.
- `Y264_B8_STAT=1`: B_8x8 / B_RECT engagement (tournament entries, quadrant
  searches, RD trials, trials that beat the best, searches dropped by the skip
  exit, rectangular searches admitted). t1 only. See
  [motion-and-modes.md](motion-and-modes.md).
- `Y264_LRSUB_PROBE=1` / `Y264_LRSUB_CENSUS=1`: wall and per-phase reads of
  the lookahead's fifteen quarter-pel plane builds. They take about 3% of
  wall, and none is free to delete: all 15 phases are read and 99.8% of rows
  touched.
- `Y264_DIRECT_WHY=1`: why a B slice fell back from temporal direct (list 0,
  co-located reference POCs starred where unresolved, block counts,
  direct-unavailable count). t1. See [b-frames.md](b-frames.md).
- `Y264_DIAG_DIRECTOK=n`: forces direct unavailable on every nth macroblock to
  isolate the `direct_ok == 0` path.
- `Y264_DIAG_COLWATCH=1`: hashes the co-located field at a B frame's first and
  last macroblock instead of at slice prep, where a hash proves nothing.
- `Y264_DIAG_TDIRLIM=n`: refuses temporal direct when the derived mvL0/mvL1
  exceeds +/-n pel.
- `Y264_DIAG_NOHPEL=1`: forces `y264_me_mc_luma_s`'s non-hpel fallback, to
  test whether a result depends on which MC path a block took. The paths are
  byte-identical, which makes it an identity-cmp axis too.
- `Y264_DIRECT_PERMB=0`: the frame-wide temporal-direct gate, for A/B against
  the per-macroblock default. It engages on only 11-24% of B frames, so a
  `--direct temporal` reading under it says nothing.
- `Y264_DIRECT_LRVOTE=1|2`: which direct derivation the lookahead would pick
  per B frame (`=1` by lowres MV distance, `=2` by lowres bi-pred SATD).
  Prints `lrvote poc= col= past= T= S= n=`. **Dedupe by POC**, since overlapping
  mb-tree windows print a frame several times. **Closed negative**: mode 2 doesn't predict new
  clips. Keep it as the baseline.
- `Y264_DIRECT_SCORE=1`: per B frame, the direct prediction's SSD and
  macroblock count. Run once per `--direct` mode and diff. **The signal is
  weak** (better prediction can code worse), so treat it as a bound, never a
  selector. t1, md5-identical.
- `Y264_BSKIP_SCREEN=2`: checks the exact B skip screen against the full
  strict probe and aborts on any disagreement, printing probes, accept rate,
  4x4 blocks and chroma planes proven, and 4x4 transforms before and after the screen. Any thread count,
  output identical to `=0`/`=1`.

### The RD-trial rate estimator

- `Y264_EST_TWICE=1`: runs every trial estimate twice and keeps the first, so
  the output is byte-identical and the instruction delta is exactly one set of
  trials. Prices arms such as `Y264_EST_FROZEN`. Read instructions from
  `/usr/bin/time -l`, not wall.
- `Y264_EST_SCRTRACE=1`: a `(site, frame, mb, dist, j, lb_j)` trace per trial,
  to price admissible screens offline by replaying each tournament. A bound
  built on stored candidate nnz overcounts what the estimator codes. t1 only.

## 2. What would a perfect version of X buy? (bounds and oracles)

These replay a recorded ideal so an arm gets a ceiling **before** a threshold.
Each must pass its own byte-identity check.

### Rate-control replay

- `scripts/rc_invariants.py`: the rate controller's invariants in seconds. 36
  one-thread cases over 6 s: the A6 set (`--bitrate`, 1.0x CBR and 1.5x cap, 6
  clips) and the cvbr_compliance set (CRF with 0.8/0.6/0.4x caps).
  - `REF` records once (cached under `$Y264_CACHE/rcreplay`, keyed on binary
    md5 and command), and `CAND`/`CAND_ENV` replay. Recording takes minutes,
    replays seconds.
  - A restructuring step must read EXACT. The invariants are 0 leaky-bucket
    underflows and A6 rate error inside [-5%, +3.4%]. `INFORM=1` reports
    without failing.
  - The VBV column is a proxy. `vbv_check.py` and `hrd_check.py` stay the
    stream gates.
- `Y264_RC_REC=<f>` / `Y264_RC_PLAY=<f>` with `scripts/rc_replay.py`: **the
  rate controller alone.** A `--threads 1` REC encode writes every
  pixel-derived controller input in consumption order, plus each slice's QP
  and RBSP size. PLAY reruns with lookahead, mb-tree, analysis, entropy coding
  and reference planes off.
  - **The recording binary must replay every slice QP EXACT.**
    `rc_replay.py` checks 18 configurations.
  - `PLAY_BIN`/`PLAY_ENV` replay another controller. Moved slices are scaled
    by the 1/qscale law (`Y264_RC_PLAY_K` scales the exponent), and a leaky-bucket VBV estimate
    prints for both sides. The Annex C
    gate stays `hrd_check.py`.
  - A new pixel-derived input must go through `RCR_VAL`, or the replay reads
    `order differs`.
- `tests/rc_selftest.py` (meson test `rc_replay`): runs the 18-mode self-test
  on a 48-frame synthetic QCIF clip with a cut, inside `make test`. Every mode
  must replay EXACT. About 3 s.
- `Y264_VBV_ROWBRAKE_TRACE=1`: one `RBT` line per picture with pass 1's CABAC
  estimate and the real bits of every macroblock row. The frame median is
  about 2% off and p90 about 6%. CAVLC P and B rows print 0.

### Mb-tree, skip, type and search oracles

- `Y264_MBT_REC=<f>` / `Y264_MBT_PLAY=<f>`: records and replays the mb-tree
  offsets of every anchor **and the reference B**, keyed on (POC, FNV-64 of
  the anchor's lowres) and portable across CRF. **Replay your own recording
  first, every time; it must be md5-identical.** A ship that adds a walk
  output makes the probe stale. The format magic flags it.
- `Y264_SKIP_ORACLE`, `..._AT`, `..._SIDE`: what a perfect early-skip
  predictor buys. Key on the absolute display index, since POC restarts per
  IDR.
- `Y264_TYPE_ORACLE`: replays x264's frame-type placement, read from its
  bitstream.
- `Y264_HEX_ORACLE`: the ME search-shape bound.
- `Y264_MEREUSE_TRACE`: tests ref (r-1)'s winning 16x16 list-0 MV, scaled by
  temporal distance, as a seed for ref r. It beats the median predictor on
  57-61% of ref>=1 searches and ties or beats the full search on 11-15%.
  Shipping it is open. t1 only, output-neutral.
- `Y264_UNSAFE_NO_EMIT` / `_NO_NAL` / `_NO_MBT` / `_NO_REFBWAIT` /
  `_NO_PREVPWAIT`: delete probes. **Gate them on output identity, never on an
  argument about consumers.** If the output moves, you're measuring a
  different encoder.

## 3. Is the arm any good? (quality gates)

### The self-A/B bands

- `ARM='<env>' python3 scripts/run_band.py`: **the standard self-A/B gate.**
  Both sides run the same ABR and CRF ladders, reported per clip on
  `VMAF_GATE` (VMAF v1 by default).
  - `BANDS=crf` runs only the gated band, for half the wall.
    `CLIPS=akiyo_cif,park_joy_720p` runs a subset.
  - **The CRF band stops early** once one clip reads worse than +1.0% or three
    worse than +0.5%. `EARLY_STOP=0` reads all. The ABR band and runs against
    the reference never stop early.
- `ARM='<env>' python3 scripts/band_at_rate.py`: **the CRF band at matched
  achieved bitrate**, the gate for any arm that moves the operating point (`run_band.py BANDS=crf`
  would measure ladder placement). It
  drives `bd_at_rate.py` per clip with `run_band.py`'s ladders, `ARM` /
  `ARM_ARGS` and `EARLY_STOP`, `JOBS` clips at a time, ending in a median,
  mean and worst.
- `BANDS=deep` on the same script: **the deep band (VMAF v1 37-66)** at
  matched rate. Its ladders (`ladders_deep.json`) come from
  `scripts/calibrate_deep.py --write`. foreman is dropped. `BANDS=all` runs
  both. Rungs print a model-derived `qp~`. **Its noise floor isn't measured on
  v1 yet.** On the earlier metric a near-inert perturbation read about +/-1.2
  per clip, so re-measure before refusing on a deep read.

### The HD band

`ARMS='a=<env>;b=<env>' python3 scripts/hd_band.py` is the bar for HD
candidates: BD on the gate score over twelve 720p and 1080p clips, several
arms per run. It costs 45 encodes and 45 VMAF scores a clip at eight arms and
is load-immune.

- Anchor (the default) and arms run the same five rungs, CRF 23 to 35, and BD
  is integrated over the range the curves share. `SOLVE=1` (with `WINDOW`,
  `ITERS`) restores the matched-byte ladder, which below `medium` misses and
  reads the miss as quality.
- Each cell prints rate overlap, quality overlap and surviving rungs. A clip
  under `MIN_RUNGS` (4 of 5) is NOT SCORED. A refused clip is re-read on
  mid-step rungs (fix A, rows print `[A]`).
- The anchor ladder is cached in `$Y264_CACHE/hdband/ladder.json` by binary
  sha, and every cell in `$Y264_CACHE/hdband/points.json`
  (`NO_POINT_CACHE=1` bypasses).
- **The null is stored, not run.** Readings are compared with
  `scripts/hd_null.py`'s fit and marked `inside null` within `NULL_SD` (2)
  robust sd. The refusal subtracts center plus half-width.
- Clips with no fit, changed anchor bytes or fix A run one live control
  (`LIVE_CONTROL`, default `Y264_MBT_DEPFLOOR=16`).
  `CONTROLS='ctl=Y264_MBT_DEPFLOOR=16;ctl2=Y264_MBT_DEPFLOOR=8'` runs two on
  every clip, and `CONTROLS=none` drops them. If the null's spread exceeds the
  bar, it prints `UNRESOLVED`, not `PASS`.
- **Early stop**: an arm is refused once one clip reads worse than
  `EARLY_ONE` (+1.0%) or `EARLY_N` (3) worse than `EARLY_SOME` (+0.5%).
  `EARLY_STOP=0` reads every clip for a report.
- `ARMS='int=@/path/to/yah264'` reads a whole build against `YAH264`.
- `CLIPS`, `RUNGS`, `FRAMES`, `JOBS`, `MIN_RUNGS`, `FIXA`, `PROBE_D`, `BAR`
  and `MEDIAN_BAR` override.

`python3 scripts/hd_null.py` (`--show` prints the fit) fits each clip's null
from the point cache, using the BD of `Y264_MBT_DEPFLOOR=16`/`=8` against the
default.

- Reads from output-identical binaries count once. Expect 5-6 per clip.
- It stores the median and robust sd (1.4826 x MAD), the plain mean and sd,
  and a reference binary (`YAH264` or `--ref`) for detecting changed defaults.
- Output: `$Y264_CACHE/hdband/null-VMAF-v1-disp.json` (`null.json` under NEG),
  per preset, threads, frames and ladder.
- A missing reference curve is encoded (five encodes a clip) unless
  `--no-encode`. Refit with `YAH264=<new build>` after changing defaults.

### The BD tools and the score

- `scripts/bdcompare.py --vmaf --no-cache`: the underlying BD tool, scoring
  `VMAF_GATE`.
  - **`--no-cache` is mandatory**: encodes are cached by command string, so a
    rebuild returns stale numbers.
  - `--models` applies only under `VMAF_GATE=VMAF-NEG`.
  - **`--subsample` refuses a stride sharing a factor with the mini-GOP
    period** (bframes + 1, default 3). Stride 4 at bframes 3 scores only
    anchors and can flip a sign. Coprime strides add +/-0.15-0.2% per clip.
    Self-A/B bands stay at 1.
- `VMAF_GATE` (read in `bdcompare.vmaf_of`, so by every band and
  `calibrate_band.py`): VMAF v1 with CAMBI ([vmaf.md](vmaf.md)).
  `VMAF_V1_BIN` and `VMAF_V1_MODELS` point at libvmaf and the v1.0.16 models.
  `VMAF_GATE=VMAF-NEG` reproduces an older NEG number.
- **The VMAF score cache** (`$Y264_CACHE/vmafscore`): an output-identical
  encode never pays libvmaf twice. The key is the bitstream md5, the source's
  path, size and mtime, the subsample and the pixel format. A v1 key adds the
  model, bit depth, libvmaf build and display scaling. A frame-cut copy uses
  its parent plus the frame count. `VMAF_SCORE_CACHE=0` bypasses it.
- `scripts/bd_at_rate.py`: BD at matched achieved bitrate. Required for
  anything that moves the CRF axis, as mb-tree on/off does.
- `scripts/bdcompare.py` with the cache on, read for its SIZE lines: **does
  `--crf N` mean the same in both encoders?** Divide the bytes at equal CRF and
  log-interpolate x264's rungs to get CRF points
  ([rate-control.md](rate-control.md)). Read bytes from the encode cache: the
  printed `NNNk` rounds 3-5% on small cells, half a point. The offset depends
  on content, not scale (two 48-frame segments of one 4K source differ by
  0.6-0.7). A black leader prints a flat ladder.

### Ladders and calibration

- `scripts/calibrate_band.py --write` (`ladders.json`, `curves.json`): the
  band ladders and the x264 curve they were solved onto. **Without `--write`
  it writes to a scratch dir**, and every run prints both destinations
  (`OVERWRITE` or `create`) first. `--dry-run` stops there. `--clips`,
  `--crfs`, `--frames`, `--jobs` and `--out-dir` narrow it.
- `scripts/calibrate_deep.py --write` (`ladders_deep.json`,
  `curves_deep.json`): the same for the deep band over CRF 30-50.
- `scripts/ladder.py`, `scripts/crf-solve.py`: ladder construction and the
  CRF that hits a target rate. `crf-solve.py` pins its point per (clip, args,
  target, tier) in `tests/.crfpin` (section 7).
- **The ladder shift**: rerun an ABR arm at ladders +/-13% in rate, **the
  cheapest disproof of an ABR refusal**. A real effect keeps its sign. One arm
  read +0.96%, +0.25% and -5.75% across three ladders.
  - The ABR per-clip noise floor runs 0.2 to 11 points. Measure it before any
    ABR refusal by nudging the current default (AC gain 1.734 or 1.785 against
    1.7), not by an arbitrary 1.02.
  - Corroborate with the **CRF low band**, points 32-41, which has no
    rate-controller feedback.

### Where the quality goes

- `python3 scripts/band_check.py`: **does it band in the dark?** v1's CAMBI
  ignores the darkest areas. On five 96-frame dark windows, with yah264 solved
  to x264's CRF 23 size, it reads flat share, texture kept, dark PSNR and
  CAMBI over dark smooth 8x8 blocks. A window fails at more than 0.05 flatter
  or 1.0 CAMBI worse. Required for any AQ, psy or RD-decision change. `WINDOWS`,
  `ARGS` and `OUT` override, and `Y264_*` knobs pass through. About 10 minutes
  at two threads.
- `scripts/hf_probe.py`: **where the texture goes**. Per-band DCT energy
  retention (LF, MF, HF zigzag rings) beside bits, PSNR and VMAF v1 on the
  minimal CQP reproducer, split by macroblock class and motion class:
  `HF_VMAF=1 ARMS="next-vs|x264-med" python3 scripts/hf_probe.py bus_cif 100 30,36,42,48`
- `scripts/hf_join.py`: the 3x3 skip/inter/intra contingency of our class
  against x264's, with MSE and AC retention per cell. A gap in the skip/skip
  cell is inherited reference quality.
- `Y264_QP_TRACE=1` with `scripts/multishot_bd.py`: **where the bits go
  across shots**, a gap the single-shot board can't see. The trace prints each
  frame's slice, coded and intended QP with the mb-tree offset and CRF DC
  term. `multishot_bd.py` encodes `local/corpus/ms_*.y4m` (plus `.cuts`) at CRF
  22-34 for any arms and prints BD on VMAF v1 plus per-shot VMAF and bit
  shares.
- `python3 scripts/m4_label.py --src-dir <dir> --out <jsonl>`: **per-sequence
  arm labels on an external corpus.** Each row has every M4 arm's BD against
  the default at matched rate and the features `tdiff`, `flat`, `tex` and
  `motion` from `Y264_PSY_FLAT_LOG` / `Y264_ADME_LOG`. Class is `default`
  unless an arm wins by 0.30%. It can't reach `tests/corpus`: **the gate
  corpus is test-only, permanently.** Resumable, about 7 s per 64-frame
  480x272 sequence. See [content-selection.md](content-selection.md).
- `scripts/shot_determinism.sh [input.y4m]`: **the per-shot re-encode
  contract** ([engine-interface.md](engine-interface.md)). Re-encodes each GOP
  alone and compares bytes. Gated at `T=1`. Threaded mismatches WARN
  (`STRICT=1` fails). K and CRF come from the environment.
- `scripts/cvbr_compliance.sh`, `scripts/vbv_check.py`: VBV and capped-VBR
  violation counts and buffer traces.

## 4. Is it fast? (wall harnesses)

**Never price an arm with a table delta.** Our CRF is a staircase, so two
table runs aren't at the same operating point.

Every wall reading here interleaves the arms round-robin with **a
duplicate-base control arm**, takes the median of 5 or more runs, and times
to `/dev/null`. Measure arms in ONE batch. Price threaded arms at `THREADS=12`
and state the tier, pure-C or as-shipped.

### Boards

- `scripts/perf-comp.sh`, `-set`, `-crf-set`, `-purec`, `-modes`: the sets
  against x264. `-purec` uses the fair pure-C reference (section 6).
  `PERF_REPEAT` knobs drive the repeated-sample timer for short CIF cells.
- `Y264_REFENC_CACHE=0 make parity-status` / `parity-status-crf`: the
  ten-clip board. Quote CRF for speed. Repeat runs agree within 0.01. Published
  parity tables come from `scripts/longform_throughput.py`.
- `scripts/parity-clips.sh`, `scripts/parity-clip-calib.sh`: the clip set and
  its calibration.
- `scripts/psnr_leg.py`: **the PSNR-Y floor** behind every `dPSNR-Y` column.
  From `clip=dB` pairs it prints the median, worst clip,
  `PSNR-FLOOR PASS/FAIL` (fails at more than 1.0 dB below x264 on any clip)
  and the `PSNR-DEBT` list at 0.5 dB. `--feature psnr` rides on the VMAF pass
  for free. **Read it on the CRF board.** Only the matched-point solve makes
  "at equal bytes" true, and ABR can deliver +10%.

### The long-form board

`scripts/longform_throughput.py` with `scripts/lf_feed.py` times **whole
titles (10+ min) and staged segments through both shipped CLIs, on stdin from
one feeder.**

- **Never time a file-path input.** The reference CLI is input-capped by path
  (600 frames of meridian 1080p: 3.9 s by file, 1.56 s piped).
- VideoToolbox decodes on the fly (scale_vt below native) with three decoders
  on chunks, since one gives about 230 fps of 4K. Chunks decode 5 s early and
  check their first and last frame by timestamp, because a keyframe seek drops
  frames. `verify-feed` checks chunked against sequential.
- `plan` dry-runs the legs with estimated walls: `timed` (G3 plus SIMD t1),
  `purec` (G1/G2, one 60 s window per 3 min) and `validate`.
- **`point` reuses solves across builds.** The reference point carries over
  given the same reference binary and subsample. Ours encodes once at the last
  rate factor: a matching md5 is `reused`, otherwise the secant starts `warm`.
  Full reuse needs the same `SOLVE_THREADS`. `LF_NO_REUSE=1` solves `cold`.

### The ffmpeg board

`scripts/ffboard.py` checks **whether the table is scoring our Y4M reader**:
both encoders run as libraries in ONE ffmpeg process. It agrees with
`parity-status-crf` within noise.

- It needs an ffmpeg with libyah264 at `../build/ffboard/ffmpeg-yah264/ffmpeg`
  ([ffmpeg.md](ffmpeg.md)), out of `/tmp`, which macOS sweeps.
- **Point `X264LIB` at the fair pure-C libx264 (section 6).**
- **Solve cache**: `$Y264_CACHE/ffboard/solve.json` (default
  `~/.cache/yah264/ffboard`), keyed by a sha of each dylib actually loaded
  (leaf from `otool -L`, symlinks resolved), each library's pkg-config
  `Version` and the ffmpeg binary's sha. Every solve prints
  `solve cache: hit`/`miss`. `--no-solve-cache` or `Y264_NO_SOLVE_CACHE=1`
  skips it.
- **Size-match guard**: `RC=crf` and `RC=abrm` rows are rate-matched, so a
  dsize median outside +/-2% or any clip outside +/-5% prints
  `INVALID: sizes unmatched` and exits 2. `RC=abr` is exempt (see its
  `n rate` / `x rate`). `Y264_DSIZE_TOL` and `Y264_DSIZE_TOL_MAX` move the
  bars. Setting the first also arms the guard at `RC=abr`.
- **Header check**: the build writes `git hash-object include/yah264.h` to
  `yah264-header.sha`, and the board refuses a `$Y264LIB/include/yah264.h`
  that differs, because a stale wrapper reads `yah264_param_t` wrongly and
  still encodes. `yah264_encoder_open` also refuses a foreign struct `size`.
  `Y264_HEADER_CHECK=0` bypasses it.
- **`Y264LIB` is weaker than it looks.** `DYLD_LIBRARY_PATH` supplies only the
  leaf name, so an install with another soname isn't loaded and the
  build-time path wins.

### Kernels and the GPU

- `./build/tools/checkasm/checkasm --bench`, rows `sad_quad` / `satd_quad`:
  **does batching a kernel pay**, against four dispatched singles. SAD reads
  1.60-1.80x and SATD about 1.01x. Read only sizes where both forms are NEON
  (not 8x4, 4x4 or 4x8).
- `sample` profiles of both shipped binaries: **each encoder's SIMD share**.
  Multiply each side's percentage by its own wall before ranking kernels in ms
  per job.
- `Y264_GPU_PHASEA=1` with `Y264_GPQ_WARMUP=10000` / `Y264_GPQ_CONSUME=0`:
  **the Metal floor and the gpq tax split.** `WARMUP=10000` arms the offload
  but never submits, so only `ngc_open` runs: the 12-17 ms device-init floor
  every Metal-armed arm pays PER TABLE CELL, which a precompiled metallib
  doesn't move. `CONSUME=0` submits but never reads, splitting chain-side from
  walk-side cost.

### Knobs and scratch

- `python3 scripts/knob_census.py` / `--check`: **the `Y264_*` knob census**
  ([knobs.md](knobs.md)). `--check` runs in `make test` and fails on an
  uncatalogued knob, a removed reader, or a "default OFF/ON" comment that
  contradicts the code. It reads getenv, the `*_tunable` helpers and the pool's
  spin-site macro.
- `KEEP_SCRATCH=1`: keeps and prints a harness's working dir. Scratch from
  `scripts/scratch.sh` or `scripts/scratch.py` is removed on exit, Ctrl-C and
  TERM. This is the ONLY way to keep the evidence of a failed cell.
- `make clean-scratch`: removes leftover scratch after a SIGKILL, panic or
  power cut: directories under `$TMPDIR` matching
  `scripts/scratch-prefixes.txt` and older than a day. `DRY=1` lists only,
  `DAYS=7` widens. Caches in `$Y264_CACHE` (default `~/.cache/yah264`) are
  never swept: `ffboard.py`'s `solve.json`, `bdcompare.py --cache`, the
  openh264 raw planes.

## 5. Is it correct? (the gates every ship passes)

```
make test                                   # unit tests (checkasm is one of them)
./build/tools/checkasm/checkasm             # every kernel against its C reference, guards on
./build/tools/checkasm/checkasm --list      # the table: group, family, the ISA each row needs
./build/tools/checkasm/checkasm --isa neon pixel    # one tier, one family
make conformance                            # recon-match (YAH264_CONF_FAST)
python3 scripts/stress_threads.py           # hang/determinism under load
scripts/tsan_catch.sh                       # TSan; the floor is 0 reports
python3 scripts/env_gate_audit.py           # every lazy env static in src/ and cli/: exits 1 on TRAP, COLD, undetermined or a waiver error
ARM='<env>' scripts/recon_sweep.sh          # recon-match ONE ARM across a config matrix
scripts/determ_repeat.sh                    # SAME config, N times -> how many bitstreams (STRICT=1 fails)
scripts/identity_ab.sh OLD NEW              # two binaries, ~100 configs, md5 per cell -> PASS/DIFF table
ARGS='--direct temporal' scripts/determ_repeat.sh   # ...and it takes MODE flags too
scripts/hygiene_check.sh                    # licences, stray patches, home paths, stray asm
scripts/abr_decode_gate.sh                  # decoder-side gate for the THREADED ABR path
scripts/x86-docker.sh                       # the whole battery as linux/amd64, under Rosetta
scripts/x86-docker.sh --backend qemu        # ...and again under QEMU, an item's final gate
YAH264_CONF_DECODERS="ffmpeg openh264 jm" make conformance   # three oracles, not one
python3 scripts/hrd_check.py STREAM.264     # Annex C CPB, read out of the stream itself
python3 scripts/level_check.py STREAM.264   # Annex A Table A-1, against the declared level
python3 scripts/hrd_check.py --self-test    # self-test cases, including negative controls
python3 scripts/level_check.py --self-test  # self-test cases, including a negative control
FF=path/to/ffmpeg WORK=dir scripts/hdr_check.sh meta  # HDR signaling, field by field against x264
```

### Kernels: checkasm

`tools/checkasm` checks **that every optimized kernel matches its C reference
exactly and stays inside the memory it declares.**

- It calls each kernel SYMBOL directly, so kernels the dispatcher skips on this
  box (such as plain-NEON twins of dotprod kernels) are tested too.
- Beyond the value check: a poisoned border on every destination, a
  row-padding check for wide stores into narrow blocks, and an
  **mmap/mprotect page guard** at BOTH tails, sized to the declared window.
  Only the page guard catches a discarded over-read ("load 8 lanes, use 4"),
  and it stops hard naming the shape. Prediction destinations are packed so
  oversized stores fault.
- `--list` shows each row's ISA. `--isa neon|sse4|avx2|avx512|all` runs one
  tier. An x86-64 build runs the portable plus x86 rows.
- Each group's BODY takes its kernel as an argument, so a new tier's row
  inherits the fills and guards.
- `--bench` is informational and single-threaded. A checkasm multiple is
  necessary, never sufficient.

### x86 in Docker

`scripts/x86-docker.sh` checks **that the encoder builds and passes its
battery as linux/amd64 on this Mac, and that the SIMD tier doesn't move a
byte.**

- **Matrix: `{rosetta, qemu}` x `{Y264_SIMD_FORCE=none, sse4, avx2}` x
  `{make test incl. checkasm, conformance --fast, identity cmp against none}`,
  plus a 10-bit build**, from `docker/x86/Dockerfile` (ubuntu 24.04, no
  libvmaf-dev). One build serves all rows because the tier is a runtime cap.
  `-Dsimd=` is CI's job.
- **Neither emulator has AVX-512.** The script prints that, so an emulated
  green never reads as "AVX-512 works".
- Rosetta runs near native but has illegal-instruction bugs, so SIGILL cases
  rerun under QEMU and are marked "emulator". QEMU's TCG is several times
  slower and is the final-gate second opinion.
- It never touches the VM's shared binfmt entries. On QEMU each binary runs
  under the image's own `qemu-x86_64`. A `-cpu Nehalem` run that must hide
  AVX2 proves it. Each capped run gets a CPU model without the next tier
  (qemu64, Nehalem, Haswell-noTSX).
- The CPU-feature banner from `checkasm --list` is the CPU's, not the
  build's.
- Before the tier legs it sets mismatched build dirs to
  `-Ddev_knobs=true -Dsimd=auto` once. `scripts/dispatch_probe.py` then wraps
  every kernel and counts calls in short 8- and 10-bit encodes (4:2:0 at odd
  width, 4:2:2, 4:4:4) per cap. `none` calls no kernel, every other cap calls
  each kernel only at its highest tier at or under the cap, and every kernel
  outside a short allow-list is reached. If not, the tier legs skip.
- **Identity**: at threads 1, `sse4` and `avx2` must match `none` byte for
  byte, at 8 bits on the board clips and at 10 bits on 4:2:0, 4:2:2 and 4:4:4.
  A difference is a kernel defect.
- Corpus clips are bind-mounted READ-ONLY. The fixture cache is keyed by the
  **ffmpeg identity** and `FIXVER`, in the container's own cache dir.
- It reads NO clocks. It's still CPU load, so coordinate it like a timed leg
  ([simd.md](simd.md)).
- The ubuntu CI job (`.github/workflows/ci.yml`) runs on every code push.

### Sanitizers and identity

- `scripts/san_matrix.sh`: **ASan/UBSan over 21 edge inputs** (odd sizes, 1-2
  frame clips with B, keyint 1, qp 0, extreme bitrates, 4:2:2/4:4:4, CBR,
  2-pass, direct temporal, the hardware backend, 1-12 threads). Builds
  `build-san/` once, about 3 min. Exit status is the number of cases with
  reports.
- `scripts/identity_ab.sh`: **do two binaries, or two env arms, produce the
  same bitstreams?** The gate for refactors, layout changes and knob removals.
  - About 100 cells (`MATRIX=quick`: 15) across every rate mode (CQP, CRF,
    ABR, capped ABR, CBR, CRF+VBV, 2-pass), presets, tunes, tools, CAVLC,
    PAFF, slices, 4:2:2, 4:4:4, 10-bit (from foreman_cif), 720p, 1080p and
    threads. About 45 s at `BUDGET=4`.
  - t1 cells are final. Threaded cells rerun `REPEAT` times and read VARIES
    when a side doesn't repeat.
  - A control cell must read DIFF. An encode that never ran can't pass.
  - It clears every `Y264_*`. `ARM_A`/`ARM_B` set env per side.

### Stream checkers

- `scripts/hrd_check.py`: **would a conforming decoder's buffer survive this
  stream?** It reads the bucket from the stream (SPS VUI hrd_parameters,
  buffering_period / pic_timing SEI) and checks underflow, overflow, peak
  fullness, arrival margin, a buffering period at every IDR, and C.3 initial
  delays.
  - Under `fixed_frame_rate_flag`, output must be one DeltaTfiDivisor apart
    (E.2.1) and removal one picture period apart. That catches a GOP join
    hiding a restarted removal count. The one-period rule is this checker's
    own, not the standard's. Under pulldown (pic_struct 5..8) a long gap only
    warns. A non-increasing removal time always fails (Annex A).
  - Exit 0 clean, 1 violation, **3 no HRD to check** (any stream without
    `--nal-hrd vbr|cbr`). `--allow-no-hrd` softens that.
  - `HRD=1 scripts/cvbr_compliance.sh` is the 36-cell leg, printing the VBV
    verdict beside both HRD verdicts.
  - **It doesn't replace `vbv_check.py`**, which is told the bucket and asks
    "did our rate control respect the cap". This one asks "would a
    receiver's buffer survive what we emitted".
- `scripts/level_check.py`: **is the declared `level_idc` the one Table A-1
  requires?** It checks frame size, MB/s, DPB, bitrate/CPB (from the HRD or
  `--bitrate`) and the vertical MV bound (a failure only under
  `--strict-mv`). `--exact` demands the declared level EQUAL the pick.
  `--y4m SRC` supplies a frame rate. There's no default one. Table A-1 is
  transcribed independently, on purpose.
- `scripts/hdr_check.sh meta`: **do players see the same HDR signaling from
  us as from the reference?** HDR10 on two clips, HLG and SDR-compatible HLG,
  from both CLIs, and with `FF` through `-c:v libyah264` against
  `-c:v libx264` into MP4 and MPEG-TS, plus a side-data input leg. It compares
  ffprobe color fields and keyframe side data, MediaInfo's HDR fields, and the
  SEI ahead of every IDR in the elementary stream (container boxes can mask
  it). Exits 1 on any difference. Level and reference count don't count.
  About a minute at 4 threads.
- `scripts/recovery_check.py`: **does an `--open-gop` stream recover where its
  recovery point SEI says?** It decodes from the cut with the parameter sets
  pasted in and compares the LAST `n - k` frames with `--dump-recon`.
  **The cut decode passes `-flags2 +showall`, and the floor is ffmpeg 6.1**,
  since display policy differs by version (6.1.1 stops two short at end of
  stream, and asked for all matches 9.0.1). Exit 0 clean, 1 mismatch,
  **2 no recovery point**, which fails an `--open-gop` encode.
- `scripts/h264_syntax.py`: not a gate. The Annex-B / SPS / SEI and slice
  HEADER reader the checkers share, with no entropy decode. `--probe STREAM`
  reports what a harness needs to choose a decoder.
- `scripts/fetch_openh264.sh`, `scripts/fetch_jm.sh`: build
  `tools/openh264/h264dec` and `tools/jm/ldecod` from source pinned by tag AND
  sha256, under `scratch/`. Only the binary is installed, nothing vendored,
  both dirs gitignored. Packaged openh264 has no CLI. CONTRIBUTING rule 4
  allows the JM as an oracle. Only `ldecod` is built.

### The env-gate audit

Knobs resolve through lazy function-local statics. The CLI opens one
encoder per GOP from concurrent workers. A gate not resolved on the main thread
gets a benign same-value race that still fogs TSan. `env_gate_audit.py` walks
the call graph from `warm_lr_statics` / `ntp_pool_create` / `ntp_bg_create`
and from `main` in `cli/` and flags:

- **TRAP**: the resolved value still satisfies its guard, so the static is
  rewritten every call (for example "unset" resolved to `-2` under `v < 0`).
- **COLD**: the gate latches and is never warmed.

No warm fixes a static resolved **inline in a hot function** (hoist it into an
accessor) or an **unconditional store** in a warm function
(`y264_tl_on = trprof_on();`, use read-compare-write).

Run it after adding a knob. `make test` and both CI jobs run
`env_gate_audit.py --self-test` and the audit. Lazy gates in headers are
refused. An `env-audit-ok: <reason>` marker waives a misjudged gate. A marker
needs a reason. It counts only in comment text:

- in a comment ending on the line above the definition, with no code on its
  lines (a block comment may open higher, and the reason may start on the next
  line)
- on the definition's own lines (a GNU-style `static int` line counts)
- anywhere in the body

Known limits, read wrongly or not at all:

- K&R definitions, macro-generated functions and signatures over 12 lines
- function-like macros that wrap the reader
- latches on a struct member, a non-static or extern variable, or filled
  through a helper that reads a literal knob
- Yoda guards, `~0u` sentinels, while-guards, reads in an else branch, and a
  guard on a local copied from a plain static
- latch flags set by `atomic_exchange` or `++` (read as never assigned)
- whether a warm runs before the workers start, for the library's warms and the
  CLI's alike
- a thread entry only ever passed through a variable or field (never named as
  a `pthread_create` or `ntp_*` argument) reads as warm when a warm function
  names it.

### Determinism

**With threads, determinism is a diagnostic. At one thread it's a gate.**
`stair_determ.sh`, `shot_determinism.sh` and the threaded identity cells in
`conformance.sh` WARN and exit 0. At `--threads 1`, `check_determinism` in
`conformance.sh`, the regress rerun, `determ_repeat.sh`, the pipe check and
`make repro` all FAIL on a difference. The threaded checks still hunt
uninitialized reads and races, and tell whether an identity-gated probe can be
read from one run.

**Run `determ_repeat.sh` on a LOADED box.** An idle 18-core box can pass a
default that emits several distinct bitstreams per eighteen runs. Interleave
both arms. The reading is the count of distinct md5s:

```sh
pids=; for i in 1 2 3 4 5 6; do (while :; do :; done) & pids+=($!); done
scripts/determ_repeat.sh; kill $pids; pgrep -f "while :" && echo "LEAKED SPINNERS"
```

**Gate the MODE, not just the default.** Use `ARGS=` (determ_repeat) and
`STAIR_DETERM_ARGS=` (stair_determ) for non-default modes. Env goes in a
SEPARATE slot: a flag passed to `env(1)` fails every run, and the empty
outputs have matching md5s.

**Kill by PID and VERIFY, never with `kill %N`.** Job-spec kills can fail
silently in non-interactive shells and leave spinners. **Check `uptime` before timing.**
Matched-rate band and BD reads are load-immune. Walls and tables aren't.

### Conformance beyond the default path

`make conformance` gates only the DEFAULT path. `recon_sweep.sh` covers an
env-gated tool across ref count, B-frames, entropy coder, direct mode and
transform (6 clips x 5 QPs x 10 configs). It clears outputs each iteration and
checks exit codes.

`scripts/abr_decode_gate.sh` is the only gate on the threaded ABR path. It
encodes the table ABR shapes at t12 and asserts every frame decodes with mean
PSNR above 25 dB (broken reads about 15, working 30-42). `ARM='<env>'` gates
an arm. Conformance can't reach that path (`--dump-recon` forces serial), the
CRF band skips ABR, and identity gates compare an encoder with itself.

For a default flip, check by hand that the escape env reproduces the OLD md5
and the new default equals the armed md5.

### What each of the three decoders can actually read

`YAH264_CONF_DECODERS="ffmpeg openh264 jm"` makes recon-match a three-oracle
gate (default: ffmpeg). **openh264 reads almost none of our output**:

| stream shape | openh264 2.6.0 | JM 19.0 | ffmpeg |
|---|---|---|---|
| 4:2:0 progressive, no B (CAVLC, CABAC, 8x8, multi-ref, crop, all-intra) | exact | exact | exact |
| any B slice | **wrong pictures, exit 0** | exact | exact |
| 4:2:2, 4:4:4 | writes nothing | exact | exact |
| `frame_mbs_only_flag == 0` (fields, either flavor) | refuses at the SPS | exact | exact |

So `conformance.sh` asks `h264_syntax.py --probe` what's in the stream and
SKIPS openh264 by stream property, printing per-decoder coverage. The
`baseline-shaped` cells exist to give openh264 something. **The JM is the
second oracle for PAFF**: an interlaced fixture (`ffmpeg tinterlace` plus an
MBAFF-coded x264 encode, 24 frames at 320x240) decodes byte-identical under the
JM and ffmpeg.

## 6. The reference side

The harnesses take external baselines from environment variables:

| variable | used by | what it must point at |
|---|---|---|
| `X264_ASM`, `X264_C` | `scripts/perf-comp.sh`, `scripts/instr-ratio.sh`, `scripts/crf-solve.py` | an x264 CLI with assembly on, and the fair pure-C build |
| `X264` | `scripts/cvbr_compliance.sh`, `scripts/ladder.py` | an x264 CLI |
| `X264LIB`, `Y264LIB` | `scripts/ffboard.py` | installed libx264 and libyah264 prefixes: `../build/ffboard/x264asm`, `../build/ffboard/x264noasm`, `../build/ffboard/y264inst`. No defaults on purpose: a stale install that loads would read as a result, and a missing one errors |
| `FF` | `scripts/ffboard.py` and the row scripts | that ffmpeg: `../build/ffboard/ffmpeg-yah264/ffmpeg`, then `/tmp/ffmpeg-yah264/ffmpeg`. `Y264_FFBOARD_ROOT` moves the set |
| `OPENH264` | `scripts/openh264-shim.sh` | an openh264 `h264enc` |
| `OPENH264_DEC` | `scripts/openh264-shim.sh --decode`, `scripts/conformance.sh` | an `h264dec`, default `tools/openh264/h264dec` from `scripts/fetch_openh264.sh` |
| `YAH264_CONF_DECODERS` | `scripts/conformance.sh` | any of `ffmpeg openh264 jm` (default `ffmpeg`). A missing binary is an ERROR, never a fall back |

**The fair pure-C build** is configured with `--disable-asm`, with
`-fno-tree-vectorize` removed from the generated config so the vectorizer stays
on (no source edits). A stock `--disable-asm` build is a scalar strawman. It
lives at `~/src/build/x264cli-purec-<rev>/x264`, from the same revision as the
SIMD CLI. A published pure-C row says so.

**Which reference binary**: say which build produced each number.

| harness | reference |
|---|---|
| `scripts/ffboard.py` | whatever `X264LIB` points at |
| `scripts/perf-comp.sh` | built from `../x264` with `--disable-lavf --disable-ffms --disable-avs --disable-swscale` |
| `scripts/multishot_bd.py` | that checkout's CLI |
| the dev box's installed x264 | 0.165.3222 (Homebrew) |

Clips come from `scripts/fetch_corpus.sh` ([corpus-sources.md](corpus-sources.md)).
Notes on instrumenting the reference encoder stay out of the public tree.

## 7. Traps that have each cost a round

### Builds and setup

- **Without `-Ddev_knobs=true` the CLI refuses any knob it doesn't carry**,
  exiting 2 with the knob's name. Treat a failed encode as a failure, never a
  result. `make` sets the option. Other build dirs need
  `meson configure <dir> -Ddev_knobs=true`.
- **Never compare builds whose `optimization` differs.** -O2 against -O3 reads
  as a 2-3% phantom win.
- **zsh doesn't word-split scalars.** Don't build CLI args in a string, and
  check that A/B arms give DIFFERENT md5s before trusting a null.
- **A stale output file scores as a pass.** If the encoder writes nothing (an
  unknown flag exits 2), a fixed filename re-scores the PREVIOUS iteration.
  Delete outputs each iteration and check the exit code.
- **A full temp volume looks like a content failure** ("no geometry in the
  recon y4m header" on the biggest clips). `conformance.sh` and `ffboard.py`
  refuse to start below 20 GB free. Check `df` first.

### Operating points and BD

- **dVMAF compares across builds only at a pinned operating point.** One CRF
  rung moves about 13% of rate, so a fresh solve can land elsewhere. The point
  is PINNED in `tests/.crfpin` per (clip, args, target, tier), resolved before
  the solve cache and part of its key. `Y264_CRF_PIN=0` bypasses it. Each solve
  prints `pin_state` and `pin_drift_pct`. **Read `pin_drift_pct` before dVMAF**: nonzero means the rate curve moved.
- **BD reads are sweep-dependent.** Check the saturation flag. A flat reference
  curve makes the number meaningless (touchdown's rows have read absurd
  values).
- **A BD-rate number isn't a bandwidth saving.** It weights every quality
  point equally and books the whole difference as rate, while a real ladder
  ships fixed rungs and gets a mix of rate and quality. An arm can read +3.9%
  BD while spending fewer bits and losing VMAF. **Quote both halves**
  (`scripts/run_band.py` prints `rungN: dRate / dVMAF`). Read the rung columns
  only in a self-A/B. Against x264 only the BD normalizes the scale mismatch.
- **The corpus median isn't the TABLE.** Clips the table doesn't score can
  move a median several points. Split by table membership.

### Timing

- **Timing the output write fakes a resolution-shaped effect.** It inflates
  the baseline most where bitstreams are biggest. One cell read 3.2x its true
  wall at a spread of 1.02. **Time to `/dev/null`, and give `--md5` its own
  untimed encode.**
- **A harness that defaults to the PURE-C tier prices the wrong tier for
  as-shipped questions.** The only tell is `tier pure` in a header line.
- **Run both throughput arms in ONE burst with mirrored launch order.** Load
  moves slower than a burst and faster than a round.
- **`/usr/bin/time -p` can't time a CIF cell.** On a 0.05-0.11 s cell its two
  decimals are a 9-14% quantum. Use `perf_counter` with repeated samples, and
  interleave the sides.
- **A desktop video process ACTIVELY DECODING** (`WallpaperAerialsExtension`,
  `VTDecoderXPCService`) spoils 720p timing while CIF stays steady. Residency
  alone is harmless. Gate on the SANITY CELLS, not `ps`.
- **`ps` `pcpu` is a decaying average and lies after a `SIGSTOP`** or
  relaunch. Use a CPU-time delta:
  ```sh
  a=$(ps -o time= -p $pid); sleep 10; b=$(ps -o time= -p $pid); echo "$a -> $b"
  ```
- **A speed number holds at ONE OPERATING POINT.** One clip ranged 1.00x to
  1.36x across four matched rates.
- **Price an arm on THEIR side before refusing it on wall.** x264 may pay a
  similar share for the same feature.
- **t1 speed doesn't predict t12, in either direction.** Deleting wavefront
  work pays more when throughput-bound. Work off the critical path pays only at
  t1. Relocated work pays nowhere.
- **Hoisting work can cost the SIMD tier what it buys the C tier.** Measure
  both tiers, plus an all-intra cell for intra changes.
- **This box is HETEROGENEOUS.** M5 Max: `hw.perflevel0.physicalcpu` = 6
  "Super" plus `hw.perflevel1` = 12 "Performance", so the ideal 12 -> 18
  speedup is about 1.43x. Check `sysctl hw.perflevel*` first.
- **Per-unit cost rising with threads isn't automatically your bug.** Measure
  each encoder's floor with N independent single-thread processes of the SAME
  binary and compare against that.
- **`sample` on x264 fed a large y4m reads about half `__mmap`/`__munmap`**,
  since x264 maps its input. Rescale before comparing shares, and loop a short
  clip so there's enough to sample.

### Probes and controls

- **`Y264_MBTREE_OFF` is a dirty control**: it also swaps the per-MB offset
  array. Use `Y264_MBTREE_STRENGTH=0`.
- **Verify the probe is in the binary you time.** A probe-less build gives a
  clean null.
- **A guard on an unpopulated signal disables what it guards** and prints
  a `+0.00%` meaning "never ran". The tell is output identical to baseline, not to the unguarded arm.
- **Check what a substituted field means.** `mb_qp_pre` is the COMBINED offset
  (AQ included). Substituting the mb-tree term alone inflated a result over 2x.
- **Keying on POC silently UNIONS GOPs**, because POC restarts at every IDR.
  **The tell is a false count that tracks the I-frame count.** Key on an
  absolute display index. When a new probe disagrees with an old one, suspect
  the new one.
- **A ship can break a probe silently.** Run its identity gate FIRST, and ask
  which recorder carries any new output.
- **A knob that overrides an AUTO default can push it DOWN.** `Y264_LA_BUF=1`
  cuts below the resolved `la_buf = bframes+1`. Print what the default
  resolved to first.
- **Read EVERY line of a stat dump.** The CLI's pool-less probe encoder prints
  first, so its `Y264_LA_STAT` line reads `pool=0 la_buf=0 la_th=0`.
- **Two instruments disagreeing by 15x is the finding.** Don't pick the bigger
  name.
- **Two paths reading one settled bound from different origins disagree.** The
  warm measures from the ring head, the walk from its anchor `d` entries in.
  The tell: turning off BOTH readers
  (`Y264_MBT_BLEG_REUSE=0 Y264_MBT_PAIR_SCALE=0`) makes them agree, and either
  alone doesn't.

### Gates and oracles

- **Only `determ_repeat.sh` re-runs one configuration against itself.**
  `stair_determ.sh` compares across thread counts and `w2_canary.sh` against a
  reference build. A nondeterministic default turns every md5 gate into a
  lottery.
- **A "byte-identical by construction" argument isn't a gate.** Run it, under load,
  against the escape.
- **TSan can miss a publish-before-write**, a ready flag set before the data
  is computed. The repeated-run md5 loop catches it.
- **TSan is silent on a stack overflow.** If variance survives every
  concurrency knob turned off, look for memory written past its end.
- **A second decoder that returns 0 isn't a second opinion.** Coverage depends
  on the stream, so a multi-oracle gate must print each oracle's coverage
  (section 5).

### Closed routes

- **SME/SME2 is closed. Don't reopen it.** It needs `-mcpu=apple-m4` or
  `native` (`-march=armv9-a+sme2` SIGILLs). Streaming 512-bit vectors buy 1.08x
  over 128-bit NEON. `smstart` plus `smstop` costs 9.6 ns against a 3.7 ns satd8x8,
  and a whole-frame ZA batched SATD reads 0.21x NEON.
- **Every Metal-armed arm pays the 12-17 ms bring-up floor per table cell.**
  Dispatch latency is fine (0.24-0.26 ms per frame of 720p lookahead work).
  The inter-round idle gap and the DVFS ramp close the route: hot-clock numbers
  misstate an anchor-cadence workload, and a sleeping warmer that reads flat
  once proves nothing.
