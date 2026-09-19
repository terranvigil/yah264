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

## 9. The x264 parity programme, wave 1 (2026-09-16)

**A-plumb.** Fifty-one options promoted from a `Y264_*` variable, a
hard-coded literal, or nothing at all. Env to flag: `--aq-mode`,
`--no-mbtree`, `--no-dct-decimate`, `--no-fast-pskip` (which needed a gate
built to promote), `--no-asm`, `--no-psy`, `--ipratio`, `--pbratio`,
`--cplxblur`, `--qblur`. Literal to parameter: `--deblock`/`--no-deblock`
and the filter offsets that reach the threshold tables, `--b-pyramid`,
`--no-weightb`, `--chroma-qp-offset` through all fourteen luma-to-chroma QP
mappings, `--qpmin`/`--qpmax`/`--qpstep`, `--vbv-init`, `--mvrange`,
`--sps-id`, `--pass 3`. `--profile` as a constraint that refuses rather than
narrows what you named. A new `src/encoder/sei.c` with the access unit
delimiter and five SEI messages: `--aud`, `--pic-struct`,
`--frame-packing`, `--cll`, `--mastering-display`,
`--alternative-transfer`, plus `--overscan`, `--videoformat`,
`--stitchable` and `--fake-interlaced`. A raw-YUV reader
(`--input-raw`/`--input-res`/`--input-csp`/`--fps`/`--input-range`),
`--seek`, `--crop-rect`, a verbosity dial and `--tune
stillimage|fastdecode`.

Three defects the wiring found, each fixed where it was found: the level's
motion-vector range was unenforced at `--threads 1`, because the install
lived only in the wavefront worker init; a GOP-parallel worker whose
`encoder_open` was refused emitted no NAL and the run exited 0 with an empty
file; and the VUI declared `log2_max_mv_length_vertical = 16` at every
level, advertising +-16384 luma samples where level 4 allows 512. The last
is the one thing in the item that moves a default stream, and it is confined
to the SPS NAL: everything after it is byte-identical on all sixty identity
cells and the reconstruction is byte-identical on all thirty.

## 10. Process rules that held

Clean room: never name another encoder's internals anywhere that ships
(the history was scrubbed once). Commits owner-attributed. Portfolio rule:
narrow-positive plus elsewhere-neutral ships; the band is the flip
authority; owner-decision items are not todos. Every report leads with the
goals. Negative results are recorded with their numbers so they are not
re-run.

## 10. One binary, both bit depths (2026-09-16, item C3-10bit)

The sample width stays a compile-time type inside the encoder, so there are
two libraries; what there is no longer is two binaries. Every build compiles
both, the 10-bit one under a `_10` symbol namespace (generated, and gated by
`scripts/symbol_check.sh` in `make test`, because a name defined in both
archives resolves by scan order with wrong pixels as the only symptom), and
one `yah264` links both and picks the encoder from the input's Y4M `C` tag.
The public header dropped its `pixel` typedef: picture planes are `void*`
with a stride in samples, which is what lets one struct serve both widths and
retires the old failure of a 10-bit library loaded under an 8-bit header.
`--output-depth 10` codes 8-bit content as High 10; `--output-depth 8` on
10-bit input is refused. The ffmpeg wrapper does the same, keyed on pix_fmt.
8-bit output byte-identical over the ten board clips x {CRF, CQP, ABR} x
{t1, t8}; 10-bit output byte-identical to the old dedicated build.

What it bought, and what it found: 10-bit is now a runtime property instead of
a rebuild, and the first 10-bit board says the path behind it is far off the
pace -- median +21% BD-rate against x264 High 10 where the SAME clips at 8
bits read +3.5% and +0.3% (docs/data/board10-2026-09-16.md). Not a regression;
that path had never been measured. It is the next 10-bit item's brief.

## 11. The x264 parity programme, wave 2 (2026-09-17)

**B-cintra.** `--constrained-intra` writes the PPS
`constrained_intra_pred_flag` and holds the encoder to it: in a P or B slice
an intra macroblock reads neither the reconstructed samples nor the intra
mode of a neighbour coded inter, so it decodes from intra data alone. The
restriction reaches everything intra prediction gathers -- I_16x16, I_4x4,
I_8x8, chroma and the 4:4:4 chroma-as-luma path -- and the mode predictor on
both entropy coders, and it withdraws the modes that read the above-left
sample (I_16x16 and chroma plane, and the three corner-reading NxN modes)
whenever that one neighbour alone is inter. Neighbour intra-ness is read off
the motion grid, which is the same "neither list used" test the deblocking
boundary strength already derives an intra block from, so no second grid has
to be kept in step with the commit sites.

Off by default and byte-identical when off: sixty identity cells (ten board
clips x {CRF 23, QP 26, the board rate} x {t1, t8}) against the pre-item
build. On, at a fixed QP 26 over 60 frames it costs +0.25% of the bytes on
stefan, +0.32% on bus, +0.45% on foreman and +0.64% on park_joy, and it is
inert in I slices by construction -- an all-intra encode differs by the
one PPS bit and reconstructs identically. What the gate had to find was the
above-left-only neighbourhood, which no natural clip in the corpus produces
at all (stefan, mobile: zero in forty frames) and a noise fixture produces
104-204 times in forty; that cell recon-matches ffmpeg and the JM at every
QP from 0 to 51 on both transform sizes.

**C1-slices.** `--slices N` cuts each picture into N independently decodable
slices on macroblock-row boundaries, one NAL each, so a decoder can
resynchronise at any of them and several can be decoded at once. Nothing
crosses a slice's first row: not the intra reference samples or the mode
predictor, not the motion-vector predictors or the P_Skip inference, not the
CAVLC nC or `mb_skip_run`, not the CABAC contexts or the mvd and ref_idx
neighbours, not the `mb_qp_delta` chain. All of that is one number in the
frame contract -- the first macroblock row this macroblock's slice owns --
published at the one per-macroblock call every analyze and emit path already
makes, so at one slice per picture it is 0 and every test reads as the
frame-edge test it replaced. The in-loop filter is deliberately NOT cut: every
slice header writes `disable_deblocking_filter_idc` 0, so the picture is
deblocked whole and the cuts do not show. The row wavefront is untouched --
slices only remove dependencies, so its existing left/above rule stays a valid
over-constraint.

Off by default and byte-identical when off: sixty identity cells against the
pre-item build, clean. **The one thing that did move is CAVLC, by design.** A P
or B CAVLC slice wrote `mb_skip_run` after its last macroblock even when that
macroblock was coded and the run was zero; 7.3.4 re-reads `more_rbsp_data`
only after a non-zero run, so a trailing zero run asks the decoder for one
macroblock beyond the slice. With one slice per picture that macroblock is
beyond the PICTURE as well and every decoder stops there, which is why it
survived until a slice had another slice after it. Fixing it moves the same
sixty cells run with `--cavlc` by 0 to 6 bytes over 40 frames -- one stop-bit
position per picture whose last macroblock is coded -- and keeping the byte
would have meant keeping a syntax element the spec does not have.

The cost is real and it is content, not resolution. At `--slices 4`, 120
frames, five CRF rungs inside the VMAF-NEG 55-95 band at matched achieved
bitrate: **+0.71% riverbed_1080p, +4.01% touchdown_420, +4.08%
pedestrian_1080p, +8.53% sunflower_1080p, median +4.05%** BD-VMAF-NEG. P_Skip
takes a zero motion vector wherever the macroblock above is unavailable
(8.4.1.1), so a slice's first row loses the skip on any moving picture, and
the clips that lose most are the ones that were going to be nearly all skip.
Per cut the detailed clips pay what an independent encoder pays (riverbed at
`--slices` 2/4/8: +0.26% / +0.70% / +1.47% of the bytes against +0.18% /
+0.61% / +1.52%) and the skip-heavy ones pay several times it (sunflower:
+2.96% / +6.81% / +14.84% against +0.51% / -0.36% / +2.09%). Recorded as
measured: this encoder leans harder on P_Skip than it has to, a slice cut is
where that shows, and closing it is its own piece of work.

Twelve conformance cells, recon-matched by ffmpeg and the JM at QP 0, 26 and
51: two and four slices on both entropy coders, B3 with the 8x8 transform,
temporal direct at `--ref 3`, AQ against the per-slice QP chain, 4:2:2 and
4:4:4, a cropped picture whose cuts land on rows the crop removes, an uneven
split (7 rows over 3 slices), one slice per row, plus the threading and
determinism canaries at `--slices 4`. Refused with `--hw`: that session cuts
its own slices and takes no count from us. `--slice-max-size` and
`--slice-max-mbs` are not implemented; `--slices` takes a count, not a cap.

**C2-PAFF-1.** `--tff` / `--bff` code each input frame as two **field
pictures** -- I and P, CAVLC and CABAC, 4:2:0. A field is a stride-doubled
view of the frame planes starting at its parity's first row, so there is no
second plane store and the macroblock coder sees an ordinary picture of half
the height; what it is told is the four places the standard makes a field
differ. Residual blocks are written in the field scan, out of a disjoint half
of the CABAC context set with its own 8x8 significance map. The deblocking
filter drops an intra HORIZONTAL macroblock edge from strength 4 to 3 and
halves the vertical motion threshold. The level's vertical motion range
halves, and the VUI declares the halved bound. The SPS says
`frame_mbs_only_flag` 0 and every slice header a `field_pic_flag` of 1,
composing with what `--fake-interlaced` already built.

A pair shares one `frame_num` and splits the frame's POC, 2n over 2n+1. Every
P field's list 0 is the SAME-PARITY field of each of the previous `--ref`
frames, which the default field list already puts at every second index for
both fields of a pair, so the reordering that compacts it is one command
repeated and needs no wrap handling. Naming only the same parity is what
keeps 8.4.1.4's cross-parity chroma motion offset out of the encoder
entirely; the price is that an I frame codes both of its fields intra rather
than predicting the second from the first, which is the first thing the
B-field item takes back. Each parity's borders are replicated as its own
picture, which is why the planes allocate twice the vertical border under
PAFF and the stride does not move. Rate control and the lookahead stay
frame-based: a frame's type applies to its pair, and the per-picture bit
target and VBV credit are each half a frame's.

An interlaced Y4M's own `It` / `Ib` tag turns field coding on with no flag at
all, and `--no-interlaced` refuses it. Where the field order came from is what
decides a conflict: under `--tff`/`--bff` a named `--bframes` above 0,
`--slices` above 1, 4:2:2, 4:4:4, `--hw` or a height that is not a multiple
of 4 is a refusal, and under
a bare `It` tag the named flag wins and one line says the field order went
unused. A B count the PRESET chose is narrowed in silence either way, the
rule `--profile` already follows. The height rule is the doubled crop unit:
it cannot crop such a picture back to itself. `--fake-interlaced` had the
same arithmetic and no such check -- on a 98-line source it declared a
100-line picture -- which this item fixes as well.

Progressive output is byte-identical: 60 of 60 identity cells (ten board
clips x {CRF 23, QP 26, the board rate} x {t1, t8}) against the pre-item
build. Twenty-two conformance cells recon-match FFmpeg **and the JM** --
openh264 refuses every field stream at the SPS, so the JM is the second
oracle here, skipped by stream property as everywhere else. What the gate
found was the field 8x8 significance map: the first transcription was three
entries short of a repeat, both decoders refused the stream, and the
positions it goes wrong at were bisected against them. Two speed-side gaps
are named rather than measured away: a field picture runs motion search
without the cached half-pel planes (they are built over the frame and
describe no field), and the residual RD model prices a field block in
frame-scan order against the frame context models.

No rate-distortion claim is made for it. At QP 26 on the two synthetic
fixtures, field coding costs 25% and 14% more bytes than frame coding for
+0.13 dB and +0.43 dB of luma PSNR -- but both fixtures are a progressive
source run through `tinterlace`, whose two fields are adjacent frames of a
50 Hz sequence and correlate vertically, which is the content frame coding is
best at. The leg that would answer the question is an interlaced board
against a field-coded reference, and the plan schedules it with the B-field
item rather than here.

## 12. The x264 parity programme, wave 3 (2026-09-17)

**B-hrd.** `--nal-hrd vbr|cbr` writes the buffer model into the stream:
`hrd_parameters` in the VUI, a `buffering_period` SEI at each buffering
period, a `pic_timing` SEI on every picture carrying `cpb_removal_delay` and
`dpb_output_delay`. Until now the VBV was a private agreement between the rate
control and itself, and three docs said so. `--nal-hrd cbr` also pads each
access unit up to the declared rate with filler (NAL type 12), which is what
makes a constant schedule true rather than claimed; `--filler`/`--no-filler`
say so explicitly and `--filler` is refused outside cbr.

The declaration is the model the rate control already obeys. `BitRate` and
`CpbSize` are the two VBV parameters rounded **up** onto the syntax's own grid,
and the initial removal delay is the occupancy the encoder started from.
Nothing was widened to make a checker pass, and `--nal-hrd vbr` leaves the
recon identical to the same VBV encode without it, on all ten board clips at
both thread counts.

**Two ledgers, deliberately.** The rate control steers on a picture's slice
bits and clamps its buffer at both ends, because that is what steering needs.
A receiver's buffer holds every byte of the access unit -- start codes,
parameter sets, SEI, filler -- and does not clamp, it fails. So the padding is
decided off a second ledger that counts what crosses the wire. The gap between
the two is 20-40 bytes per access unit, about 2.5% of a frame at 400 kbit/s,
and the rate control does not budget for it: invisible on a cell with
headroom, not invisible on one at the boundary. That one is recorded against
the rate control, not against the signalling.

Gate: `HRD=1 scripts/cvbr_compliance.sh`, the 36 capped-VBR cells re-encoded in
both modes and read by `hrd_check.py`, which is told nothing on its command
line. It is read on a **partition** rather than as a count, because the
declaration is the encoder's own model written down: a cell whose rate control
already underflows emits a stream that underflows the declaration, and a raw
"N of 36" would score the rate control and call it a signalling result. Every
cell VBV-clean on main is HRD-clean in both modes except two, named below.
64 of 66 arms, all 33 vbr arms clean.

Three defects found by building it, two of them ours and one in the checker:

- The access-unit opener ran once per **slice**, not once per picture, so
  `--slices N` with `--pic-struct` wrote N delimiters and N `pic_timing` SEIs
  inside one access unit. Present since `--slices` shipped.
- `cpb_removal_delay` was computed after moving the buffering-period anchor
  instead of before, so every IDR after the first declared itself removed at
  the same instant as the stream's opening picture. It read as an underflow at
  the first mid-stream scene cut of every clip that has one, at every bitrate,
  and as nothing at all on the clips that do not.
- `hrd_check.py` had one epsilon doing two jobs: 1e-9 is right for the arrival
  and removal comparisons, which are in seconds, and meaningless for the
  fullness comparison, which is in bits. A 20 Mbit buffer accumulates 1e-8 to
  1e-7 bits of rounding, so a compliant CBR stream padded to sit exactly on
  `CpbSize` -- which is what the padding is for -- was failed by nought point
  nothing of a bit.

**Open, and not this item's to close: CBR across a GOP join.** A GOP-parallel
encode opens one encoder per GOP, and each assumes it inherits the handoff
occupancy, half the buffer. Under CBR the true occupancy at the join is
whatever the previous segment left, and where that is higher the next segment
under-pads. park_joy at 9600 kbit/s ends its first GOP at 93% of the buffer
against an assumed 50% and peaks at 107.4%; the other five park_joy cells and
all six ducks cells are clean, so it is the two arms of one cell. The join
*clock* is exact -- that was the anchor defect above, and a segmented encode
now places its buffering periods only at segment starts, where the caller's
frame counts are provably right. What remains is the *occupancy* handoff, and
both candidate fixes cost something: padding each segment's last access unit
down to the handoff level spends about 10% more bits at keyint 250, and
assuming a full buffer instead trades the overflow risk for an underflow one.
Measured and left for the owner.

**C2-PAFF-2.** Field coding takes back the two simplifications PAFF-1 shipped
with, and gains B fields and slices.

A field's reference list now names **both parities**, in the order the standard
derives: the same-parity field of the nearest frame, then the opposite-parity
field of that frame, then the same-parity field of the one before it, and so on
down the list. The slice header names every entry outright by its picture
number, as a modulo subtraction from the running predecessor, so the list the
encoder built is the list the decoder gets whatever its own default derivation
would have produced. `--ref` still counts reference FRAMES, so a field list
holds up to twice a frame picture's entries and reaches the same distance back.
Reading `--ref` as a field count instead was tried and lost: it halves the reach
and costs 0.3 dB at equal bits. The references a field macroblock searches
double with the list, which is what that reach is paid for with.

Naming the opposite parity is what **8.4.1.4** is for. In 4:2:0 the two parities
site their chroma rows a quarter of a chroma sample apart inside their own
fields, so a prediction that crosses parity moves chroma by that quarter where
luma moves by nothing. The correction is a constant per parity pair. It is
resolved once onto the reference view and added at all twenty-two chroma
motion-compensation sites. Only one sign reconstructs what a decoder
reconstructs; the other two candidates were encoded and refused by both
decoders.

With that in place **the second field of an I frame is a P field**, predicting
from the first field of its own pair, so a key frame costs one intra field
rather than two. It is quantised as the intra field it replaces, because it is
still half of a key picture.

**B fields** compose with all of it: implicit weights are POC distances between
fields, list 1 is the future anchor's same-parity field, and temporal direct
reads the same-parity half of the pair's stored co-located grid. That last one
is why the per-4x4 motion grids split by parity, at offsets 0 and half of a
frame-sized grid, so the single DPB store after a pair carries both fields'
motion. Where a co-located reference does not resolve, the fallback to spatial
is per macroblock, not per slice. **`--slices`** composes too: the cuts are the
field's rows, not the frame's.

Two bugs found on the way, both invisible until a field could name the opposite
parity. The **weighted-prediction estimate** walked a whole frame's rows at the
frame stride over what is, under field coding, a field view: it read the other
parity interleaved with its own, and once a field could reference its pair's
first field it read the rows that picture was about to reconstruct into. Those
hold the previous frame's reconstruction in a reused encoder and zeroed pages in
a fresh one. The bitstream therefore depended on how the GOPs had been handed
out: three distinct streams at threads 1, 2 and 8, each reproducible on its own.
The sanitisers are silent on it. The memory is allocated and written, just not
by that picture. Filling every plane allocation with a constant named it in
one run: the output moved with the constant, and the thread counts agreed as
soon as it was filled at all. The second was the **per-slice CABAC engine
re-initialisation**, which cleared the engine's field flag, so every slice after
the first wrote its residual in the frame scan out of the frame half of the
context set. The flag is a property of the picture, not of one slice's engine.

A third turned up under ThreadSanitizer once `--keyint 1` field coding became a
gated path. PAFF-1's field-scan permutation tables are built by the warm at
ENCODER OPEN, and the CLI opens one encoder per GOP from several workers at
once, so at `--keyint 1` a dozen opens are inside that builder together. Every
writer stores the same byte, which is why nothing ever misbehaved, but the same
value is not the absence of a race. It builds once now, the way the trellis prep
rows beside it already did.

Two threading levers step aside for field coding. The one that overlaps a
mini-GOP's B frames with their anchor sizes its whole budget in rows of a single
coded picture, and a field pair is two pictures interleaved in one buffer
publishing into one watermark. The one that runs two sibling leaves at once
gives each a private reconstruction, and a field pair's two pictures share one.
Both are gated on the parameters alone, so declining them changes no bits. They
are named in docs/options.md beside the two speed gaps PAFF-1 already recorded.

Progressive output is byte-identical to the pre-item build across sixty cells:
ten board clips, three rate modes, two thread counts, bitstream and
reconstruction both.

**Follow-up, named rather than guessed: what field coding costs against frame
coding.** The figures in docs/options.md were taken when field coding was I and
P only, against a same-parity reference list, with both fields of an I frame
coded intra. All three have changed here. The paragraph says so and keeps the
old reading rather than inventing a new one. Re-measuring it wants a quiet box,
which is also what the interlaced rate-distortion board wants, so the two
belong in the same sitting.

**B-opengop** (the wave's heavy-leg item). `--open-gop` makes every
keyframe but the encoder instance's first a **non-IDR I picture with a
recovery_point SEI** (`recovery_frame_cnt` 0, `exact_match_flag` 1): the
decoded picture buffer is not flushed, POC and FrameNum run straight through,
and the B frames that precede the key in display order stay B frames --
backward to the anchor behind the key, forward to the key itself -- where a
closed GOP had to flush them as non-reference P. A keyframe a `--plan` zone
forced stays an IDR, because the engine interface promises a plan keyframe is
addressable as a segment on its own. Off by default; sixty identity cells (ten
board clips x {CRF 23, QP 26, the board rate} x {t1, t8}) byte-identical
against the pre-item build.

**It takes the adaptive scene-cut keys too, and at the shipped keyint that is
the only key it takes.** A 120-frame encode at `--keyint 250` has one keyframe,
its first, and that one is an IDR either way, so on nine of the twelve band
clips the flag cannot move a byte and does not. The three whose content cuts
move: at the same CRF the stream is 11.0% smaller on samsung_720p, 6-7% smaller
on coastguard_cif and 0.4% smaller on sintel_720p.

What the recovery point claims is exact and narrow -- start decoding at that
access unit with nothing before it but the parameter sets, and every picture
from the key onward IN OUTPUT ORDER is the picture a decode from the stream's
IDR would have produced -- and two things in the encoder exist only to keep it
true. No picture after a key may reference one from before it, so the reference
lists are cut; and the bound for the key's own leading B frames is not that key
but **the key before it**, because those frames are output before their own
recovery point and after the previous one. And the leading B frames are coded
as NON-REFERENCE pictures, because a reference among them carries a FrameNum
above the key's and so heads the PicNum-descending default list of everything
after it, holding the wrong picture on a cold start; keeping it out of our own
list does not help, since the index still names it on the decoder's side.
`scripts/recovery_check.py` cuts the stream at the recovery point and compares
the tail against `--dump-recon`; it found both defects, and it is the gate
B-intra-refresh should reuse in wave 5.

**The cut decode asks for every picture (2026-09-19).** Three cells --
`ogop_rec_b3`, `ogop_rec_mref`, `ogop_rec_cavlc` -- failed at the second
recovery point under ffmpeg 6.1.1 and passed under 9.0.1 on the same bytes,
which is what kept the ubuntu CI job red. Nothing in the stream was ambiguous.
The recovery point declares `recovery_frame_cnt` 0, `exact_match_flag` 1 and
`broken_link_flag` 0, and D.3.8 makes that a promise about every picture at or
after it in OUTPUT order, which is exactly the set the checker reads. No sample
differed either. What differed was which of those pictures the decoder chose to
hand out: 9.0.1 emitted all four covered ones and suppressed the three
uncovered leading B frames, while 6.1.1 emitted the first two and then stopped,
never draining the last two out of its buffer at the end of the bitstream. That
is a shortfall against the output process of 8.2.1 and Annex C, under which
every picture still in the buffer is output when the bitstream ends, so 9.0.1
is the one that is right. It is also a display policy and not a decode result:
asked with `-flags2 +showall`, 6.1.1 produced all seven pictures, and its last
four are byte-identical to the encoder's reconstruction. So the flag goes on
the cut decode, both versions honour it, and the selection the recovery point
calls for is made in the checker by the tail rather than borrowed from
whichever ffmpeg is installed. The stream did not move and no encoder byte
changed.

**The cost is parallelism, not bits.** A GOP boundary is what lets the CLI hand
a GOP to its own encoder instance and an open GOP has none, so the encode is one
instance and `--threads` spends its budget on the row wavefront. With
`--cut-split` the cuts still split, because those really are IDRs. On an input
whose frame count cannot be read the schedule cannot be planned and the encode
hands itself to the serial path, saying so when `--threads` asked for more.
Refused with `--stitchable`, whose whole promise is a cut point at every
keyframe.

Band, BD-VMAF-NEG at matched achieved bitrate, 120 frames. Read at **keyint 30
on both arms** rather than at the default, because the default cadence never
fires inside the band's own frame count and would have printed 0.00% nine times:

| clip | BD | clip | BD |
|---|---|---|---|
| park_joy_720p | -7.72% | bus_cif | -1.48% |
| samsung_720p | -5.49% | coastguard_cif | -0.80% |
| stefan_cif | -4.68% | tempete_cif | -0.25% |
| ducks_720p | -3.34% | foreman_cif | **+1.84%** |
| touchdown_420 | -3.33% | mobile_cif | **+1.87%** |
| sintel_720p | -1.72% | akiyo_cif | **+3.63%** |

**median -1.60%, mean -1.79%, 9/12 negative, worst +3.63%.**

At the shipped keyint 250, on the three clips where the flag is live there at
all, it reads the other way: coastguard_cif **+0.59%**, samsung_720p **+0.65%**,
sintel_720p **-0.19%**. All three are smaller at the same CRF -- samsung by
11.0%, coastguard by 6-7%, sintel by 0.4% -- and two of the three are slightly
WORSE at matched achieved rate. So at a scene cut the bytes the open key saves
do not come back as quality. Both shapes are real and they answer different
questions: keyint 30 is what the flag does to a periodic-keyframe stream, which
is what it is for, and keyint 250 is what it does to a stream whose only key is
a cut.

**The three positive clips are the item's recorded cost**, and the owner shipped
it with them (2026-09-17) because the flag is opt-in and off, so nothing
regresses. Two causes are tangled and neither is separated: a GOP boundary is an
ENCODER-INSTANCE boundary in this CLI, so at keyint 30 the default arm runs four
instances over 120 frames -- each restarting the CRF rate control, the mb-tree
warmup and the first-frame VBV bound -- against the arm's one, which means the
band measures the reordering change and a rate-control-continuity change
together; and the leading B run is priced flat, as non-reference B at the
non-pyramid leaf cascade, where it used to be non-reference P. `akiyo_cif` needs
a full CRF point below the default at three of four rungs to hit the same bytes,
which is the shape of a pricing change rather than a prediction one. **The
flat-B pricing is the named follow-up**: one line in `code_b_hier` and one
CIF-7 band would say whether it is the cause.

Gate: `make conformance --fast` 1418/1418 with FFmpeg **and the JM** (663 checks
each, 0 mismatched) over twenty-five new open-GOP cells;
`determ_repeat ARGS='--open-gop'` 24/24 configs reproducible over 8 runs each
under six spinners; `stress_threads` 0 hangs in 360 encodes; `san_matrix` 0
reports over 26 cases; TSan floor 0 over 36 reps on the default, on
`--open-gop` and on the pre-item build alike. That last one earned its place
twice over: the first cut wrote the POC base and the recovery bounds where the
key is decided, which is upstream of the staircase drain and so a data race on
values `build_list0` reads from the chain threads, and TSan reported it in two
of twelve reps. It also exposed that `tsan_catch.sh` printed "no report in N
reps" when its default binary did not exist -- a vacuous pass that read as a
clean gate, now a refusal.

## 13. x86-64

**Wave 0 of the x86 programme: the dispatch and the build, with no kernels
yet.** The encoder ships 54 NEON kernels and nothing for x86-64, and two lines
were the reason. `y264_asm_on()` tested `Y264_CPU_NEON` by name, and
`y264_pixel_init` tested it again, so every dispatch site in the tree was
aarch64-only by construction however complete the x86 feature detection beneath
it was. This item removes the name.

- **`Y264_CPU_SIMD_ANY`** is the OR of the tiers the build compiled in, and
  `y264_asm_on(cls)` now asks `(detect & SIMD_ANY) && !(asm_off & cls)`. It is
  a compile-time constant, so the gate is still an inlined acquire load and two
  tests. `YAH264_NO_ASM` and `Y264_ASM_OFF` mean exactly what they meant.
- **`y264_cpu_tier()`** names the best tier the build has and the CPU can run.
  A tier is selected only when its WHOLE feature set is present, not its
  headline bit: a file compiled with `-mavx2 -mfma -mbmi2` may emit an FMA
  anywhere in itself, so AVX2 without FMA3 is a fault and not a slow path.
- **`Y264_SIMD_FORCE=none|sse4|avx2|avx512`** clears the feature bits above the
  tier it names, so one binary answers what the lower-tier build would have
  done. That makes the ISA tier an A/B axis with `YAH264_NO_ASM=1` as the
  control column, instead of a second build directory; it is also an identity
  axis, since no kernel moves a byte.
- **The build shape.** meson gains `simd` (a build-time cap: `auto`, `none`,
  and the three x86 tier names) and `avx512` (off, a gated experiment). On
  x86-64 each tier is its own static library with its own `-m<feature>` flags
  and its own per-family opt-in list, folded into the 8-bit library alone; the
  dispatching files stay baseline x86-64, because a dispatcher compiled for a
  tier faults before it can decide not to use it. `hygiene_check.sh` refuses
  `__asm__` under `src/dsp/x86/` and `-march=` in a meson file.
- **The empty files are the point.** `src/dsp/x86/<family>_<tier>.c` exist and
  compile with nothing in them, so every x86 build from here exercises the
  shape -- three libraries, their flags, their macros, their linkage -- rather
  than the shape being written blind on the day the first kernel lands.
- `-Dsimd=none` now builds a genuinely pure-C binary on aarch64 too: the NEON
  translation units are left out rather than routed past, which is the one
  thing the runtime escape cannot do.
- aarch64 output is byte-identical to before, verified over the ten board clips
  at CRF, CQP and ABR, at one thread and eight, bitstream and reconstruction,
  at the default and under `YAH264_NO_ASM=1` and each of the nine
  `Y264_ASM_OFF` classes.

The kernels themselves are waves 1 to 3 of docs/x86-plan.md; their names are
declared in `src/dsp/arch.h` already.

**The validation kit, and the one gate that is not emulated.**
`docker/x86/Dockerfile` and `scripts/x86-docker.sh` run the whole correctness
battery as linux/amd64 on a machine with no x86 in it: `{rosetta, qemu}` x
`{Y264_SIMD_FORCE=none, sse4, avx2}` x `{make test incl. checkasm,
conformance --fast, identity cmp}`, plus a 10-bit build, to one pass/fail
report. Neither emulator has AVX-512. The script asserts its absence and
prints the assertion, so an emulated green can never be filed as AVX-512
coverage. A case that dies of SIGILL under Rosetta is rerun under QEMU and
marked "emulator" rather than passed. On a tree with no x86 kernels the sse4
and avx2 tiers must be byte-identical to none, which makes the kit its own
negative control, and it is: 60 encodes per tier over the ten board clips, at
CRF, CQP and ABR, at one thread and four, identical md5s.
The CI ubuntu job is the leg that is NOT emulated. It is an AMD EPYC with
AVX2, and it now runs on every push that touches code. It gained the
`-Dsimd=auto` build, `checkasm --isa avx2`, the same identity cmp on the
synthetic conformance clips, and an SSE4.2-only `-Dsimd=sse4` build with its
own checkasm. macOS stays manual and every timing step stays informational.

**Both defects the kit found were in the tree, not in x86.** A stray `#endif`
had left the CLI's video-signal state, its engine hooks, its log level and its
progress line inside an `#if defined(__APPLE__)`, so the CLI had not compiled
on Linux on any architecture, and the span had grown to 289 lines before
anyone asked. Then `conformance.sh`'s raw-input check turned out to carry the
SAR across to the raw arm but not the colour range, which passed for as long
as the suite only ever ran on macOS: Homebrew's ffmpeg writes no XCOLORRANGE
tag and both arms signalled nothing, Ubuntu's writes one and the two
arms differ by the byte it costs. Neither is an x86 finding. Both are the
argument for the push trigger, made by the tree itself.

**The ubuntu job goes green (2026-09-19).** The third thing the kit found was
not an x86 finding either. Three open-GOP recovery cells failed under the
runner's ffmpeg 6.1.1 and passed under the box's 9.0.1, on the same stream and
the same samples, because the two versions disagree about which pictures to
output after a mid-stream start; `recovery_check.py` now asks for all of them
and selects the covered tail itself, which is the entry under "## 12". Beside
it, `conformance.sh` keys its fixture cache by the **ffmpeg identity** as well
as by `FIXVER`. The synthetic clips are written by ffmpeg, the two ffmpegs do
not write the same Y4M header, and one worktree is read by both of them, so a
container run had been changing the result of the next native run in the same
tree. That one cost a round before it was named.

## 14. The x264 parity programme, wave 4

**B-rcbounds.** Three rate-control bounds, none of which moves a default byte.

**`--crf-max Q`** is a ceiling on the QP the buffer may raise a `--crf` frame
to. It is refused without `--crf` and without a VBV, because in every other
mode there is no raise for it to bound and an inert rate-control flag is the
worst of the three outcomes. It bounds the raise and not the mapping: a frame
the rate factor itself put above the ceiling keeps the QP it was given, which
is why the pre-VBV base is carried beside the raised one rather than inferred.

What it costs is the buffer model, and that is the option rather than a
defect. A frame the VBV would have starved is coded at the ceiling instead and
the bucket goes negative. `scripts/vbv_check.py --crf-max Q` reports the
under-run with its size and exits 0; without the flag the same stream reads
`UNDERFLOW` and exits 1. Both readings are true, and the gate takes the first
one, because a cell that skipped the checker would stop noticing the day the
under-run stopped being bounded.

The ceiling is read on the CODED QP. The first version bounded the base, which
every B frame then stepped over through the frame-type cascade: at `--crf-max
34` on foreman the P frames landed on 34 and the B frames on 37. A bound
nobody can see in the picture is not a bound.

**`--ratetol F`** is ABR's allowed drift from its target before the correction
term answers, as a fraction of the target bitrate. The default is 1.0, which
is the value every ABR encode has used, so the flag at its default is
byte-identical to no flag. The number was measured before it was written down:
at 120 frames on the ten board clips today's ABR lands between -34.1%
(stefan_cif) and +8.5% (ducks_720p) of target, median absolute deviation 9.6%.
Smaller tightens the loop and `inf` takes the term out, in that order --
stefan_cif at 400 kbit/s reads -25.6% tight, -33.0% at the default and -37.0%
at `inf`.

**`--qpfile FILE`** forces a frame's type and an absolute QP, one line per
frame, through the same plan interface `--plan` uses: the zone array gains a
sibling, `yah264_frame_force_t`, and the engine interface gains it with it. The
QP is absolute and last: the frame-type cascade, a zone's offset and the VBV
are all upstream of it. A force the encoder cannot place is refused by frame
number and not approximated, because a caller that asked for a placement and
silently got another one cannot find out. `i`, a non-IDR I frame, is refused
outright: it needs an open GOP.

Three defects were found by needing the paths they sit on, none of them this
item's own. `--frame-stats` published through an array that only the
whole-input scan ever allocated, so on the ordinary path, and on any piped
input, it wrote an empty file and said nothing. `--plan`
never reached the serial encoder at all, which is the path `--dump-recon` and
every unsplittable input take. And the input reader was joined twice on every
`--cut-split` run: once where the whole-input mode waits it out, once again in
the common teardown. Joining a `pthread_t` twice is undefined, and the
sanitiser aborts on it; `--plan ... idr` and any `--qpfile` that names a key
frame reach the same path, which is how this item found it. All three are fixed
here; none moves a bitstream.

Gate: six cells under "rate-control bounds", with the bound itself asserted
rather than the flag's parsing -- no coded QP above `--crf-max`, every
forced type and QP read back out of `--frame-stats`, the three `--ratetol` arms
ordered by how closely they track the target, and `--ratetol 1.0` byte-identical
to no flag. Identity over the ten board clips at CRF, CQP and ABR, at one
thread and eight.

**B-partitions.** `--partitions` names which macroblock partition shapes the
mode decision may try, as a comma-separated list of `p8x8`, `p4x4`, `b8x8`,
`i8x8` and `i4x4`, plus the two words `none` and `all`. The shapes were already
in the encoder, reachable only as diagnostic environment variables and an
effort tier buried in the subme test; this gives them one flag and one
vocabulary, and it gives the encoder a `partitions` field in
`yah264_param_t` that a library caller can set.

The default is DERIVED, not tabulated, and that is the whole of why every
preset is byte-identical to the commit before it. Spelling the sets out per
preset would have frozen them against the flags that actually decide them:
`--preset medium --subme 9` searched the sub-8x8 shapes before this item and
searches them after, because the derived set reads the subme the run resolved
to rather than the preset's name. The rule is three lines --
every preset splits P and B and tries 4x4 intra, the sub-8x8 P shapes arrive
with the full-RD tier at subme 8, and 8x8 intra needs the 8x8 transform to
carry its residual -- and it lives in one inline function in the public header
so the CLI and the library cannot drift apart about it. `--log-level debug`
prints the list a run resolved to. At medium that is
`p8x8,b8x8,i8x8,i4x4`, so `all` adds exactly one shape there, `p4x4`; at slow
and above the default already is `all`.

Two combinations are refused with the rule rather than narrowed: `p4x4`
without `p8x8`, because the sub-8x8 shapes divide an 8x8 block and without the
8x8 split there is no block to divide, and `i8x8` without `--transform-8x8`,
because nothing else can carry an 8x8 intra residual. The derived default
reaches neither, so a mask that does is one somebody typed. A preset or a
profile that turns the transform off still narrows the derived set in silence,
which is the line this encoder already draws between a tool you asked for and
a tool a preset chose. The refusals cost `--partitions all` at ultrafast,
which has no 8x8 transform to code `i8x8` with; spell the list, or add the
transform.

The environment variables stay the finer say, on the `--subpel` convention:
`Y264_P_RECT=0` drops the P 16x8 and 8x16 searches inside `p8x8`, and
`Y264_B_RECT=1` restores the B rectangles inside `b8x8`, which are off at
every preset. So the flag selects the family and the variable still narrows
within it.

Dropping a shape does not make the encode smaller, and the numbers say so. At
`--qp 26` over 60 frames, `none` costs bits AND quality on both clips read:
foreman_cif is 114175 bytes at 37.93 dB PSNR-Y by default against 136474 at
35.04 with no splits at all, and park_joy_720p is 4021452 at 35.89 against
4178322 at 34.49. That is +19.5% and +3.9% of rate for -2.90 dB and -1.40 dB.
The prediction gets worse and the residual pays for it twice.

In the other direction `all`, which at medium means adding `p4x4`, is a wash
at this operating point: foreman +0.7% of rate for +0.02 dB, park_joy +0.9%
for -0.001 dB. The sub-8x8 shapes sit behind the subme-8 tier in the derived
default for a reason, and this is the reason. The flag makes the arm
reachable; it does not recommend it.

Gate: thirteen cells under "partitions", one per mask class -- `none` in both
entropy coders, each shape family alone, `i8x8` without `i4x4`, `all`, and a
4:2:2 and an odd-geometry clip -- all recon-matched, because a class nobody
encodes is a set of mb_type and sub_mb_type codes nobody has decoded. Four
sanitiser cells on the classes that leave a result struct half-filled. Identity
over the ten board clips at CRF, CQP and ABR at one thread and eight, and over
every preset with `--subme` and `--no-transform-8x8` crossed against it.

**B-weightp.** `--weightp 0|1|2`, and the default is 1, which is what every
encode before the flag did.

**1** estimates one luma weight and offset per active list-0 reference from the
frame's DC ratio against that reference, and writes them in the P slice
header's `pred_weight_table`. Chroma stays identity. Naming it moved nothing:
sixty identity cells -- the ten board clips at CRF, CQP and ABR, at one thread
and eight -- are byte-identical to the build before the flag.

**0** clears `weighted_pred_flag`. No P slice header carries a weight table and
the weight leaves the prediction. It is the only flag here that can LOWER the
declared profile. Annex A.2.1 forbids weighted prediction in Baseline, this
encoder signalled it unconditionally, and the derivation therefore had a line
in it that said never claim Baseline. With the tool off the line has no reason
left: a CAVLC stream with no B frames and no 8x8 transform now declares
Baseline, where the same encode declared Main before. `--tune fastdecode`
turns it off with the rest of what costs a decoder, which closes the gap
docs/options.md had recorded against that tune, and makes the tune the one
arrangement that reaches Baseline without naming a profile.

**2** adds a duplicate list-0 slot. Where the frame-level estimate has already
fired on a reference, the picture is scored against that reference tile by
tile, one tile per macroblock, at the weight and without it. If the weight wins
on every tile there is nothing to choose and the reference's own slot carries
it, exactly as mode 1 would. If it wins on most tiles but not all, the
reference is coded TWICE -- weighted in the slot in front, plain in the slot
behind -- and the mode decision answers per macroblock. The duplicate needs a
slot, so mode 2 is refused at `--ref 1`, and it is refused with field coding.

Three things had to be true for that slot to work at all, and each one was
found by it not being true.

A list that names the same picture twice cannot be spelled by the default
derivation, and it cannot be spelled by a partial reorder either: 8.2.4.3.1
drops every other copy of the picture it has just placed, from the slot after
the insertion onward. The only spelling is to name every entry of the list in
turn, which leaves each earlier copy standing. The field path already wrote
exactly that, for the same reason in a different shape, so the two share it now.

The search cannot see the weight -- it reads the reference plane, and the
weight is applied to the prediction -- so the two slots score identically and
the ref_idx bits settle it against the duplicate every time. The search runs on
the weighted slot and skips the plain twin; the twin is an escape, chosen once
the motion is settled, by building the block both ways and comparing the
prediction error, the ref_idx bits AND the motion vector difference. That last
term is not optional. The two slots are different reference INDICES, 8.4.1.3
derives the motion predictor from whichever neighbours share the index, and a
partition that steps off the majority index loses the neighbours it was
predicted from. Without the term the escape was taken by macroblocks scattered
across the picture, each one dragging its own predictor and its neighbours'.

And 8.7.2.1 asks whether the two sides of an edge reference the same PICTURE,
not the same index -- its own note says so. That is a distinction without a
difference until a list names one picture twice. The deblocking filter now
takes a refIdx-to-picture map, identity on every other encode, and the slices
that carry a duplicate take the C strength derivation rather than a second
kernel.

Mode 2 ships OFF, and the number is why. The bar was BD-VMAF-NEG <= 0 on the
fade clips and within +0.2% per clip elsewhere. On a fade-in/fade-out CIF clip,
five CRF rungs at matched achieved bytes, it is **+0.29%**, and the first half
of the bar is already failed. What the duplicate buys is real -- 2336
partitions over 13 frames of that clip take the escape -- and it does not cover
the wider ref_idx every partition of those frames pays for it.

A pixel search around the DC seed, five weights by five offsets scored on a
decimated grid, was tried and is REFUSED at **+0.65%** on the same clip. A
zero-motion SAD ranks a weight by how much of the frame's error it absorbs, and
on anything that moves, most of that error is motion. It stays reachable as
`Y264_WEIGHTP_REFINE=1`, with that number against it.

Elsewhere the mode is inert, because the duplicate fires only where the
frame-level estimate already did. Over the twelve-clip HD band, 120 frames at
each of the five band rungs, it spends a slot on exactly two clips: bbb_720p
(41 slots, **+0.06%**) and perseverance_720p (1 slot, -0.45%, which at one slot
in five 120-frame encodes is the band's own solve noise around a rate staircase
and not the mode). On the other ten it spends none at all, and `--weightp 2` is
then BYTE-IDENTICAL to `--weightp 1` -- 22 cells cmp'd, eleven clips at two CRF
rungs each, every one identical -- so its BD is 0.00% by measurement and not by
argument. The crossfade fixture is one of those ten. Over the ten board clips
at the encoder's own defaults, where the estimate fires more readily, the
duplicate takes 53 of 295 P slices.

Gate: ten cells under "explicit P weighted prediction" -- mode 0 in both
entropy coders and with B frames, mode 2 in both entropy coders and with B
frames, the 8x8 transform and four references, on a uniform fade and on a
crossfade fixture built for it, plus a thread-determinism cell -- all
recon-matched against FFmpeg and libde265. One checker asserts what a
recon-match cannot see: the three refusals write no stream, `--weightp 1` is
byte-identical to no flag, `--weightp 0` declares Baseline, and the duplicate
slot actually fires on the crossfade clip, so the cells above are testing the
path they name.

## 15. HD parity, stage 2

**cpu-lowrate-hd.** Three ways to stop paying a full tournament for a skip
verdict, one of which holds its quality and ships on.

The measurement behind all three is docs/hd-parity-plan.md's stage 0. On the
HD clips people test with, at the bitrates they ship at, this encoder retires
57% to 66% more instructions than the reference and runs them at 27% to 34%
higher instructions per cycle. The deficit is what survives that trade, so it
is work volume and not code quality, and it is collected by doing less rather
than by writing faster kernels. The largest single compartment is the path to a
skip verdict in the B macroblock tournament: on a 1080p clip at its matched
rate, 970,377 B macroblocks end as skip and 372,328 of them, 38.4%, have run a
full motion search first.

The candidate the profile pointed at is not the one that ships, and the one
that ships is the one the corpus had already refused five times in another
form. Both of those are results.

**`--p-part-gate`, on at 400 lambdas.** Where the 16x16 search has already
found a cheap answer and the lookahead's neighbourhood says the block is
homogeneous and nothing downstream leans on it, none of the P splits is
searched: not the rectangles, not the 8x8 quadrants. Five earlier arms asked a
different question -- whether the 8x8 split EARNED its rectangles -- and had to
run the 8x8 search in order to ask it. That search is the larger bill, 1,297,408
quadrant searches against 668,272 at 16x16 on the low-rate 1080p cell, and it
is the one this gate deletes.

It is worth -1.98% and -1.97% of instructions retired on the two profile cells,
which is modest, and the reason it is modest is the reason it keeps its
quality: the gate saturates against its own interlocks. Tripling the threshold
to 1200 lambdas moves sunflower from -1.98% to -2.24%. On the HD band its BD is
+0.09% median, **-0.15% mean**, and -1.29% to +0.20% per clip, so no clip pays
more than the bar. Inert at `--subme` 9 and above, so `veryslow` and `placebo`
are byte-identical with the previous release; `--no-p-part-gate` reproduces it
on the others.

**`--b-preme-skip`, off, and this is the expensive number.** A B macroblock
whose skip residual already costs less than the cheapest syntax any coded mode
could emit has its verdict settled before the motion search runs. This is the
largest single compartment left: 38.4% of the B macroblocks that end as skip on
sunflower_1080p have run a full motion search first, and gating them is worth
-5.87% of instructions retired at its narrow setting and -12.06% at its wide
one.

It is off because it costs +2.34% BD-VMAF-NEG on sunflower_1080p, and the
ladder says exactly where: nothing at CRF 23, +0.02 at 26, and **-2.72 VMAF-NEG
at CRF 35**. The bound is rate-aware -- it is a distortion measured in lambdas
-- and rate-aware the wrong way round, because lambda grows as the rate falls,
so the gate is most generous precisely where a wrong skip is most expensive.
That is the same failure the mid-tournament exit's own notes record for a
lambda-scaled ref-B readmission, reproduced here from the other end of the
tournament.

Reading the same distortion as an ABSOLUTE bound was the one theory left, and
modes 3 and 4 are it. At the mid-tournament exit's own 512 it selects the empty
set; widened to 2048 it takes sunflower from +2.34% to +0.56% and pushes
perseverance_1080p from +0.35% to +0.89%. It moves the damage rather than
removing it. Both bounds are kept behind the flag with their numbers, because
the compartment is too large to leave unmarked and the next attempt should
start from neither of them.

**`--rd-surv-rank`, off.** RD only the top-ranked candidates of each B set
instead of everything the score threshold admits. -4.79% and -3.52% of
instructions for a band median of +0.63% and four clips past +1.6%. It was the
cheapest experiment of the three and it answers more than itself: cutting the
survivor list by RANK bounds what cutting it by SCORE could ever buy, so the
+0.63% closes the tighter-threshold direction as well.

**Nothing was composed.** The composition of the arms that pass is the P gate
by itself; composing a refusal with a pass only moves the refusal's damage into
the default.

The measurement is `scripts/hd_band.py`, which is new here: twelve 720p and
1080p clips, five rungs from CRF 23 to 35, matched achieved bitrate, several
arms against one default curve. The standing twelve-clip ladder is seven CIF
clips and five HD ones, and every one of the five refusals this item inherited
was taken on the CIF half. Two of the three candidates here read clean on a
median and lose two to three points of VMAF-NEG on one 1080p clip, which is
what the HD band exists to see.

Gates: `make test` 10/10 with regress 8/8; conformance with nine new cells
under "lowrate gates", one per mode plus the composition, a 4:2:2 clip and an
odd geometry, all recon-matched; five new `san_matrix` cells, one per mode and
one composed, at CRF rather than CQP because these gates read a lambda;
`determ_repeat` with `--p-part-gate 400` under load; `tsan_catch`;
`env_gate_audit` 0 TRAP; `hygiene_check` clean; `knob_census` regenerated. The
default moves, deliberately: over the ten board clips at CRF 23 it writes
-0.31% to +0.13% of the previous bytes, and `t8 == t12` and repeats byte for
byte at each.

## 16. ABI 3: the parameter struct declares its own size

`yah264_param_t` gained a `size` field, first in the struct.
`yah264_param_default()` writes it with `sizeof(yah264_param_t)` and
`yah264_encoder_open()` refuses a struct whose size is not the library's own,
naming both numbers. `YAH264_ABI_VERSION` is 3 and the soname moved with it,
so an ffmpeg built against ABI 2 cannot load an ABI 3 install at all.

It exists because of a board, not a crash. The in-process speed board on
2026-09-19 ran an ffmpeg whose wrapper had been built before B-partitions added
a field to the struct. The wrapper compiled, linked and loaded -- the soname
matched and every symbol resolved -- and handed the library a struct in the old
layout, which the library read as the current one. Every field past the added
one was some other field's bytes. It encoded. park_joy solved to CRF 42.7 at 55
Mbps and timed 32x slower than x264, and nothing in the loader, the wrapper or
the encoder said a word. The `size` field is the check that case had none of:
one comparison, at a known offset, before any other field is read.

Nothing about an encode moves. The field is written once and read once, and the
ten board clips at CRF 23, QP 26 and the board's ABR targets are byte-identical
against main at one thread and at eight. What a caller has to do is recompile,
which ABI 2 already required of it.

The same item gave the board two guards of its own (docs/instruments.md): the
solve cache is keyed by a hash of all three binaries after a cache from a
retired library produced a table at dsize -12%, and an unmatched dsize now
prints `INVALID: sizes unmatched` and exits non-zero instead of printing a
median that reads like a result.

## 17. HD parity, stage 3

**hd-stage3.** The work a low-rate HD frame pays whether its residual is cheap
or not: the lookahead, the macroblock tree and the deblocking filter. Three
candidates, three refusals, and a measurement that redirects the stage.

Stage 2 priced the decision work. This stage priced the fixed work, by deleting
each stage and reading instructions retired at the SAME BYTE COUNT rather than
at the same CRF. That distinction is the whole measurement. Every fixed stage
here is load-bearing on the operating point -- delete the tree and the same CRF
writes 13% fewer bytes -- so read naively, three of the six deletions come back
appearing to cost MORE than the baseline they took work out of. Solved back
onto the baseline's bytes, on the two 1080p cells the plan names:

| deleted | sunflower | bbb10s | the reference encoder, same deletion |
|---|---|---|---|
| the whole lookahead, window and tree | **-14.75%** | -0.99% | +0.64% / +2.50% |
| the tree alone | -2.52% | +7.96% | (no separate flag) |
| the deblocking filter | +3.53% | -1.27% | +2.52% / +0.88% |
| slice_data emission | -0.71% | -1.09% | -- |

The first row is the stage. Ours is 14.75% of a low-rate 1080p encode on one
cell; deleting the reference encoder's makes it SLOWER on both, which is what a
lookahead that earns its keep downstream looks like. At matched rate we retire
1.618x its instructions on that cell and 1.371x with both lookaheads deleted, so
the lookahead carries about a quarter of the excess there.

The third row closed a target the plan had named. The deblocking filter cannot
be deleted for a saving on either encoder and both sit inside the same band, so
its arithmetic is not where the excess lives and no per-edge candidate was
opened. The known gap in deblock kernel coverage is a stage-4 item and stays
there.

**`--lr-settle`, off, and it is the near miss.** The lowres block search starts
at a predictor its already-searched neighbours and the previous field agree on.
Where that leaves almost no residual, the candidate list, the hexagon, the
square refine and the subpel diamonds are all bought for a block whose answer
was in hand. Deleting them is worth **-3.45% of instructions** at a threshold of
2 SAD per lowres pixel and **-1.18%** at 1, and it reaches the whole lookahead,
because the motion field and the tree's walk run the same block search.

Its band is the best any candidate of either stage has produced: at 1 the median
is -0.12%, the mean **-0.21%**, and ten of twelve clips are ahead of or level
with the default. It still costs **+0.51% on sunflower_1080p**, and the bar is
read per clip because half a corpus can sit under a median. Two controls settle
that. A near-inert arm -- one measured as a null on instructions but still
perturbing the tree's offsets -- reads -0.09% to +0.13% on the same clips, which
puts the HD band's per-clip floor at about +/-0.15 rather than the deep band's
+/-1.2; and shifting the whole ladder keeps the sign at +0.32%. So the clip is
paying, and the gate saturates on both sides: raising the threshold buys no more
instructions and halving it gave up two thirds of the saving to take the worst
clip from +0.86% to +0.51%.

**`--lr-subgate`, off, refused twice.** Skipping the lowres subpel refine on an
already-cheap whole-pel winner is worth -1.61% alone and costs +1.10% on its own
worst clip; on top of the settle exit it adds two tenths of a percent, because
the two doors open onto the same blocks.

**`--mbt-depfloor`, off, and it is a clean null.** Refusing the tree's deposit
where the block's propagation fraction is negligible reads inside a quarter of a
percent of the baseline at every threshold on both cells, in both directions.
The tree's time is in its motion search, not in its accumulators, and that
number closes the whole "cheapen the deposit" direction rather than one
threshold of it. It earned its keep as the noise control above.

Nothing ships on, so every preset is byte-for-byte what it was: the identity cmp
is 60 of 60 cells across three rate modes and two thread counts. What the stage
leaves behind is the table, and the table says the prize is still there. The
largest untouched bucket is the tree's own memo hit rate -- 72 sources per
encode with no reusable pair field, 28% of the walk -- which is a key, not a
quality trade, and it is the next item.

## 18. HD parity, stage 4

Stage 3 priced the fixed cost per frame. This stage asks a narrower question:
of the work our SIMD already covers, how good are the kernels, family by
family, against the best hand-written assembly in the field.

The measurement came first. Every kernel family was timed on this machine in
both projects' own benchmark harnesses, at the same block shapes, single
threaded, and set beside what the family is worth: `Y264_ASM_OFF=<class>`,
instructions retired, on the two matched-rate 1080p cells stage 0 named.

The share table is the part worth carrying. **Pixel metrics are 32.0% and
29.3% of a low-rate 1080p encode**, motion compensation 9.5% and 8.9%,
transforms 4.2% and 4.5%, coefficient scans and deblocking under 4% each. The
nine classes sum to within a tenth of a point of switching all of them off at
once, and every one of the twenty encodes wrote the same bytes, which is the
bit-exactness claim restated for free.

**The class that is a third of the encode has no gap to close.** Our pixel
metrics are ahead of or level with the reference's assembly on thirteen of
seventeen shapes, and the four losses are under a nanosecond each. That
answers the question the stage was set: the deficit is not craft.

Two families were rewritten.

**Sum of squared differences, by dot product.** The old kernel folded two
widening products into one accumulator, so every row of a block waited on the
previous row -- and at 8x8 it was slower than its own C. Dotting the byte
differences with themselves lands the squares in 32-bit lanes directly, four
chains keep them independent, and the shapes the encoder asks for are
straight-line. 16x16 goes 6.96 ns to 3.07, 8x8 2.36 to 1.71, both now ahead of
the reference's 3.72 and 1.93. Worth **-0.27% and -0.29%** of instructions on
the two cells.

**Boundary strength, half the compares.** One compare per side per list
answers both "used in this list" and "intra", and the motion test folds the
two components with a max before the threshold instead of thresholding both.
18.25 ns to 15.6. **And it is a null on the encode**, at -0.055% and +0.040%,
because the kernel runs once per macroblock and was 0.28% to begin with. It
merges because it is strictly less work, and it is recorded because it is the
second measured instance of the rule that a checkasm multiple is necessary and
never sufficient.

Three things were refused with their numbers. The **whole-edge deblocking
filter** stays refused: the gap reproduces at 1.79x rather than the 5x that was
claimed for it, out of a class bounded by 3.5%, and the last time it was built
its wall was null. The **int16 inverse 8x8 transform** stays refused on the
overflow proof the code already carries. The **half-pel plane cache** is not a
kernel question at all and is the largest thing this stage found: our
`hpel` class is a measured null on both cells because the encoder filters per
block where the reference filters per frame. That is a design item, and it is
written up for the owner rather than opened here.

Every kernel is bit-exact with its C reference under checkasm's page guards,
the identity cmp against the previous build is 60 of 60 cells byte-identical
in three rate modes at two thread counts, and running with all SIMD disabled
reproduces both the bitstream and the reconstruction byte for byte on the
seven CIF clips.

## 19. The tree's memo key

**mbt-memo.** Stage 3 left the tree's Phase A memo hit rate as its largest
untouched bucket: 72 sources per encode with no reusable pair field, 154 ms of
the 553 ms walk on sunflower_1080p. A memo miss on a source that has not
changed is a keying defect, so this item went looking for one. There is none.
The 72 are genuinely new sources and the item closes with the number.

`Y264_MBT_MISSWHY` splits MBT_SPLIT's `nobleg` bucket by the field that was
missing, and `=2` prints one line per miss. Every miss on both cells is the
same shape: a typed leaf, both leg buffers allocated, `bleg_have` clear, and
**no future anchor in the walk's bracket set at all**. That holds on 72 of 72
on sunflower_1080p and 48 of 48 on bbb10s_1080p_o120. They are the window's
last B run, the leaves sitting past the final typed anchor. A leaf's pair legs
are written by the finalize of the anchor AFTER it, and for these leaves that
anchor has not been typed yet. So there is no stored field to key against,
over-specified or otherwise. The key itself is two anchor POCs and nothing
else. It carries no thread id and no frame index.

The offsets say the same thing structurally. Every miss lands at ring offset 36
or 37, the two frontier positions, except where a scene cut or an IDR truncates
the enumeration early and the frontier lands sooner. Each frontier leaf is then
computed a second time on the next walk, under its real bracket, which is what
MBT_SPLIT's `futkey` column counts. That second compute is the honest one and
the first one is the deposit the walk needs now.

One candidate is left behind, and it is a quality question rather than a keying
one. The FIRST leaf of a frontier run has a previous-frame leg whose reference
IS its past anchor, so a field with the right geometry does exist for half of
the 72. It is a whole-pel field and the reuse path consumes quarter-pel, and
handing it over swaps a full diamond for a three-candidate eval, which moves
bits. That is a band-gated item, not this one.

The instrument ships default-inert and nothing else moved. The two cells are
md5-identical to main, MBT_REC/MBT_PLAY replays clean on both, and the identity
cmp is 60 of 60 cells across three rate modes and two thread counts.
Instructions retired move +0.010% on sunflower_1080p and -0.010% on
bbb10s_1080p_o120, both inside the counter's own 0.03% floor.

## 20. MC from the half-pel planes

Stage 4's motion-compensation table ended on a design question rather than a
kernel one. Our qpel luma predictor ran the six-tap filter from the integer
plane on the call, where the reference encoder reads a half-pel plane built
once per frame. The two rows were not commensurable and the record said so.
This item went to route our motion compensation onto the planes the encoder
already builds.

**Most of it was already routed, and that is the item's first finding.** The
plane read has been the default path for the sub-pel probes and for every RD
candidate since the S1 and F1 work. On `sunflower_1080p` at its matched CRF,
180 frames, the op ledger counts 4.36 G pixels predicted off the planes and
95 M through the six-tap: 97.9% of luma MC pixels never touch the filter. The
six-tap calls that remain are two shapes. One is the P_Skip candidate, which
is 367,200 of the 373,982 remaining calls -- exactly one per P macroblock,
because that site called `y264_mc_luma` directly. The other is the
out-of-window fallback, which is correct and stays: a block whose read window
falls outside the planes' allocated border has no plane to read.

So the item is the P_Skip site plus the kernel that makes routing it possible.
`y264_mc_luma_hp` is the plane read as one function: the sixteen quarter-pel
positions as a pair of tables per 8.4.2.2.1, a strided copy for the four
whole/half positions and a two-tap rounding average for the twelve quarter
ones. It takes the destination stride, which the previous plane read did not:
the RD candidates all write a stride-16 block, and the P_Skip candidate writes
straight into the reconstruction at its own stride. `y264_me_mc_luma_s` is the
registry lookup and the window test around it, and `y264_me_mc_luma` is that
with the stride fixed at 16.

The staircase needs no new argument at the skip site. The candidate is already
refused when its vertical MV exceeds `stair_mvy_max`, which is the same cap the
sub-pel searches read the planes under, and the row gate publishes half-pel
rows ahead of the bound either of them can touch.

**The kernel is inline in the header, and the measurement is why.** Put the
body in `mc.c` and call it across the translation unit, and the board reads
+0.22% to +0.46% instructions on all twelve cells -- the plane read is called
tens of millions of times per HD frame, and at that rate the call costs more
than the copy it wraps. Moved back inline, with `y264_mc_luma_hp` in `mc.c`
left as the same body behind a linkable symbol for checkasm, the same board
reads between -0.284% and +0.018%: down on eleven of twelve cells,
-0.158% on `sunflower_1080p` and -0.012% on `bbb10s_1080p_o120`. The one
positive cell is inside the counter's own floor.

This item may not move a bit and does not. The identity cmp is 60 of 60 cells
byte-identical against a main build -- ten board clips, CRF and CQP and ABR,
one thread and eight -- with `--dump-recon` on the CIF-7 clips and both SIMD
escapes on top of it. The new `mc_luma_hp` checkasm group drives the kernel
against `y264_mc_luma_c` at all sixteen phases and all seven partition shapes,
from the far border to the far border, with the row-padding check and a page
guard sized to exactly the window each plane declares.
