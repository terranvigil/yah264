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
| `Y264_BPROF=1` | per-stage wall inside the B **and P** macroblock tournaments, binned by final verdict, with MBs-touched counts | t1 only. Prints `BPROF` (B) and `PPROF` (P) tables at exit. Default inert, verified md5-identical |
| `Y264_BPROF2=1` | sub-decomposes ONE B RD trial into encode / dist / bits and reports the tr-pre share | the tool that gave the RD-trial arm its corrected size |
| `Y264_MBT_SPLIT=1` | mb-tree by stage: invq / bind / phaseA / phaseB / finish, plus source and memo-miss counts | showed ref-B's cost is 95% phase A. That killed a stale recovery plan |
| `Y264_MBT_MISSWHY=1\|2` | **why a Phase A source had no pair legs to reuse**: splits MBT_SPLIT's `nobleg` bucket into anchor / untyped / null-leg / no-have. `=2` prints one line per miss with the source's POC, its ring offset, the settled bound, the STORED pair's anchor POCs and the pair the walk WANTED. Needs `Y264_MBT_SPLIT=1` beside it, since the bucket it splits belongs to that probe. `=2` is t1 only because the lines interleave on the pool | closed the stage-3 memo question in one read. All 72 nobleg sources on sunflower_1080p and all 48 on bbb10s_1080p_o120 are window-frontier leaves with **no future anchor bracket at all** (`want <poc>/INT_MIN`). The next anchor's finalize has not yet written their pair legs. It is not a keying defect: the key is two anchor POCs, with no thread id and no frame index. Counting only, default inert, verified md5-identical on/off |
| `-Dc_args=-DY264_STAGE_PROF` + `Y264_STAGE_PROF=1` | per-stage ms inside the macroblock loop and deblock | **t1 ONLY: it fails SILENTLY at higher widths**. A stage-prof build at `--threads 12` writes a zero-byte bitstream and exits 0. The same build is fine at t1. The normal build is byte-identical at t8/t12/t18, so the fault is in the probe, not in shipped code. Read a number off it only from a t1 run |
| `-Dc_args=-DY264_IPROF` (a separate release build dir, e.g. `meson setup build-ip --buildtype=release -Dc_args=-DY264_IPROF`) | **exact retired instructions by stage**: integer ME, sub-pel, RD trial, RD trial bits, commit re-encode, intra, entropy emit, deblock, lookahead, mb-tree, the rest of the macroblock decision, hpel + borders; printed as `IPROF` lines at exit | reads the thread's own instruction counter (`thread_selfcounts`, no root needed) at scope entry and exit. The innermost open scope owns the instructions. One read costs ~1,690 instructions with the kernel's share included, and that measured cost is taken back off. It counts per thread, so it works at any `--threads`. Output is unchanged, because the normal build compiles it out entirely. Use it instead of a time profile whenever the question is "how many instructions". A time sample charges memory stalls to whatever was stalling, even a sample weighted by the interval's instruction delta. On sunflower that inflated the per-trial psy term ~6x. Stage totals reach 97% of an unperturbed run. Frame-level bookkeeping makes up the rest. |
| `Y264_THREAD_PROF=1` | per-stage buckets across the whole encode | **profiler buckets have lied four times**; corroborate before acting |
| the same **plus `Y264_NTP_PROF=1`** | adds a **pool-idle** column per driver stage: how many of its milliseconds ran with no live pool job. | A bucket's wall cannot separate a stage that holds the machine idle from one that overlaps the wavefront. Reading it as if it could cost two rounds. It named the serial I-slice analyze in one read |
| `Y264_NTP_PROF=1` idle split | busy / midrow / gate / ramp / tail / nojob per worker | the sleep is split against the pool's empty clock. An older `tail` reading of 31.1% was inflated by the serial term. The real number is 6.6% |
| `Y264_GOP_TRACE=1` (CLI) | **the GOP-parallel schedule over time**: one stderr line per GOP (worker, frame-thread share `k`, pulled / start / end on the encode's clock, `tcpu` the worker thread's CPU seconds, `pcpu` / `pinstr` the process's CPU seconds and retired instructions over the GOP), plus the reader's EOF, the dispatch time and plan, and the process totals at exit | the only instrument that sees the CLI's outer layer. The pool profilers above live inside one GOP's encoder. On 2026-09-26 it found that a stdin encode reads its whole window, 2880 frames of 1080p, before the first GOP starts. It also found that, reading by path, a GOP could not start until earlier GOPs had consumed their way down to it. With `Y264_GOP_FORCE_G=1 Y264_GOP_FORCE_K=1` it prices every GOP at one frame thread in the threaded configuration: the input a schedule model needs. **Which cost column to read:** at one frame thread `tcpu` is the whole GOP because there is no pool. `pcpu` and `pinstr` are process-wide, so they belong to one GOP only with `Y264_GOP_FORCE_G=1`. That is also the only form that prices k > 1, since the pool's threads are not the worker's. Per-GOP CPU seconds on this box move with the core type a thread ran on, because the 12 slower cores run at ~0.75 of the 6 fast ones. So a per-GOP cost for a model is `pinstr` at `FORCE_G=1`, and CPU seconds are the cross-check. Measured with it on 2026-09-26: k=2 costs +2.5% instructions and +7-11% CPU-s over k=1 on 1080p GOPs. k=3 costs the same as k=2, so the step is the pool engaging, not a per-thread slope. 2160p GOPs cost 3.9-4.6x their 1080p twins with the same per-GOP shape. Scheduling-only, output unchanged |
| `Y264_LED=...` | op ledger: counts of real operations, not sampled time | use when a keyword-sampled profile is suspect |
| `scripts/instr-ratio.sh N` | instruction-count ratio against x264 beside the wall ratio | quotes the table's factor without timer noise. It reads two counters into one column pair: `/usr/bin/time -l` on macOS, `perf stat -x, -e instructions,cycles,task-clock` on Linux. **The Linux arm needs perf to be readable.** A stock kernel ships `kernel.perf_event_paranoid` at 2 or 4. The counter then comes back empty and the row prints `NO-COUNTER`, so an unreadable counter never shows up as a plausible zero. `sysctl kernel.perf_event_paranoid=1`, or a privileged container, is the fix. Do not read it under an emulator. QEMU's and Rosetta's instruction counts measure the EMULATOR's work, not the encoder's, so the x86 column is a rented-metal reading |
| `scripts/alloc_census.sh OUT -- CMD` (`tools/alloc_census/`) | **which call sites hold the memory and which churn it**: every allocation of 256 KiB or more, by its two innermost frames and size class, with count, peak GiB live and GiB allocated over the run | dyld interposing, macOS, no rebuild. One run found both 2160p memory items of 2026-09-27. The per-frame decision records took ~150 MB a frame and churned 426 GB over 3000 frames. The input store held ~15 GiB under hungry GOP workers. Peak "live" is virtual, since a worst-case slice buffer that is never written counts in full. Read it beside the peak footprint `/usr/bin/time -l` prints |
| `sample <pid> 15 1 -file F` + `scripts/sample_phases.py F` | **CPU by encoder stage at any thread count**, from a plain macOS `sample`. Blocked waits are dropped while a yield spin is kept. Every sample is walked from leaf to root onto stage rules | the threaded twin of BPROF. Kernel time is charged to the user frame that trapped. That is how the memory compressor's cost showed up by stage: copies, input read and the mb-tree AQ pass shrank 2-4x once the footprint fit. `--rules` loads another binary's rules, which are kept in local notes and not in the tree. `--bin NAME=PATH` gives an nm fallback for unsymbolized frames |
| `xctrace record --template 'CPU Counters' --attach <pid>` + `scripts/cc_split.py` | **useful / processing (memory+execution stall) / delivery / discarded issue-slot shares**, cycle-weighted | ATTACH, never `--launch`: launching hung on this box. Threads created after the attach are missed, so attach in steady state. 2160p t12: our processing share 43-45% against the reference's 32-34% under memory pressure, 35% once the footprint fit |
| **the memory-pressure check** | whether a CPI or kernel-time difference is the box's compressor: `/usr/bin/time -l` peak footprint vs max RSS, page reclaims and sys time, and `vm_stat` Decompressions deltas during the run | on the shared box anything past ~20-30 GiB resident gets compressed. 2160p at t12 ran 55 GiB peak, 29 GiB resident, 18-28M reclaims and a fifth of its CPU in the kernel. Check this before reading any 2160p CPI |
| `Y264_SIMD_FORCE=none\|sse4\|avx2\|avx512`, beside `Y264_ASM_OFF=<class>` | **the two SIMD ablation axes, and they answer different questions.** | `Y264_ASM_OFF` takes one dispatch CLASS to its C fallback and leaves the rest as shipped. Its wall delta is what that class's kernels currently buy. `Y264_SIMD_FORCE` takes the whole binary down to one ISA TIER by clearing the feature bits above it. One binary can then be asked what the SSE4.2 build would have done on an AVX2 box. That is the tier A/B without a second build directory, with `YAH264_NO_ASM=1` as the control column. Neither moves a byte of output because every kernel is bit-exact with its C reference. That makes both of them identity-cmp axes as well. `none` is the twin of `YAH264_NO_ASM`. The three x86 tier names are inert on aarch64: there is one tier there and nothing above it to cap. The BUILD-time twin is meson's `-Dsimd=`. It leaves the kernels out of the binary instead of routing past them |
| `Y264_ME_ETSTAT=1` | **what an integer-search early-out would delete and what it would cost**: post-seed integer probes bucketed by how good the best SEED already was (cost per pixel), with how often the hex/grid/diamond then moved the MV and by how far. | Read the MV columns, not the gain: integer-SATD-gain volume does not predict BD. t1, default inert, verified md5-exact |
| `Y264_B8_STAT=1` | engagement counters for the B_8x8 / B_RECT arm: B macroblocks reaching the tournament, quadrant searches run, RD trials run, trials that beat the running best, searches thrown away by the mid-tournament skip exit, rectangular searches admitted | t1 (plain globals). One read named all three of that arm's levers: search 100%, RD 69-78%, useful 4.5-11%, wasted 19-30%. `docs/b-8x8.md` |
| `Y264_LRSUB_PROBE=1` / `Y264_LRSUB_CENSUS=1` | direct wall of the lookahead's fifteen quarter-pel phase-plane builds, and per-phase read counts | 98.0 ms on samsung = 3.0% of wall. **All 15 phases read, 99.8% of rows touched**, so nothing in it can be deleted for free |
| `Y264_DIRECT_WHY=1` | **why a B slice fell back from temporal direct**: its list 0, the distinct co-located reference POCs (starred where they do not resolve), the block count behind each, and the per-macroblock direct-unavailable count. | It showed that the gate was frame-wide when the spec requirement is per-macroblock (`docs/b-direct-mode.md`, third round). t1 |
| `Y264_DIAG_DIRECTOK=n` | force B direct UNAVAILABLE on every nth macroblock, content-independently. | Isolates the `direct_ok == 0` path from whatever made it common. Under spatial direct it is bit-exact over three t18 runs. That is how the path was cleared as the source of the per-macroblock gate's nondeterminism |
| `Y264_DIAG_COLWATCH=1` | hash the co-located field at the FIRST and LAST macroblock of a B frame instead of at slice prep. | A prep-time hash proves nothing about a field read during the macroblock loop while the picture supplying it is still being encoded. This is what cleared the co-located field properly |
| `Y264_DIAG_TDIRLIM=n` | refuse temporal direct for a macroblock whose derived mvL0/mvL1 exceeds +/-n pel. | Tests whether an out-of-range derived vector is behind a result. It cleared the per-MB nondeterminism, which came from an uninitialized `dmv` and not an out-of-range one. The guard is still unbuilt |
| `Y264_DIAG_NOHPEL=1` | force `y264_me_mc_luma_s`'s non-hpel fallback everywhere. | Tests whether a result depends on WHICH of the two motion-compensation paths a block took. The fast path is taken only when a per-thread hpel registry allows it. It now covers every luma MC the analysis runs: the sub-pel probes, the RD candidates and the P_Skip prediction. Both paths are byte-identical, so it is an identity-cmp axis as well |
| `Y264_DIRECT_PERMB=0` | restores the old frame-wide temporal-direct gate, for A/B against the per-macroblock default. | That default has shipped ON at every thread count since 2026-09-01. **Every `--direct temporal` measurement taken before 2026-08-31 was of the frame-gated mode, which engaged on 11-24% of B frames**, so none of them is evidence about temporal direct. The `=0` path is byte-identical to the build before the per-macroblock gate, so it is also the control for the uninitialized-`dmv` fix |
| `Y264_DIRECT_LRVOTE=1\|2` | **per B frame, which direct derivation the LOOKAHEAD would pick**, simulated on the lowres field in mb-tree Phase B. `=1` scores each derivation by distance from the lowres search result, `=2` by the lowres SATD of its bi-prediction. | Prints `lrvote poc= col= past= T= S= n=`. **Dedupe by POC when aggregating**, since the mb-tree window overlaps and a frame prints several times. Verified md5-identical on/off. **CLOSED NEGATIVE 2026-09-01**: mode 2 separates the 16 labeled clips at rho -0.76, then goes 0 for 3 on new predictions with rho +0.000 within one instrument. Keep it as the measured baseline and do not re-derive it. `docs/b-direct-mode.md` |
| `Y264_DIRECT_SCORE=1` | **per B frame, the direct prediction's SSD against the source and the macroblock count.** Run the same encode once per `--direct` mode and diff the rows. | That answers whether a per-FRAME direct-mode choice could beat the per-clip one. t1, verified md5-identical on/off. **Its own validation says the signal is WEAK**: coastguard predicts better under temporal yet codes worse, and on samsung it is the reverse. Read it as a bound on the cheap proxy, not as a selector. `docs/b-direct-mode.md` |
| `Y264_BSKIP_SCREEN=2` | **does the exact B skip screen hold, and how much probe work does it remove**: re-runs the full strict probe and every proven block beside the screened one and aborts on any disagreement. | It prints probes, accept rate, luma 4x4 blocks and chroma planes proven, and 4x4 transforms before and after. Any thread count, output identical to `=0`/`=1`. 2026-09-26: 75-86% of the probe's transforms removed on sunflower/pedestrian/bbb_2160p/meridian_2160p, with zero violations over QP 4-51, 4:2:0/4:2:2/4:4:4 and 10-bit |
| `Y264_EST_TWICE=1` | **what the RD-trial rate estimator costs, with no decision change mixed in**: every trial estimate runs twice and the first is kept, so the bitstream is byte-identical. The instruction delta against a plain run is exactly one set of trials under whichever pricing is armed | priced the frozen-trial arm (`Y264_EST_FROZEN`). The estimator is 1.9-5.3% of t1 instructions under adaptive pricing. Frozen pricing lowers that by 9-60% per cell. Read instructions from `/usr/bin/time -l`, not wall. Verified md5-identical on/off |
| `Y264_EST_SCRTRACE=1` | per-est-trial `(site, frame, mb, dist, j, lb_j)` trace for pricing ADMISSIBLE-SCREEN ideas offline (replay each MB tournament in call order against a running best) | priced the shape-3 screen family with no plumbing. Dist-only fires 4.5%. The sign-count bits bound reaches 6.5-7.5% but is INADMISSIBLE, with 6% violations, because stored candidate nnz overcounts what est codes. The losers-at-call-time ceiling is 33.8%. t1 only, output verified md5-identical on/off |

## 2. What would a perfect version of X buy? (bounds and oracles)

These replay a recorded ideal so an arm gets a ceiling **before** it gets a
threshold. Every one must pass its own byte-identity check.

| instrument | bounds | notes |
|---|---|---|
| `scripts/rc_invariants.py` | **the rate controller's invariants in seconds, through the replay harness**: the A6 set (--bitrate with a 1.0x CBR and a 1.5x cap, 6 clips) and the cvbr_compliance set (CRF with 0.8/0.6/0.4x caps), 36 cases at one thread over 6 s. Each case is recorded once with `REF` (cached under `$Y264_CACHE/rcreplay`, keyed on the binary md5 and the command) and replayed with `CAND`/`CAND_ENV`. For a restructuring step the identity column must read EXACT. The invariant columns (0 leaky-bucket underflows, A6 rate error inside [-5%, +3.4%]) are read off the replayed sizes | The first run records (minutes) and later runs replay (seconds). The VBV column is a proxy. vbv_check.py and hrd_check.py on real encodes stay the stream gates. `INFORM=1` reports invariant failures without failing |
| `Y264_RC_REC=<f>` / `Y264_RC_PLAY=<f>` + `scripts/rc_replay.py` | **the rate controller alone, in a fraction of the encode's time.** A `--threads 1` encode with REC writes, in consumption order, every pixel-derived value the controller reads (lookahead complexity sums, full-resolution complexity probes, the AQ frame-DC term, cut and anchor flags, the opening fit's median, the mb-tree mean offset) along with each slice's QP and RBSP size. PLAY runs the same command with lookahead, mb-tree, macroblock analysis, entropy coding and reference planes switched off and feeds the values back. **Replaying with the recording binary must reproduce every slice QP (EXACT)**: `rc_replay.py` checks that over 18 rate-control configurations and fails otherwise. With `PLAY_BIN`/`PLAY_ENV` it replays another controller over the same inputs. When a slice's QP moves, its size is scaled by the 1/qscale law (`Y264_RC_PLAY_K` scales the exponent) and counted as modeled, so those bits are estimates. A leaky-bucket VBV estimate prints for both sides. The Annex C gate stays `hrd_check.py` on a real encode | One thread only, because the consumption order is fixed there. A new pixel-derived input to the controller must be routed through `RCR_VAL`. Otherwise the replay reads `order differs` at it, which is the harness telling you so |
| `tests/rc_selftest.py` (meson test `rc_replay`) | **rate-control coverage in `make test`, no corpus**: writes a 48-frame synthetic QCIF clip with a cut and runs `rc_replay.py`'s 18-mode self-test on it. Every mode's decide and account path runs and must replay EXACT | ~3 s. It proves the controller is a function of its recorded inputs. Whether it controls well is for rc_invariants.py and the stream gates to show |
| `Y264_VBV_ROWBRAKE_TRACE=1` | **how good is pass 1's entropy estimate, row by row**: one `RBT` line per picture with the pass-1 CABAC estimate and the real coded bits of every MB row (I, P and B, serial and wavefront). It's the input of the planned row-level VBV brake | 10-04 read on 10 cells (6 clips x CRF/VBV x t1/t4, 1045 pictures): frame median 2.2% off, p90 6.4%. Cumulative at 25% of the rows: median 2.6%, p90 7.8%. Worst clip sintel (~6% median). CABAC only. CAVLC P/B rows print 0 |
| `Y264_MBT_REC=<f>` / `Y264_MBT_PLAY=<f>` | mb-tree offsets: record every anchor's field **and the reference B's** (the walk has two outputs), replay both. Keyed on (POC, FNV-64 of the anchor's lowres), CRF-portable. **Replaying your OWN recording must be md5-identical.** That gate has caught three harness bugs. The third was this probe going stale against a ship on the day of the ship. Recordings carry a format magic, so a stale one says so |
| `Y264_SKIP_ORACLE`, `..._AT`, `..._SIDE` | what a perfect early-skip predictor would buy. The key is the ABSOLUTE display index. POC restarts per IDR and silently unions GOPs, which inflated a bound 1.98x -> 1.39x |
| `Y264_TYPE_ORACLE` | replay x264's frame-type placement, read from its bitstream, through us. Their placement costs us +1.64% wall |
| `Y264_HEX_ORACLE` | ME search-shape bound |
| `Y264_MEREUSE_TRACE` | census of discarded-MV class (a): ref (r-1)'s winning 16x16 list-0 MV, scaled by temporal distance, as a seed for ref r's search. Read 2026-09-16 at medium, 60 frames, one thread. The scaled seed beats the median predictor the search starts from on 57-61% of ref>=1 searches (foreman_cif / park_joy_720p / crowd_run_1080p, mean full-pel SAD gain ~2200-2600 when it does). It beats or ties the full search's own winner on 11-15% (mean gain 220-520). That's no wash: it's a speed lead (better start point) and a small quality lead (search misses). Output-neutral, default inert, t1-only. Shipping the seed is an open item |
| `Y264_UNSAFE_NO_EMIT` / `_NO_NAL` / `_NO_MBT` / `_NO_REFBWAIT` / `_NO_PREVPWAIT` | delete probes: what disappears if a whole stage does. **Gate these on output identity, never on an argument about consumers.** An unsafe delete once read FASTER than a byte-identical replay of strictly less work. Deleting the est-CABAC-context copies read a reproducible 5.0% of samsung's wall, where the byte-identical refactor collected 0.3-0.7%. The probe changed what the RD estimator saw, so the encoder made different decisions and did a different amount of work. A delete probe whose output moves is measuring a different encoder, not the thing deleted |

## 3. Is the arm any good? (quality gates)

| instrument | use |
|---|---|
| `ARM='<env>' python3 scripts/run_band.py` | **the standard self-A/B gate.** It runs band-calibrated ABR and CRF ladders and reports per clip, with no single summary mean. Both sides run the same ladder, so rates match by construction. `BANDS=crf` runs only the band the standing rule gates on, for half the wall. `CLIPS=akiyo_cif,park_joy_720p` runs a subset, which is what an ABR gate aimed at two named clips needs. **Scores VMAF-NEG alone by default** (`VMAF_MODELS=neg`). `both` restores the plain model. Under `neg` the per-rung dVMAF columns are NEG deltas. **The CRF band of a self-A/B stops early** once one clip reads worse than +1.0% or three read worse than +0.5%. `EARLY_STOP=0` reads all. The early stop never applies to the ABR band or to a run against the reference encoder |
| `scripts/bdcompare.py --vmaf --no-cache` | the underlying BD tool. **`--no-cache` is mandatory**: the tool caches ENCODES by command string, so a rebuild silently returns stale numbers. `--models neg` scores VMAF-NEG alone, as the bands do by default. On sunflower_1080p at 120 frames that costs about 8.5 CPU-s against 14.9 for both models, beside a 7.3 CPU-s encode. The default here is `both`, which the spec report and publication passes keep. NEG's value is identical in either mode. **`--subsample` refuses a stride that shares a factor with the mini-GOP period** (bframes + 1, read from each template, 3 if absent). Stride 4 at bframes 3 scores only anchor frames. It moved sunflower's NEG from 92.62 to 94.62 and flipped a band arm's sign. Coprime strides avoid the bias but add +/-0.15-0.2% per clip, so self-A/B bands stay at 1 |
| **the VMAF score cache** (`bdcompare.vmaf_of`, every band) | **an output-identical encode never pays libvmaf twice.** Every score is stored under `$Y264_CACHE/vmafscore`, one file per key. The key is the BITSTREAM's md5 plus the source clip's identity (resolved path, size and mtime, where a frame-cut copy carries its parent's identity plus the frame count), the subsample, the decode pixel format and any v1 model path. The binary and the command aren't part of it, so a lossless rebuild, an inert knob or a repeated anchor hits the cache. A two-model entry can serve a NEG-only request, but never the reverse. Hit and computed counts print at the end of a band. `VMAF_SCORE_CACHE=0` bypasses it |
| `scripts/bd_at_rate.py` | BD at MATCHED ACHIEVED bitrate. Required for anything that moves the CRF axis, because mb-tree on/off translates it |
| `scripts/bdcompare.py` with the CACHE ON, read for its SIZE lines | **does `--crf N` mean here what it means there?** Run both encoders at the same CRF numbers and divide the bytes. The x264 CRF that matches our bytes, log-interpolated across its own rungs, turns the ratio into CRF points. That's the unit the question is asked in. It does no timing, so it is load-immune and runs beside anything. Read exact bytes out of the encode cache rather than off the printed `NNNk`, because a small cell rounds to 3-5%, which is half a CRF point. The 2026-09-16 answer and the two arms that make it worse are in `docs/rate-control.md`. **The traps are that the offset is content, not scale**: two 48-frame segments of ONE 4K source differ by 0.6-0.7 of a point. The other trap is that an easy clip's first frames can be a black leader. A black leader codes to nothing at every CRF and prints a flat ladder |
| `ARM='<env>' python3 scripts/band_at_rate.py` | **the whole CRF band at matched achieved bitrate**. It takes `run_band.py`'s ladders and `ARM` / `ARM_ARGS` convention and drives `bd_at_rate.py` per clip, several clips at a time (`JOBS`), ending with a median/mean/worst summary. This is the gate for any arm that moves the operating point. On such an arm `run_band.py BANDS=crf` would measure ladder placement instead. NEG-only scoring and the early stop work as in `run_band.py` (`VMAF_MODELS`, `EARLY_STOP`). Once the arm is refused, clips not yet started are dropped |
| `BANDS=deep ARM='<env>' python3 scripts/band_at_rate.py` | **the DEEP band (VMAF-NEG 55-83) at matched achieved bitrate**. It covers the regime the standing band cannot see, where the regime-shaped arms live. Ladders are solved onto x264's own deep curve by `scripts/calibrate_deep.py --write` (`ladders_deep.json`). 10 of 12 clips span it. Two clips, ducks and park_joy, don't span it and are dropped. `BANDS=all` concatenates both ladders. The default `band` keeps every historical number comparable. Rungs print a model-derived `qp~` so regime knobs read in QP space. **The deep band's noise floor is MEASURED at ~+/-1.2 per clip**: a near-inert AC-gain 1.734 perturbation reads foreman -1.20, bus -0.91, akiyo +0.60. A deep read inside +/-1.2 is the band talking, not the arm |
| `ARMS='a=<env>;b=<env>' python3 scripts/hd_band.py` | **the HD band: BD-VMAF-NEG on twelve 720p and 1080p clips, several arms in ONE run.** The standing ladder has seven CIF clips against five HD ones. The clips engineers test with are the HD ones, at the rates they ship at. docs/hd-parity-plan.md makes this band the bar that every stage-2 candidate is read against. It runs five rungs, CRF 23 to 35, with the DEFAULT side as the anchor. One anchor curve serves every arm in the run. **Anchor and arm run at the SAME five CRF values, and the BD is integrated over the range the two curves share**. That is what BD-rate is defined to do. It does NOT solve the arm onto the anchor's bytes any more. That solve could not be read below `medium`. The frame QP is an integer, so a clip's CRF-to-bytes curve is a staircase. A rung on a step edge has no CRF on either curve that produces the target bytes. At `fast`, 89 of 180 solves missed by over 2%, and the band then read the miss as quality (`local/records/band-interp-2026-09-20.md`). **The guard is the rate overlap.** Every cell prints its rate overlap, its quality overlap and the rungs that survived. A clip under `MIN_RUNGS` (4 of 5) is NOT SCORED. 4 of 5 is the ordinary reading because two curves that don't coincide lose one endpoint at each end. 3 is a whole rung of dislocation. **A clip the guard refuses is re-read on rungs moved to the middle of their steps**. This is fix A, and those rows print `[A]`. The anchor's ladder is probed once per clip and preset at two encodes a rung. It is cached in `$Y264_CACHE/hdband/ladder.json` keyed by the binary's sha, as ffboard's solve cache is. A moved rung narrows the staircase offset without removing it, so the record says which clips were read that way. **The null is STORED, not run** (since 2026-10-03). `scripts/hd_null.py` fits each clip's null spread from the control reads already in the point cache (below). Each arm prints its reading referenced to the stored center, marked `inside null` where it lies within `NULL_SD` (2) robust sd of it. The referenced refusal subtracts the most favorable plausible null read: the center plus that half-width. It is the stored analog of the old rule, which took the more favorable of two live controls. Re-run on the 09-29 band, it keeps every verdict. Subtracting the bare center instead had turned one PASS (shields +0.40 raw) into a refusal. Some clips run ONE live control (`LIVE_CONTROL`, default `Y264_MBT_DEPFLOOR=16`) and are referenced to that read. That happens when a clip has no fit, when its anchor bytes differ from the fit's reference binary because the defaults changed since the fit, or when it is read under fix A. Where the null's read-to-read spread (sqrt 2 x the robust sd) exceeds the bar, the band prints `UNRESOLVED` instead of `PASS`. A bar under the instrument's own spread is not a reading. `CONTROLS='ctl=Y264_MBT_DEPFLOOR=16;ctl2=Y264_MBT_DEPFLOOR=8'` restores the old two live controls on every clip. `CONTROLS=none` drops every referenced column. **Scores VMAF-NEG alone** (`VMAF_MODELS=both` restores the two-model pass). Every score goes through the score cache. **Early stop**: an arm is refused and its still-queued cells skipped once one clip reads worse than `EARLY_ONE` (+1.0%) or `EARLY_N` (3) clips read worse than `EARLY_SOME` (+0.5%). Both triggers already fail the per-clip bar, so the stop cannot change a verdict. Replayed in band order over the 37 arms in the point cache with 11+ clips on the nominal rungs, it skips 14% of their clips and changes no verdict. `EARLY_STOP=0` reads every clip, which a report wants. **An arm can be another binary**: `ARMS='int=@/path/to/yah264'` (optionally followed by env) reads a whole build against `YAH264` as the anchor. Its cells are keyed on that binary's sha, so they share the cache with a band run on it. Every (clip, arm, rung) cell is cached in `$Y264_CACHE/hdband/points.json` under the same key shape. Hit and miss counts print per clip and per run, so a band that reused a table can't be mistaken for one that measured it. `NO_POINT_CACHE=1` bypasses the cache. `SOLVE=1` restores the matched-byte ladder and its `UNBRACKETED` class, unchanged, for the pages that document it. `CLIPS`, `RUNGS`, `FRAMES`, `JOBS`, `MIN_RUNGS`, `FIXA`, `PROBE_D`, `BAR`, `MEDIAN_BAR`, `CONTROLS` override. `WINDOW` and `ITERS` belong to `SOLVE=1`. Cost is 45 encodes and 45 VMAF scores a clip at eight arms, against 245 encodes under the solve. It is load-immune, so it runs beside other work inside the core cap |
| `python3 scripts/hd_null.py` (`--show` prints the stored fit) | **the HD band's per-clip null spread, fitted from the point cache instead of run in every band.** For every binary that has both a default curve and a `Y264_MBT_DEPFLOOR=16`/`=8` curve on a clip, it takes the control's BD-VMAF-NEG the way the band scores an arm. It drops reads repeated by output-identical binaries, since a lossless series of builds would otherwise pool one measurement ten times. Per clip it stores the median and the robust sd (1.4826 x MAD) beside the plain mean and sd. With them go the date, the binaries pooled and a reference binary (`YAH264` or `--ref`). The band compares anchor bytes against that reference to detect changed defaults. The fit is written to `$Y264_CACHE/hdband/null.json`, per preset, thread count, frame count and ladder. **The first fit (2026-10-03) found 5-6 DISTINCT reads per band clip, not the 20+ the raw cache suggests**: the rest were output-identical repeats. The reference binary's default curve comes from the point cache when a band has run on it. Otherwise the fit encodes it itself (bytes only, five encodes a clip). `--no-encode` skips that. Refit with `YAH264=<new build>` after a change to the defaults |
| `python3 scripts/m4_label.py --src-dir <dir> --out <jsonl>` | **per-sequence arm labels + lookahead features on an EXTERNAL corpus**, one JSONL row per sequence. Each row holds BD-NEG for each M4 arm against the shipped default at matched achieved rate. The baseline's CRF points set the byte targets and each arm is binary-searched onto them. The row also holds the four input-derived frame features (`tdiff`, `flat`, `tex`, `motion`) dumped by `Y264_PSY_FLAT_LOG` / `Y264_ADME_LOG`. When no arm beats the default by 0.30%, the row gets a third class, `default`. The script takes a `--src-dir` and has no path into `tests/corpus`, so the train/test split is structural: **the 12-clip gate corpus is TEST-ONLY, permanently.** Resumable (rows already in `--out` are skipped). ~7 s per 64-frame 480x272 sequence. Method: `docs/m4-selector-method.md` |
| `scripts/shot_determinism.sh [input.y4m]` | **the per-shot re-encode contract**. It encodes with GOP boundaries on the cuts and writes every GOP to its own segment. Then it re-encodes each GOP's frames alone with the same pinned frame threads and compares bytes. An orchestrator's convex-hull probing and partial re-encode stood on this promise (docs/engine-interface.md). Since 2026-09-25 it reports a mismatch as WARN and exits 0 (`STRICT=1` to fail). K and CRF come from the environment |
| `Y264_QP_TRACE=1` + `scripts/multishot_bd.py` | **where the bits go across SHOTS**. The trace prints every frame's slice QP, mean coded QP and mean intended QP (slice + offset map), along with the mb-tree mean offset and the CRF DC term. On a skip-heavy shot the coded mean hides the allocation. `multishot_bd.py` encodes the S0 multi-shot sequences (`local/corpus/ms_*.y4m` + `.cuts`) at CRF 22-34 for any set of arms (our env/flags, x264). It prints BD-VMAF-NEG per pair plus per-shot VMAF and bit shares. The ten-clip board is single-shot by construction and cannot see an across-shot allocation gap. The 2026-09-05 record shows a 6-14% per-clip lead turning into a 5-13% deficit on the same clips concatenated |
| `scripts/calibrate_band.py --write`, `ladders.json`, `curves.json` | the band ladders themselves and the x264 curve they were solved onto. **A bare run writes nothing into the tree.** Without `--write` the two json files land in a fresh scratch directory whose path the run prints. Every run prints both destinations, each marked `OVERWRITE` or `create`, before the first encode starts. `--dry-run` prints that plan and stops, which is how you check a command without spending the hours. `--clips`, `--crfs`, `--frames` and `--jobs` cut a full sweep down to a cell or two, for the same reason. `--out-dir` names a destination of your own |
| `scripts/calibrate_deep.py --write`, `ladders_deep.json`, `curves_deep.json` | the same, for the deep band (VMAF-NEG 55-83) over CRF 30-50. Same flags, same rule: the tree's ladders move only under `--write` |
| **the ladder shift**: rerun the same ABR arm at ladders +/-13% in rate | **the cheapest disproof of an ABR refusal, and it needs no extra tooling.** A real effect keeps its sign. The band's own chaos does not. `Y264_B_SKIP_EXIT=3` on akiyo read +0.96%, +0.25% and -5.75% across three ladders. That turned a would-be kill into a non-result. The ABR band's per-clip noise floor runs from 0.2 to 11 points, so measure it before recording any ABR refusal. Measure it by perturbing the CURRENT default rather than an arbitrary 1.02. The AC gain default is 1.7, so 1.02 is a 40% field change that measures the arm instead of the floor. Use 1.734 / 1.785. Corroborate with the **CRF low band**, points 32-41. It reaches the same QP regime with no rate-controller feedback loop, and akiyo read +0.00% there |
| `scripts/hf_probe.py` | **where the texture goes**. It runs both encoders on the minimal CQP reproducer and reports per-band DCT energy retention (recon/src, LF/MF/HF zigzag rings) beside bits, PSNR and VMAF-NEG. Results split by the decoder's per-MB class and a source motion class. This pinned the "buys MSE vs buys texture" mechanism to the DEEP-QUANT regime (QP ~42-48). `HF_VMAF=1 ARMS="next-vs\|x264-med" python3 scripts/hf_probe.py bus_cif 100 30,36,42,48` |
| `scripts/hf_join.py` | the attribution half: per-MB 3x3 skip/inter/intra contingency of our class against x264's, with each side's MSE and AC retention per cell. The skip/skip cell is the reference-chain control. Neither side coded it, so a gap there is inherited reference quality (foreman QP48: 142.8 vs 129.6 MSE on 64.9% of MBs) |
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
several arms in ONE batch. A knee measured one invocation at a time once came
back non-monotonic in the gate's own strength, while the same arms in one batch
ordered correctly. Price every threaded arm at `THREADS=12` and state the tier,
because a pure-C reading and an as-shipped reading answer different questions.

| instrument | use |
|---|---|
| `scripts/perf-comp.sh`, `-set`, `-crf-set`, `-purec`, `-modes` | the comparison sets against x264. `-purec` has the autovec-fairness fix. `PERF_REPEAT` knobs drive the repeated-sample timer that resolves the short CIF cells |
| `Y264_REFENC_CACHE=0 make parity-status` / `parity-status-crf` | the ten-clip board's tables, from before parity. The published parity tables come from the long-form board (`scripts/longform_throughput.py`) and its report. CRF is the one to quote for speed. Timing is `perf_counter` with the two arms interleaved. The same binary run twice reads within 0.01 |
| `scripts/parity-clips.sh`, `scripts/parity-clip-calib.sh` | the table's clip set and its per-clip calibration |
| `scripts/psnr_leg.py` | **the PSNR-Y floor**. Every board's `dPSNR-Y` column routes through this one adjudicator. Fed `clip=dB` pairs, it prints the median, the worst clip, `PSNR-FLOOR PASS/FAIL` at more than 1.0 dB below x264 on ANY clip, and the `PSNR-DEBT` list at 0.5 dB. It judges per clip because a median floor isn't a floor: half the corpus can sit under it and the row still passes. The number costs nothing. `--feature psnr` rides on the libvmaf pass that already scores dVMAF, and the pooled VMAF mean is bit-identical with the feature on. **Read the column on the CRF board.** The leg is "at equal bytes", and only the matched-point solve makes that true. ABR asks for the same rate and delivers +10% on foreman_cif |
| `scripts/ffboard.py` | **the control for "is the table scoring our Y4M reader?"** Both encoders are called as libraries inside ONE ffmpeg process: one demuxer, one thread pool, no CLI on either side. It answers the input-path question and doesn't replace the table. Run against `parity-status-crf` at the same points, the two agree within noise on every clip, so the input path isn't where the gap lives. It needs an ffmpeg built with libyah264 at `../build/ffboard/ffmpeg-yah264/ffmpeg`. docs/ffmpeg-integration-plan.md rebuilds it. It lived under `/tmp` until macOS swept that away on 2026-09-17. **Point `X264LIB` at a libx264 whose `-fno-tree-vectorize` was stripped.** Otherwise the pure-C arm reads our vectorized C against their scalar C, which is worth a third of goal 2 |
| `scripts/longform_throughput.py` + `scripts/lf_feed.py` | **the long-form board: whole titles (10+ min) and the staged segments, both CLIs as shipped, both reading stdin from one feeder.** Never use a file path in a timed run. On 2026-09-24 the reference CLI reading a large Y4M by path was input-capped (600 frames meridian 1080p: 3.9 s by file, 1.56 s piped), which voided every earlier by-path long-form ratio. VideoToolbox decodes full titles on the fly (+ scale_vt below native). The timed feeder runs three decoders on chunks because one delivers only ~230 fps of a 4K master. Every chunk decodes 5 s early and proves its first/last frame from ffmpeg's per-frame timestamps. That proof is needed because these masters are nearly IDR-free and a seek to a recovery-point keyframe drops frames. `verify-feed` checks chunked against sequential per frame. `plan` is the dry run of the timed legs (`timed` G3 + SIMD t1, `purec` G1/G2 on one 60 s window per 3 min, `validate`) with estimated walls. Pure-C reference: `~/src/build/x264cli-purec-b35605a/x264` (same commit as the SIMD CLI, asm off, vectorizer on). **`point` reuses solves across builds** (2026-10-03). The reference point is carried over from any earlier build of ours with the same reference binary and subsample (its direct twin if it had been moved onto ours). Ours is encoded once at the newest earlier solve's rate factor. If the md5 equals the stored one, the solve is recorded for the new build with no more encodes (`reused`). Anything else starts the secant from that rate factor and slope, with that encode as its first step (`warm`). Reuse needs the same `SOLVE_THREADS` as the earlier solve, since threaded output can differ by thread count. A solve at another count still warm-starts. Every new record stores the bitstream md5, the slope and the encode count. `LF_NO_REUSE=1` solves from scratch (`cold`) |
| `scripts/ffboard.py` solve cache | **keyed by the three binaries, not by the clip alone.** `$Y264_CACHE/ffboard/solve.json` (default `~/.cache/yah264/ffboard`) holds the CRF and ABR solves and survives between boards and between weeks. **It moved out of `$TMPDIR` on 2026-09-19.** There, a sweep or a disk-full clear-out would have taken a record that is expensive to rebuild. The board's per-run bitstreams and y4m stay in scratch and are removed when it exits. Until 2026-09-18 the key was clip, frames, target, preset, threads and tolerance, with nothing in it about which libraries produced the answer. A cache from the libnext264 era was reused against a yah264 whose CRF scale had moved since. The board came out at dsize -12% on the median and -24 to -37% on five clips. Every wall ratio on it was meaningless. The key now carries a short sha of each dylib the ffmpeg will actually load (leaf name from `otool -L`, symlinks resolved, because a prefix is rebuilt in place and a path is not an identity). It also carries each library's pkg-config `Version` and a sha of the ffmpeg binary. **Every solve prints `solve cache: hit`/`miss` with that key**, because a board that reused a whole table used to look exactly like a board that solved one. `--no-solve-cache`, or `Y264_NO_SOLVE_CACHE=1`, neither reads nor writes it |
| `scripts/ffboard.py` size-match guard | **the board refuses to print a median it can't stand behind.** Every `RC=crf` and `RC=abrm` row is rate-matched by construction. An unmatched dsize therefore means the two encoders were timed doing different amounts of work. The ratio is then partly the bit difference. After the table, a dsize median outside +/-2% or any one clip outside +/-5% prints `INVALID: sizes unmatched`, names the worst clip, and exits 2 without adjudicating the PSNR leg. There are two bars because they catch different failures. The median catches a scale that has moved under the whole table. The per-clip bar catches the single solve that didn't converge. `RC=abr` is exempt and says so on the line. It hands both encoders the same target without delivering the same bits. Its `n rate` / `x rate` columns are there for that. `Y264_DSIZE_TOL` and `Y264_DSIZE_TOL_MAX` move the bars. Setting the first also turns the guard on at `RC=abr` |
| `scripts/ffboard.py` header check | **the ffmpeg wrapper is ABI-bound to the `include/yah264.h` it was compiled against, and the board is where that gets noticed.** The build writes `git hash-object include/yah264.h` to `yah264-header.sha` beside the binary. The board hashes `$Y264LIB/include/yah264.h` and refuses a mismatch, naming both blobs and both paths. It exists because on 2026-09-19 a wrapper one field behind the library read `yah264_param_t` at the wrong offsets, opened, encoded, and solved park_joy to CRF 42.7 at 55 Mbps. The library's own guard is the other half (ABI 3: the param struct has its own `size` and `yah264_encoder_open` refuses a foreign one). This check needs no cooperation from the library at all. `Y264_HEADER_CHECK=0` bypasses it, for a run that is reproducing the failure on purpose. **`Y264LIB` is less authoritative than it looks.** `DYLD_LIBRARY_PATH` only supplies the leaf name the binary asks dyld for. An install whose soname differs isn't loaded at all. The build-time absolute path is silently used instead |
| `./build/tools/checkasm/checkasm --bench` (the `sad_quad` / `satd_quad` rows) | **whether BATCHING a kernel pays, before anything is wired to it.** Each row is the batched form against four dispatched singles. It prices the whole fuse-the-calls family in one command. SAD reads 1.60-1.80x and SATD reads **1.01x**, so the answer is load-boundedness. Read only the sizes where BOTH forms are NEON. 8x4 mixes a NEON batch against a C single, and 4x4/4x8 have neither |
| `sample` profiles of both shipped binaries | **the SIMD share of each encoder's time**, as whole-binary percentages: the reference spends about half its non-I/O time in vector code. Rank our own kernels in milliseconds per job, multiplying each side's percentage by its own wall first, since percentages aren't comparable across two binaries. Ranked that way we LEAD on subpel refine (0.62x), SAD (0.69x) and chroma MC (0.26x) and lose 9.3x on deblock |
| `Y264_GPU_PHASEA=1` + `Y264_GPQ_WARMUP=10000` / `Y264_GPQ_CONSUME=0` | **the per-process Metal floor and the gpq tax split.** `WARMUP=10000` arms the per-push phase-A offload but never submits a job, so only the background `ngc_open` runs. It reads bus +16 ms / stefan +17 ms of wall. That is the 12-17 ms bring-up floor any Metal-armed arm pays PER TABLE CELL. The floor is device init, not shaders. A precompiled metallib doesn't move it. `CONSUME=0` submits rounds but never reads them, which splits chain-side from walk-side cost |
| `python3 scripts/knob_census.py` / `--check` | **the `Y264_*` knob census** (`docs/knobs.md`, generated): every env knob's reader, default and tier (shipped default / instrument / arm). `--check` runs inside `make test`. It fails on an uncatalogued knob, a catalog entry whose reader was removed, or a comment claiming "default OFF/ON" that contradicts the code default. It reads getenv, the `*_tunable` helpers and the thread pool's spin-site macro. The census now reads 405 knobs = 211 defaults + 104 instruments + 90 arms. Regenerate instead of trusting this line, since docs/knobs.md is the count |
| `KEEP_SCRATCH=1` | **keep a harness's working directory instead of removing it, and print where it is.** Every harness here builds its scratch through `scripts/scratch.sh` or `scripts/scratch.py`. Those remove it on exit, on Ctrl-C and on a TERM from a killed agent. Set this when a cell failed and the bitstream or the recon is the evidence. By design it's the ONLY way any of these directories survives its run |
| `make clean-scratch` | **the sweep for scratch no trap could remove**, after a SIGKILL, a panic or a power cut. It matches only the prefixes in `scripts/scratch-prefixes.txt`, only directories, only under `$TMPDIR`, and only ones older than a day. It prints each path with its size. `DRY=1` lists without removing. `DAYS=7` widens the age. Caches that are MEANT to outlive a run aren't under `$TMPDIR` at all. `ffboard.py`'s `solve.json`, `bdcompare.py --cache` and the openh264 raw planes live in `$Y264_CACHE` (default `~/.cache/yah264`), which nothing sweeps |

## 5. Is it correct? (the gates every ship passes)

```
make test                                   # unit, 13/13 (checkasm is one of them)
./build/tools/checkasm/checkasm             # every kernel against its C reference, guards on
./build/tools/checkasm/checkasm --list      # the table: group, family, the ISA each row needs
./build/tools/checkasm/checkasm --isa neon pixel    # one tier, one family
make conformance                            # recon-match, 254/254 (YAH264_CONF_FAST)
python3 scripts/stress_threads.py           # hang/determinism under load
scripts/tsan_catch.sh                       # TSan; the floor is 0 reports
python3 scripts/env_gate_audit.py           # every lazy env static: TRAP + COLD, exits 1 on TRAP
ARM='<env>' scripts/recon_sweep.sh          # recon-match ONE ARM across a config matrix
scripts/determ_repeat.sh                    # SAME config, N times -> how many bitstreams (diagnostic; STRICT=1 fails)
scripts/identity_ab.sh OLD NEW             # two binaries, ~100 configs, md5 per cell -> PASS/DIFF table
ARGS='--direct temporal' scripts/determ_repeat.sh   # ...and it takes MODE flags too
scripts/hygiene_check.sh                 # licences, stray patches, home paths, stray asm
scripts/abr_decode_gate.sh                  # decoder-side gate for the THREADED ABR path
scripts/x86-docker.sh                       # the whole battery as linux/amd64, under Rosetta
scripts/x86-docker.sh --backend qemu        # ...and again under QEMU, an item's final gate
YAH264_CONF_DECODERS="ffmpeg openh264 jm" make conformance   # three oracles, not one
python3 scripts/hrd_check.py STREAM.264     # Annex C CPB, read out of the stream itself
python3 scripts/level_check.py STREAM.264   # Annex A Table A-1, against the declared level
python3 scripts/hrd_check.py --self-test    # 8 cases, two of them negative controls
python3 scripts/level_check.py --self-test  # 7 cases, one of them a negative control
```

| instrument | answers | notes |
|---|---|---|
| `tools/checkasm` | does every optimized kernel still produce exactly what its C reference produces, and does it stay inside the memory it declares | **49 groups, and what changed on 2026-09-17 is what a green run MEANS.** Each group compares the C reference against the kernel SYMBOL. It used to compare the two through the public entry point with `Y264_ASM_OFF` flipped in between, a harness an environment variable could turn into a C-against-C tautology. That shape also left untested every kernel the dispatcher does not select. On a FEAT_DotProd box the plain-NEON 16x16 SAD and variance kernels had never once been exercised, though they still ship and still run on a box without dotprod. `dequant_4x4/8x8` and `ssd_16xh/8xh` had no group at all. All four, and both dotprod twins, are rows now. Three nets sit on top of the value comparison. The first is a poisoned border on every destination. The second is a row-padding check for a kernel that stores a wide vector into a narrow block. The third is an **mmap/mprotect page guard** sized to exactly the window the kernel declares and run at BOTH tails. Only the page guard catches a read whose value is discarded, the classic "load 8 lanes, use 4" tail. The scramble check misses that read because the output does not move. The sanitizer stays quiet too, since the bytes are still inside the allocation. A page fault is a hard stop that names the shape rather than a counted failure. `--list` prints the table with the ISA each row needs. `--isa neon\|sse4\|avx2\|avx512\|all` runs one tier. The table is per ARCHITECTURE, so the group count is too. An aarch64 build runs the 49 above. An x86-64 build runs the portable rows plus the x86 ones: 84 groups since wave 3b, 38 per tier (15 pixel, 6 mc, 11 transform, 3 predict, 3 deblock) and 46 of them at `--isa sse4`. Since that wave a group's BODY is written once and takes its kernel as an argument, with one thin row per tier on top. A new tier's twin therefore inherits the adversarial fills and page guards of the row it joins instead of restating them. Wave 3b put the last two families on that shape. The move brought four page guards the NEON rows had never had: the two luma deblock edge windows, the chroma edge's four rows, every prediction destination block and the 32-byte flat intra8x8 edge array. The destination blocks are packed, so a store sized to the register faults instead of merely disagreeing. `--bench` is informational and single-threaded. A checkasm multiple is necessary, never sufficient |
| `scripts/x86-docker.sh` ("x86 in Docker") | does this encoder build and pass its correctness battery as linux/amd64, on a box that has no x86 in it, and does the SIMD tier axis move a byte | **The matrix is `{rosetta, qemu}` x `{Y264_SIMD_FORCE=none, sse4, avx2}` x `{make test incl. checkasm, conformance --fast, identity cmp against `none`}`, plus a 10-bit build**, out of `docker/x86/Dockerfile`, which builds on ubuntu 24.04. The image has no libvmaf-dev because Ubuntu packages none and every quality harness here reaches VMAF through ffmpeg anyway. The tier axis is the RUNTIME cap, so one build serves all three rows. `-Dsimd=` is the build-time twin and is CI's job. **What an emulated pass is worth is the whole question, so the script says it out loud.** Neither emulator has AVX-512. The script ASSERTS its absence and prints the assertion, because an emulated green that nobody qualified is how "AVX-512 works" gets into a document without ever having executed. The two backends fail differently and both are needed. Rosetta translates ahead of time and runs near native. It also has a history of illegal-instruction bugs. A case that dies of SIGILL is rerun under QEMU, with its Rosetta row marked "emulator" rather than passed. QEMU's TCG interprets and is several times slower. It is the independent second opinion, reserved for an item's final gate. The feature probe differs by backend too. Under Rosetta `/proc/cpuinfo` is an x86 one and gets printed. Under qemu-user it is the HOST's arm64 file and says nothing. The probe that counts on both is the binary's own CPUID as `checkasm --list` prints it. Docker Desktop registers both interpreters for the same ELF magic, so selecting QEMU means DISABLING the Rosetta binfmt entry for the leg. The script announces that, traps it and restores it. **The negative control is the reason to run this before the kernels land**: on a tree with no x86 kernels, `sse4` and `avx2` must be byte-identical to `none` by construction. A difference there is a defect in this harness and not a finding about the encoder. Corpus clips are bind-mounted READ-ONLY at their own paths, symlink chain and all. They are never copied. The corpus is ~25 GB and is not ours to redistribute. The script reads NO clocks, because an emulated wall time is not a measurement. An emulated run is still CPU load like an encode, so it is coordinated the way a timed leg is (docs/x86-plan.md, "Box rule"). The first thing it found was not an x86 defect at all. A stray `#endif` had left ~170 lines of the CLI inside an `#if defined(__APPLE__)`, so the CLI had not compiled on Linux on ANY architecture. Nothing had asked the question because the ubuntu CI job was manual-only at the time. It now runs on every push that touches code (`.github/workflows/ci.yml`). The fixture cache is keyed by the **ffmpeg identity** (the binary's path and its version banner) beside `FIXVER`. The synthetic clips are written by ffmpeg and two ffmpegs do not write the same Y4M header. When the cache was shared, a container run changed the result of the next native run in the same tree. The container still gets its own cache directory mounted over the tree's. That mount is also what keeps its writes out of the worktree. |
| `scripts/san_matrix.sh` | **the encoder under ASan/UBSan over 21 edge inputs** (odd sizes, 1-2 frame clips with B, keyint 1, qp 0, extreme bitrates, 4:2:2/4:4:4, CBR, 2-pass, direct temporal, the hardware backend, 1-12 threads) | Builds `build-san/` once. ~3 min. The exit status is the number of cases with reports. On 2026-09-04 it found two memory bugs that recon-match couldn't see |
| `scripts/identity_ab.sh` | **do two binaries (or one binary under two env arms) produce the same bitstreams?** The gate for any change that must not move a bit: refactors, layout changes, knob removals that keep the default branch | About 100 cells at full (`MATRIX=quick`: 15): every rate mode (CQP, CRF, ABR, capped ABR, CBR, CRF+VBV, 2-pass), presets, tunes, coding tools, CAVLC, PAFF, slices, 4:2:2, 4:4:4 and 10-bit (derived from foreman_cif with ffmpeg), 720p and 1080p, plus threaded cells. t1 cells are final. Threaded cells are re-run `REPEAT` times per side and read VARIES (a warning) when a side doesn't repeat. A control cell asks the two sides for different streams and must read DIFF, so an encode that never ran can't pass. Every `Y264_*` in the caller's env is cleared, and `ARM_A`/`ARM_B` set env per side. Full matrix about 45 s at `BUDGET=4` |
| `scripts/hrd_check.py` | would a conforming decoder's coded picture buffer survive this stream: underflow, overflow, peak fullness, tightest arrival margin | Takes NOTHING about the bucket on the command line. BitRate, CpbSize, cbr_flag, the initial delay and every removal time come from the SPS VUI hrd_parameters and the buffering_period / pic_timing SEI. Exit 0 means clean and 1 a violation. **3 = the stream carries no HRD and nothing was checked**. That is every stream without `--nal-hrd`, where a vacuous pass would read as a green gate on every VBV stream in the tree. `--allow-no-hrd` softens that for callers that want it. Since B-hrd, `--nal-hrd vbr\|cbr` makes yah264's own streams readable by it. `HRD=1 scripts/cvbr_compliance.sh` is the 36-cell leg. It prints the VBV verdict beside both HRD verdicts because a cell whose rate control already underflows its own model emits a stream that underflows that model's declaration. A raw HRD count would score the rate control while calling it a signaling result. **Not a replacement for `vbv_check.py`**, which is TOLD the bucket and simulates the ENCODER's law. That one asks "did our rate control respect the cap we gave it". This one asks "would a receiver's buffer survive what we emitted" |
| `scripts/level_check.py` | is the declared `level_idc` the one Table A-1 requires, and is any leg of it exceeded | Frame size, MB/s, DPB, bitrate/CPB (from the HRD, or from `--bitrate` when the stream has none), and the vertical MV bound the VUI declares. `--exact` demands the declared level EQUAL the pick rather than merely reach it. `--y4m SRC` reads the frame rate off the source clip when the stream has no VUI timing. There is deliberately no default frame rate. Table A-1 is transcribed independently on purpose, because a checker sharing the encoder's own table agrees with it by construction and gates nothing |
| `scripts/recovery_check.py` | does an `--open-gop` stream really recover where its recovery point SEI says it does | Cuts the elementary stream at the chosen recovery point, pastes the parameter sets in front, decodes that, and compares the LAST `n - k` frames against the encoder's own `--dump-recon`. The tail is the whole trick. The pictures the recovery point does not cover are the ones with a picture order count below its own. They land at the FRONT in output order, so counting back from the last frame makes their number irrelevant. **The cut decode passes `-flags2 +showall`, and the version floor for that is ffmpeg 6.1.** Which pictures a decoder hands to a display after a mid-stream start is a display policy and not a decode result. The versions disagree about it on the same bytes. 9.0.1 emits the covered pictures and suppresses the uncovered leading B frames. 6.1.1 emits the first of them and then stops two short, never draining the last two out of its buffer when the bitstream ends. That falls short of the output process 8.2.1 and Annex C describe, where every picture still in the buffer is output at the end of the bitstream. It is a policy defect and NOT a decode defect, though. Asked for all of them, 6.1.1 produces the same samples 9.0.1 does, to the byte, on all three cells. So the selection is made here rather than borrowed from the decoder. That is what makes the three `ogop_rec_*` cells read the same under either version. Exit 0 means clean and 1 a mismatch. **2 = the stream carries no recovery point**. For an `--open-gop` encode that is itself a failure, which is why it is not 0 |
| `scripts/h264_syntax.py` | (not a gate) the Annex-B / SPS / SEI reader the two checkers share, plus `--probe STREAM` for the stream properties a harness needs in order to choose a decoder | Parses parameter sets, SEI and slice HEADERS only. It entropy-decodes no slice data. That keeps the two checkers cheap enough to run inside a gate |
| `scripts/fetch_openh264.sh`, `scripts/fetch_jm.sh` | build the extra conformance decoders into `tools/openh264/h264dec` and `tools/jm/ldecod` | Source is pinned by tag AND sha256 and downloaded and built under `scratch/`. Only the binary is installed. Nothing is vendored and both directories are gitignored. The CLI needs a source build because the packaged openh264 ships the library only. CONTRIBUTING rule 4 allows the JM as an oracle: run it and compare its output. Only `ldecod` is built |

`env_gate_audit.py` exists because the TSan floor is not naturally zero. The
encoder resolves its ~400 `Y264_*` knobs through lazy function-local statics.
The CLI opens one encoder per GOP from concurrent workers, so any gate not
resolved on the main thread first gets its first touch on whichever worker gets
there. These races are benign same-value init. They still FOG real hunts: the
4:4:4 MC border overflow of 08-29 sat behind four of them. The audit walks the
call graph from `warm_lr_statics` / `ntp_pool_create` / `ntp_bg_create` and
names two failures. A **TRAP** gate's resolved value still satisfies its guard,
so the gate rewrites its static on every call. That makes a perpetual writer no
warm can quiet. `Y264_WF_CAPK` resolved "unset" to `-2` under a `v < 0` guard
and did exactly this. A **COLD** gate latches and is never warmed. Run the
audit after adding a knob. Two things it cannot see have both bitten. A
static resolved **inline inside a hot function** is unreachable by any warm and
has to be hoisted into an accessor first. An **unconditional store** in a warm
function (`y264_tl_on = trprof_on();`) is a perpetual writer that is not a
getenv guard at all. In anything a warm touches, use read-compare-write instead
of a plain store.


**Determinism is a diagnostic with threads, a gate at one thread (owner,
2026-09-25).** Threaded encodes don't need repeatable byte-exact output, so
`stair_determ.sh`, `shot_determinism.sh` and the threaded identity cells in
`conformance.sh` print WARN and exit 0 on a difference. At `--threads 1` two
runs must match. `check_determinism` in `conformance.sh`, the regress rerun,
`determ_repeat.sh` at t1, the pipe check at t1 and `make repro` all FAIL on a
difference. They stay useful for hunting an uninitialized read or a race, and
for telling whether an identity-gated probe can be read from one run. The notes
below are about using them that way.

**Run `determ_repeat.sh` on a LOADED box.** On an idle one it passes against a
default that emits six distinct bitstreams per eighteen runs. The mb-tree
Phase-A warm shipped that way. A clean 16/16 pass the same morning missed it,
while six busy-loop spinners in the background made it fail on four
configurations within twelve runs each. That gap is expected, because the
defects this gate exists for depend on timing and an idle 18-core box runs the
threaded path the same way every time.
Interleave the two arms so both see the same load. The reading is the count of
distinct md5s, not pass/fail:

```sh
pids=; for i in 1 2 3 4 5 6; do (while :; do :; done) & pids+=($!); done
scripts/determ_repeat.sh; kill $pids; pgrep -f "while :" && echo "LEAKED SPINNERS"
```

**Gate the MODE, not just the default.** `determ_repeat.sh` and
`stair_determ.sh` both encode with the shipped defaults unless told otherwise.
A defect confined to a non-default coding mode is therefore invisible to the
whole battery. Per-macroblock temporal direct was irreproducible above one
thread for a week while every gate read green, because nothing in the tree ever
ran `--direct temporal` under repetition. `ARGS=` (here) and
`STAIR_DETERM_ARGS=` (stair_determ) are the slots for that. Both scripts keep
env in a SEPARATE slot on purpose. Passed to `env(1)`, an encoder flag makes it
reject the command. Every run then produces nothing, and the nothings have
matching md5s.

**Kill by PID and VERIFY, never `kill %N`**: in non-interactive shells some
job-spec kills fail silently. Ten leaked spinners (2 per gate, accumulated over
a day) once pinned the box at load ~14 and corrupted every speed number for ten
hours, including two full table runs. **Check `uptime` before any timing
session.** Band and BD reads at matched rate are load-immune. Walls and tables
are not.

`make conformance` gates the DEFAULT path. A coding tool behind an env knob has
its own conformance surface: the cross product of ref count, B-frame count,
entropy coder, direct mode and transform. `recon_sweep.sh` is that cross
product (6 clips x 5 QPs x 10 configs). It deletes its outputs before every
iteration and checks the encoder's exit code. The failure it was written after
was a rejected config that left the previous iteration's files in place, which
then scored as a pass.

`scripts/abr_decode_gate.sh` encodes the table ABR shapes at t12. It asserts
that every input frame decodes AND that mean PSNR clears a floor (25 dB; broken
emission reads ~15, working reads ~30-42). It exists because a broken threaded
ABR path passed the whole rest of the battery: conformance cannot reach the
threaded path (`--dump-recon` forces the serial streaming path), the CRF band
never runs ABR, and identity gates compare an encoder with itself. `ARM='<env>'`
gates an arm.

**THREE DECODERS, AND WHAT EACH ONE CAN ACTUALLY READ.**
`YAH264_CONF_DECODERS="ffmpeg openh264 jm"` turns the recon-match gate into a
three-oracle gate (default: ffmpeg alone). ffmpeg and the JM read everything
this encoder emits. **openh264 reads almost none of it**, measured on 2.6.0 on
2026-09-16:

| stream shape | openh264 2.6.0 | JM 19.0 | ffmpeg |
|---|---|---|---|
| 4:2:0 progressive, no B (CAVLC, CABAC, 8x8, multi-ref, crop, all-intra) | exact | exact | exact |
| any B slice | **wrong pictures, exit 0** | exact | exact |
| 4:2:2, 4:4:4 | writes nothing | exact | exact |
| `frame_mbs_only_flag == 0` (fields, either flavor) | refuses at the SPS | exact | exact |

The middle row is the one to remember. On B slices openh264 emits the right
NUMBER of frames with the wrong CONTENT and returns success. A harness that
gated on its exit status would report a clean second oracle while comparing
garbage. `conformance.sh` therefore asks `h264_syntax.py --probe` what is in the
stream and SKIPS openh264 by stream property, never by trusting the decoder.
For the same reason the run prints a per-decoder coverage line, because a
decoder that checked nothing must not be readable as a decoder that agreed. On
the fast suite that line read 21 cells checked for openh264 against 317 for
each of the others. The six `baseline-shaped` cells exist so that 21 is not 3.

**DOES OPENH264 DECODE FIELD PICTURES? NO.** Not for PAFF either. Given an
interlaced stream it refuses while parsing the sequence parameter set, prints
`frame_mbs_only_flag (0) not supported`, and decodes zero frames. That flag is
0 for PAFF and for MBAFF alike, so the refusal comes at the sequence level,
before any `field_pic_flag` is read. No field-coded stream of any shape gets
past it. **The JM is therefore the PAFF item's second oracle**. It has been
checked on the shape that item will produce: an interlaced fixture
(`ffmpeg tinterlace` + `x264 --tff`, 24 frames at 320x240) decodes to output
byte-identical with ffmpeg's. (x264 codes interlace as MBAFF, so it cannot
produce a true `field_pic_flag == 1` fixture. The SPS-level refusal is what
settles the question for both.)

**A FINDING FROM level_check's first run, for A-plumb or B-hrd to take:** yah264
writes `log2_max_mv_length_vertical = 16` into the VUI at every level. That
advertises a vertical MV range of +-16384 luma samples where level 1.3 allows
+-128 and level 4 allows +-512. The encoder's search is clamped to the level
correctly. The DECLARATION does not follow it, though, while the reference
narrows its own to the level. This is not a conformance failure on its own, so
`level_check.py` reports it as a note and fails only under `--strict-mv`.

Plus, by hand for any default flip: the escape env must reproduce the OLD
default md5 exactly, the new default must equal the explicitly-armed md5, and
`t8 == t18 == t8-noasm`. (samsung ABR has a known, documented t1-vs-t8
divergence. Reproduce it with the escape armed before blaming your change.)

## 6. The reference side

The comparison harnesses drive external baselines through environment
variables. The repository can't provide any of them:

| variable | used by | what it must point at |
|---|---|---|
| `X264_ASM`, `X264_C` | `scripts/perf-comp.sh`, `scripts/instr-ratio.sh`, `scripts/crf-solve.py` | an x264 CLI with assembly on / a pure-C build with assembly off and the compiler's vectorizer left on. That's the fair build. A stock `--disable-asm` build is a scalar strawman |
| **which reference binary** | every board | **Three harnesses reach for the reference without all reaching for the same build. Say which one produced a number.** `scripts/ffboard.py` loads whatever `X264LIB` points at. `scripts/perf-comp.sh` builds from the `../x264` source checkout with `--disable-lavf --disable-ffms --disable-avs --disable-swscale`. `scripts/multishot_bd.py` uses that checkout's CLI. The installed reference on the dev box is 0.165.3222 (Homebrew). The pure-C arm is none of these as shipped. It is `--disable-asm` with `-fno-tree-vectorize` stripped from `config.mak` (a build-flag change, no source edits), and a published pure-C row should say so |
| `X264` | `scripts/cvbr_compliance.sh`, `scripts/ladder.py` | an x264 CLI |
| `X264LIB`, `Y264LIB` | `scripts/ffboard.py` | installed prefixes of libx264 and libyah264 for an ffmpeg that links both. They live at `../build/ffboard/x264asm`, `../build/ffboard/x264noasm` and `../build/ffboard/y264inst`. They have no defaults on purpose: a stale install that loads reads as a result, while a missing one reads as an error. Since 2026-09-19 the board hashes both dylibs into the solve-cache key and checks `$Y264LIB/include/yah264.h` against the header the ffmpeg was built with. A stale install now gets named instead of measured |
| `FF` | `scripts/ffboard.py` and the row scripts | that ffmpeg (default `../build/ffboard/ffmpeg-yah264/ffmpeg`, then the old `/tmp/ffmpeg-yah264/ffmpeg`). **The set moved out of `/tmp` on 2026-09-17 because macOS swept it and the board lost its in-process arm.** `Y264_FFBOARD_ROOT` moves the whole set. docs/ffmpeg-integration-plan.md rebuilds it |
| `OPENH264` | `scripts/openh264-shim.sh` | an openh264 `h264enc` binary |
| `OPENH264_DEC` | `scripts/openh264-shim.sh --decode`, `scripts/conformance.sh` | an openh264 `h264dec` binary. It defaults to `tools/openh264/h264dec`, which `scripts/fetch_openh264.sh` builds. The packaged openh264 has no CLI |
| `YAH264_CONF_DECODERS` | `scripts/conformance.sh` | which oracles run: any of `ffmpeg openh264 jm` (default `ffmpeg`). A named decoder whose binary is missing is an ERROR, never a silent fall back to ffmpeg |

The clips come from `scripts/fetch_corpus.sh`, which fetches the board's ten
and the wider corpus. docs/corpus-sources.md says what is and isn't fetchable.
Notes about instrumenting the reference encoder itself stay out of the public
tree and live with the measurement records.

## 7. Traps that have each cost a round

- **The table's dVMAF is only comparable across builds if the operating point
  is pinned.** `crf-solve.py` cached the solved point keyed on **the BYTES of
  the binary**. Any code change forced a fresh solve, even a byte-identical
  one. Our CRF staircase (~13% of rate per rung) then let the fresh solve land
  on a neighboring rung: two builds with identical output on all six table
  clips read **-0.56 against -0.49**. They differed only on foreman and bus,
  at 411 vs 409 and 379 vs 375 kbit/s. The operating point is now PINNED per
  (clip, args, target, tier) and not per binary (`tests/.crfpin`,
  `Y264_CRF_PIN=0` bypasses). The pin is resolved BEFORE the solve cache and is
  part of its key. Every solve prints `pin_state` and `pin_drift_pct`.
  **Read `pin_drift_pct` before reading dVMAF**: nonzero means this build's rate
  curve moved and the comparison shifted with it.
- **A timed output write does not cancel out of a ratio, and it fakes a
  resolution-shaped effect.** It inflates the BASELINE. The arm's cost as a
  percentage then comes out too small, most of all where the bitstream is
  biggest. Back to back on an idle box, the same park_joy encode reads 1.63 s
  to `/dev/null` and 2.1-10.2 s writing its 9.5 MB bitstream to the temp
  directory. A spread guard does not always catch it: one cell read 3.2x its
  true wall at a spread of 1.02. It survives because CIF cells (~1 MB) barely
  show it. That alone produced a recorded finding that our B_8x8 arm cost
  x264's price on CIF and twice it at 720p. With the write removed the ratio is
  flat and the worst cell is CIF. **Time to `/dev/null`, and give `--md5` its
  own untimed encode.**
- **A wall harness that defaults to the PURE-C tier prices the wrong tier for
  any as-shipped question**. The only tell is a `tier pure` token in a header
  line. A NEON kernel's first A/B read +0.03 to +0.32% and looked like a
  failure, on a tier the kernel does not exist on.
- **Refuse to compare two builds whose `optimization` differs.** A meson -O2
  build against an -O3 build once read as a 2-3% phantom win.
- **Put both arms of a throughput test in ONE burst with mirrored launch
  order.** The box's load moves slower than a burst and faster than a round.
- **`Y264_MBTREE_OFF` is a dirty control.** Besides turning off propagation, it
  swaps the per-MB offset to a different AQ array. Use `Y264_MBTREE_STRENGTH=0`
  for a clean propagation on/off. The tell was a "gain" of -5.75% at
  strength 0.
- **zsh does not word-split scalars.** Never build CLI args in a string
  variable. Verify that A/B configs produce DIFFERENT md5s before trusting a
  null.
- **Verify the probe is in the binary you time.** One clean null came from a
  probe-less build.
- **A guard that reads an unpopulated signal disables the thing it guards** and
  prints a clean `+0.00%` meaning "never ran". The tell: output byte-identical
  to baseline rather than to the un-guarded arm.
- **Check what a substituted field is interpreted AS.** `mb_qp_pre` reads the COMBINED offset (AQ folded in). A substituted offset map
that held the mb-tree term alone inflated a load-bearing result by more than
2x.
- **Keying a probe on POC silently UNIONS GOPs, and it has cost two rounds.**
  POC restarts at every IDR, so a set keyed on `(POC, ...)` matches entries
  belonging to different frames. It inflated the skip oracle's bound
  1.98x -> 1.39x. It also invented a 66% "recompute" rate for the mb-tree
  phase-A memo that did not exist. A 3-way associative memo was built on that
  rate, came back inert (miss count 277 -> 277) and was reverted. **The tell is
  in the probe's own output: the false count tracked the I-FRAME COUNT** (bus 1
  I-frame / 0 recomputes, ducks 2 / 81, pjoy 3 / 300, samsung 5 / 183). The
  one single-GOP clip was the only one where the key could not be wrong, and it
  was the one reporting nothing to win. Key on an absolute display index. When
  a new probe disagrees with an existing one, suspect the new one first.
- **BD reads are sweep-dependent.** Check the saturation flag. A reference
  curve too flat in the band makes the number meaningless (touchdown's slope is
  2.27 and its rows have read +341269% before).
- **Nothing else re-runs one configuration against itself.** `stair_determ.sh`
  compares ACROSS thread counts and `w2_canary.sh` against a reference build.
  Neither catches a default that emits a different bitstream every run. One did
  for a day (3-5 distinct md5s per 12 runs at foreman `--ref 1` t18) and no
  battery noticed. For that day every md5 identity gate in the tree was a
  lottery ticket. `scripts/determ_repeat.sh` is the instrument for it
  (informational since 2026-09-25; `STRICT=1` fails on a difference). It was
  verified by failing against the binary it was written for.
- **A "byte-identical by construction" argument is not a gate, and this tree
  has shipped two of them that were false.** The A1 pair seed and the mb-tree
  Phase-A warm both carried a paragraph of reasoning where a measurement
  belonged. Both emitted several distinct bitstreams per twelve runs. When a
  feature's whole claim is that it does not change the output, the claim IS the
  gate: run it, under load, against the escape.
- **TWO code paths reading the same settled bound from different origins will
  disagree.** The warm measures its window offsets from the ring head. The walk
  measures them from its anchor, `d` entries further in. So the same source is
  `laoff` in one and `laoff - d` in the other, and one bound cannot serve both.
  The tell: turning off BOTH settled-bounded readers
  (`Y264_MBT_BLEG_REUSE=0 Y264_MBT_PAIR_SCALE=0`) made the two paths agree
  exactly, while turning off either one alone did not.
- **TSan can be silent on a publish-before-write.** The flag advertising "these
  motion fields are ready" was set before the batch that computed them. Because
  the write usually landed first, TSan saw no race on most runs. The
  repeated-run md5 loop caught it. TSan only confirmed it afterwards, on the
  OTHER reader.
- **TSan is silent on a stack overflow.** Under threads, B_8x8's output was
  nondeterministic run to run and TSan found nothing. The cause was a
  stride-16 motion-compensation write into a 64-pixel buffer. When run-to-run
  variance survives every concurrency knob being turned off, stop looking for a
  race and look for memory being written past its end.
- **A SECOND DECODER THAT RETURNS 0 IS NOT A SECOND OPINION.** openh264 2.6.0
  decodes a stream with B slices into the right NUMBER of frames with the wrong
  CONTENT and exits 0. It says nothing on stderr and writes a full-size file. A
  recon-match arm gated on its exit status would have read as a clean
  independent confirmation while comparing garbage. So would one gated on "did
  it write anything". `conformance.sh` asks `scripts/h264_syntax.py --probe`
  what is in the stream and skips the decoder by stream property instead. The
  general form: **an oracle's capability is a property of the stream, not of
  its return code**. A multi-oracle gate must print how much each oracle
  actually covered, or "green" stops meaning anything (openh264 covers 21 of
  317 recon cells here).
- **A stale output file scores as a pass.** Take an A/B loop that writes to a
  fixed filename and then compares it. Whenever the encoder writes nothing, the
  loop re-scores the PREVIOUS iteration. An unknown flag does exactly that: the
  encoder exits 2 and prints help. Delete the outputs at the top of every
  iteration and check the exit code. This is the second half of the zsh
  word-split trap. The scalar arrives as one argument, the CLI rejects it, and
  the harness reports the arm you never ran as clean. It voided a B_8x8 bisect
  and sent it after the wrong subsystem.
- **A ship can break a probe silently.** The mb-tree replay oracle went stale
  the day `Y264_MBT_BREF` gave the walk a second output. The next session to
  use it got a VOID on every clip. Run a probe's own identity gate FIRST, every
  time. When a feature adds an output, ask which recorder carries it.
- **This box cannot time a 720p cell reliably while a desktop video process is
  ACTIVELY DECODING.** With `WallpaperAerialsExtension` and
  `VTDecoderXPCService` running, the same park_joy encode to `/dev/null` reads
  1.63, 1.93, 4.28 and 5.24 s within minutes. Meanwhile the CIF cells hold to a
  1.03 spread. Check a 720p cell by hand before trusting a 720p row.
  **RESIDENCY IS NOT THE CONDITION, and reading it as one cost a round.**
  `WallpaperAerialsExtension` is resident essentially always and burns
  8.7-13.6% of ONE core when no wallpaper is displayed. At that level the
  sanity cells reproduce the reference table to the hundredth on both tiers
  (ducks t18 pure-C 1.84/1.82/1.81, foreman t1 0.80/0.79/0.79). Suspending it
  changes nothing. Gate on the SANITY CELLS instead of `ps` output.
- **`ps` `pcpu` is a DECAYING AVERAGE, not an instantaneous sample, and it lies
  after a `SIGSTOP`.** A suspended process stops being scheduled. The kernel's
  estimate freezes at whatever it was and reports that value indefinitely
  after `SIGCONT`. It once read 47-55% for a process actually using 13.6% of a
  core. Worse, `pkill`ing a system extension makes launchd relaunch it, and the
  relaunch burst inflates the estimate. If you kill a process to quiet the box
  and then read `ps`, you measure your own perturbation. **Ground truth is a
  cumulative-CPU-time delta**, which needs no interpretation:
  ```sh
  a=$(ps -o time= -p $pid); sleep 10; b=$(ps -o time= -p $pid); echo "$a -> $b"
  ```
- **A BD-rate number is not a bandwidth saving, and it scores only half of what
  an arm does.** BD-rate is the mean horizontal gap between two RD curves,
  integrated with UNIFORM weight over a quality interval. Both of its two
  consequences bite when a result leaves this repo:
  1. It weights every operating point equally. Real ladders ship specific rungs
     to specific audiences, and the gap at those rungs can look nothing like
     the average. "x264 veryslow beats medium by 9.3% BD" does not mean any
     particular deployment saves 9.3% of its bandwidth.
  2. It converts the entire difference into RATE at matched quality. A shipped
     ladder holds the rungs fixed, so the difference actually arrives as a MIX
     of a little rate and a little quality. BD keeps the rate half and discards
     the quality half. Which half you collect depends on where your audience
     sits. Near the top of the ladder it shows up as bitrate. Lower down it
     shows up mostly as quality.

  Measured here: the corrected mb-tree splice on bus reads **+3.90% BD**, which
  sounds like "spends 3.9% more bits". Per rung it spends **0.45-1.58% FEWER**
  bits and gives up **0.43-1.00 VMAF**. The BD number is a quality loss
  re-expressed in rate units. That is why `scripts/run_band.py` prints
  `rungN: dRate / dVMAF` beside every BD line: **quote both halves, and never
  translate a BD number into a bandwidth claim.** But read the rung columns only
  in a self-A/B, where both sides run the same ladder on the same encoder.
  Against x264 the two CRF scales are different operating points. There the
  columns show the scale mismatch rather than an efficiency difference (bus
  reads +42-45% rate / +2.4-4.5 VMAF at every rung against x264), and the BD
  number is the one that normalizes it. The two answer different questions.
  Each misleads in the other's setting.
- **A speed number is a number about ONE OPERATING POINT, and the curve runs
  from 1.00x to 1.36x on a single clip.** Measured on bus_cif at four solved
  matched points: 1.36 / 1.18 / 1.06 / 1.01 at 144 / 375 / 1252 / 3485 kbit/s,
  with dVMAF -2.78 / -1.58 / +0.01 / +0.00 beside it. ducks_720p runs the same
  curve in reverse (1.18x at crf 35, 0.93x at its table crf 23). **The clip we
  "win on" is the low-QP end of the same line, not a different path.** Before
  attributing anything to a clip, check whether you are looking at its rate.
- **The corpus median and the TABLE are not the same number, and they have
  differed by four points.** Full corpus BD against x264 read -0.85% while the
  table's own six clips read +3.54%. The corpus median was carried by
  touchdown, sintel, coastguard and akiyo, and the table scores none of them. A
  plan built on "we are at parity now" from the corpus number is planning
  against the wrong set. Split by table membership before drawing a conclusion.
- **Before refusing an arm on wall, price the same arm on THEIR side.** B_8x8
  plus B_RECT sat refused for a day at "10-20% wall". The reference encoder
  ships the identical feature at medium and is faster than us anyway. Priced
  properly, it costs x264 4.6-10.6%. The refusal was never about the modes. It
  was an implementation gap nobody had looked for, because the number had no
  denominator.
- **A t1 speed number does not predict its t12 value, and the sign of the error
  is not constant.** Three arms were measured the same day. `Y264_P_SKIP_EXIT`
  went from 0.6-2.2% at t1 pure-C to **exactly null** at t12 as-shipped.
  `Y264_PART_EARLYTERM=3` kept everything. The byte-identical coverage round
  transferred at 1.06x median and **1.36x on bus**, while its all-intra cell
  collapsed to 0.16x. The rule that fits: **work deletion on the wavefront
  transfers and pays more where the machine is throughput-bound, work already
  off the critical path pays only at t1, and work relocation pays nowhere.**
- **Hoisting work out of a loop can cost the SIMD tier what it buys the C
  one.** Lifting the Intra8x8 reference-sample filter out of the nine-mode
  decision loop routed the loop away from the per-mode NEON builders: pure-C
  +0.7%, as-shipped **-2.1 to -2.9%** on all-intra. Both tiers gain only once
  the hoisted state has a form BOTH builders read. Measure a refactor on both
  tiers, always, and with an all-intra cell if it touches intra.
- **`/usr/bin/time -p` cannot time a CIF cell.** It prints two decimals, and
  the CIF cells run 0.05-0.11 s at 12 threads. So bus_cif (ours 0.108, theirs
  0.082) can only print 0.11/0.08 = 1.375 or 0.10/0.08 = 1.25. That is exactly
  the "1.22x and 1.38x on two runs of the identical config" once recorded as
  noise. Each such row carries a 9-14% quantum. **The worst-clip metric is the
  one this corrupts most**, because MAX picks whichever row rounded up.
  The 720p rows are fine. Timing is `perf_counter` with repeated samples now.
  Do not reintroduce a two-decimal timer. Do not time side A five times and
  THEN side B five times either: that puts a drifting box's whole drift in the
  ratio.
- **This box is HETEROGENEOUS and the ideal speedup is not the core count.**
  M5 Max: `hw.perflevel0.physicalcpu` = 6 "Super" plus `hw.perflevel1` = 12
  "Performance". Twelve threads is not two thirds of eighteen threads'
  capacity. The ideal 12 -> 18 speedup is about 1.43x. Check
  `sysctl hw.perflevel*` before calling a scaling number a defect.
- **Per-unit cost inflating with thread count is not automatically your bug.**
  Both encoders pay it here (ours 1.62x, x264's 1.28x on foreman at t12). The
  only way to assign it is N independent single-thread processes of the SAME
  binary. They share nothing, so the curve is that binary's own machine floor.
  x264 threaded sits at or below its floor. Ours sits 15-27% above ours. A raw inflation number without
  that denominator says nothing.
- **`sample` on x264 fed a large y4m reads ~49% `__mmap`/`__munmap`, and taking
  its coverage share raw halves it.** The profile shows x264 mapping its input file where we stream ours, so a looped multi-GB clip puts the file in its profile and not in ours.
  On bus at t12 their NEON share reads 24.9% of all work samples. The
  comparable number is **48.9% of the non-I/O half**. Rescale off the mmap base
  before comparing the two encoders, or the coverage gap appears to close on
  its own. Coverage profiles also need an encode long enough to sample. bus_cif
  is 150 frames and too short, so loop it.
- **An env knob that overrides an AUTO default can override it DOWNWARD**, and
  then the arm is a regression the knob did not cause. `Y264_LA_BUF=1` was
  recorded twice as "a 1-5% regression nobody has diagnosed" and once as the
  broken prerequisite blocking a route worth 4%. The shipped default already
  resolves `la_buf = bframes+1 = 4`, so `=1` was cutting three frames off the
  encoder's own lead. The cost is monotone in what the knob removes (samsung 0
  -> -6.41%, 1 -> -5.01%, 4 = the default, reading -0.14% because it IS the
  baseline). Before pricing a knob, print what the default resolved it to.
- **Read EVERY line of a per-encoder stat dump, not the first.** The CLI opens
  a pool-less probe encoder that prints before the real one. Its
  `Y264_LA_STAT` line says `pool=0 la_buf=0 la_th=0` on a default that runs
  `pool=12 la_buf=4 la_th=1`. Reading line one says the feature is off when it
  is on.
- **Two instruments in one dump that disagree by 15x is the finding.** An audit
  resolved `tail 31.1%` against `pool-empty 28%` by picking the bigger name and
  dismissing the other. The bucket was wrong (it billed a whole sleep to the
  class sampled at its entry), and a plan was written on it.
- **A full temp volume does not read as a full temp volume; it reads as a
  content failure.** Two weeks of harness runs left 409 GB under `$TMPDIR`
  (9772 directories, none of them removed by the run that made them). The data
  volume hit 100% on 2026-09-19. The merge gate showed twenty conformance cells
  failing with **"no geometry in the recon y4m header"** on the big qp0 clips.
  The recon had been truncated to nothing, and the cells were read as an
  encoder defect first. `conformance.sh` and `ffboard.py` now refuse to start
  below 20 GB free and name `make clean-scratch`. The harnesses remove their own
  scratch on every exit path. If a gate starts failing on the largest clips
  only, check `df` before reading the failure.
- **SME/SME2 is closed on every form; do not reopen it.** It is reachable only
  with `-mcpu=apple-m4` or `native` (generic `-march=armv9-a+sme2` compiles and
  SIGILLs). 512-bit streaming vectors buy **1.08x** over 128-bit NEON. Since
  `smstart` plus `smstop` costs **9.6 ns** against a 3.7 ns satd8x8, per-block
  use is impossible. One arm batched whole-frame SATD through the ZA
  outer-product engine as two matmuls, bit-exact, with one streaming region per
  frame. It reads **0.21x NEON** (18.9 vs 3.89 ns per 8x8). The peak-rate
  instruction floor is itself below NEON parity (SMOPA peak 0.47 ns, MOVA
  0.31 ns, tile write-to-read round trip 7.33 ns).
- **Any Metal-armed arm pays the 12-17 ms per-process bring-up floor per table
  cell**. That floor is device init rather than shaders, and a precompiled
  metallib does not move it. Dispatch latency is not the problem. The
  lookahead's per-frame pixel work (scale and costmap on a 720p frame, end to
  end including submit and fence) runs at 0.24-0.26 ms each. The lookahead and
  mb-tree actually burn 3.5 ms/frame, so that is ~7x headroom. What closes the
  route is the inter-round idle gap and the DVFS ramp. Every hot-clock number
  is a lie about an anchor-cadence workload. A sleeping warmer that reads flat
  ONCE is not evidence, since that failed reproduction twice.
