# The engine interface: what an orchestrator needs from an encoder

Status 2026-09-06: the yah264 side is built (shot-based plan S4). The
interface is written to be implemented by all three sibling encoders (yah264,
next265, nextav1) so that one orchestration layer, in its own repository, can
drive any of them; nothing below names an H.264 concept that the other two
codecs lack.

## Why an interface at all

Per-shot encoding in the Netflix style is a search run above the encoder:
encode every shot at several operating points, measure, build each shot's
rate-quality curve, pick one point per shot so the title sits at one
quality-per-bit slope, then produce the final stream. The encoder's part is
small and exact, and every stage of the search depends on three promises the
encoder has to make:

1. **It tells the orchestrator where the shots are, and what they cost,
   without a trial encode.** The lookahead already computes per-frame costs;
   the orchestrator must not run its own analysis pass to learn what the
   encoder knows.
2. **It takes a plan.** Forced keyframes at chosen frames, and per-frame-range
   operating-point offsets, inside one encode, without the orchestrator having
   to split the input into files.
3. **A shot encoded alone is byte-identical to that shot inside the full
   encode**, given the same parameters. Without this, probing a shot's curve
   and then assembling the title from the probes is not the same stream, and
   every partial re-encode is a re-encode of everything.

Plus two conveniences that turn out to matter: per-frame decisions (type, QP)
without decoding the stream, and output already split at the plan's
keyframes so a packager or a partial re-encode can address a shot as a file.

## The contract

| element | what it is | yah264 (CLI / library) |
|---|---|---|
| **Shot table** | `[first, last]` per shot, with the lookahead's mean and peak intra cost and mean inter cost; costs comparable across shots of one input | `--shot-table` (JSON on stderr); `yah264_scan_shots()` |
| **Plan** | zones over input-frame ranges: `idr` forces a keyframe (and a GOP boundary) at `first`; `qp+N` / `qp-N` offsets every frame's QP in the range on top of whatever the rate control chose, in CRF, CQP and ABR alike; zones may not overlap | `--plan FILE`, one zone per line `first last [idr] [qp+N]`; `yah264_encoder_set_zones()` |
| **Determinism** | encoding frames `[a, b)` alone, with the same parameters and the same pinned frame-thread count, reproduces the bytes the full encode produced for the GOP `[a, b)` | `--gop-threads K` pins every GOP instance; `scripts/shot_determinism.sh` is the test (5/5 GOPs on the CIF sequence, K=2; the 720p sequence at K=3) |
| **Frame stats** | per coded frame, in coding order: input index, slice type, keyframe, reference, slice QP, bytes; no decode needed | `--frame-stats FILE` (JSON lines, plus the GOP index and its frame-thread count); `yah264_encoder_frame_stats()` + `yah264_encoder_frame_order()` (bytes come from the NALs the same call returns) |
| **Segment output** | the stream also written as one file per GOP, so a shot is addressable as a file and a re-encoded shot drops into place | `--segment-out PATTERN` (`%d` = GOP index; each segment starts with its own parameter sets) |

Rules the orchestrator can rely on:

- Frame indices count input frames from zero, in input order, everywhere
  (shot table, plan, stats, segments).
- A plan keyframe always starts a new GOP, so segments and zones align; a
  zone's QP offset applies to whole frames and does not require alignment.
- The rate control inside a zone is the encoder's own (CRF's per-frame terms
  included); the offset is added after it. This is what makes a shot's
  rate-quality curve a function of the offset alone.
- Parameter sets are repeated per segment; concatenating segments in order is
  a valid stream and equals the unsplit output.
- Nothing here changes the default stream: with no plan and no pinning the
  output is byte-identical to the encoder without these hooks.

## What is deliberately not in the interface

- No resolution switching inside one encode: a rung is one encode of the
  plan at one resolution; the orchestrator owns the scaler and the ladder.
- No quality metric: the encoder reports bits and decisions; the orchestrator
  scores the decode with the metric it is optimising.
- No trial encodes inside the encoder: the plan is the whole message.

## Checklist for next265 and nextav1

Shot table from the lookahead; a zone list with keyframe force and QP offset;
per-instance thread pinning and a determinism test like
`scripts/shot_determinism.sh`; per-frame stats in coding order with an
index-to-packet map; per-GOP output with repeated parameter sets. The CLI flag
names above are the shared spelling.
