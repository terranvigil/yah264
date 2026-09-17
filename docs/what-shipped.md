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
