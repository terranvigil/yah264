# Instruments: what already exists, and what each one answers

This is the catalog of **measurement tools that have produced a result and are
reusable**. It doesn't list feature knobs. `docs/knobs.md` lists every
`Y264_*` env var with its reader, default and tier.
`scripts/knob_census.py` generates that file.

Read this before building a new probe. It exists to prevent a recurring
failure: a session spends its first hour rebuilding a profiler, an oracle or an
A/B harness that's already in the tree under a name nobody remembered.

## 1. Where does the time go?

| instrument | answers | notes |
|---|---|---|
| `Y264_BPROF=1` | per-stage wall inside the B **and P** macroblock tournaments, binned by final verdict, with MBs-touched counts | t1 only. Prints `BPROF` (B) and `PPROF` (P) tables at exit. Default inert, md5-identical |
| `Y264_BPROF2=1` | sub-decomposes ONE B RD trial into encode / dist / bits and reports the tr-pre share | sizes the RD-trial arm |
| `Y264_MBT_SPLIT=1` | mb-tree by stage: invq / bind / phaseA / phaseB / finish, plus source and memo-miss counts | ref-B's cost is ~95% phase A |
| `Y264_MBT_MISSWHY=1\|2` | **why a Phase A source had no pair legs to reuse**: splits MBT_SPLIT's `nobleg` bucket into anchor / untyped / null-leg / no-have. `=2` prints one line per miss with the source's POC, its ring offset, the settled bound, the STORED pair's anchor POCs and the pair the walk WANTED | Needs `Y264_MBT_SPLIT=1` beside it. `=2` is t1 only because the lines interleave on the pool. The misses it finds are window-frontier leaves with no future anchor bracket yet (`want <poc>/INT_MIN`), not a keying defect. Counting only, md5-identical on/off |
| `-Dc_args=-DY264_STAGE_PROF` + `Y264_STAGE_PROF=1` | per-stage ms inside the macroblock loop and deblock | **t1 ONLY: it fails SILENTLY at higher widths** (a zero-byte bitstream and exit 0). The fault is in the probe, not shipped code. Read a number off it only from a t1 run |
| `-Dc_args=-DY264_IPROF` (a separate release build dir, e.g. `meson setup build-ip --buildtype=release -Dc_args=-DY264_IPROF`) | **exact retired instructions by stage**: integer ME, sub-pel, RD trial, RD trial bits, commit re-encode, intra, entropy emit, deblock, lookahead, mb-tree, the rest of the macroblock decision, hpel + borders; printed as `IPROF` lines at exit | Reads the thread's own instruction counter (`thread_selfcounts`, no root needed) at scope entry and exit. The innermost open scope owns the instructions. One read costs ~1,690 instructions, and that cost is taken back off. Per thread, so it works at any `--threads`. The normal build compiles it out. Use it instead of a time profile whenever the question is "how many instructions": a time sample charges memory stalls to whatever was stalling (it inflated the per-trial psy term ~6x on sunflower). Stage totals reach 97% of an unperturbed run; frame-level bookkeeping is the rest |
| `Y264_THREAD_PROF=1` | per-stage buckets across the whole encode | **profiler buckets have lied before**; corroborate before acting |
| the same **plus `Y264_NTP_PROF=1`** | adds a **pool-idle** column per driver stage: how many of its milliseconds ran with no live pool job | A bucket's wall can't separate a stage that holds the machine idle from one that overlaps the wavefront. This column can |
| `Y264_NTP_PROF=1` idle split | busy / midrow / gate / ramp / tail / nojob per worker | the sleep is split against the pool's empty clock |
| `Y264_GOP_TRACE=1` (CLI) | **the GOP-parallel schedule over time**: one stderr line per GOP (worker, frame-thread share `k`, pulled / start / end on the encode's clock, `tcpu` the worker thread's CPU seconds, `pcpu` / `pinstr` the process's CPU seconds and retired instructions over the GOP), plus the reader's EOF, the dispatch time and plan, and the process totals at exit | The only instrument that sees the CLI's outer layer; the pool profilers above live inside one GOP's encoder. With `Y264_GOP_FORCE_G=1 Y264_GOP_FORCE_K=1` it prices every GOP at one frame thread, the input a schedule model needs. **Which cost column to read:** at one frame thread `tcpu` is the whole GOP. `pcpu` and `pinstr` are process-wide, so they belong to one GOP only with `Y264_GOP_FORCE_G=1`, which is also the only form that prices k > 1. Per-GOP CPU seconds move with the core type a thread ran on (the 12 slower cores run at ~0.75 of the 6 fast ones), so a per-GOP cost for a model is `pinstr` at `FORCE_G=1`, with CPU seconds as the cross-check. k=2 costs about +2.5% instructions over k=1 on 1080p GOPs, and k=3 the same as k=2: the step is the pool engaging. Scheduling-only, output unchanged |
| `-Dc_args=-DY264_OP_LEDGER` (a dedicated build dir) | op ledger: counts of real operations (SATD calls by site, early exits, skips, intra admits), not sampled time; `Y264LED` lines at exit | use when a sampled profile is suspect |
| `scripts/instr-ratio.sh N` | instruction-count ratio against x264 beside the wall ratio | Quotes the table's factor without timer noise. It reads two counters: `/usr/bin/time -l` on macOS, `perf stat -x, -e instructions,cycles,task-clock` on Linux. **The Linux arm needs perf to be readable.** With `kernel.perf_event_paranoid` at a stock 2 or 4 the row prints `NO-COUNTER`, never a plausible zero; `sysctl kernel.perf_event_paranoid=1` or a privileged container fixes it. Don't read it under an emulator: QEMU's and Rosetta's counts measure the emulator. The x86 column is a rented-metal reading |
| `scripts/alloc_census.sh OUT -- CMD` (`tools/alloc_census/`) | **which call sites hold the memory and which churn it**: every allocation of 256 KiB or more, by its two innermost frames and size class, with count, peak GiB live and GiB allocated over the run | dyld interposing, macOS, no rebuild. Peak "live" is virtual, since a worst-case slice buffer that's never written counts in full. Read it beside the peak footprint `/usr/bin/time -l` prints |
| `sample <pid> 15 1 -file F` + `scripts/sample_phases.py F` | **CPU by encoder stage at any thread count**, from a plain macOS `sample`. Blocked waits are dropped while a yield spin is kept. Every sample is walked from leaf to root onto stage rules | The threaded twin of BPROF. Kernel time is charged to the user frame that trapped, which is how the memory compressor's cost shows up by stage. `--rules` loads another binary's rules (kept in local notes, not the tree). `--bin NAME=PATH` gives an nm fallback for unsymbolized frames |
| `xctrace record --template 'CPU Counters' --attach <pid>` + `scripts/cc_split.py` | **useful / processing (memory+execution stall) / delivery / discarded issue-slot shares**, cycle-weighted | ATTACH, never `--launch`: launching hangs on this box. Threads created after the attach are missed, so attach in steady state |
| **the memory-pressure check** | whether a CPI or kernel-time difference is the box's compressor: `/usr/bin/time -l` peak footprint vs max RSS, page reclaims and sys time, and `vm_stat` Decompressions deltas during the run | On the shared box anything past ~20-30 GiB resident gets compressed. Check this before reading any 2160p CPI |
| `Y264_SIMD_FORCE=none\|sse4\|avx2\|avx512`, beside `Y264_ASM_OFF=<class>` | **the two SIMD ablation axes, and they answer different questions** | `Y264_ASM_OFF` takes one dispatch CLASS to its C fallback and leaves the rest as shipped; its wall delta is what that class's kernels buy. `Y264_SIMD_FORCE` takes the whole binary down to one ISA TIER by clearing the feature bits above it, so one binary can say what the SSE4.2 build would do on an AVX2 box, with `YAH264_NO_ASM=1` as the control column. Neither moves a byte of output, so both are identity-cmp axes too. `none` is the twin of `YAH264_NO_ASM`. The x86 tier names are inert on aarch64. The BUILD-time twin is meson's `-Dsimd=`, which leaves the kernels out of the binary |
| `Y264_ME_ETSTAT=1` | **what an integer-search early-out would delete and what it would cost**: post-seed integer probes bucketed by how good the best SEED already was (cost per pixel), with how often the hex/grid/diamond then moved the MV and by how far | Read the MV columns, not the gain: integer-SATD-gain volume doesn't predict BD. t1, default inert, md5-exact |
| `Y264_B8_STAT=1` | engagement counters for the B_8x8 / B_RECT arm: B macroblocks reaching the tournament, quadrant searches run, RD trials run, trials that beat the running best, searches thrown away by the mid-tournament skip exit, rectangular searches admitted | t1 (plain globals). One read names the arm's levers: search, RD, useful and wasted shares. docs/motion-and-modes.md |
| `Y264_LRSUB_PROBE=1` / `Y264_LRSUB_CENSUS=1` | direct wall of the lookahead's fifteen quarter-pel phase-plane builds, and per-phase read counts | About 3% of wall. All 15 phases are read and 99.8% of rows touched, so nothing in it can be deleted for free |
| `Y264_DIRECT_WHY=1` | **why a B slice fell back from temporal direct**: its list 0, the distinct co-located reference POCs (starred where they don't resolve), the block count behind each, and the per-macroblock direct-unavailable count | t1. docs/b-frames.md |
| `Y264_DIAG_DIRECTOK=n` | force B direct UNAVAILABLE on every nth macroblock, content-independently | Isolates the `direct_ok == 0` path from whatever made it common |
| `Y264_DIAG_COLWATCH=1` | hash the co-located field at the FIRST and LAST macroblock of a B frame instead of at slice prep | A prep-time hash proves nothing about a field read during the macroblock loop while the picture supplying it is still being encoded |
| `Y264_DIAG_TDIRLIM=n` | refuse temporal direct for a macroblock whose derived mvL0/mvL1 exceeds +/-n pel | Tests whether an out-of-range derived vector is behind a result |
| `Y264_DIAG_NOHPEL=1` | force `y264_me_mc_luma_s`'s non-hpel fallback everywhere | Tests whether a result depends on WHICH of the two motion-compensation paths a block took. The fast path is taken only when a per-thread hpel registry allows it, and covers every luma MC the analysis runs. Both paths are byte-identical, so it's an identity-cmp axis as well |
| `Y264_DIRECT_PERMB=0` | the frame-wide temporal-direct gate, for A/B against the per-macroblock default | The frame-wide gate engages on only 11-24% of B frames, so a `--direct temporal` measurement under it says nothing about temporal direct |
| `Y264_DIRECT_LRVOTE=1\|2` | **per B frame, which direct derivation the LOOKAHEAD would pick**, simulated on the lowres field in mb-tree Phase B. `=1` scores each derivation by distance from the lowres search result, `=2` by the lowres SATD of its bi-prediction | Prints `lrvote poc= col= past= T= S= n=`. **Dedupe by POC when aggregating**: the mb-tree window overlaps and a frame prints several times. md5-identical on/off. **Closed negative**: mode 2 doesn't predict new clips. Keep it as the baseline and don't re-derive it. docs/b-frames.md |
| `Y264_DIRECT_SCORE=1` | **per B frame, the direct prediction's SSD against the source and the macroblock count.** Run the same encode once per `--direct` mode and diff the rows | Answers whether a per-FRAME direct-mode choice could beat the per-clip one. t1, md5-identical on/off. **The signal is weak** (a mode that predicts better can code worse), so read it as a bound on the cheap proxy, not as a selector. docs/b-frames.md |
| `Y264_BSKIP_SCREEN=2` | **does the exact B skip screen hold, and how much probe work does it remove**: re-runs the full strict probe and every proven block beside the screened one and aborts on any disagreement | Prints probes, accept rate, luma 4x4 blocks and chroma planes proven, and 4x4 transforms before and after the screen. Any thread count, output identical to `=0`/`=1` |
| `Y264_EST_TWICE=1` | **what the RD-trial rate estimator costs, with no decision change mixed in**: every trial estimate runs twice and the first is kept, so the bitstream is byte-identical. The instruction delta against a plain run is exactly one set of trials under whichever pricing is armed | Prices pricing arms such as `Y264_EST_FROZEN`. Read instructions from `/usr/bin/time -l`, not wall. md5-identical on/off |
| `Y264_EST_SCRTRACE=1` | per-est-trial `(site, frame, mb, dist, j, lb_j)` trace for pricing ADMISSIBLE-SCREEN ideas offline (replay each MB tournament in call order against a running best) | Prices a screen family with no plumbing. Check admissibility: a bound built on stored candidate nnz overcounts what the estimator codes. t1 only, md5-identical on/off |

## 2. What would a perfect version of X buy? (bounds and oracles)

These replay a recorded ideal so an arm gets a ceiling **before** it gets a
threshold. Every one must pass its own byte-identity check.

| instrument | bounds | notes |
|---|---|---|
| `scripts/rc_invariants.py` | **the rate controller's invariants in seconds, through the replay harness**: the A6 set (`--bitrate` with a 1.0x CBR and a 1.5x cap, 6 clips) and the cvbr_compliance set (CRF with 0.8/0.6/0.4x caps), 36 cases at one thread over 6 s. Each case is recorded once with `REF` (cached under `$Y264_CACHE/rcreplay`, keyed on the binary md5 and the command) and replayed with `CAND`/`CAND_ENV`. For a restructuring step the identity column must read EXACT. The invariant columns (0 leaky-bucket underflows, A6 rate error inside [-5%, +3.4%]) are read off the replayed sizes | The first run records (minutes) and later runs replay (seconds). The VBV column is a proxy; vbv_check.py and hrd_check.py on real encodes stay the stream gates. `INFORM=1` reports invariant failures without failing |
| `Y264_RC_REC=<f>` / `Y264_RC_PLAY=<f>` + `scripts/rc_replay.py` | **the rate controller alone, in a fraction of the encode's time.** A `--threads 1` encode with REC writes, in consumption order, every pixel-derived value the controller reads (lookahead complexity sums, full-resolution complexity probes, the AQ frame-DC term, cut and anchor flags, the opening fit's median, the mb-tree mean offset) along with each slice's QP and RBSP size. PLAY runs the same command with lookahead, mb-tree, macroblock analysis, entropy coding and reference planes switched off and feeds the values back. **Replaying with the recording binary must reproduce every slice QP (EXACT)**: `rc_replay.py` checks that over 18 rate-control configurations. With `PLAY_BIN`/`PLAY_ENV` it replays another controller over the same inputs. When a slice's QP moves, its size is scaled by the 1/qscale law (`Y264_RC_PLAY_K` scales the exponent) and counted as modeled. A leaky-bucket VBV estimate prints for both sides. The Annex C gate stays `hrd_check.py` on a real encode | One thread only, because the consumption order is fixed there. A new pixel-derived input to the controller must be routed through `RCR_VAL`, or the replay reads `order differs` at it |
| `tests/rc_selftest.py` (meson test `rc_replay`) | **rate-control coverage in `make test`, no corpus**: writes a 48-frame synthetic QCIF clip with a cut and runs `rc_replay.py`'s 18-mode self-test on it. Every mode's decide and account path runs and must replay EXACT | ~3 s. It proves the controller is a function of its recorded inputs. Whether it controls well is for rc_invariants.py and the stream gates |
| `Y264_VBV_ROWBRAKE_TRACE=1` | **how good is pass 1's entropy estimate, row by row**: one `RBT` line per picture with the pass-1 CABAC estimate and the real coded bits of every MB row (I, P and B, serial and wavefront). The input of a row-level VBV brake | Frame median ~2% off, p90 ~6%. CABAC only; CAVLC P/B rows print 0 |
| `Y264_MBT_REC=<f>` / `Y264_MBT_PLAY=<f>` | mb-tree offsets: record every anchor's field **and the reference B's** (the walk has two outputs), replay both. Keyed on (POC, FNV-64 of the anchor's lowres), CRF-portable | **Replaying your OWN recording must be md5-identical.** Run that first, every time: a ship that adds a walk output makes the probe stale. Recordings carry a format magic, so a stale one says so |
| `Y264_SKIP_ORACLE`, `..._AT`, `..._SIDE` | what a perfect early-skip predictor would buy | The key is the ABSOLUTE display index. POC restarts per IDR and silently unions GOPs |
| `Y264_TYPE_ORACLE` | replay x264's frame-type placement, read from its bitstream, through us | |
| `Y264_HEX_ORACLE` | ME search-shape bound | |
| `Y264_MEREUSE_TRACE` | census of discarded-MV class (a): ref (r-1)'s winning 16x16 list-0 MV, scaled by temporal distance, as a seed for ref r's search | The scaled seed beats the median predictor on 57-61% of ref>=1 searches and beats or ties the full search's own winner on 11-15%: a speed lead and a small quality lead. Shipping the seed is open. Output-neutral, default inert, t1 only |
| `Y264_UNSAFE_NO_EMIT` / `_NO_NAL` / `_NO_MBT` / `_NO_REFBWAIT` / `_NO_PREVPWAIT` | delete probes: what disappears if a whole stage does | **Gate these on output identity, never on an argument about consumers.** A delete probe whose output moves is measuring a different encoder, not the thing deleted: it changes what the RD estimator sees, so the encoder makes different decisions and does a different amount of work |

## 3. Is the arm any good? (quality gates)

| instrument | use |
|---|---|
| `ARM='<env>' python3 scripts/run_band.py` | **the standard self-A/B gate.** It runs band-calibrated ABR and CRF ladders and reports per clip, with no single summary mean. Both sides run the same ladder, so rates match by construction. `BANDS=crf` runs only the band the standing rule gates on, for half the wall. `CLIPS=akiyo_cif,park_joy_720p` runs a subset. Scores the gate (`VMAF_GATE`, VMAF v1 by default). **The CRF band of a self-A/B stops early** once one clip reads worse than +1.0% or three read worse than +0.5%; `EARLY_STOP=0` reads all. The early stop never applies to the ABR band or to a run against the reference encoder |
| `scripts/bdcompare.py --vmaf --no-cache` | the underlying BD tool. **`--no-cache` is mandatory**: the tool caches ENCODES by command string, so a rebuild silently returns stale numbers. It scores the gate (`VMAF_GATE`); `--models` applies only under `VMAF_GATE=VMAF-NEG`. **`--subsample` refuses a stride that shares a factor with the mini-GOP period** (bframes + 1, from each template, 3 if absent): stride 4 at bframes 3 scores only anchor frames and can flip an arm's sign. Coprime strides avoid the bias but add +/-0.15-0.2% per clip, so self-A/B bands stay at 1 |
| **the VMAF score cache** (`bdcompare.vmaf_of`, every band) | **an output-identical encode never pays libvmaf twice.** Every score is stored under `$Y264_CACHE/vmafscore`, one file per key. The key is the BITSTREAM's md5 plus the source clip's identity (resolved path, size and mtime; a frame-cut copy carries its parent's identity plus the frame count), the subsample and the decode pixel format; a v1 key adds the model, the encode bit depth, the libvmaf build and whether the picture was scaled to the display. The binary and the command aren't part of it, so a lossless rebuild, an inert knob or a repeated anchor hits the cache. Hit and computed counts print at the end of a band. `VMAF_SCORE_CACHE=0` bypasses it |
| `VMAF_GATE` (`bdcompare.vmaf_of`, so every band and `calibrate_band.py`) | The quality score: VMAF v1 and its CAMBI ([vmaf.md](vmaf.md)). `VMAF_V1_BIN` and `VMAF_V1_MODELS` point at the libvmaf build and the v1.0.16 models. `VMAF_GATE=VMAF-NEG` reproduces an older NEG number |
| `scripts/bd_at_rate.py` | BD at MATCHED ACHIEVED bitrate. Required for anything that moves the CRF axis, because mb-tree on/off translates it |
| `scripts/bdcompare.py` with the CACHE ON, read for its SIZE lines | **does `--crf N` mean here what it means there?** Run both encoders at the same CRF numbers and divide the bytes. The x264 CRF that matches our bytes, log-interpolated across its own rungs, turns the ratio into CRF points. No timing, so it's load-immune. Read exact bytes out of the encode cache rather than the printed `NNNk`, since a small cell rounds to 3-5%, half a CRF point. docs/rate-control.md has the answer. **Traps:** the offset is content, not scale (two 48-frame segments of one 4K source differ by 0.6-0.7 of a point), and a black leader codes to nothing at every CRF and prints a flat ladder |
| `ARM='<env>' python3 scripts/band_at_rate.py` | **the whole CRF band at matched achieved bitrate**. It takes `run_band.py`'s ladders and `ARM` / `ARM_ARGS` convention and drives `bd_at_rate.py` per clip, several clips at a time (`JOBS`), ending with a median/mean/worst summary. The gate for any arm that moves the operating point, where `run_band.py BANDS=crf` would measure ladder placement instead. `EARLY_STOP` works as in `run_band.py`. Once the arm is refused, clips not yet started are dropped |
| `BANDS=deep ARM='<env>' python3 scripts/band_at_rate.py` | **the DEEP band (55-83 on the gate score; its ladders were solved on VMAF-NEG and need re-solving on v1) at matched achieved bitrate**, the regime the standing band can't see. Ladders are solved onto x264's own deep curve by `scripts/calibrate_deep.py --write` (`ladders_deep.json`). ducks and park_joy don't span it and are dropped. `BANDS=all` concatenates both ladders. Rungs print a model-derived `qp~` so regime knobs read in QP space. **The deep band's noise floor is ~+/-1.2 per clip** (measured with a near-inert perturbation). A deep read inside +/-1.2 is the band talking, not the arm |
| `ARMS='a=<env>;b=<env>' python3 scripts/hd_band.py` | **the HD band: BD on the gate score over twelve 720p and 1080p clips, several arms in ONE run**, the bar HD candidates are read against. Five rungs, CRF 23 to 35, with the DEFAULT side as the anchor; one anchor curve serves every arm. **Anchor and arm run at the SAME five CRF values, and the BD is integrated over the range the two curves share.** It doesn't solve the arm onto the anchor's bytes: the frame QP is an integer, so a CRF-to-bytes curve is a staircase, and below `medium` a byte solve misses often and the band reads the miss as quality. **The guard is the rate overlap.** Every cell prints its rate overlap, quality overlap and surviving rungs. A clip under `MIN_RUNGS` (4 of 5) is NOT SCORED; 4 of 5 is ordinary because two curves that don't coincide lose an endpoint at each end. **A refused clip is re-read on rungs moved to the middle of their steps** (fix A, rows print `[A]`). The anchor's ladder is probed once per clip and preset and cached in `$Y264_CACHE/hdband/ladder.json` keyed by the binary's sha. **The null is STORED, not run**: `scripts/hd_null.py` fits each clip's null spread from the point cache. Each arm prints its reading against the stored center, marked `inside null` within `NULL_SD` (2) robust sd. The refusal subtracts the most favorable plausible null read (center plus half-width). Clips with no fit, with anchor bytes that differ from the fit's reference binary, or read under fix A run ONE live control (`LIVE_CONTROL`, default `Y264_MBT_DEPFLOOR=16`). Where the null's read-to-read spread exceeds the bar, the band prints `UNRESOLVED` instead of `PASS`. `CONTROLS='ctl=Y264_MBT_DEPFLOOR=16;ctl2=Y264_MBT_DEPFLOOR=8'` runs two live controls on every clip; `CONTROLS=none` drops the referenced columns. **Early stop**: an arm is refused and its queued cells skipped once one clip reads worse than `EARLY_ONE` (+1.0%) or `EARLY_N` (3) clips worse than `EARLY_SOME` (+0.5%); both already fail the per-clip bar, so the stop can't change a verdict. `EARLY_STOP=0` reads every clip, which a report wants. **An arm can be another binary**: `ARMS='int=@/path/to/yah264'` (optionally followed by env) reads a whole build against `YAH264` as the anchor. Every (clip, arm, rung) cell is cached in `$Y264_CACHE/hdband/points.json`; hit and miss counts print per clip and per run. `NO_POINT_CACHE=1` bypasses it. `SOLVE=1` restores the matched-byte ladder. `CLIPS`, `RUNGS`, `FRAMES`, `JOBS`, `MIN_RUNGS`, `FIXA`, `PROBE_D`, `BAR`, `MEDIAN_BAR`, `CONTROLS` override; `WINDOW` and `ITERS` belong to `SOLVE=1`. Cost is 45 encodes and 45 VMAF scores a clip at eight arms. Load-immune, so it runs beside other work inside the core cap |
| `python3 scripts/hd_null.py` (`--show` prints the stored fit) | **the HD band's per-clip null spread, fitted from the point cache.** For every binary with both a default curve and a `Y264_MBT_DEPFLOOR=16`/`=8` curve on a clip, it takes the control's BD on the gate score the way the band scores an arm. It drops reads repeated by output-identical binaries (a lossless series of builds would otherwise pool one measurement many times). Per clip it stores the median and robust sd (1.4826 x MAD) beside the plain mean and sd, with the binaries pooled and a reference binary (`YAH264` or `--ref`) whose anchor bytes the band compares against to detect changed defaults. Written to `$Y264_CACHE/hdband/null-VMAF-v1-disp.json` (`null.json` under NEG), per preset, thread count, frame count and ladder. Expect only 5-6 distinct reads per clip. The reference binary's default curve comes from the point cache when available; otherwise the fit encodes it (bytes only, five encodes a clip). `--no-encode` skips that. Refit with `YAH264=<new build>` after a change to the defaults |
| `python3 scripts/m4_label.py --src-dir <dir> --out <jsonl>` | **per-sequence arm labels + lookahead features on an EXTERNAL corpus**, one JSONL row per sequence, labeled with its metric: BD on the gate score for each M4 arm against the shipped default at matched achieved rate (the baseline's CRF points set the byte targets; each arm is binary-searched onto them), plus the four input-derived frame features (`tdiff`, `flat`, `tex`, `motion`) dumped by `Y264_PSY_FLAT_LOG` / `Y264_ADME_LOG`. When no arm beats the default by 0.30%, the row's class is `default`. It has no path into `tests/corpus`, so the train/test split is structural: **the gate corpus is TEST-ONLY, permanently.** Resumable. ~7 s per 64-frame 480x272 sequence. docs/content-selection.md |
| `scripts/shot_determinism.sh [input.y4m]` | **the per-shot re-encode contract**. Encodes with GOP boundaries on the cuts, writes every GOP to its own segment, then re-encodes each GOP alone with the same pinned frame threads and compares bytes (docs/engine-interface.md). Gated at `T=1`; with threads a mismatch is WARN and exit 0 (`STRICT=1` fails). K and CRF come from the environment |
| `Y264_QP_TRACE=1` + `scripts/multishot_bd.py` | **where the bits go across SHOTS**. The trace prints every frame's slice QP, mean coded QP and mean intended QP (slice + offset map), with the mb-tree mean offset and the CRF DC term; on a skip-heavy shot the coded mean hides the allocation. `multishot_bd.py` encodes the multi-shot sequences (`local/corpus/ms_*.y4m` + `.cuts`) at CRF 22-34 for any set of arms (our env/flags, x264) and prints BD on VMAF v1 per pair plus per-shot VMAF and bit shares. The single-shot board can't see an across-shot allocation gap, and one can turn a per-clip lead into a deficit on the same clips concatenated |
| `scripts/calibrate_band.py --write`, `ladders.json`, `curves.json` | the band ladders and the x264 curve they were solved onto. **A bare run writes nothing into the tree**: without `--write` the json files land in a fresh scratch directory whose path the run prints. Every run prints both destinations, each marked `OVERWRITE` or `create`, before the first encode. `--dry-run` prints that plan and stops. `--clips`, `--crfs`, `--frames` and `--jobs` cut a sweep down to a cell or two. `--out-dir` names a destination |
| `scripts/calibrate_deep.py --write`, `ladders_deep.json`, `curves_deep.json` | the same, for the deep band (55-83, ladders still from VMAF-NEG) over CRF 30-50. Same flags; the tree's ladders move only under `--write` |
| **the ladder shift**: rerun the same ABR arm at ladders +/-13% in rate | **the cheapest disproof of an ABR refusal, with no extra tooling.** A real effect keeps its sign; the band's own chaos doesn't (one arm read +0.96%, +0.25% and -5.75% across three ladders). The ABR band's per-clip noise floor runs from 0.2 to 11 points, so measure it before recording any ABR refusal, by perturbing the CURRENT default slightly (an AC gain of 1.734 / 1.785 against the 1.7 default), not by an arbitrary 1.02. Corroborate with the **CRF low band**, points 32-41, which reaches the same QP regime with no rate-controller feedback loop |
| `python3 scripts/band_check.py` | **does it band in the dark?** v1's CAMBI ignores the darkest areas, so a BD band can pass an arm that flattens dark gradients. On five 96-frame windows of dark, real frames (bright beside dark, and dark with faint grain), it solves yah264 onto x264's CRF 23 size. Over the 8x8 blocks the source makes dark and smooth, it reads the share the decode leaves flat, texture kept, dark PSNR and CAMBI. It fails a window more than 0.05 flatter or 1.0 CAMBI worse than x264. Required for any AQ, psy or RD-decision change. `WINDOWS`, `ARGS` (extra yah264 flags) and `OUT` override, and `Y264_*` knobs reach the encodes from the environment. About 10 minutes at two threads |
| `scripts/hf_probe.py` | **where the texture goes**. Runs both encoders on the minimal CQP reproducer and reports per-band DCT energy retention (recon/src, LF/MF/HF zigzag rings) beside bits, PSNR and VMAF v1, split by the decoder's per-MB class and a source motion class. `HF_VMAF=1 ARMS="next-vs\|x264-med" python3 scripts/hf_probe.py bus_cif 100 30,36,42,48` |
| `scripts/hf_join.py` | the attribution half: per-MB 3x3 skip/inter/intra contingency of our class against x264's, with each side's MSE and AC retention per cell. The skip/skip cell is the reference-chain control: neither side coded it, so a gap there is inherited reference quality |
| `scripts/cvbr_compliance.sh`, `scripts/vbv_check.py` | VBV and capped-VBR compliance: violation counts and buffer traces |
| `scripts/ladder.py`, `scripts/crf-solve.py` | ladder construction and the CRF point that hits a target rate on a given clip. `crf-solve.py` PINS its operating point per (clip, args, target, tier) in `tests/.crfpin`. See the first trap in section 7 |

## 4. Is it fast? (wall harnesses)

**Never price an arm with a table delta.** Our CRF is a staircase. A changed
field re-picks rungs, so two table runs aren't at the same operating point.

Every wall measurement here follows one pattern. Several arms are measured
INTERLEAVED round-robin against the baseline, medians of 5 or more, **with a
duplicate-base control arm**. Every timed run writes to `/dev/null`. A
drifting box then moves all arms together instead of whichever one it happened
to hit. The control says whether the reading is usable at all. Measure
several arms in ONE batch: arms measured one invocation at a time can come back
out of order. Price every threaded arm at `THREADS=12` and state the tier,
because a pure-C reading and an as-shipped reading answer different questions.

| instrument | use |
|---|---|
| `scripts/perf-comp.sh`, `-set`, `-crf-set`, `-purec`, `-modes` | the comparison sets against x264. `-purec` uses the fair pure-C reference (section 6). `PERF_REPEAT` knobs drive the repeated-sample timer that resolves the short CIF cells |
| `Y264_REFENC_CACHE=0 make parity-status` / `parity-status-crf` | the ten-clip board's tables. The published parity tables come from the long-form board (`scripts/longform_throughput.py`) and its report. CRF is the one to quote for speed. Timing is `perf_counter` with the two arms interleaved; the same binary run twice reads within 0.01 |
| `scripts/parity-clips.sh`, `scripts/parity-clip-calib.sh` | the table's clip set and its per-clip calibration |
| `scripts/psnr_leg.py` | **the PSNR-Y floor**. Every board's `dPSNR-Y` column routes through this one adjudicator. Fed `clip=dB` pairs, it prints the median, the worst clip, `PSNR-FLOOR PASS/FAIL` at more than 1.0 dB below x264 on ANY clip, and the `PSNR-DEBT` list at 0.5 dB. Per clip, because a median floor isn't a floor. The number costs nothing: `--feature psnr` rides on the libvmaf pass that already scores dVMAF, and the pooled VMAF mean is bit-identical with it on. **Read the column on the CRF board.** The leg is "at equal bytes", and only the matched-point solve makes that true; ABR asks for the same rate and can deliver +10% |
| `scripts/ffboard.py` | **the control for "is the table scoring our Y4M reader?"** Both encoders are called as libraries inside ONE ffmpeg process: one demuxer, one thread pool, no CLI on either side. It answers the input-path question and doesn't replace the table; against `parity-status-crf` at the same points the two agree within noise. It needs an ffmpeg built with libyah264 at `../build/ffboard/ffmpeg-yah264/ffmpeg` (docs/ffmpeg.md rebuilds it). Keep it out of `/tmp`, which macOS sweeps. **Point `X264LIB` at the fair pure-C libx264 (section 6)**, or the pure-C arm reads our vectorized C against scalar C |
| `scripts/longform_throughput.py` + `scripts/lf_feed.py` | **the long-form board: whole titles (10+ min) and the staged segments, both CLIs as shipped, both reading stdin from one feeder.** Never use a file path in a timed run: the reference CLI reading a large Y4M by path is input-capped (600 frames of meridian 1080p: 3.9 s by file, 1.56 s piped). VideoToolbox decodes full titles on the fly (+ scale_vt below native). The timed feeder runs three decoders on chunks because one delivers only ~230 fps of a 4K master. Every chunk decodes 5 s early and proves its first/last frame from ffmpeg's per-frame timestamps, because these masters are nearly IDR-free and a seek to a recovery-point keyframe drops frames. `verify-feed` checks chunked against sequential per frame. `plan` is the dry run of the timed legs (`timed` G3 + SIMD t1, `purec` G1/G2 on one 60 s window per 3 min, `validate`) with estimated walls. **`point` reuses solves across builds.** The reference point carries over from any earlier build of ours with the same reference binary and subsample. Ours is encoded once at the newest earlier solve's rate factor; if the md5 equals the stored one the solve is recorded with no more encodes (`reused`), otherwise the secant starts from that rate factor and slope (`warm`). Reuse needs the same `SOLVE_THREADS`, since threaded output can differ by thread count; another count still warm-starts. Every record stores the bitstream md5, the slope and the encode count. `LF_NO_REUSE=1` solves from scratch (`cold`) |
| `scripts/ffboard.py` solve cache | **keyed by the three binaries, not by the clip alone.** `$Y264_CACHE/ffboard/solve.json` (default `~/.cache/yah264/ffboard`) holds the CRF and ABR solves across boards. The key carries a short sha of each dylib the ffmpeg will actually load (leaf name from `otool -L`, symlinks resolved, because a prefix is rebuilt in place and a path isn't an identity), each library's pkg-config `Version`, and a sha of the ffmpeg binary. A key without the libraries reuses solves made against a different CRF scale and voids every ratio on the board. **Every solve prints `solve cache: hit`/`miss` with that key.** `--no-solve-cache`, or `Y264_NO_SOLVE_CACHE=1`, neither reads nor writes it. The per-run bitstreams and y4m stay in scratch |
| `scripts/ffboard.py` size-match guard | **the board refuses to print a median it can't stand behind.** Every `RC=crf` and `RC=abrm` row is rate-matched by construction, so an unmatched dsize means the two encoders did different amounts of work. A dsize median outside +/-2% or any clip outside +/-5% prints `INVALID: sizes unmatched`, names the worst clip, and exits 2 without adjudicating the PSNR leg. The median bar catches a scale that moved under the whole table; the per-clip bar catches one solve that didn't converge. `RC=abr` is exempt and says so (its `n rate` / `x rate` columns are there for that). `Y264_DSIZE_TOL` and `Y264_DSIZE_TOL_MAX` move the bars; setting the first also turns the guard on at `RC=abr` |
| `scripts/ffboard.py` header check | **the ffmpeg wrapper is ABI-bound to the `include/yah264.h` it was compiled against.** The build writes `git hash-object include/yah264.h` to `yah264-header.sha` beside the binary. The board hashes `$Y264LIB/include/yah264.h` and refuses a mismatch, naming both blobs and paths. A wrapper one field behind the library reads `yah264_param_t` at the wrong offsets and still encodes. The library's own guard is the other half (the param struct carries its `size` and `yah264_encoder_open` refuses a foreign one). `Y264_HEADER_CHECK=0` bypasses it. **`Y264LIB` is less authoritative than it looks**: `DYLD_LIBRARY_PATH` only supplies the leaf name the binary asks dyld for, so an install whose soname differs isn't loaded at all and the build-time absolute path is used instead |
| `./build/tools/checkasm/checkasm --bench` (the `sad_quad` / `satd_quad` rows) | **whether BATCHING a kernel pays, before anything is wired to it.** Each row is the batched form against four dispatched singles. SAD reads 1.60-1.80x and SATD reads ~1.01x, so the answer is load-boundedness. Read only sizes where BOTH forms are NEON (8x4 mixes a NEON batch with a C single; 4x4/4x8 have neither) |
| `sample` profiles of both shipped binaries | **the SIMD share of each encoder's time**, as whole-binary percentages. Rank kernels in milliseconds per job, multiplying each side's percentage by its own wall first, since percentages aren't comparable across two binaries |
| `Y264_GPU_PHASEA=1` + `Y264_GPQ_WARMUP=10000` / `Y264_GPQ_CONSUME=0` | **the per-process Metal floor and the gpq tax split.** `WARMUP=10000` arms the per-push phase-A offload but never submits a job, so only the background `ngc_open` runs: that's the 12-17 ms bring-up floor any Metal-armed arm pays PER TABLE CELL. The floor is device init, not shaders; a precompiled metallib doesn't move it. `CONSUME=0` submits rounds but never reads them, which splits chain-side from walk-side cost |
| `python3 scripts/knob_census.py` / `--check` | **the `Y264_*` knob census** (`docs/knobs.md`, generated): every env knob's reader, default and tier (shipped default / instrument / arm). `--check` runs inside `make test` and fails on an uncatalogued knob, a catalog entry whose reader was removed, or a comment claiming "default OFF/ON" that contradicts the code default. It reads getenv, the `*_tunable` helpers and the thread pool's spin-site macro. docs/knobs.md has the count |
| `KEEP_SCRATCH=1` | **keep a harness's working directory instead of removing it, and print where it is.** Every harness builds its scratch through `scripts/scratch.sh` or `scripts/scratch.py`, which remove it on exit, on Ctrl-C and on a TERM from a killed agent. Set this when a cell failed and the bitstream or the recon is the evidence. It's the ONLY way any of these directories survives its run |
| `make clean-scratch` | **the sweep for scratch no trap could remove**, after a SIGKILL, a panic or a power cut. It matches only the prefixes in `scripts/scratch-prefixes.txt`, only directories, only under `$TMPDIR`, and only ones older than a day. It prints each path with its size. `DRY=1` lists without removing; `DAYS=7` widens the age. Caches MEANT to outlive a run (`ffboard.py`'s `solve.json`, `bdcompare.py --cache`, the openh264 raw planes) live in `$Y264_CACHE` (default `~/.cache/yah264`), which nothing sweeps |

## 5. Is it correct? (the gates every ship passes)

```
make test                                   # unit tests (checkasm is one of them)
./build/tools/checkasm/checkasm             # every kernel against its C reference, guards on
./build/tools/checkasm/checkasm --list      # the table: group, family, the ISA each row needs
./build/tools/checkasm/checkasm --isa neon pixel    # one tier, one family
make conformance                            # recon-match (YAH264_CONF_FAST)
python3 scripts/stress_threads.py           # hang/determinism under load
scripts/tsan_catch.sh                       # TSan; the floor is 0 reports
python3 scripts/env_gate_audit.py           # every lazy env static: TRAP + COLD, exits 1 on TRAP
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
```

| instrument | answers | notes |
|---|---|---|
| `tools/checkasm` | does every optimized kernel still produce exactly what its C reference produces, and does it stay inside the memory it declares | Each group compares the C reference against the kernel SYMBOL, not through the public entry point, so every kernel that ships is exercised, including ones the dispatcher doesn't select on this box (plain-NEON twins of dotprod kernels, for example). Three nets sit on top of the value comparison: a poisoned border on every destination; a row-padding check for a kernel that stores a wide vector into a narrow block; and an **mmap/mprotect page guard** sized to exactly the window the kernel declares, run at BOTH tails. Only the page guard catches a read whose value is discarded (the classic "load 8 lanes, use 4" tail), which the scramble check and the sanitizer both miss. A page fault is a hard stop that names the shape. `--list` prints the table with the ISA each row needs; `--isa neon\|sse4\|avx2\|avx512\|all` runs one tier. The table is per architecture: an x86-64 build runs the portable rows plus the x86 ones. A group's BODY is written once and takes its kernel as an argument, with one thin row per tier on top, so a new tier's twin inherits the row's adversarial fills and page guards. Prediction destination blocks are packed, so a store sized to the register faults instead of merely disagreeing. `--bench` is informational and single-threaded. A checkasm multiple is necessary, never sufficient |
| `scripts/x86-docker.sh` ("x86 in Docker") | does the encoder build and pass its correctness battery as linux/amd64 on a box with no x86 in it, and does the SIMD tier axis move a byte | **The matrix is `{rosetta, qemu}` x `{Y264_SIMD_FORCE=none, sse4, avx2}` x `{make test incl. checkasm, conformance --fast, identity cmp against none}`, plus a 10-bit build**, from `docker/x86/Dockerfile` (ubuntu 24.04, no libvmaf-dev; quality harnesses reach VMAF through ffmpeg). The tier axis is the RUNTIME cap, so one build serves all three rows; `-Dsimd=` is CI's job. **Neither emulator has AVX-512**, and the script asserts and prints that, so an emulated green can't be read as "AVX-512 works". Rosetta translates ahead of time and runs near native, but has illegal-instruction bugs: a case that dies of SIGILL is rerun under QEMU and its Rosetta row marked "emulator". QEMU's TCG is several times slower, the independent second opinion for an item's final gate. Under Rosetta `/proc/cpuinfo` is an x86 one; under qemu-user it's the host's arm64 file. The probe that counts on both is the binary's own CPUID as `checkasm --list` prints it. Docker Desktop registers both interpreters for the same ELF magic, so selecting QEMU DISABLES the Rosetta binfmt entry for the leg; the script announces, traps and restores it. **Negative control**: with no x86 kernels, `sse4` and `avx2` must be byte-identical to `none` by construction; a difference is a harness defect. Corpus clips are bind-mounted READ-ONLY at their own paths, never copied. The script reads NO clocks: an emulated wall time isn't a measurement. An emulated run is still CPU load, so coordinate it like a timed leg (docs/simd.md). The fixture cache is keyed by the **ffmpeg identity** (path and version banner) beside `FIXVER`, since two ffmpegs don't write the same Y4M header; the container gets its own cache directory mounted over the tree's, which also keeps its writes out of the worktree. The ubuntu CI job (`.github/workflows/ci.yml`) runs on every push that touches code |
| `scripts/san_matrix.sh` | **the encoder under ASan/UBSan over 21 edge inputs** (odd sizes, 1-2 frame clips with B, keyint 1, qp 0, extreme bitrates, 4:2:2/4:4:4, CBR, 2-pass, direct temporal, the hardware backend, 1-12 threads) | Builds `build-san/` once. ~3 min. The exit status is the number of cases with reports. It finds memory bugs recon-match can't see |
| `scripts/identity_ab.sh` | **do two binaries (or one binary under two env arms) produce the same bitstreams?** The gate for any change that must not move a bit: refactors, layout changes, knob removals that keep the default branch | About 100 cells at full (`MATRIX=quick`: 15): every rate mode (CQP, CRF, ABR, capped ABR, CBR, CRF+VBV, 2-pass), presets, tunes, coding tools, CAVLC, PAFF, slices, 4:2:2, 4:4:4 and 10-bit (derived from foreman_cif with ffmpeg), 720p and 1080p, plus threaded cells. t1 cells are final. Threaded cells are re-run `REPEAT` times per side and read VARIES (a warning) when a side doesn't repeat. A control cell asks the two sides for different streams and must read DIFF, so an encode that never ran can't pass. Every `Y264_*` in the caller's env is cleared, and `ARM_A`/`ARM_B` set env per side. Full matrix about 45 s at `BUDGET=4` |
| `scripts/hrd_check.py` | would a conforming decoder's coded picture buffer survive this stream: underflow, overflow, peak fullness, tightest arrival margin | Takes NOTHING about the bucket on the command line: BitRate, CpbSize, cbr_flag, the initial delay and every removal time come from the SPS VUI hrd_parameters and the buffering_period / pic_timing SEI. Exit 0 is clean, 1 a violation, **3 = the stream carries no HRD and nothing was checked** (every stream without `--nal-hrd vbr\|cbr`), so a vacuous pass can't read as green. `--allow-no-hrd` softens that. `HRD=1 scripts/cvbr_compliance.sh` is the 36-cell leg; it prints the VBV verdict beside both HRD verdicts, because a cell whose rate control underflows its own model emits a stream that underflows that model's declaration. **Not a replacement for `vbv_check.py`**, which is TOLD the bucket and simulates the encoder's law ("did our rate control respect the cap"). This one asks "would a receiver's buffer survive what we emitted" |
| `scripts/level_check.py` | is the declared `level_idc` the one Table A-1 requires, and is any leg of it exceeded | Frame size, MB/s, DPB, bitrate/CPB (from the HRD, or `--bitrate` when the stream has none), and the vertical MV bound the VUI declares (a note, a failure only under `--strict-mv`). `--exact` demands the declared level EQUAL the pick. `--y4m SRC` reads the frame rate off the source when the stream has no VUI timing; there's deliberately no default frame rate. Table A-1 is transcribed independently on purpose: a checker sharing the encoder's table agrees with it by construction |
| `scripts/recovery_check.py` | does an `--open-gop` stream really recover where its recovery point SEI says it does | Cuts the elementary stream at the recovery point, pastes the parameter sets in front, decodes that, and compares the LAST `n - k` frames against the encoder's own `--dump-recon`. The pictures the recovery point doesn't cover have a POC below its own and land at the FRONT in output order, so counting back from the last frame makes their number irrelevant. **The cut decode passes `-flags2 +showall`; the floor is ffmpeg 6.1.** Which pictures a decoder shows after a mid-stream start is display policy, and versions differ (6.1.1 stops two short of draining its buffer at end of stream; asked for all, it produces the same samples as 9.0.1), so the selection is made here rather than borrowed from the decoder. Exit 0 clean, 1 mismatch, **2 = no recovery point**, itself a failure for an `--open-gop` encode |
| `scripts/h264_syntax.py` | (not a gate) the Annex-B / SPS / SEI reader the two checkers share, plus `--probe STREAM` for the stream properties a harness needs to choose a decoder | Parses parameter sets, SEI and slice HEADERS only, with no slice-data entropy decode, which keeps the checkers cheap enough for a gate |
| `scripts/fetch_openh264.sh`, `scripts/fetch_jm.sh` | build the extra conformance decoders into `tools/openh264/h264dec` and `tools/jm/ldecod` | Source pinned by tag AND sha256, downloaded and built under `scratch/`; only the binary is installed. Nothing is vendored and both directories are gitignored. The CLI needs a source build because the packaged openh264 ships the library only. CONTRIBUTING rule 4 allows the JM as an oracle: run it and compare its output. Only `ldecod` is built |

`env_gate_audit.py` exists because the TSan floor isn't naturally zero. The
encoder resolves its `Y264_*` knobs through lazy function-local statics. The
CLI opens one encoder per GOP from concurrent workers, so any gate not resolved
on the main thread first gets its first touch on whichever worker gets there.
These races are benign same-value init, but they fog real hunts. The audit
walks the call graph from `warm_lr_statics` / `ntp_pool_create` /
`ntp_bg_create` and names two failures. A **TRAP** gate's resolved value still
satisfies its guard, so the gate rewrites its static on every call, a perpetual
writer no warm can quiet (resolving "unset" to `-2` under a `v < 0` guard does
exactly this). A **COLD** gate latches and is never warmed. Run the audit after
adding a knob. Two things it can't see: a static resolved **inline inside a hot
function** is unreachable by any warm and must be hoisted into an accessor
first; and an **unconditional store** in a warm function
(`y264_tl_on = trprof_on();`) is a perpetual writer that isn't a getenv guard
at all. In anything a warm touches, use read-compare-write instead of a plain
store.

**Determinism is a diagnostic with threads, a gate at one thread.** Threaded
encodes don't need repeatable byte-exact output, so `stair_determ.sh`,
`shot_determinism.sh` and the threaded identity cells in `conformance.sh` print
WARN and exit 0 on a difference. At `--threads 1` two runs must match:
`check_determinism` in `conformance.sh`, the regress rerun, `determ_repeat.sh`
at t1, the pipe check at t1 and `make repro` all FAIL on a difference. The
threaded checks stay useful for hunting an uninitialized read or a race, and for
telling whether an identity-gated probe can be read from one run. The notes
below are about using them that way.

**Run `determ_repeat.sh` on a LOADED box.** On an idle one it can pass a default
that emits several distinct bitstreams per eighteen runs: the defects it exists
for depend on timing, and an idle 18-core box runs the threaded path the same
way every time. Interleave the two arms so both see the same load. The reading
is the count of distinct md5s, not pass/fail:

```sh
pids=; for i in 1 2 3 4 5 6; do (while :; do :; done) & pids+=($!); done
scripts/determ_repeat.sh; kill $pids; pgrep -f "while :" && echo "LEAKED SPINNERS"
```

**Gate the MODE, not just the default.** `determ_repeat.sh` and
`stair_determ.sh` both encode with the shipped defaults unless told otherwise,
so a defect confined to a non-default coding mode is invisible to the whole
battery. `ARGS=` (here) and `STAIR_DETERM_ARGS=` (stair_determ) are the slots
for that. Both scripts keep env in a SEPARATE slot on purpose: passed to
`env(1)`, an encoder flag makes it reject the command, every run produces
nothing, and the nothings have matching md5s.

**Kill by PID and VERIFY, never `kill %N`**: in non-interactive shells some
job-spec kills fail silently, and leaked spinners corrupt every speed number
after them. **Check `uptime` before any timing session.** Band and BD reads at
matched rate are load-immune. Walls and tables aren't.

`make conformance` gates the DEFAULT path. A coding tool behind an env knob has
its own conformance surface: the cross product of ref count, B-frame count,
entropy coder, direct mode and transform. `recon_sweep.sh` is that cross
product (6 clips x 5 QPs x 10 configs). It deletes its outputs before every
iteration and checks the encoder's exit code, so a rejected config can't leave
the previous iteration's files to score as a pass.

`scripts/abr_decode_gate.sh` encodes the table ABR shapes at t12. It asserts
that every input frame decodes AND that mean PSNR clears a floor (25 dB; broken
emission reads ~15, working reads ~30-42). Nothing else covers the threaded ABR
path: conformance can't reach it (`--dump-recon` forces the serial streaming
path), the CRF band never runs ABR, and identity gates compare an encoder with
itself. `ARM='<env>'` gates an arm.

**Three decoders, and what each one can actually read.**
`YAH264_CONF_DECODERS="ffmpeg openh264 jm"` turns the recon-match gate into a
three-oracle gate (default: ffmpeg alone). ffmpeg and the JM read everything
this encoder emits. **openh264 reads almost none of it**:

| stream shape | openh264 2.6.0 | JM 19.0 | ffmpeg |
|---|---|---|---|
| 4:2:0 progressive, no B (CAVLC, CABAC, 8x8, multi-ref, crop, all-intra) | exact | exact | exact |
| any B slice | **wrong pictures, exit 0** | exact | exact |
| 4:2:2, 4:4:4 | writes nothing | exact | exact |
| `frame_mbs_only_flag == 0` (fields, either flavor) | refuses at the SPS | exact | exact |

On B slices openh264 emits the right NUMBER of frames with the wrong CONTENT
and returns success. `conformance.sh` therefore asks `h264_syntax.py --probe`
what's in the stream and SKIPS openh264 by stream property, never by trusting
the decoder. The run prints a per-decoder coverage line, because a decoder that
checked nothing must not read as a decoder that agreed; openh264 covers only a
small fraction of the cells, and the `baseline-shaped` cells exist to give it
some.

openh264 doesn't decode field pictures at all, PAFF included: it refuses
`frame_mbs_only_flag (0)` while parsing the SPS and decodes zero frames. **The
JM is the second oracle for PAFF.** An interlaced fixture (`ffmpeg tinterlace`
plus an MBAFF-coded x264 encode, 24 frames at 320x240) decodes byte-identical
under the JM and ffmpeg.

For any default flip, by hand: the escape env must reproduce the OLD default
md5 exactly, and the new default must equal the explicitly armed md5.

## 6. The reference side

The comparison harnesses drive external baselines through environment
variables. The repository can't provide any of them:

| variable | used by | what it must point at |
|---|---|---|
| `X264_ASM`, `X264_C` | `scripts/perf-comp.sh`, `scripts/instr-ratio.sh`, `scripts/crf-solve.py` | an x264 CLI with assembly on / a pure-C build with assembly off and the compiler's auto-vectorizer left on. That's the fair build. A stock `--disable-asm` build is a scalar strawman |
| **which reference binary** | every board | **Say which build produced a number**, because the harnesses don't all reach for the same one. `scripts/ffboard.py` loads whatever `X264LIB` points at. `scripts/perf-comp.sh` builds from the `../x264` source checkout with `--disable-lavf --disable-ffms --disable-avs --disable-swscale`. `scripts/multishot_bd.py` uses that checkout's CLI. The installed reference on the dev box is 0.165.3222 (Homebrew). **The fair pure-C build** is configured with `--disable-asm`, then `-fno-tree-vectorize` is removed from the generated build config so the compiler's vectorizer stays on (a build-flag change, no source edits). It lives at `~/src/build/x264cli-purec-<rev>/x264`, built from the same revision as the SIMD CLI. A published pure-C row says so |
| `X264` | `scripts/cvbr_compliance.sh`, `scripts/ladder.py` | an x264 CLI |
| `X264LIB`, `Y264LIB` | `scripts/ffboard.py` | installed prefixes of libx264 and libyah264 for an ffmpeg that links both, at `../build/ffboard/x264asm`, `../build/ffboard/x264noasm` and `../build/ffboard/y264inst`. No defaults on purpose: a stale install that loads reads as a result, while a missing one reads as an error. The board hashes both dylibs into the solve-cache key and checks `$Y264LIB/include/yah264.h` against the header the ffmpeg was built with |
| `FF` | `scripts/ffboard.py` and the row scripts | that ffmpeg (default `../build/ffboard/ffmpeg-yah264/ffmpeg`, then `/tmp/ffmpeg-yah264/ffmpeg`). Keep the set out of `/tmp`, which macOS sweeps. `Y264_FFBOARD_ROOT` moves the whole set. docs/ffmpeg.md rebuilds it |
| `OPENH264` | `scripts/openh264-shim.sh` | an openh264 `h264enc` binary |
| `OPENH264_DEC` | `scripts/openh264-shim.sh --decode`, `scripts/conformance.sh` | an openh264 `h264dec` binary. Defaults to `tools/openh264/h264dec`, which `scripts/fetch_openh264.sh` builds. The packaged openh264 has no CLI |
| `YAH264_CONF_DECODERS` | `scripts/conformance.sh` | which oracles run: any of `ffmpeg openh264 jm` (default `ffmpeg`). A named decoder whose binary is missing is an ERROR, never a silent fall back to ffmpeg |

The clips come from `scripts/fetch_corpus.sh`, which fetches the board's ten
and the wider corpus. docs/corpus-sources.md says what is and isn't fetchable.
Notes about instrumenting the reference encoder itself stay out of the public
tree and live with the measurement records.

## 7. Traps that have each cost a round

- **The table's dVMAF is only comparable across builds if the operating point
  is pinned.** Our CRF staircase (~13% of rate per rung) lets a fresh solve land
  on a neighboring rung, so two builds with identical output can read different
  dVMAF. The operating point is PINNED per (clip, args, target, tier), not per
  binary (`tests/.crfpin`, `Y264_CRF_PIN=0` bypasses). The pin is resolved
  BEFORE the solve cache and is part of its key. Every solve prints `pin_state`
  and `pin_drift_pct`. **Read `pin_drift_pct` before reading dVMAF**: nonzero
  means this build's rate curve moved and the comparison shifted with it.
- **A timed output write doesn't cancel out of a ratio, and it fakes a
  resolution-shaped effect.** It inflates the BASELINE, most where the
  bitstream is biggest, so the arm's cost comes out too small. A spread guard
  doesn't always catch it (one cell read 3.2x its true wall at a spread of
  1.02). **Time to `/dev/null`, and give `--md5` its own untimed encode.**
- **A wall harness that defaults to the PURE-C tier prices the wrong tier for
  any as-shipped question.** The only tell is a `tier pure` token in a header
  line.
- **Refuse to compare two builds whose `optimization` differs.** A meson -O2
  build against an -O3 build reads as a 2-3% phantom win.
- **Put both arms of a throughput test in ONE burst with mirrored launch
  order.** The box's load moves slower than a burst and faster than a round.
- **`Y264_MBTREE_OFF` is a dirty control.** Besides turning off propagation, it
  swaps the per-MB offset to a different AQ array. Use `Y264_MBTREE_STRENGTH=0`
  for a clean propagation on/off.
- **zsh doesn't word-split scalars.** Never build CLI args in a string
  variable. Verify that A/B configs produce DIFFERENT md5s before trusting a
  null.
- **Verify the probe is in the binary you time.** A probe-less build gives a
  clean null.
- **A guard that reads an unpopulated signal disables the thing it guards** and
  prints a clean `+0.00%` meaning "never ran". The tell: output byte-identical
  to baseline rather than to the unguarded arm.
- **Check what a substituted field is interpreted AS.** `mb_qp_pre` reads the
  COMBINED offset (AQ folded in). A substituted offset map holding the mb-tree
  term alone inflated a result by more than 2x.
- **Keying a probe on POC silently UNIONS GOPs.** POC restarts at every IDR, so
  a set keyed on `(POC, ...)` matches entries from different frames and invents
  hits or recomputes. **The tell is a false count that tracks the I-frame
  count.** Key on an absolute display index. When a new probe disagrees with an
  existing one, suspect the new one first.
- **BD reads are sweep-dependent.** Check the saturation flag. A reference
  curve too flat in the band makes the number meaningless (touchdown's rows
  have read absurd values).
- **Only `determ_repeat.sh` re-runs one configuration against itself.**
  `stair_determ.sh` compares ACROSS thread counts and `w2_canary.sh` against a
  reference build; neither catches a default that emits a different bitstream
  every run, which makes every md5 identity gate a lottery ticket.
- **A "byte-identical by construction" argument isn't a gate.** When a
  feature's whole claim is that it doesn't change the output, the claim IS the
  gate: run it, under load, against the escape.
- **TWO code paths reading the same settled bound from different origins will
  disagree.** The warm measures its window offsets from the ring head; the walk
  measures them from its anchor, `d` entries further in. One bound can't serve
  both. The tell: turning off BOTH settled-bounded readers
  (`Y264_MBT_BLEG_REUSE=0 Y264_MBT_PAIR_SCALE=0`) makes the two paths agree,
  while turning off either alone doesn't.
- **TSan can be silent on a publish-before-write.** A ready flag set before the
  batch that computes the data races only when the write lands late. The
  repeated-run md5 loop catches it; TSan may not.
- **TSan is silent on a stack overflow.** When run-to-run variance survives
  every concurrency knob being turned off, stop looking for a race and look for
  memory written past its end.
- **A second decoder that returns 0 isn't a second opinion.** An oracle's
  capability is a property of the stream, not of its return code. A
  multi-oracle gate must print how much each oracle actually covered (section
  5).
- **A stale output file scores as a pass.** An A/B loop that writes a fixed
  filename re-scores the PREVIOUS iteration whenever the encoder writes nothing
  (an unknown flag exits 2 and prints help). Delete the outputs at the top of
  every iteration and check the exit code. This is the other half of the zsh
  word-split trap.
- **A ship can break a probe silently.** Run a probe's own identity gate FIRST,
  every time. When a feature adds an output, ask which recorder carries it.
- **This box can't time a 720p cell reliably while a desktop video process is
  ACTIVELY DECODING** (`WallpaperAerialsExtension`, `VTDecoderXPCService`). The
  CIF cells stay steady meanwhile. Residency isn't the condition: the extension
  is resident almost always at a negligible load. Gate on the SANITY CELLS, not
  on `ps` output.
- **`ps` `pcpu` is a decaying average, not an instantaneous sample, and it lies
  after a `SIGSTOP`** or a launchd relaunch. Ground truth is a cumulative
  CPU-time delta:
  ```sh
  a=$(ps -o time= -p $pid); sleep 10; b=$(ps -o time= -p $pid); echo "$a -> $b"
  ```
- **A BD-rate number isn't a bandwidth saving, and it scores only half of what
  an arm does.** BD-rate integrates the horizontal gap between two RD curves
  with UNIFORM weight over a quality interval. It weights every operating point
  equally, where real ladders ship specific rungs. And it converts the whole
  difference into RATE at matched quality, where a fixed ladder delivers a MIX
  of rate and quality. An arm can read +3.9% BD while spending fewer bits per
  rung and giving up VMAF. That's why `scripts/run_band.py` prints
  `rungN: dRate / dVMAF` beside every BD line: **quote both halves, and never
  translate a BD number into a bandwidth claim.** Read the rung columns only in
  a self-A/B. Against x264 the two CRF scales are different operating points,
  the rung columns show the scale mismatch, and the BD number is the one that
  normalizes it.
- **A speed number is about ONE OPERATING POINT.** On a single clip the ratio
  can run from 1.00x to 1.36x across four matched rates. Before attributing
  anything to a clip, check whether you're looking at its rate.
- **The corpus median and the TABLE aren't the same number.** A median carried
  by clips the table doesn't score can differ from the table by several points.
  Split by table membership before drawing a conclusion.
- **Before refusing an arm on wall, price the same arm on THEIR side.** A
  feature both encoders ship at medium may cost x264 a similar share; a number
  with no denominator hides an implementation gap.
- **A t1 speed number doesn't predict its t12 value, and the sign of the error
  isn't constant.** Work deletion on the wavefront transfers and pays more where
  the machine is throughput-bound; work already off the critical path pays only
  at t1; work relocation pays nowhere.
- **Hoisting work out of a loop can cost the SIMD tier what it buys the C
  one.** Measure a refactor on both tiers, always, and with an all-intra cell if
  it touches intra.
- **`/usr/bin/time -p` can't time a CIF cell.** Two decimals on a 0.05-0.11 s
  cell is a 9-14% quantum, and the worst-clip metric suffers most because MAX
  picks whichever row rounded up. Use `perf_counter` with repeated samples, and
  don't time side A five times and THEN side B five times.
- **This box is HETEROGENEOUS and the ideal speedup isn't the core count.**
  M5 Max: `hw.perflevel0.physicalcpu` = 6 "Super" plus `hw.perflevel1` = 12
  "Performance". The ideal 12 -> 18 speedup is about 1.43x. Check
  `sysctl hw.perflevel*` before calling a scaling number a defect.
- **Per-unit cost inflating with thread count isn't automatically your bug.**
  Both encoders pay it. The only way to assign it is N independent
  single-thread processes of the SAME binary, which share nothing, so the curve
  is that binary's own machine floor. Compare each encoder's threaded cost to
  its own floor.
- **`sample` on x264 fed a large y4m reads about half `__mmap`/`__munmap`**,
  because x264 maps its input file where we stream ours. Rescale off the mmap
  base before comparing coverage shares. Coverage profiles also need an encode
  long enough to sample; loop a short clip.
- **An env knob that overrides an AUTO default can override it DOWNWARD**, and
  then the arm is a regression the knob didn't cause (`Y264_LA_BUF=1` cuts
  frames off a default that resolves `la_buf = bframes+1`). Before pricing a
  knob, print what the default resolved it to.
- **Read EVERY line of a per-encoder stat dump, not the first.** The CLI opens
  a pool-less probe encoder that prints before the real one, so its
  `Y264_LA_STAT` line reads `pool=0 la_buf=0 la_th=0` on a default that runs
  with them on.
- **Two instruments in one dump that disagree by 15x is the finding.** Don't
  resolve it by picking the bigger name.
- **A full temp volume reads as a content failure**, for example conformance
  cells failing with "no geometry in the recon y4m header" on the biggest
  clips. `conformance.sh` and `ffboard.py` refuse to start below 20 GB free and
  name `make clean-scratch`. If a gate fails on the largest clips only, check
  `df` first.
- **SME/SME2 is closed on every form; don't reopen it.** It's reachable only
  with `-mcpu=apple-m4` or `native` (generic `-march=armv9-a+sme2` compiles and
  SIGILLs). 512-bit streaming vectors buy 1.08x over 128-bit NEON.
  `smstart` plus `smstop` costs 9.6 ns against a 3.7 ns satd8x8, so per-block
  use is impossible, and a whole-frame batched SATD through the ZA
  outer-product engine reads 0.21x NEON.
- **Any Metal-armed arm pays the 12-17 ms per-process bring-up floor per table
  cell.** Dispatch latency isn't the problem (per-frame lookahead pixel work at
  720p runs 0.24-0.26 ms end to end). What closes the route is the inter-round
  idle gap and the DVFS ramp: a hot-clock number misstates an anchor-cadence
  workload, and a sleeping warmer that reads flat once isn't evidence.
