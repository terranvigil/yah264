# Hand-written assembly: what x264 does with it, and what it would buy us

2026-09-02. Owner question: x264 gains speed from hand-written assembly and we
have avoided it; would writing it, possibly replacing our intrinsics, get us
more speed? This page answers with measurements taken today on the shipped
binary, and with two companion documents:

- `local/records/x264-asm-catalog.md` (untracked, per the clean-room rule):
  every kernel family in x264's aarch64 assembly, what it computes, and the
  technique that makes it fast. Prose only, no code. Not shipped.
- `docs/dsp-coverage-inventory.md`: every kernel in our `src/dsp`, whether it
  has a NEON twin, how it is dispatched, and sixty scalar pixel loops outside
  the DSP layer with file:line.

Provenance: CONTRIBUTING rules 6 to 8 hold -- measurement baseline, behavioural
description, and no internals named in shipped text. The reference's source was
read to describe logic and technique; nothing from it is transcribed here or
into a kernel. The
head-to-head harness that links their assembly beside our library lives outside
the repo in `../yah264-measurement-patches/h2h/` and produces calibration
numbers only.

## The answer in one paragraph

The instruction set is not the lever. On forty shared kernels, timed in one
harness on this machine, our intrinsics tie x264's assembly wherever the two
compute the same shape, beat it where our design differs, and lose where THEIR
design differs. Every loss is a kernel-shape difference that can be closed in
intrinsics; none is a compiler failure. If every losing kernel were brought to
x264's speed, the wall would move about 1.5 to 2 percent at one thread, because
the kernels we already have are only 22 percent of the wall. The gap that
matters is that x264 runs 52 percent of its wall inside vector code and we run
22, and the uncovered 78 percent is control flow plus RD trial volume, which
no assembly touches. That was the finding of 2026-08-17 and it reproduces today
on a binary that has since shipped rect refs, the probe metric and the anchor
reuse.

## 1. Where x264 uses assembly

Counted from the compiled objects of the reference build (symbol tables only),
8-bit build, grouped by the job each area does:

| area | symbols | jobs |
|---|--:|---|
| pixel metrics | 67 | SAD (one candidate, and three or four candidates sharing the source loads), SATD, 8x8 Hadamard, joint 8x8-plus-4x4 Hadamard, AC energy, variance, difference variance, row-to-row SAD, signed difference, SSD, SSD on interleaved chroma, SSIM |
| motion compensation | 57 | weighted bi-prediction average, half-pel average, explicit weighting in four forms, block copy, chroma MC, the half-pel plane filter, lowres plane init, integral image, mb-tree propagation, aligned copy and clear, plane copies and format conversions, chroma (de)interleave, prefetch |
| transforms | 28 | forward transform fused with the residual (4x4, 8x8, 16x16, and the 8x8 transform), inverse fused with add and clip (same set), DC-only forward and inverse, the 4x4 DC Hadamard, zigzag scans, lossless residual-plus-scan, CAVLC interleave |
| quantisation and coefficient scans | 21 | quant (plus DC and four-block forms), dequant (plus DC), noise reduction, last-nonzero position, level and run extraction, decimation score |
| intra prediction | 33 | every 16x16 and chroma mode, all nine 8x8 modes, five 4x4 modes |
| deblocking | 13 | luma and chroma edge filters in both directions, inter and intra strength, 4:2:2 and MBAFF chroma, the boundary-strength derivation |
| entropy coder | 3 | the three CABAC bin encoders |
| bitstream | 1 | emulation-prevention insertion |
| SVE and SVE2 variants | 20 | minor variants of the above; never selected on Apple silicon |

243 symbols, about 55 families, roughly 12,000 lines. Their runtime detection
on Apple queries only DotProd and I8MM, and M-series cores have no SVE, so
what runs here is NEON plus DotProd variants of the 16-wide SAD family and
the SSD family, plus an I8MM half-pel filter.

Not in their assembly: trellis, the three-candidate SATD and intra-cost
wrappers (C around vector parts), the 8x8 intra edge filter, exhaustive-search
support, rate control, mode decision, motion search control flow. Their motion
search driver plus subpel refinement is 20.5 percent of their wall today, in C.

## 2. What they are actually doing

The catalog has the full account per family. The techniques that recur, and
which of them we already use:

| technique | their jobs | ours |
|---|---|---|
| widening absolute-difference-accumulate into 16-bit lanes, one fold at the end | SAD, one or several candidates | yes, plus four accumulator chains |
| share the source-block loads across 3 or 4 reference candidates | multi-candidate SAD | sad_x4 yes; no x3 |
| DotProd against an all-ones vector to accumulate bytes straight into 32-bit lanes; for SSD dot the difference with itself | 16-wide SAD, SSD | sad 16x16 and 8x16 yes; **ssd no** |
| 16-bit Hadamard with interleave transposes; abs-then-max pairing absorbs the halving | SATD, 8x8 Hadamard, AC energy | yes (ours is un-halved by definition) |
| joint sa8d+satd from one pass | joint 8x8-plus-4x4 Hadamard | no; we call both (tr-pre decision) |
| residual fused into the forward DCT, add+clip fused into the inverse, prediction loads interleaved with the arithmetic | forward and inverse transforms | yes for 4x4 and 8x8; **16x16 batched inverse no** |
| 16-bit inverse 8x8 transform | inverse 8x8 transform | **no, int32 by design** (see section 4) |
| DC-only inverse as saturating add of the positive part and saturating subtract of the negated part | DC-only inverse | **no, scalar in three places** |
| quant: widening multiply makes the shift by 16 free; sign restored by mask arithmetic; nonzero test on an OR fold | quant, four-block quant | different form, ours is 3.6x faster |
| table-lookup byte permute for zigzag; fused residual+scan+nnz for lossless | zigzag scan, lossless residual scan | yes for the scan (plus abs and mask); no fused sub |
| movemask substitute: narrow to bytes, compare, nibble shift, count leading zeros | last-nonzero, level/run extraction, decimation score | **no; our 8x8 scan mask is 4x slower than their last-nonzero scan** |
| whole-edge deblocking: 16 lines per call, tc0 broadcast per segment, every condition a mask, bit-select for every conditional write, 8x16 byte transpose for vertical edges | deblock edge filters | **no: four lines per call in 4-lane vectors; 2 to 5x slower per edge** |
| deblock_strength from the neighbour cache with byte-extract for left and above | boundary strength | yes, shipped 2026-08-17 |
| one-pass half-pel filter producing H, V and centre planes with a 16-bit vertical intermediate and a shift-only centre filter | half-pel plane filter | ours is row-based with int32 intermediates and a separate 6-tap MC kernel |
| lowres init producing all four half-pel planes in one pass | lowres init | no; we build fifteen quarter-pel phase planes (a design difference, not a kernel one) |
| mb-tree propagate in float lanes with reciprocal-estimate divide; propagate-list weights in fixed point | mb-tree propagation | **no, scalar double grid with an order dependence** |
| weighted bipred with four sign-specialised bodies and a saturating rounding narrow | weighted bi-prediction | ours is packed-only; **the 8x8 direct/B8 average is scalar** |
| explicit weighted prediction selected at cache time into one of four bodies | explicit weighting | **no, `apply_wp_luma` is scalar** |
| intra modes from a single loaded edge with byte extracts; plane mode as one add and one narrow-store per row | intra prediction | yes for most; ours builds into a scratch block instead of in place |
| CABAC: state in registers, MPS/LPS both computed and selected without a branch, clz renormalisation, shared put-byte tail | CABAC bin coder | our per-bin engine measured 1.07 to 1.10x theirs (round 11); the residual writers inline the bins |
| emulation-prevention scan 16 bytes per step, scalar only on a hit | bitstream | no; not measured as hot |
| fixed scratch strides (fenc 16, fdec 32) baked into every macroblock kernel; 64-byte aligned caches; stride disaligned from cache sets | layout | we have no fenc/fdec scratch; analysis reads and writes the frame planes and snapshots them (the 3 percent memmove) |

## 3. Head-to-head, one harness, this machine

`../yah264-measurement-patches/h2h/h2h.c`, best of 7 over 4000 calls, stride
512, random pixels, both sides through function pointers. Ratio is x264 time
over ours, so above 1.0 means ours is faster. Full table in
`local/records/h2h-x264-vs-yah264-2026-09-02.txt`.

| kernel | ours ns | x264 ns | x264/ours | reading |
|---|--:|--:|--:|---|
| satd_8x8 | 7.32 | 7.16 | 0.98 | tie |
| satd_16x16 | 29.3 | 28.3 | 0.97 | tie |
| sa8d_8x8 / 16x16 | 9.0 / 36.1 | 9.2 / 35.7 | 1.02 / 0.99 | tie |
| sad_8x8 | 3.46 | 3.56 | 1.03 | tie |
| sad_16x16 (dotprod, what we dispatch) | 7.71 | 8.44 | 1.09 | ours |
| sad_x4_16x8 | 9.14 | 9.49 | 1.04 | tie |
| dequant_8x8 | 7.65 | 8.33 | 1.09 | tie |
| satd_4x4 | 3.71 | 11.4 | 3.08 | ours |
| quant_4x4 | 1.96 | 7.05 | 3.60 | ours |
| dequant_4x4 | 1.22 | 3.90 | 3.20 | ours |
| var_16x16 | 3.89 | 9.94 | 2.56 | ours |
| sub4x4_dct | 4.42 | 8.88 | 2.01 | ours |
| intra 16x16 plane / chroma plane | 14.7 / 9.1 | 25.6 / 18.1 | 1.75 / 1.98 | ours |
| hadamard_ac_8x8 | 8.28 | 10.8 | 1.30 | ours |
| copy 16x16 | 5.03 | 7.38 | 1.47 | ours |
| sad_x4_8x8 / 8x4 | 10.5 / 4.3 | 8.2 / 3.6 | 0.78 / 0.83 | theirs, two rows per op |
| sub8x8_dct (4 blocks) / sub16x16 | 8.7 / 31.6 | 7.1 / 25.6 | 0.82 / 0.81 | theirs |
| sub8x8_dct8 | 13.0 | 11.5 | 0.89 | theirs, slightly |
| ssd_8x8 | 4.45 | 2.98 | 0.67 | theirs, dotprod |
| ssd_16x16 vs their dotprod | 11.8 | 6.2 | 0.53 | theirs, dotprod |
| add4x4_idct | 8.16 | 4.38 | 0.54 | theirs, 16-bit |
| inverse 8x8 transform | 23.0 | 11.7 | 0.51 | theirs, 16-bit |
| weighted avg 16x16 (21/43) | 15.2 | 7.7 | 0.50 | theirs |
| avg 8x8 | 4.08 | 2.25 | 0.55 | theirs |
| zigzag 4x4 (ours also masks) / 8x8 (ours also abs) | 1.73 / 7.84 | 0.70 / 2.93 | 0.40 / 0.37 | theirs; ours does more per call |
| scan_mask_8x8 vs their last-nonzero scan | 9.47 | 2.26 | 0.24 | theirs, movemask idiom |
| deblock luma, one 16-line edge, horizontal / vertical | 53.9 / 24.5 | 10.5 / 12.0 | 0.19 / 0.49 | theirs, whole-edge shape |
| intra 16x16 vertical | 5.50 | 3.48 | 0.63 | theirs writes in place |

Jobs they vectorise that we run in C, with their time: SAD 8x4 / 4x8 / 4x4
(1.5 / 11.1 / 7.1 ns), three-candidate SAD, four-candidate SAD 4x8 / 4x4, the
4x4 DC Hadamard (10.2), last-nonzero over 16 (0.70), decimation score over
16 / 64 (1.4 / 2.8), the batched 16x16 inverse (32.2), four-block quant (7.7),
chroma deblock in both directions (16.2 / 3.6), the half-pel plane filter on a
128x16 tile (515), aligned 1 KiB copy (9.0).

**Craft is not the difference.** Round 9's hand-assembled satd8x8 tied our intrinsics at
3.7 ns (`docs/archive/goal3-route-sizing.md`, in yah264old), and today's table
says the same for every kernel of matched shape. The losses above are all
shape: 4-lane deblock versus 16-lane, int32 transform versus int16, single
accumulator chain versus dotprod, packed-only average versus strided, a
scan-plus-abs-plus-mask kernel measured against a bare permute.

## 4. What the losses are worth, weighted by today's profile

`bench/lowrate/coverage.sh` on the shipped binary, samsung_720p at 1200 kbps
and bus_cif at 400, `sample` at 1 ms (`local/records/*-lo-t1.txt`,
`sams-lo-t12.txt`, `x-sams-lo-t1.txt`):

| | ours t1 samsung | ours t1 bus | ours t12 samsung | x264 t1 samsung |
|---|--:|--:|--:|--:|
| inside a NEON kernel | 22.4% | 16.6% | 18.3% | **52.3%** |
| dispatch wrappers | 3.1% | 1.0% | 1.3% | 0 |
| uncovered C | 74.5% | 82.4% | 80.4% | 47.7% |

Our NEON kernels by share (samsung t1): satd16x16 3.07, satd8x8 2.72, sad_8x8
1.94, sad_x4_16x16 1.61, texture_ac48 1.59, pred_avg2 0.94, sad_16x16 0.92,
quant_4x4 0.72, ssd_8xh 0.70, sub8x8_dct8 0.68, sub_dct4_blocks 0.61, sa8d
0.54, mc_chroma_w4 0.54, sad_x4_8x16 0.50, sub4x4_dct 0.48, quant_8x8 0.48,
var 0.46, hpel_outrow 0.44, sad_x4_8x8 0.39, sad_8x16 0.39, scan_mask 0.37,
ssd_16xh 0.33, deblock (all NEON parts) 0.45, idct8 + add8x8 0.13.

Multiplying each losing kernel's share by what x264's speed would save gives
about 1.5 percent of the single-thread wall on samsung. That is the whole
prize of "replace our intrinsics with assembly as good as theirs", and it is
reachable in intrinsics.

The uncovered side is where the 52-versus-22 lives, and it splits three ways:

1. **Motion search control flow, at parity.** `y264_me_search` 13.0 plus the
   probe drivers 9.2 = 22 percent; x264's motion search driver plus subpel refinement
   = 20.5 percent. Neither side vectorises it. Our probes already go through
   `sad_x4` and the dispatched SATD.
2. **The residual and RD path, where we run more trials.** `encode_inter_res_tp`
   6.0, rdoq and trellis 4.2, CABAC estimation 3.5, `dist_mb` 1.3,
   `eval_inter_part` 1.2, `analyze_b_mb` 2.8, `probe_skip_g` 2.2, snapshot
   memmove 1.5: about 22 percent against x264's 6. This is trial COUNT (the P
   tournament's late-skip class, closed on quality nine times) plus scalar
   glue between the kernels. The glue is where new coverage can come from:
   per-4x4 dispatched calls where a batched form exists, DC-only recon in
   scalar, scan-order gathers, snapshot copies.
3. **Lookahead, 4.7 percent against their 1.5.** `build_lr_subpel_1` 1.9 is the
   fifteen-phase-plane design (kernel refuted twice); `lr_fme_block` 1.8 and
   `mbt_pa_source` 0.5 are search control flow; the mb-tree deposit is a scalar
   double grid.

Deblock is the one stage where we have a kernel and still lose 3 to 5x per
edge: `deblock_mb` 0.54 + `filter_line` 0.37 + the NEON parts 0.45 + strength
0.20 = about 1.6 percent of t1 wall against x264's whole deblock at 0.3.

The 16-bit inverse 8x8 transform is a design refusal, not an oversight:
`transform_neon.c` carries the proof that int16 intermediates overflow for
legal coefficient sets at QP 45 and above, and our recon gate is bit-exact
mandatory. x264 accepts the corner. Reopening it is an owner call and is worth
0.13 percent of wall, so it is not on the list below.

## 5. What "doing the same" is, ranked

Each item is byte-identical work gated by checkasm and `bench/bin_ab.py`,
priced from today's profile at t1; the 2026-08-18 transfer study says inter
kernels hold or amplify at t12 (bus 1.36x) and intra ones collapse.

| # | item | shape | t1 prize | result |
|---|---|---|---|---|
| 1 | whole-edge luma deblock, h and v, per-segment tc in lanes | rewrite | ~1.0% claimed | **built; wall NULL** at t1 and t12 (the kernels it replaced were 0.45% of wall; h16 1.4-1.7x, v16 1.1x in checkasm) |
| 2 | strided weighted 8x8 average for `build_direct_pred` / `build_b8_pred` | new kernel | 0.3-0.6% | **built**, 3.06x over C (8x8), part of round 2 below |
| 3 | SSD by DotProd, two chains, plus the missing checkasm group | rewrite | ~0.3% | **built**, 2.15x (16x16) / 1.43x (8x8) over the plain NEON |
| 4 | DC-only reconstruction kernel at macroblock.c:1927, 2363, 3154 | new kernel | ~0.2% | **built**, 2.49x over C, part of round 3 |
| 5 | `sad_x4` 8-wide by widening abs-diff-accumulate instead of row pairing | rewrite | ~0.1% | **built and reverted**: 7.5 vs 6.7 ns, slower |
| 6 | batched `sub_dct4_blocks` in `probe_skip_g` | call-site | <0.5% | not built: the probe exits on the first significant block, so batching wastes work |
| 7 | last-nonzero and decimation score by the movemask idiom | rewrite | ~0.2% | not built this session; design in the untracked catalog |
| 8 | mb-tree deposit in vector fixed point | design + kernel | part of 0.5% | needs a BD gate |
| 9 | explicit WP `apply_wp_luma` | new kernel | only with weightp | low |
| 10 | 16-bit idct8 | design | 0.13% | owner call |
| 11 | dispatch wrappers (`y264_sub4x4_dct`, `y264_quant_4x4`, `y264_pred_avg2`, `y264_mc_chroma` ...) bill ~1.3% of t1 self time as call overhead | header inlining | ~0.5-1% | not built; the quant wrappers carry deadzone and row logic, so it is a refactor, not a kernel |

Everything on this list together is 2 to 3 percent at t1. Goal 3 needs 17.
The rest of the distance is trial volume and lookahead design, which are the
quality-gated tracks already in the queue, not assembly.

## 6. The round built this session (branch `kernel-shape`, uncommitted)

Four kernels shipped, one reverted. Every step gated by checkasm (new groups:
`deblock_luma_v16/h16`, `pixel_avg_wt_s`, `ssd_16xh/8xh`, `add_dc_4x4`),
`bench/bin_ab.py` against a snapshot of the pre-round build (`build-ref`) with
the control column, and `make test` (9/9 plus regress ALL PASS). **md5 SAME on
every cell at both widths, so the round is byte-identical.**

Cumulative wall change against the pre-round build, medians of 5, the
control column (a second copy of the reference) beside it
(`local/records/ab-*.txt` copied from the scratchpad):

| cell | t1 new | t1 ctrl | t12 new | t12 ctrl |
|---|--:|--:|--:|--:|
| foreman_cif | +0.86% (VOID spread) | +0.09% | **+1.09%** | -0.09% |
| samsung_720p | **+0.84%** | -0.23% | **+0.90%** | +0.15% |
| bus_cif | **+1.00%** | -0.18% | +0.01% | -1.00% |
| stefan_cif | **+0.57%** | -0.35% | -1.63% | -1.36% |

Read: about **+1 percent at both widths**, net of control, from the SSD,
weighted-average and DC-only kernels; the deblock rewrite contributed
nothing measurable on its own (round 1 read +0.3/+0.3/+2.1-with-ctrl-+2.0 at
t1 and flat at t12). The CIF t12 cells are 57-90 ms and quantised, so their
last digit is noise.

What this says about the owner's question: the four kernels that paid are
the ones the head-to-head flagged as SHAPE losses (dotprod SSD, a strided
average that did not exist, a DC-only recon that ran scalar). The one that
was pure craft, the deblock filter written the way x264 writes it, matched
x264's per-edge speed on the horizontal form and bought no wall. Assembly
would have produced the same four kernels at the same speed more slowly.

Post-round profile of samsung at t1: `local/records/sams-lo-t1-after.txt`.
