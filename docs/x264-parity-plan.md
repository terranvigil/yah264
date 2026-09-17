# The x264 parity programme: closing the remaining feature gaps

## Context

The option census (x264 `--fullhelp` r3222 against `cli/yah264_cli.c:1995-2223`)
found ~60 x264 options yah264 lacks: ~30 are plumbing (the behaviour exists behind
a `Y264_*` env knob or a hard-coded literal), ~20 are real coding or rate-control
features, and a handful are structural (field pictures, slices, runtime bit depth).
The README claims "roughly the same feature set and options as x264"; this plan
makes that true, or names what we refuse. Decisions of record: PAFF only (no
MBAFF), runtime 10-bit via the yah265 two-library pattern with 8-bit output
byte-identical, no OpenCL, everything else tiered and done, three worktrees at a
time with one heavy (band/board) leg per wave. Exit criterion: a census re-run in
which every x264 option is PRESENT, RENAMED, or on the published refusal list.

Repo: the `yah264-int` checkout (branch `main`). Worktrees:
`git worktree add ../yah264-<id> -b item-<id> main`. Each item gets a brief, one
worktree, one gated merge. Trailer:
`Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`. No x264 internals named
in anything that ships.

## What we don't do (see docs/what-we-dont-do.md)

| x264 option | Recommendation | Reason |
|---|---|---|
| `--opencl*` | Refuse (decided) | No OpenCL target; GPU path is Metal. |
| MBAFF | Refuse (decided) | 8-12k lines through the macroblock/CABAC/deblock hot paths; PAFF covers field sources. |
| `--sliced-threads` | Refuse | Threading is GOP-parallel plus row wavefront; slices as a threading vehicle cost bits and buy nothing here. |
| `--muxer` `--demuxer` `--input-fmt` `--index` `--vf` | Refuse | Containers and filters are ffmpeg's job; we take y4m/raw and AVFrames. |
| `--thread-input` | Refuse | Avisynth only. |
| `--non-deterministic` | Refuse | Determinism is a published guarantee. |
| `--cpu-independent` | Always on, no flag | Every kernel is checkasm-equal to its C reference. |
| `--tcfile-*` `--timebase` `--force-cfr` `--dts-compress` | Refuse | Elementary-stream output; the ffmpeg wrapper owns timing. |
| `--pulldown` | Refuse (lean) | Telecine flagging for a broadcast mux nobody uses here. |
| `--dump-yuv` | Renamed `--dump-recon` | Same thing. |
| `--slow-firstpass` | Refuse | Pass 1 already runs the full preset; the allocator is the value. |
| `--psnr` `--ssim` | Refuse | Scored externally by the harness; in-encoder metrics cost wall on every encode. |
| `--avcintra-class` | Refuse | Broadcast intra-only profile with vendor CQM tables and padding; heavy, niche. |
| `--output-csp` | Refuse | No conversion stage; ffmpeg converts. `--output-depth` is the one exception (C3). |
| `--lookahead-threads` `--mvrange-thread` | Absorbed | One thread budget; the staircase lag is the MV constraint. |
| `--input-depth` | Auto | The library is picked from the input tag (C3). |
| `--bluray-compat` | Owner decides after slices + HRD + `--b-pyramid strict` ship | It is a constraint bundle over those three. |

Everything else absent is built below.

## Conventions for every item

Identity cmp = the ten board clips (`scripts/parity-clips.sh:101`) at `--crf 23`,
`--qp 26`, `--bitrate <board rate>`, threads 1 and 8, `cmp` against a main build:
defaults must be byte-identical unless the item says otherwise. Standard gate =
`make test`, `make conformance` (fast), `scripts/stress_threads.py`,
`scripts/tsan_catch.sh`, `scripts/env_gate_audit.py`, `scripts/hygiene_check.sh`,
`scripts/determ_repeat.sh` (with `ARGS=` for a new mode, run under load),
`scripts/san_matrix.sh`. New stream features join `scripts/conformance.sh` via
`add "<label>" check_clip ...` lines (job list `:355-503`, fixtures `:285-354`,
bump `FIXVER`). Each item updates `docs/options.md` (PRESENT/RENAMED/refused row)
in its commit and adds its `docs/what-shipped.md` row on merge. Env knobs keep
overriding new flags (the `--subpel` convention). Heavy legs (band/board) run on a
quiet box (load < 3), jobs x threads <= 18, one at a time.

## Wave 1: A-plumb, T-oracles, C3-10bit (heavy leg: 10-bit board)

**A-plumb** (L, one item, defaults byte-identical). Flags by where they land:
- Env to flag (`cli/yah264_cli.c` parser + `yah264_param_t` field + reader):
  `--aq-mode` (`macroblock.c:308`), `--no-mbtree` (`encoder.c:4742`),
  `--ipratio`/`--pbratio` (`encoder.c:4587-4588`), `--cplxblur`/`--qblur`
  (`encoder.c:4581-4583`), `--no-dct-decimate` (`macroblock.c:3479`), `--no-asm`,
  `--no-psy`, `--no-fast-pskip`.
- Literal to flag (`src/encoder/encoder.c`): `--deblock a:b`/`--no-deblock`
  (`:2727-2730`, offsets into `src/encoder/deblock.c`), `--b-pyramid none|normal`
  (`:4263`; `strict` deferred to C4), `--no-weightb` (`:4407`),
  `--chroma-qp-offset` (`set.h:60`, `:2438`), `--qpmin`/`--qpmax`/`--qpstep`,
  `--vbv-init` (`:10590`), `--mvrange` (`src/encoder/me.c`), `--sps-id`
  (`:4262`), `--pass 3`.
- `--profile baseline|main|high|high10|high422|high444`: validate and constrain
  (`encoder.c:4255-4260`), write `constraint_set*` flags in `set.c`.
- New NAL/SEI syntax (new `src/encoder/sei.c`, called from AU assembly at
  `encoder.c:5236-5240`): `--aud`, `--pic-struct` (VUI flag `set.c:163` +
  pic_timing SEI), `--frame-packing`, `--cll`, `--mastering-display`,
  `--alternative-transfer`, `--fake-interlaced`, `--overscan`/`--videoformat`
  (VUI), `--stitchable` (content-independent SPS).
- CLI: raw-yuv reader with `--input-res`/`--input-csp`/`--input-range`/`--fps`,
  `--crop-rect`, `--seek`, `-v`/`--quiet`/`--no-progress`/`--log-level`,
  `--tune stillimage|fastdecode` (`src/encoder/params.c` tune table).
Gate: identity cmp with no flags; one `add "plumbing"` conformance line per flag
that changes the stream; `level_check.py` on the profile cells; standard gate;
`knob_census --check` stays green. Accept: identity clean, all adds recon-match.

**T-oracles** (M, tooling only): `scripts/hrd_check.py` (parse VUI HRD,
buffering_period, pic_timing; simulate the CPB; self-test on `x264 --nal-hrd cbr`
output); `scripts/level_check.py` (Annex A Table A-1; declared level equals the
auto pick and is never exceeded); `scripts/conformance.sh` gains
`YAH264_CONF_DECODERS="ffmpeg openh264 jm"` with per-decoder `md5frames`, openh264
via `scripts/openh264-shim.sh`, JM `ldecod` built from JVT source under
`tools/jm/` by a fetch script (not vendored; allowed as an oracle by CONTRIBUTING
rule 4). Check whether openh264 decodes field pictures (encode an `x264 --tff`
fixture); if not, JM is PAFF's second oracle. Gate: fast conformance with every
decoder green on main.

**C3-10bit** (L, the yah265 pattern):
1. `src/common/symbols10.h` generated from `nm -g build/libyah264.a` (326
   symbols) as `#define name name_10`; port `scripts/symbol_check.sh` into
   `make test`.
2. `meson.build:141`: two library targets (8-bit: current flags + NEON; 10-bit:
   `-UY264_BIT_DEPTH -DY264_BIT_DEPTH=10 -include symbols10.h`, C only), one
   installed header, pkg-config `yah264` and `yah264_10`, soversion bump.
3. `include/yah264.h`: drop the public `pixel` typedef; `yah264_picture_t.plane`
   becomes `void *plane[3]` (`:94-95`) with stride in samples; recon callback
   (`:496`) and `yah264_scan_shots`/`scan_idr_frames` (`:600,:605`) take `void *`
   plus a depth; `YAH264_API(name)` suffix macro over the 26 public symbols.
4. `cli/yah264_cli.c`: link both static libraries; dispatch table keyed on the
   y4m `p10` tag (the refusal at `:2296-2300` becomes selection);
   `--output-depth 10` upshifts 8-bit input by `<< 2`; recon dumper (`:141-146`)
   writes the library's depth.
5. `../ffmpeg-yah265/libavcodec/libyah264.c`: add `YUV420P10/422P10/444P10`
   (`:103-105`, `:557`), pick the table by pix_fmt, drop the `pixel*` cast
   (`:408-409`).
6. `src/hw/vt.c:175` keeps refusing 10-bit; `scripts/regress.py:93-114` now keeps
   `yuv420p10le` on the default binary.
Gate: 8-bit identity cmp against main (the point of the item); `add "10-bit"`
fixtures (testsrc2 `yuv420p10le`, 420/422/444, cabac/cavlc, b3, 8x8, crop);
`san_matrix.sh` `o10` cell; `symbol_check.sh`; ffmpeg wrapper smoke; heavy leg:
10-bit board (6-8 BVI-AOM clips converted to y4m, `bd_at_rate.py` against
`x264 --profile high10`). Accept: identity clean, 10-bit recon-match, board BD
recorded as the first baseline.

## Wave 2: C2-PAFF-1, B-cintra, C1-slices (heavy leg: slices cost band)

**C2-PAFF-1** (L): `--tff`/`--bff`, I and P field pictures, CAVLC and CABAC, no B
fields yet. Design: a field is a stride-doubled view of the frame planes
(`y264_frame_t` already carries one stride per plane, `macroblock.h:41-65`); SPS
`frame_mbs_only_flag = 0`, `height_in_map_units = hmb/2` (`encoder.c:4391-4393`),
crop units doubled (`:4396-4399`). DPB stays by frame with per-field POC (top `2n`,
bottom `2n+1`, lsb bits widened `:4268-4269`) and per-field reference marking; a
new `build_list0_field()` beside `build_list0` (`:2072-2127`) builds the
alternating-parity list; `frame_num` shared by both fields (`:12116-12153`). Slice
header (`:2453-2470`) gains the `field_pic_flag`/`bottom_field_flag` branch. Field
scan tables in `src/dsp/transform.c` (and RDOQ, CAVLC, CABAC coefficient order);
CABAC field context offsets in `src/encoder/cabac.c`; parity-dependent chroma MV
offset in MC; field deblock rules (intra horizontal edges bS 3, vertical MV
threshold 2) in `src/encoder/deblock.c`; vertical MV range halved. Lookahead and RC
stay frame-based; a frame's type applies to its field pair; weighted P uses field
POC distances. Refuse `--tff` with `--hw`, with `--slices` (until PAFF-2), with B
frames. CLI accepts y4m `It`/`Ib` (`cli:2277-2280`) and writes it on `--dump-recon`.
Gate: fixtures via ffmpeg `tinterlace` (recipes below); `add "PAFF"` cells
(tff/bff, cavlc/cabac, 8x8, mref3, crop, aq); recon-match on weaved frames with
ffmpeg plus the second oracle; `ARGS='--tff' determ_repeat.sh`; `check_threading`
with `--tff`; progressive identity cmp. Accept: two-decoder recon-match, identity
clean.

**B-cintra** (S): `--constrained-intra` sets `pps.constrained_intra_pred_flag`
(`set.c:190`); intra neighbour availability in `macroblock.c` excludes inter
neighbours in P/B slices. Gate: `add` cells (p, b3 cabac, 8x8, crop), identity.

**C1-slices** (L): `--slices N` (`--slice-max-size`/`--slice-max-mbs` later).
Design: independent slices on row boundaries, `first_mb_in_slice = row0 * wmb`.
Neighbour availability (`macroblock.h:97`, `mv_nb_t.avail` `macroblock.c:172-244`,
intra, CAVLC nC, CABAC ctx, P_Skip `:598-605`) becomes "outside the slice" via a
per-row `slice_top[mby]`; `mb_qp_delta` predictor resets per slice. Factor the
slice header out of `encoder.c:2453-2731` into `write_slice_header()`;
`y264_frame_emit` (`macroblock.h:297`) emits one NAL per slice with CABAC/CAVLC
re-init; the three staircase paths that init CABAC (`:12581`, `:14478`, `:15759`)
follow. Deblock stays whole-picture (filters across slice edges,
`disable_deblocking_filter_idc 0`). Wavefront unchanged (slices only remove
dependencies). Gate: `add "slices"` cells (2 cavlc, 4 cabac, b3 8x8, crop, aq,
temporal direct); `check_threading`/`check_determinism` with `--slices 4`;
`ARGS='--slices 4' determ_repeat.sh`; `--slices 1` identity; heavy leg: BD cost at
`--slices 4` on the 1080p band clips (reported, expected 0.5-1.5%). Accept:
recon-match with all decoders, identity clean, cost in the ledger row.

## Wave 3: C2-PAFF-2, B-hrd, B-opengop (heavy leg: open-gop band)

**C2-PAFF-2** (L): B field pictures: implicit bipred on field POCs
(`macroblock.h:60`), temporal direct with field colocated selection and POC
scaling (`macroblock.c:5508-5560`), the staircase B pipeline on field pairs
(`encoder.c:686-695`, `:2414`), `--slices` with `--tff`. Gate: `add "PAFF B"`
cells (b2 cavlc, b3 cabac, pyramid, temporal direct, slices4);
`stress_threads.py` with `--tff --bframes 3`; BD vs `x264 --tff` on the interlaced
fixture board, reported. Accept: two-decoder recon-match, progressive identity.

**B-hrd** (M): `--nal-hrd vbr|cbr`, `--filler`. HRD params in the VUI
(`set.c:161-162`) from `vbv_maxrate`/`vbv_bufsize`; buffering_period SEI at every
IDR and pic_timing SEI per picture (reusing A-plumb's `sei.c`); CBR pads with
filler NAL (type 12). Removal times come from the VBV RC (`encoder.c:10590-10621`).
Gate: `hrd_check.py` on all 36 `cvbr_compliance.sh` cells in both modes;
`add "nal-hrd" check_rc`; `abr_decode_gate.sh`; `vbv_check.py`; identity. Accept:
36/36 HRD-clean, recon-match, identity clean.

**B-opengop** (M): `--open-gop`: non-IDR I at keyint with a recovery_point SEI,
leading B frames may reference the previous anchor, DPB not flushed, POC continues.
Files: `encoder.c` GOP/type decision, staircase planner, DPB marking, `sei.c`.
Gate: `add "open-gop"` cells; a new `check_recovery` that decodes from the second I
onward and asserts frames after the recovery point match recon; `Y264_CUT_SPLIT`
IDRs stay IDR; heavy leg: band at keyint 250; identity. Accept: band BD <= 0,
recon-match.

## Wave 4: B-rcbounds, B-weightp, B-partitions (heavy leg: weightp band)

**B-rcbounds** (M): `--crf-max` (VBV-driven QP in CRF may not exceed the mapped
QP, RC sites `encoder.c:10590-11006`), `--ratetol` (ABR overshoot tolerance),
`--qpfile` (per-frame forced type/QP via a `frame_forces` array; the `--plan`
plumbing is the template). Gate: `check_rc` cells, `vbv_check.py`, `--frame-stats`
diffed against the qpfile, identity. Accept: forced frames honoured exactly.

**B-weightp** (M): `--weightp 0|1|2`: 0 clears `weighted_pred_flag`
(`encoder.c:4410`); 1 = today's frame-level fade detection; 2 = per-reference
weight/offset search on the lowres with a duplicate-reference slot. Files:
`encoder.c:2040-2042`, `macroblock.c:827`, `me.c`. Gate: `add` cells on
`syn_fade`; heavy leg: band on fade clips plus the standard band; `--weightp 1`
identity. Accept: BD <= 0 on fades and neutral elsewhere, else 2 ships off.

**B-partitions** (S): `--partitions p8x8,p4x4,b8x8,i8x8,i4x4,none,all` promoting
`Y264_P_RECT`/`Y264_B_RECT` (`macroblock.c:4783`, `:6661`) plus the intra and
p4x4 masks. Gate: `add` per mask class, identity, `knob_census --check`.

## Wave 5: B-intra-refresh, B-cqmfile, B-me-modes (heavy leg: intra-refresh band)

**B-intra-refresh** (M): moving intra column (width from keyint), recovery_point
SEI per period, MV constraint so no MB references un-refreshed area. Files:
`encoder.c` type decision and schedule, `macroblock.c` forced intra + MV clamp,
`me.c`, `sei.c`. Gate: `add` cells; `check_recovery` from mid-stream; heavy leg:
band vs `--keyint` at the same interval (cost reported); identity.

**B-cqmfile** (S-M): `--cqmfile` (JM format), `--cqm4iy/4ic/4py/4pc/8iy/8py`
(+ 4:4:4 chroma 8x8). Parser in `cli`, lists in `y264_cqm_t`, quant tables
`transform.c:517/:910`. Gate: `add` with a checked-in fixture, flat/jvt identity.

**B-me-modes** (M): `--me esa|tesa` in `src/encoder/me.c` behind the selector
(`cli:2142`), `--no-chroma-me`, `--no-mixed-refs`. Gate: `add` cells,
`ARGS='--me esa' determ_repeat.sh`, identity; BD leg deferred to wave 6.

## Wave 6: B-nr, C4-bluray (owner), me-modes BD leg, census exit

**B-nr** (M): `--nr N` adaptive coefficient offset in the quant path
(`transform.c` / `macroblock.c`). Gate: `add nr_100/nr_1000`, identity; heavy leg:
band on the grain set and the standard band. Accept: gain on grain, neutral
elsewhere; ships off by default.

**C4-bluray-compat** (owner decides, S-M if go): `--slices 4` at 1080p, `--nal-hrd
vbr`, `--b-pyramid strict` (finish the deferred half of A-plumb), `--aud`,
`--pic-struct`, per-level ref/keyint caps; `level_check.py` as the gate.

**Census exit**: re-run the census against `x264 --fullhelp r3222`; every option
resolves to PRESENT (flag in `cli/yah264_cli.c`), RENAMED (row in
`docs/options.md`), or REFUSED (row in the published list). Zero unresolved is the
exit; the README parity sentence is written from that run.

## Fixture recipes

- Fields: `ffmpeg -f lavfi -i testsrc2=size=320x240:rate=50 -vf
  tinterlace=mode=interleave_top,setfield=tff -frames:v 24 -pix_fmt yuv420p
  syn_tff.y4m` (and `interleave_bottom`/`bff`); the y4m carries `It`/`Ib`.
- Depth: `ffmpeg -f lavfi -i testsrc2=size=320x240:rate=30 -frames:v 12 -pix_fmt
  yuv420p10le -strict -1 syn_p10.y4m` (and 422p10/444p10), as `san_matrix.sh:33`.
- 10-bit board: `ffmpeg -i /Volumes/seagate/media/train-corpus/bvi-aom/<clip>/
  <clip>_10bit_420.mp4 -pix_fmt yuv420p10le -strict -1 <clip>_p10.y4m` for 6-8
  clips, listed as `CLIPS_P10` in `scripts/parity-clips.sh`.
- Interlaced BD board: `tinterlace` over foreman, park_joy, pedestrian so PAFF has
  a rate-matched reading against `x264 --tff`.

## Cross-cutting docs

`docs/options.md` row per flag per item; `docs/what-shipped.md` row per merge and
a Refused row per confirmed entry of the list above; `docs/plan.md:237-241` Phase 8
status updated once per wave (PAFF replaces "only if a broadcast customer demands
it" once C2 ships); README parity sentence only at census exit.

## Verification

Per item: standard gate + the item's conformance adds + identity cmps; one heavy
leg per wave on a quiet box. After each merge to main: `scripts/regress.py` (after
C3 it includes `yuv420p10le`), full `make conformance` (QPs 0 6 18 26 37 51) with
all configured decoders, `board-bd` / parity status to confirm the goal figures and
the PSNR-Y floor did not move, `determ_repeat.sh` with the accumulated new-mode
`ARGS`. Programme exit is the census re-run above.
