# What shipped in yah264, and what each piece bought

A ledger of the work, 2026-06 to 2026-09-06, written so a sibling encoder
can run the same programme: same goals, same gates, same order of attack.
Numbers are the ones recorded when each item shipped (BD-rate on VMAF-NEG at
matched achieved bitrate unless said otherwise; "band" = the 12-clip CRF
band at matched rate; "board" = the ten-clip rate-matched speed board).
Where an item was tried and refused, it is listed too, because not
repeating a dead end is half the programme.

## 1. The goals and the instruments that judge them

**Goals** (docs/plan.md, site/results.md): three speed goals against x264
medium at one pinned matched operating point, each with four legs: median
wall <= 1.00x, worst clip < 1.15x, quality within 0.5 dVMAF, size within
1.0%. Goal 1 pure C single thread, goal 2 pure C multi-thread, goal 3 as
shipped (NEON) multi-thread. Read 2026-09-05: G1 0.92x / 1.15x, G2 0.84x /
1.08x, G3 0.96x / 1.15x, dVMAF +0.22, dsize +0.1%. Quality against x264 on
the CRF band: CIF -5.7%, 720p -14%, 1080p +0.4%; deep band (VMAF-NEG 55-83)
9/10 clips, median about -12%.

**The PSNR-Y floor**, added 2026-09-14 (owner) as a fifth reading on the same
legs and the first one that is a floor rather than a bar to reach: PSNR-Y at
equal bytes against x264, FAIL at more than 1.0 dB below on any clip, 0.5 dB
below recorded as debt and listed rather than gated. It exists because the
H.265 sibling's campaign found VMAF-NEG and PSNR disagreeing on nine of
fifteen clips while the level gap at equal bytes stayed small (median -0.08
dB, worst -0.87), which is exactly the shape in which a future item buys
VMAF-NEG by letting pixel accuracy slide and no column notices. Nothing is
decided on it; the quality leg is still dVMAF. It costs no extra encode:
`--feature psnr` rides on the libvmaf pass that already scores the dVMAF
column, and the pooled VMAF mean is bit-identical with it on, so the older
columns reproduce to the digit. First read (CRF at matched achieved bitrate,
ten clips, pure C): median -0.07 dB, worst bus_cif -0.47 dB, floor PASS,
debt list empty. Read it on the CRF board rather than the ABR one -- ABR
asks both encoders for the same rate but does not deliver it, so "equal
bytes" is only true where the solve makes it true. One adjudicator,
scripts/psnr_leg.py, so the CLI boards and the ffmpeg board cannot drift to
different verdicts on the same numbers.

**Gates every ship passes** (docs/instruments.md section 5): recon-match
conformance against ffmpeg across QPs, chroma formats, bit depths and
geometries (318 cells fast, more full); repeat determinism at fixed thread
count (16 configs x 12 runs); ASan/UBSan matrix over 21 edge inputs;
checkasm for every SIMD kernel; unit tests incl. the public API; the knob
census (every env knob documented); the env-gate audit (no cold statics
first-touched on worker threads); CRF byte-identity against main when a
change claims to be inert; the band at matched achieved bitrate for any arm
that moves the operating point; the board for anything that claims speed.

**Instruments** (docs/instruments.md): the in-process ffmpeg board (both
encoders as libraries in one process, one demuxer), band_at_rate.py /
bd_at_rate.py (matched achieved bitrate, the only honest BD), bdcompare.py
(quick 3-clip BD), the deep-band ladders, per-caller thread profiler,
oracle/ceiling probes (perfect early-skip, perfect selector), the multi-shot
BD gate with x264 as control, QP_TRACE (per-frame intended vs coded QP),
shot_determinism.sh. The methodology rules that cost a round each: zsh does
not word-split (build A/B args in bash); verify A/B md5s differ; never
rebuild while a harness runs; never adjudicate a margin under the board's
own rate tolerance; a delta is not a function's cost; price an arm on the
reference's side too.

## 2. Rate-distortion tools (in the order they were worth building)

| item | what | bought | notes |
|---|---|---|---|
| RDOQ (trellis quant) | greedy with exact pixel distortion, elimination + bidirectional level nudging, on all block types; bit-estimate path for the RD loop | large (the single biggest quality step); round-to-nearest seed -2.83% corpus | a coefficient-domain Viterbi was NOT the path here (exact distortion beats separable approximations); DC-block RDOQ refused (>1 dB loss) |
| CABAC-accurate RD rate model | mode decision on a CABAC bit estimate with context state, not tables | the last 8% of the RD gap | est-path build; cost dominated by the per-candidate bit estimate |
| Psy-RD | 4x4-SATD-AC + 8x8-SA8D-AC energy term in mode decision, strength 2.0 | -0.9/-0.3/-0.8% NEG on the calibration clips; flat 2.0 shipped | QP-ramp form refused; band is the flip authority |
| Trellis intra lambda (Q1) | QP-ramped intra lambda | shipped; regime-shaped arms want the ramp form | |
| Psy-trellis | reconstruction-spectrum term inside the Viterbi lattice, gated on flat-content share (sintel-only default) | sintel -8.99 low band / -3.21 matched | class is FLAT/DARK, not grain; the greedy-path gate refused on wall (+30%); calm gate default-off |
| Rate-aware partition decision | partition SATD with a rate term; rect refs only on their 8x8 halves' refs; transform-size pre-decision (confident 4x4 only) | rects: t12 -3..-7.5% wall at +0.06% band; TR fix -1.30% band | PART_EARLYTERM is a rate trade, bands decide |
| B_8x8 / B direct auto | B_8x8 at QGATE 10; per-slice direct spatial/temporal by a sampled skippability score | B_8x8 -1.93% 12/12; direct auto station2 -30 / stockholm -7 on the band | QGATE 6 refused (0.07% BD for 2.7% wall); per-shot direct selector CLOSED negative |
| B intra admit margin, B skip exit | admission margin 12; B_SKIP_EXIT 3 | shipped | perfect early-skip oracle worth 11-17%; every admissible screen reaches ~8% of it; content-adaptive skip gates closed (cost BD on animation) |
| dct-decimate | thresholds 3/2, lighter than the reference's | shipped | |
| Weighted prediction | frame-level DC fade detection | shipped | |
| Deblock | full, with the list-1 refIdx term | shipped | |

## 3. Motion estimation

| item | what | bought | notes |
|---|---|---|---|
| Quarter-pel ME | hex and UMH, median MV prediction, subpel tiers per preset | baseline | medium = hex |
| Multiple references | across IPPP, flat-B and B-pyramid | baseline | multiref gotchas recorded |
| Bounded qpel-RD refinement | hysteresis-gated +-1 quarter-pel around the winner | shipped | |
| ME lambda calibration | to the exponential lambda table | shipped | |
| Half-pel plane reuse; lookahead anchor reuse | frame-wide half-pel planes reused; anchors reuse the lookahead field | anchor reuse t1 -1.1..-1.6%, t12 -0.6..-1.3% at +0.04 band | |
| Lookahead probe metric | SAD probes in the three lowres searches, SATD final | SATD 18.1G -> 10.0G px | |
| Level MV clamp | MV range to the level's limits, temporal-direct legality | codec item, 2026-09-04 | |
| Refused | ME early-out family (rescues exhausted), emergent ME, insurance-RD admission gate as default | | listed so they are not retried |

## 4. Rate control

| item | what | bought | notes |
|---|---|---|---|
| The family | CQP, CRF, ABR, two-pass, VBV/capped VBR | baseline | cvbr_compliance read 34/36 at both windows when this shipped; STALE, the current read is 29/36 (2026-09-03, docs/rate-control.md), against the reference's 18/18 over the 18 cells its own feature set covers |
| mb-tree | lookahead propagation over the rc-lookahead window, per-MB offsets combined with AQ, non-centred under CRF/ABR; AC gain 1.7, MB lambda 5, reference-B field | the whole quality gap closed (-0.85% median residual); sita P-half +10.66 -> +5.89 | consumption jointly adapted: do not re-sweep one constant |
| Variance AQ | auto-variance mode 2, strength 0.4; absolute anchor for the anchors' field | shipped | AQ 1.0 loses ~5% everywhere (2026-09-06 oracle) |
| CRF complexity device | absolute AQ anchor + the frame-duration term; the mb-tree operating-point shift | -5.17% median (anchor 4.5) | |
| The across-shot term (2026-09-05) | the AQ frame-mean at the reference's strength on the CRF base QP, B via cascade | multi-shot -8.2 / -4.1 / -6.2% vs the old default; band median +0.30 | found by the multi-shot gate; the board cannot see it |
| ABR opening | lookahead-fitted opening QP; I credited at its own QP | t12 median rate error 12.5 -> 6.6% | rate error is content on the old corpus, not a defect |
| RCP_LAG / rate-factor controller | in-flight predictor, ring, warm phase, nodrain | ABR wall and accuracy | the warm phase refused as a trade (shields +4% BD) |
| Refused | CRF mb-tree compensation, QP-ramp psy, a magnitude clamp on mb-tree offsets (changed CRF output; the span window shipped instead) | | |

## 5. Threading

| item | what | bought | notes |
|---|---|---|---|
| GOP-parallel encoding | independent GOP instances across workers, longest-first queue, per-GOP frame-thread share | the multi-thread speed goals | cut-split GOPs opt-in |
| Row wavefront | hybrid GOP + row model, broadcast herd fixed, deadlocks fixed | goal 3 | design LOCKED; mb-tree barrier is the remaining serial share (4% at t12) |
| W2 pipelined emit, fpipe, staircase | entropy emit trails analysis; reference staircase | t12 wins | per-caller wait profiler; settled bound is the lead |
| Lookahead thread / lead | decoupled lookahead with a lead | | pool minimum unwound 8 -> 2 |
| Determinism | byte-identical across runs at fixed thread count; thread-variant output is fine (owner) | | TSan blind to two real races; the determ gate needs load |

## 6. SIMD

| item | what | bought | notes |
|---|---|---|---|
| NEON kernels | MC (half/quarter-pel), SAD via UDOT (i8mm/dotprod), SATD 8x8/16x16 direct, 4x4 DCT/IDCT, deblock, dequant, ssd, quant | ~4.2x on the kernels; goal 3 | checkasm for every kernel; hand asm ties intrinsics on every matched shape (40-kernel h2h); the gap is coverage (52% vs 22%) |
| Refused | PGO/LTO (loss), SME/ZA (closed on every form), GPU lookahead (12-17 ms per-process Metal floor), batching except load-bound kernels | | |

## 7. Codec breadth and correctness

10-bit (`-Dbit_depth=10`), 4:2:2 and 4:4:4, both entropy coders, B-pyramid,
interlaced input refused honestly; level derivation with MaxBR; MV clamp to
the level; CABAC slice-capacity backstop and cabac_zero_words; CAVLC
level-prefix bound at Main; colour and range VUI signalling; FrameNumWrap
DPB eviction (a 560-frame cell); Baseline never signalled with weighted
prediction; mb_qp_delta span window. Hardware mode (`--hw videotoolbox`)
with our scene cut. The 2026-09-04 review's fixes: two memory bugs, a DPB
wrap crash, a thread-dependent opening, wavefront OOM commit, races in the
profiler counters, six cold statics, two leaks.

## 8. Shot-based and the engine interface (2026-09-05/06)

Shot table from the pre-scan (`--shot-table`), cut-split IDRs, per-shot CRF
(`--shot-crf`, -6..-11% vs cut-split, but +2.6% vs flat on the BBB window:
measure before trusting); the across-shot term in the default path; the
engine hooks (zones, frame stats, pinned GOP threads, segment output,
determinism contract 5/5); the multi-shot corpus and gate with x264 as the
control; the orchestrator design and its P1 prototype (measured hull -3.4 /
-4.6% over per-shot CRF, -11.7 / -2.2% over flat). Per-shot tool selection
closed at a ~1% perfect-selector ceiling.

## 9. Process rules that held

Clean room: never name another encoder's internals anywhere that ships
(the history was scrubbed once). Commits owner-attributed. Portfolio rule:
narrow-positive plus elsewhere-neutral ships; the band is the flip
authority; owner-decision items are not todos. Every report leads with the
goals. Negative results are recorded with their numbers so they are not
re-run.
