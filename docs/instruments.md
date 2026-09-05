# Instruments: what already exists, and what each one answers

This is the catalog of **measurement tools that have produced a result and are
reusable**. It is not the feature-knob list; every `Y264_*` env var, its
reader, default and tier lives in `docs/knobs.md`, which
`scripts/knob_census.py` generates.

Read this before building a new probe. The recurring failure it exists to
prevent is a session spending its first hour rebuilding a profiler, an oracle
or an A/B harness that is already in the tree under a name nobody remembered.

## 1. Where does the time go?

| instrument | answers | notes |
|---|---|---|
| `Y264_BPROF=1` | per-stage wall inside the B **and P** macroblock tournaments, binned by final verdict, with MBs-touched counts | t1 only. Prints `BPROF` (B) and `PPROF` (P) tables at exit. Default inert, verified md5-identical |
| `Y264_BPROF2=1` | sub-decomposes ONE B RD trial into encode / dist / bits, and the tr-pre share | the tool that re-sized the RD-trial arm honestly |
| `Y264_MBT_SPLIT=1` | mb-tree by stage: invq / bind / phaseA / phaseB / finish, plus source and memo-miss counts | showed ref-B's cost is 95% phase A, killing a stale recovery plan |
| `-Dc_args=-DY264_STAGE_PROF` + `Y264_STAGE_PROF=1` | per-stage ms inside the macroblock loop and deblock | **t1 ONLY, and it fails SILENTLY at higher widths**: a stage-prof build at `--threads 12` writes a zero-byte bitstream and exits 0. The same build at t1 is fine and the normal build is byte-identical at t8/t12/t18, so this is the probe, not a shipped defect. Read a number off it only from a t1 run |
| `Y264_THREAD_PROF=1` | per-stage buckets across the whole encode | **profiler buckets have lied four times**; corroborate before acting |
| the same **plus `Y264_NTP_PROF=1`** | adds a **pool-idle** column per driver stage: how many of its milliseconds ran with no live pool job. A bucket's wall cannot separate a stage that holds the machine idle from one that overlaps the wavefront, and reading it as if it could cost two rounds. Named the serial I-slice analyze in one read |
| `Y264_NTP_PROF=1` idle split | busy / midrow / gate / ramp / tail / nojob per worker | the sleep is split against the pool's empty clock. An older `tail` reading of 31.1% was inflated by the serial term; the real number is 6.6% |
| `Y264_LED=...` | op ledger: counts of real operations, not sampled time | use when a keyword-sampled profile is suspect |
| `scripts/instr-ratio.sh N` | instruction-count ratio against x264 beside the wall ratio | quotes the table's factor without timer noise |
| `Y264_ME_ETSTAT=1` | **what an integer-search early-out would delete and what it would cost**: post-seed integer probes bucketed by how good the best SEED already was (cost per pixel), with how often the hex/grid/diamond then moved the MV and by how far. Read the MV columns, not the gain: integer-SATD-gain volume does not predict BD. t1; default inert, verified md5-exact |
| `Y264_B8_STAT=1` | engagement counters for the B_8x8 / B_RECT arm: B macroblocks reaching the tournament, quadrant searches run, RD trials run, trials that beat the running best, searches thrown away by the mid-tournament skip exit, rectangular searches admitted | t1 (plain globals). Named all three of that arm's levers in one read: search 100%, RD 69-78%, useful 4.5-11%, wasted 19-30%. `docs/b-8x8.md` |
| `Y264_LRSUB_PROBE=1` / `Y264_LRSUB_CENSUS=1` | direct wall of the lookahead's fifteen quarter-pel phase-plane builds, and per-phase read counts | 98.0 ms on samsung = 3.0% of wall against x264's 4.5%; **all 15 phases read, 99.8% of rows touched**, so there is no free deletion in it |
| `Y264_DIRECT_WHY=1` | **why a B slice fell back from temporal direct**: its list 0, the distinct co-located reference POCs starred where they do not resolve, the block count behind each, and the per-macroblock direct-unavailable count. This is what showed the gate was frame-wide when the spec requirement is per-macroblock (`docs/b-direct-mode.md`, third round). t1 |
| `Y264_DIAG_DIRECTOK=n` | force B direct UNAVAILABLE on every nth macroblock, content-independently. Isolates the `direct_ok == 0` path from whatever made it common; under spatial direct it is bit-exact over three t18 runs, which is how that path was cleared as the source of the per-macroblock gate's nondeterminism |
| `Y264_DIAG_COLWATCH=1` | hash the co-located field at the FIRST and LAST macroblock of a B frame, rather than at slice prep. A prep-time hash proves nothing about a field read during the macroblock loop when the picture supplying it is still being encoded; this is what cleared the co-located field properly |
| `Y264_DIAG_TDIRLIM=n` | refuse temporal direct for a macroblock whose derived mvL0/mvL1 exceeds +/-n pel. Tests whether an out-of-range derived vector is behind a result, which is the shape of a guard x264 carries and we do not. It cleared the per-MB nondeterminism (that was an uninitialised `dmv`, not an out-of-range one) and the guard is still unbuilt |
| `Y264_DIAG_NOHPEL=1` | force `y264_me_mc_luma`'s non-hpel fallback everywhere. Tests whether a result depends on WHICH of the two motion-compensation paths a block took, since the fast path is gated on a per-thread hpel registry |
| `Y264_DIRECT_PERMB=0` | restores the old frame-wide temporal-direct gate, for A/B against the per-macroblock default (which ships ON at every thread count since 2026-09-01). **Every `--direct temporal` measurement taken before 2026-08-31 was of the frame-gated mode, which engaged on 11-24% of B frames**, so none of them is evidence about temporal direct. The `=0` path is byte-identical to `77c8b46`, so it is also the control for the uninitialised-`dmv` fix |
| `Y264_DIRECT_LRVOTE=1\|2` | **per B frame, which direct derivation the LOOKAHEAD would pick**, simulated on the lowres field in mb-tree Phase B: `=1` scores each derivation by distance from the lowres search result, `=2` by the lowres SATD of its bi-prediction. Prints `lrvote poc= col= past= T= S= n=`; **dedupe by POC when aggregating** (the mb-tree window overlaps, so a frame prints several times). Verified md5-identical on/off. **CLOSED NEGATIVE 2026-09-01**: mode 2 separates the 16 labelled clips (rho -0.76) and then goes 0 for 3 on new predictions, rho +0.000 within one instrument -- keep it as the measured baseline, do not re-derive it. `docs/b-direct-mode.md` |
| `Y264_DIRECT_SCORE=1` | **per B frame, the direct prediction's SSD against the source and the macroblock count.** Run the same encode once per `--direct` mode and diff the rows: it answers whether a per-FRAME direct-mode choice could beat the per-clip one. t1; verified md5-identical on/off. **Its own validation says the signal is WEAK** -- coastguard predicts better under temporal and codes worse, samsung the reverse -- so read it as a bound on the cheap proxy, not as a selector. `docs/b-direct-mode.md` |
| `Y264_EST_SCRTRACE=1` | per-est-trial `(site, frame, mb, dist, j, lb_j)` trace for pricing ADMISSIBLE-SCREEN ideas offline (replay each MB tournament in call order against a running best) | priced the shape-3 screen family with no plumbing: dist-only fires 4.5%, the sign-count bits bound 6.5-7.5% while INADMISSIBLE (6% violations, since stored candidate nnz overcounts what est codes), losers-at-call-time ceiling 33.8%. t1 only; output verified md5-identical on/off |

## 2. What would a perfect version of X buy? (bounds and oracles)

These replay a recorded ideal so an arm gets a ceiling **before** it gets a
threshold. Every one must pass its own byte-identity check.

| instrument | bounds |
|---|---|
| `Y264_MBT_REC=<f>` / `Y264_MBT_PLAY=<f>` | mb-tree offsets: record every anchor's field **and the reference B's** (the walk has two outputs), replay both. Keyed on (POC, FNV-64 of the anchor's lowres), CRF-portable. **Replaying your OWN recording must be md5-identical**; that gate has caught three harness bugs, the third being this probe going stale against a ship on the day of the ship. Recordings carry a format magic, so a stale one says so |
| `Y264_SKIP_ORACLE`, `..._AT`, `..._SIDE` | what a perfect early-skip predictor would buy. Key is the ABSOLUTE display index, not POC: POC restarts per IDR and silently unions GOPs, which inflated a bound 1.98x -> 1.39x |
| `Y264_TYPE_ORACLE` | replay x264's frame-type placement through us. Their placement is +1.64% wall for us |
| `Y264_HEX_ORACLE` | ME search-shape bound |
| `Y264_UNSAFE_NO_EMIT` / `_NO_NAL` / `_NO_MBT` / `_NO_REFBWAIT` / `_NO_PREVPWAIT` | delete probes: what disappears if a whole stage does. **Gate these on output identity, never on an argument about consumers.** An unsafe delete once read FASTER than a byte-identical replay of strictly less work; deleting the est-CABAC-context copies read a reproducible 5.0% of samsung's wall where the byte-identical refactor collected 0.3-0.7%. The probe changed what the RD estimator saw, so the encoder made different decisions and did a different amount of work. A delete probe whose output moves is measuring a different encoder, not the thing deleted |

## 3. Is the arm any good? (quality gates)

| instrument | use |
|---|---|
| `ARM='<env>' python3 scripts/run_band.py` | **the standard self-A/B gate.** Band-calibrated ladders, both ABR and CRF, per clip, no single summary mean. The same ladder runs on both sides so rates match by construction. `BANDS=crf` runs only the band the standing rule gates on, for half the wall; `CLIPS=akiyo_cif,park_joy_720p` runs a subset, which is what an ABR gate aimed at two named clips needs |
| `scripts/bdcompare.py --vmaf --no-cache` | the underlying BD tool. **`--no-cache` is mandatory**: it caches by command string, so a rebuild silently returns stale numbers |
| `scripts/bd_at_rate.py` | BD at MATCHED ACHIEVED bitrate. Required for anything that moves the CRF axis, since mb-tree on/off translates it |
| `ARM='<env>' python3 scripts/band_at_rate.py` | **the whole CRF band at matched achieved bitrate**: `run_band.py`'s ladders and `ARM` / `ARM_ARGS` convention, driving `bd_at_rate.py` per clip, several clips at a time (`JOBS`), with a median/mean/worst summary. This is the gate for any arm that moves the operating point, where `run_band.py BANDS=crf` would measure ladder placement instead |
| `BANDS=deep ARM='<env>' python3 scripts/band_at_rate.py` | **the DEEP band (VMAF-NEG 55-83) at matched achieved bitrate**: the regime the standing band cannot see, and where the regime-shaped arms live. Ladders are solved onto x264's own deep curve by `scripts/calibrate_deep.py` (`ladders_deep.json`; 10 of 12 clips span it, ducks and park_joy honestly dropped). `BANDS=all` concatenates both ladders; the default `band` keeps every historical number comparable. Rungs print a model-derived `qp~` so regime knobs read in QP space. **The deep band's noise floor is MEASURED at ~+/-1.2 per clip** (a near-inert AC-gain 1.734 perturbation reads foreman -1.20, bus -0.91, akiyo +0.60), so a deep read inside +/-1.2 is the band talking, not the arm |
| `python3 scripts/m4_label.py --src-dir <dir> --out <jsonl>` | **per-sequence arm labels + lookahead features on an EXTERNAL corpus**, one JSONL row per sequence: BD-NEG for each M4 arm against the shipped default at matched achieved rate (the baseline's CRF points set the byte targets, each arm is binary-searched onto them), plus the four input-derived frame features (`tdiff`, `flat`, `tex`, `motion`) dumped by `Y264_PSY_FLAT_LOG` / `Y264_ADME_LOG`. Labels a third class, `default`, when no arm beats the default by 0.30%. Takes a `--src-dir` and has no path into `tests/corpus`, so the train/test split is structural: **the 12-clip gate corpus is TEST-ONLY, permanently.** Resumable (rows already in `--out` are skipped). ~7 s per 64-frame 480x272 sequence. Method: `docs/m4-selector-method.md` |
| `Y264_QP_TRACE=1` + `scripts/multishot_bd.py` | **where the bits go across SHOTS**: the trace prints every frame's slice QP, mean coded QP and mean intended QP (slice + offset map; on a skip-heavy shot the coded mean hides the allocation) with the mb-tree mean offset and the CRF DC term; `multishot_bd.py` encodes the S0 multi-shot sequences (`local/corpus/ms_*.y4m` + `.cuts`) at CRF 22-34 for any set of arms (our env/flags, x264) and prints BD-VMAF-NEG per pair plus per-shot VMAF and bit shares. The ten-clip board is single-shot by construction and cannot see an across-shot allocation gap: the 2026-09-05 record shows a 6-14% per-clip lead turning into a 5-13% deficit on the same clips concatenated |
| `scripts/calibrate_band.py`, `ladders.json`, `curves.json` | the band ladders themselves and the x264 curve they were solved onto |
| **the ladder shift**: rerun the same ABR arm at ladders +/-13% in rate | **the cheapest disproof of an ABR refusal, and it needs no extra tooling.** A real effect keeps its sign; the band's own chaos does not. `Y264_B_SKIP_EXIT=3` on akiyo read +0.96%, +0.25% and -5.75% across three ladders, which turned a would-be kill into a non-result. The ABR band's per-clip noise floor runs from 0.2 to 11 points, so measure it before recording any ABR refusal, and perturb the CURRENT default rather than an arbitrary 1.02 (the AC gain default is 1.7, so 1.02 is a 40% field change and measures the arm, not the floor; use 1.734 / 1.785). Corroborate with the **CRF low band**, points 32-41: it reaches the same QP regime with no rate-controller feedback loop, and akiyo read +0.00% there |
| `scripts/hf_probe.py` | **where the texture goes**: both encoders on the minimal CQP reproducer, per-band DCT energy retention (recon/src, LF/MF/HF zigzag rings) beside bits, PSNR and VMAF-NEG, split by the decoder's per-MB class and a source motion class. This pinned the "buys MSE vs buys texture" mechanism to the DEEP-QUANT regime (QP ~42-48). `HF_VMAF=1 ARMS="next-vs\|x264-med" python3 scripts/hf_probe.py bus_cif 100 30,36,42,48` |
| `scripts/hf_join.py` | the attribution half: per-MB 3x3 skip/inter/intra contingency of our class against x264's, with each side's MSE and AC retention per cell. The skip/skip cell is the reference-chain control: neither side coded it, so a gap there is inherited reference quality (foreman QP48: 142.8 vs 129.6 MSE on 64.9% of MBs) |
| `scripts/cvbr_compliance.sh`, `scripts/vbv_check.py` | VBV and capped-VBR compliance: violation counts and buffer traces |
| `scripts/ladder.py`, `scripts/crf-solve.py` | ladder construction and the CRF point that hits a target rate on a given clip. `crf-solve.py` PINS its operating point per (clip, args, target, tier) in `tests/.crfpin`; see the first trap in section 7 |

## 4. Is it fast? (wall harnesses)

**Never price an arm with a table delta.** Our CRF is a staircase, so a changed
field re-picks rungs and two table runs are not at the same operating point.

The pattern every wall measurement here follows: several arms measured
INTERLEAVED round-robin against the baseline, medians of 5 or more, **with a
duplicate-base control arm**, all timed runs writing to `/dev/null`. A drifting
box then moves all arms together instead of whichever one it happened to hit,
and the control says whether the reading is usable at all. Measure several arms
in ONE batch: a knee measured one invocation at a time came back non-monotonic
in the gate's own strength while the same arms in one batch ordered correctly.
Price every threaded arm at `THREADS=12`, and state the tier, because a
pure-C reading and an as-shipped reading answer different questions.

| instrument | use |
|---|---|
| `scripts/perf-comp.sh`, `-set`, `-crf-set`, `-purec`, `-modes` | the comparison sets against x264. `-purec` carries the autovec-fairness fix. `PERF_REPEAT` knobs drive the repeated-sample timer that resolves the short CIF cells |
| `Y264_REFENC_CACHE=0 make parity-status` / `parity-status-crf` | the tables. CRF is the one to quote for speed. Timing is `perf_counter` with the two arms interleaved, and the same binary twice reads within 0.01 |
| `scripts/parity-clips.sh`, `scripts/parity-clip-calib.sh` | the table's clip set and its per-clip calibration |
| `scripts/ffboard.py` | **the control for "is the table scoring our Y4M reader?"** Both encoders called as libraries inside ONE ffmpeg process: one demuxer, one thread pool, no CLI on either side. It answers the input-path question rather than replacing the table -- run against `parity-status-crf` at the same points the two agree within noise on every clip, so the input path is not where the gap lives. Needs an ffmpeg built with libyah264 (docs/ffmpeg-integration-plan.md). **Point `X264LIB` at a libx264 whose `-fno-tree-vectorize` was stripped** or the pure-C arm reads our vectorized C against their scalar C, worth a third of goal 2 |
| `./build/tools/checkasm/checkasm --bench` (the `sad_x4` / `satd_x4` rows) | **whether BATCHING a kernel pays, before anything is wired to it.** Each row is the batched form against four dispatched singles. It prices the whole fuse-the-calls family in one command, and the answer is load-boundedness: SAD reads 1.60-1.80x, SATD reads **1.01x**. Read only the sizes where BOTH forms are NEON: 8x4 mixes a NEON batch against a C single, and 4x4/4x8 have neither |
| `nm -gU` on both shipped binaries | **the SIMD coverage gap as a job list**, with no source reading and no provenance question: 198 x264 8-bit NEON kernels against our ~53, and twenty families where we have nothing, several sitting on measured hot spots. Ranked in milliseconds per job (multiply each side's percentage by its own wall first, since percentages are not comparable across two binaries), we LEAD on subpel refine (0.62x), SAD (0.69x) and chroma MC (0.26x) and lose 9.3x on deblock |
| `Y264_GPU_PHASEA=1` + `Y264_GPQ_WARMUP=10000` / `Y264_GPQ_CONSUME=0` | **the per-process Metal floor and the gpq tax split.** `WARMUP=10000` arms the per-push phase-A offload but never submits a job, so only the background `ngc_open` runs, and it reads bus +16 ms / stefan +17 ms of wall: the 12-17 ms bring-up floor any Metal-armed arm pays PER TABLE CELL (device init, not shaders; a precompiled metallib does not move it). `CONSUME=0` submits rounds but never reads them, splitting chain-side from walk-side cost |
| `python3 scripts/knob_census.py` / `--check` | **the `Y264_*` knob census** (`docs/knobs.md`, generated): every env knob's reader, default and tier (shipped default / instrument / kept arm). `--check` runs inside `make test` and fails on an uncatalogued knob, a catalog entry whose reader was removed, or a comment claiming "default OFF/ON" that contradicts the code default. Latest census: 262 knobs = 95 defaults + 67 instruments + 100 arms, with zero harnesses arming nonexistent knobs |

## 5. Is it correct? (the gates every ship passes)

```
make test                                   # unit, 9/9
make conformance                            # recon-match, 254/254 (YAH264_CONF_FAST)
python3 scripts/stress_threads.py           # hang/determinism under load
scripts/tsan_catch.sh                       # TSan; the floor is 0 reports
python3 scripts/env_gate_audit.py           # every lazy env static: TRAP + COLD, exits 1 on TRAP
ARM='<env>' scripts/recon_sweep.sh          # recon-match ONE ARM across a config matrix
scripts/determ_repeat.sh                    # SAME config, N times -> must be one bitstream
ARGS='--direct temporal' scripts/determ_repeat.sh   # ...and it takes MODE flags too
scripts/hygiene_check.sh                 # licences, stray patches, home paths, stray asm
scripts/abr_decode_gate.sh                  # decoder-side gate for the THREADED ABR path
```

`env_gate_audit.py` exists because the TSan floor is not naturally zero. The
encoder resolves its ~200 `Y264_*` knobs through lazy function-local statics,
and the CLI opens one encoder per GOP from concurrent workers, so every gate
that is not resolved on the main thread first first-touches on whichever worker
gets there. The races are benign same-value init, but they FOG real hunts --
the 4:4:4 MC border overflow (08-29) sat behind four of them. The audit walks
the call graph from `warm_lr_statics` / `ntp_pool_create` / `ntp_bg_create` and
names two failures: **TRAP**, where the resolved value still satisfies the
guard so the gate rewrites its static on every call (a perpetual writer no warm
can quiet -- `Y264_WF_CAPK` resolved "unset" to `-2` under a `v < 0` guard and
did exactly this), and **COLD**, where it latches but nothing warms it. Run it
after adding a knob. Two things it cannot see, both of which have bitten: a
static resolved **inline inside a hot function** is unreachable by any warm and
has to be hoisted into an accessor first, and an **unconditional store** in a
warm function (`y264_tl_on = trprof_on();`) is a perpetual writer that is not a
getenv guard at all. Read-compare-write, not a plain store, in anything a warm
touches.


**Run `determ_repeat.sh` on a LOADED box.** On an idle one it passes against a
default that emits six distinct bitstreams per eighteen runs. The mb-tree
Phase-A warm shipped that way and a clean 16/16 pass the same morning missed
it; six busy-loop spinners in the background made it fail on four
configurations within twelve runs each. The defects this gate exists for are
timing-dependent, and an idle 18-core box runs the threaded path the same way
every time. Interleave the two arms so both see the same load, and treat the
count of distinct md5s, not pass/fail, as the reading:

```sh
pids=; for i in 1 2 3 4 5 6; do (while :; do :; done) & pids+=($!); done
scripts/determ_repeat.sh; kill $pids; pgrep -f "while :" && echo "LEAKED SPINNERS"
```

**Gate the MODE, not just the default.** `determ_repeat.sh` and
`stair_determ.sh` both encode with the shipped defaults unless told otherwise,
so a defect confined to a non-default coding mode is invisible to the whole
battery. Per-macroblock temporal direct was irreproducible above one thread for
a week while every gate read green, because nothing in the tree ever ran
`--direct temporal` under repetition: `ARGS=` (here) and `STAIR_DETERM_ARGS=`
(stair_determ) are the slots for that. Both scripts keep env in a SEPARATE slot
on purpose -- an encoder flag passed to `env(1)` makes it reject the command, all
runs then produce nothing, and the nothings have matching md5s.

**Kill by PID and VERIFY, never `kill %N`**: in non-interactive shells some
job-spec kills fail silently, and ten leaked spinners (2 per gate, accumulated
over a day) once pinned the box at load ~14 and corrupted every speed number
for ten hours, including two full table runs. **Check `uptime` before any timing
session.** Band and BD reads at matched rate are load-immune; walls and tables
are not.

`make conformance` gates the DEFAULT path. A coding tool behind an env knob has
its own conformance surface, the cross product of ref count, B-frame count,
entropy coder, direct mode and transform, and `recon_sweep.sh` is that cross
product (6 clips x 5 QPs x 10 configs). It deletes its outputs before every
iteration and checks the encoder's exit code, because the failure it was
written after is a rejected config leaving the previous iteration's files in
place and scoring them as a pass.

`scripts/abr_decode_gate.sh` encodes the table ABR shapes at t12 and asserts
that every input frame decodes AND that mean PSNR clears a floor (25 dB; broken
emission reads ~15, working reads ~30-42). It exists because a broken threaded
ABR path passed the whole rest of the battery: conformance cannot reach the
threaded path (`--dump-recon` forces the serial streaming path), the CRF band
never runs ABR, and identity gates compare an encoder with itself. `ARM='<env>'`
gates an arm.

Plus, by hand for any default flip: the escape env must reproduce the OLD
default md5 exactly, the new default must equal the explicitly-armed md5, and
`t8 == t18 == t8-noasm`. (samsung ABR has a known, documented t1-vs-t8
divergence; reproduce it with the escape armed before blaming your change.)

## 6. The reference side

The comparison harnesses drive external baselines through environment
variables, none of which the repository can provide:

| variable | used by | what it must point at |
|---|---|---|
| `scripts/san_matrix.sh` | **the encoder under ASan/UBSan over 21 edge inputs** (odd sizes, 1-2 frame clips with B, keyint 1, qp 0, extreme bitrates, 4:2:2/4:4:4, CBR, 2-pass, direct temporal, the hardware backend, 1-12 threads). Builds `build-san/` once; ~3 min; exit status = cases with reports. Found two memory bugs on 2026-09-04 that recon-match could not see |
| `X264_ASM`, `X264_C` | `scripts/perf-comp.sh`, `scripts/instr-ratio.sh`, `scripts/crf-solve.py` | an x264 CLI with assembly on / a pure-C build with assembly off and the compiler's vectoriser left on (the fair build; a stock `--disable-asm` build is a scalar strawman) |
| `X264` | `scripts/cvbr_compliance.sh`, `scripts/ladder.py` | an x264 CLI |
| `X264LIB`, `Y264LIB` | `scripts/ffboard.py` | installed prefixes of libx264 and libyah264 for an ffmpeg that links both |
| `FF` | `scripts/ffboard.py` and the row scripts | that ffmpeg (default `/tmp/ffmpeg-yah264/ffmpeg`) |
| `OPENH264` | `scripts/openh264-shim.sh` | an openh264 `h264enc` binary |

The clips come from `scripts/fetch_corpus.sh` (the board's ten and the wider
corpus; see docs/corpus-sources.md for what is and is not fetchable). Notes
about instrumenting the reference encoder itself stay out of the public tree;
they live with the measurement records.

## 7. Traps that have each cost a round

- **The table's dVMAF is only comparable across builds if the operating point
  is pinned.** `crf-solve.py` cached the solved point keyed on **the BYTES of
  the binary**, so any code change, including a byte-identical one, forced a
  fresh solve, and our CRF staircase (~13% of rate per rung) let the fresh
  solve land on a neighbouring rung: two builds with identical output on all
  six table clips read **-0.56 against -0.49**, differing only on foreman and
  bus at 411 vs 409 and 379 vs 375 kbit/s. The operating point is now PINNED
  per (clip, args, target, tier) and not per binary (`tests/.crfpin`,
  `Y264_CRF_PIN=0` bypasses), the pin is resolved BEFORE the solve cache and is
  part of its key, and every solve prints `pin_state` and `pin_drift_pct`.
  **Read `pin_drift_pct` before reading dVMAF**: nonzero means this build's rate
  curve moved and the comparison shifted with it.
- **A timed output write does not cancel out of a ratio, and it fakes a
  resolution-shaped effect.** It inflates the BASELINE, so the arm's cost as a
  percentage comes out too small, and most so where the bitstream is biggest.
  The same park_joy encode reads 1.63 s to `/dev/null` and 2.1-10.2 s writing
  its 9.5 MB bitstream to the temp directory, back to back on an idle box, and
  a spread guard does not always catch it (one cell read 3.2x its true wall at
  a spread of 1.02). CIF cells (~1 MB) barely show it, which is how it
  survives. That alone produced a recorded finding that our B_8x8 arm cost
  x264's price on CIF and twice it at 720p; with the write removed the ratio is
  flat and the worst cell is CIF. **Time to `/dev/null`, and give `--md5` its
  own untimed encode.**
- **A wall harness that defaults to the PURE-C tier prices the wrong tier for
  any as-shipped question**, and the only tell is a `tier pure` token in a
  header line. A NEON kernel's first A/B read +0.03 to +0.32% and looked like a
  failure, on a tier the kernel does not exist on.
- **Refuse to compare two builds whose `optimization` differs.** A meson -O2
  build against an -O3 build once read as a 2-3% phantom win.
- **Put both arms of a throughput test in ONE burst with mirrored launch
  order.** The box's load moves slower than a burst and faster than a round.
- **`Y264_MBTREE_OFF` is a dirty control.** It swaps the per-MB offset to a
  different AQ array, not just propagation. Use `Y264_MBTREE_STRENGTH=0` for a
  clean propagation on/off. The tell was a "gain" of -5.75% at strength 0.
- **zsh does not word-split scalars.** Never build CLI args in a string
  variable; verify A/B configs produce DIFFERENT md5s before trusting a null.
- **Verify the probe is in the binary you time.** One clean null came from a
  probe-less build.
- **A guard that reads an unpopulated signal disables the thing it guards** and
  prints a clean `+0.00%` meaning "never ran". The tell: output byte-identical
  to baseline rather than to the un-guarded arm.
- **Check what a substituted field is interpreted AS.** `mb_qp_pre` reads the
  COMBINED x264-style offset (AQ folded in); x264's dump is the mb-tree term
  alone. That mismatch inflated a load-bearing result by more than 2x.
- **Keying a probe on POC silently UNIONS GOPs, and it has cost two rounds.**
  POC restarts at every IDR, so a set keyed on `(POC, ...)` matches entries
  belonging to different frames. It inflated the skip oracle's bound
  1.98x -> 1.39x, and it invented a 66% "recompute" rate for the mb-tree
  phase-A memo that did not exist: a 3-way associative memo was built on it,
  came back inert (miss count 277 -> 277), and was reverted. **The tell is in
  the probe's own output: the false count tracked the I-FRAME COUNT** (bus 1
  I-frame / 0 recomputes, ducks 2 / 81, pjoy 3 / 300, samsung 5 / 183), and the
  one single-GOP clip, the only one where the key could not be wrong, was the
  one reporting nothing to win. Key on an absolute display index, and when a
  new probe disagrees with an existing one, suspect the new one first.
- **BD reads are sweep-dependent.** Check the saturation flag; a reference
  curve too flat in the band makes the number meaningless (touchdown's slope is
  2.27 and its rows have read +341269% before).
- **Nothing else re-runs one configuration against itself.** `stair_determ.sh`
  compares ACROSS thread counts and `w2_canary.sh` against a reference build;
  neither catches a default that emits a different bitstream every run. One did
  for a day (3-5 distinct md5s per 12 runs at foreman `--ref 1` t18) and no
  battery noticed, which also made every md5 identity gate in the tree a
  lottery ticket for that day. `scripts/determ_repeat.sh` is that gate, and it
  is verified by failing against the binary it was written for.
- **A "byte-identical by construction" argument is not a gate, and this tree
  has shipped two of them that were false.** The A1 pair seed and the mb-tree
  Phase-A warm both carried a paragraph of reasoning where a measurement
  belonged; both emitted several distinct bitstreams per twelve runs. When a
  feature's whole claim is that it does not change the output, the claim IS the
  gate: run it, under load, against the escape.
- **TWO code paths reading the same settled bound from different origins will
  disagree.** The warm's window offsets are measured from the ring head, the
  walk's from its anchor `d` entries further in; the same source is `laoff` in
  one and `laoff - d` in the other, and one bound cannot serve both. The tell
  was that turning off BOTH settled-bounded readers
  (`Y264_MBT_BLEG_REUSE=0 Y264_MBT_PAIR_SCALE=0`) made the two paths agree
  exactly while turning off either one alone did not.
- **TSan can be silent on a publish-before-write.** The flag advertising "these
  motion fields are ready" was set before the batch that computed them, and the
  write usually landed first, so TSan saw no race on most runs. The
  repeated-run md5 loop caught it; TSan only confirmed it afterwards, on the
  OTHER reader.
- **TSan is silent on a stack overflow.** B_8x8's output was nondeterministic
  run to run under threads and TSan found nothing, because the cause was a
  stride-16 motion-compensation write into a 64-pixel buffer, not a race. When
  run-to-run variance survives every concurrency knob being turned off, stop
  looking for a race and look for memory being written past its end.
- **A stale output file scores as a pass.** An A/B loop that writes to a fixed
  filename and then compares it re-scores the PREVIOUS iteration whenever the
  encoder writes nothing, which is what an unknown flag does (it exits 2 and
  prints help). Delete the outputs at the top of every iteration and check the
  exit code. This is the zsh word-split trap's second half: the scalar arrives
  as one argument, the CLI rejects it, and the harness reports the arm you
  never ran as clean. It void'd a B_8x8 bisect and sent it after the wrong
  subsystem.
- **A ship can break a probe silently.** The mb-tree replay oracle went stale
  the day `Y264_MBT_BREF` gave the walk a second output, and the next session
  to use it got a VOID on every clip. Run a probe's own identity gate FIRST,
  every time, and when a feature adds an output, ask which recorder carries it.
- **This box cannot time a 720p cell reliably while a desktop video process is
  ACTIVELY DECODING.** With `WallpaperAerialsExtension` and
  `VTDecoderXPCService` running, the same park_joy encode to `/dev/null` reads
  1.63, 1.93, 4.28 and 5.24 s within minutes while the CIF cells hold to a 1.03
  spread. Check a 720p cell by hand before trusting a 720p row.
  **RESIDENCY IS NOT THE CONDITION, and reading it as one cost a round.**
  `WallpaperAerialsExtension` is resident essentially always and burns
  8.7-13.6% of ONE core when no wallpaper is displayed. At that level the
  sanity cells reproduce the reference table to the hundredth on both tiers
  (ducks t18 pure-C 1.84/1.82/1.81, foreman t1 0.80/0.79/0.79), and suspending
  it changes nothing. Gate on the SANITY CELLS, not on `ps` output.
- **`ps` `pcpu` is a DECAYING AVERAGE, not an instantaneous sample, and it lies
  after a `SIGSTOP`.** A suspended process stops being scheduled, so the
  kernel's estimate freezes at whatever it was and reports that value
  indefinitely after `SIGCONT`; it once read 47-55% for a process actually
  using 13.6% of a core. Worse, `pkill`ing a system extension makes launchd
  relaunch it, and the relaunch burst inflates the estimate, so killing a
  process to quieten the box and then reading `ps` measures your own
  perturbation. **Ground truth is a cumulative-CPU-time delta**, which needs no
  interpretation:
  ```sh
  a=$(ps -o time= -p $pid); sleep 10; b=$(ps -o time= -p $pid); echo "$a -> $b"
  ```
- **A BD-rate number is not a bandwidth saving, and it scores only half of what
  an arm does.** BD-rate is the mean horizontal gap between two RD curves,
  integrated with UNIFORM weight over a quality interval. Two consequences,
  both of which bite when a result leaves this repo:
  1. It weights every operating point equally. Real ladders ship specific rungs
     to specific audiences, and the gap at those rungs can look nothing like
     the average. "x264 veryslow beats medium by 9.3% BD" does not mean any
     particular deployment saves 9.3% of its bandwidth.
  2. It converts the entire difference into RATE at matched quality, but a
     shipped ladder holds the rungs fixed, so the difference actually arrives
     as a MIX of a little rate and a little quality, and BD keeps the rate half
     and discards the quality half. Which half you collect depends on where
     your audience sits: near the top of the ladder it shows up as bitrate,
     lower down it shows up mostly as quality.

  Measured here: the corrected mb-tree splice on bus reads **+3.90% BD**, which
  sounds like "spends 3.9% more bits". Per rung it spends **0.45-1.58% FEWER**
  bits and gives up **0.43-1.00 VMAF**. The BD number is a quality loss
  re-expressed in rate units. `scripts/run_band.py` prints
  `rungN: dRate / dVMAF` beside every BD line for exactly this reason: **quote
  both halves, and never translate a BD number into a bandwidth claim.** But
  read the rung columns only in a self-A/B, where both sides run the same
  ladder on the same encoder. Against x264 the two CRF scales are different
  operating points, so the columns show the scale mismatch rather than an
  efficiency difference (bus reads +42-45% rate / +2.4-4.5 VMAF at every rung
  against x264) and the BD number is the one that normalises it. The two answer
  different questions and each misleads in the other's setting.
- **A speed number is a number about ONE OPERATING POINT, and the curve runs
  from 1.00x to 1.36x on a single clip.** Measured on bus_cif at four solved
  matched points: 1.36 / 1.18 / 1.06 / 1.01 at 144 / 375 / 1252 / 3485 kbit/s,
  with dVMAF -2.78 / -1.58 / +0.01 / +0.00 beside it. ducks_720p runs the same
  curve in reverse (1.18x at crf 35, 0.93x at its table crf 23), so **the clip
  we "win on" is the low-QP end of the same line, not a different path.**
  Before attributing anything to a clip, check whether you are looking at its
  rate.
- **The corpus median and the TABLE are not the same number, and they have
  differed by four points.** Full corpus BD against x264 read -0.85% while the
  table's own six clips read +3.54%, because the corpus median was carried by
  touchdown, sintel, coastguard and akiyo, none of which the table scores. A
  plan built on "we are at parity now" from the corpus number is planning
  against the wrong set. Split by table membership before drawing a conclusion.
- **Before refusing an arm on wall, price the same arm on THEIR side.** B_8x8
  plus B_RECT sat refused for a day at "10-20% wall" against a reference
  encoder that ships the identical feature at medium and is faster than us
  anyway. Priced properly, it costs x264 4.6-10.6%. The refusal was never about
  the modes; it was an implementation gap nobody had looked for, because the
  number had no denominator.
- **A t1 speed number does not predict its t12 value, and the sign of the error
  is not constant.** Three arms measured the same day: `Y264_P_SKIP_EXIT` went
  0.6-2.2% at t1 pure-C to **exactly null** at t12 as-shipped;
  `Y264_PART_EARLYTERM=3` kept everything; the byte-identical coverage round
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
  the CIF cells run 0.05-0.11 s at 12 threads, so bus_cif (ours 0.108, theirs
  0.082) can only print 0.11/0.08 = 1.375 or 0.10/0.08 = 1.25, which is exactly
  the "1.22x and 1.38x on two runs of the identical config" once recorded as
  noise. Each such row carries a 9-14% quantum, and **the worst-clip metric is the one
  this corrupts most**, because MAX picks whichever row rounded up.
  The 720p rows are fine. Timing is `perf_counter` with repeated samples now;
  do not reintroduce a two-decimal timer, and do not time side A five times and
  THEN side B five times, which puts a drifting box's whole drift in the ratio.
- **This box is HETEROGENEOUS and the ideal speedup is not the core count.**
  M5 Max: `hw.perflevel0.physicalcpu` = 6 "Super" plus `hw.perflevel1` = 12
  "Performance". Twelve threads is not two thirds of eighteen threads'
  capacity; the ideal 12 -> 18 speedup is about 1.43x. Check
  `sysctl hw.perflevel*` before calling a scaling number a defect.
- **Per-unit cost inflating with thread count is not automatically your bug.**
  Both encoders pay it here (ours 1.62x, x264's 1.28x on foreman at t12). The
  only way to assign it is N independent single-thread processes of the SAME
  binary, which share nothing, so the curve is that binary's own machine floor.
  x264 threaded sits at or below its floor because its frame threads share
  reference memory; ours sits 15-27% above ours. A raw inflation number without
  that denominator says nothing.
- **`sample` on x264 fed a large y4m reads ~49% `__mmap`/`__munmap`, and taking
  its coverage share raw halves it.** x264 mmaps its input where we stream
  ours, so a looped multi-GB clip puts the file in its profile and not in ours.
  On bus at t12 their NEON share reads 24.9% of all work samples and **48.9% of
  the non-I/O half**, which is the comparable number. Rescale off the mmap base
  before comparing the two encoders, or the coverage gap appears to close on
  its own. Coverage profiles also need an encode long enough to sample: bus_cif
  is 150 frames and too short, so loop it.
- **An env knob that overrides an AUTO default can override it DOWNWARD**, and
  then the arm is a regression the knob did not cause. `Y264_LA_BUF=1` was
  recorded twice as "a 1-5% regression nobody has diagnosed" and once as the
  broken prerequisite blocking a route worth 4%. The shipped default already
  resolves `la_buf = bframes+1 = 4`, so `=1` was cutting three frames off the
  encoder's own lead; the cost is monotone in what the knob removes (samsung 0
  -> -6.41%, 1 -> -5.01%, 4 = the default, reading -0.14% because it IS the
  baseline). Before pricing a knob, print what the default resolved it to.
- **Read EVERY line of a per-encoder stat dump, not the first.** The CLI opens
  a pool-less probe encoder that prints before the real one, and its
  `Y264_LA_STAT` line says `pool=0 la_buf=0 la_th=0` on a default that runs
  `pool=12 la_buf=4 la_th=1`. Reading line one says the feature is off when it
  is on.
- **Two instruments in one dump that disagree by 15x is the finding.** An audit
  resolved `tail 31.1%` against `pool-empty 28%` by picking the bigger name and
  dismissing the other. The bucket was wrong (it billed a whole sleep to the
  class sampled at its entry) and a plan was written on it.
- **SME/SME2 is closed on every form; do not reopen it.** It is reachable only
  with `-mcpu=apple-m4` or `native` (generic `-march=armv9-a+sme2` compiles and
  SIGILLs). 512-bit streaming vectors buy **1.08x** over 128-bit NEON, and
  `smstart` plus `smstop` costs **9.6 ns** against a 3.7 ns satd8x8, so
  per-block use is impossible. Batching whole-frame SATD through the ZA
  outer-product engine as two matmuls, bit-exact, with one streaming region per
  frame, reads **0.21x NEON** (18.9 vs 3.89 ns per 8x8), and the peak-rate
  instruction floor is itself below NEON parity (SMOPA peak 0.47 ns, MOVA
  0.31 ns, tile write-to-read round trip 7.33 ns).
- **Any Metal-armed arm pays the 12-17 ms per-process bring-up floor per table
  cell**, which is device init rather than shaders; a precompiled metallib does
  not move it. Dispatch latency is not the problem: the lookahead's per-frame
  pixel work (scale and costmap on a 720p frame, end to end including submit
  and fence) runs at 0.24-0.26 ms each against the 3.5 ms/frame the lookahead
  and mb-tree actually burn, ~7x headroom. What closes the route is the
  inter-round idle gap and the DVFS ramp: every hot-clock number is a lie about
  an anchor-cadence workload, and a sleeping warmer that reads flat ONCE is not
  evidence, since that failed reproduction twice.
