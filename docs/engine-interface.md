# The engine interface: what an orchestrator needs from an encoder

Status 2026-09-06: the yah264 side is built (shot-based plan S4). The
interface is written to be implemented by all three sibling encoders (yah264,
yah265, yaav1) so that one orchestration layer, in its own repository, can
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
   encoder knows. No trial encode, but not free either: the shot table comes
   from an analysis pre-scan that needs the whole input, so this promise is a
   file promise and not a pipe one.
2. **It takes a plan.** Forced keyframes at chosen frames, and per-frame-range
   operating-point offsets, inside one encode, without the orchestrator having
   to split the input into files.
3. **A shot encoded alone is byte-identical to that shot inside the full
   encode**, given the same parameters *and the same pinned frame-thread
   count*. The pinning is not a footnote: without it a GOP instance is sized
   from the whole-machine budget and the bytes move. Without the promise,
   probing a shot's curve and then assembling the title from the probes is not
   the same stream, and every partial re-encode is a re-encode of everything.
   *2026-09-25: byte-exact repeatable output is no longer a requirement
   (owner), so this promise is now best effort. `scripts/shot_determinism.sh`
   reports it and no longer fails on it; an orchestrator must treat a
   re-encoded shot as a valid drop-in segment (each segment carries its own
   parameter sets), not as the identical bytes. Whether the promise should be
   kept as a product feature is an owner call.*

Plus two conveniences that turn out to matter: per-frame decisions (type, QP)
without decoding the stream, and output already split at the plan's
keyframes so a packager or a partial re-encode can address a shot as a file.

## The contract

| element | what it is | yah264 (CLI / library) |
|---|---|---|
| **Shot table** | `[first, last]` per shot, with the lookahead's mean and peak intra cost and mean inter cost; costs comparable across shots of one input | `--shot-table` (JSON on stderr); `yah264_scan_shots()` |
| **Plan** | zones over input-frame ranges: `idr` forces a keyframe (and a GOP boundary) at `first`; `qp+N` / `qp-N` offsets every frame's QP in the range on top of whatever the rate control chose, in CRF, CQP and ABR alike; zones may not overlap | `--plan FILE`, one zone per line `first last [idr] [qp+N]`; `yah264_encoder_set_zones()` |
| **Frame forces** | one record per named input frame, the finest grain of the same plan: a frame type (IDR, anchor, B) and an ABSOLUTE coded QP, either of which may be left to the encoder. A frame no record names is left entirely to the encoder. A force the encoder cannot place is refused by frame number rather than approximated | `--qpfile FILE`, one line per frame `<frame> <type> <qp>`; `yah264_encoder_set_frame_forces()` |
| **Determinism** | encoding frames `[a, b)` alone, with the same parameters and the same pinned frame-thread count, reproduces the bytes the full encode produced for the GOP `[a, b)` | `--gop-threads K` pins every GOP instance; `scripts/shot_determinism.sh` reports it (5/5 GOPs on the CIF sequence, K=2; the 720p sequence at K=3), informational since 2026-09-25 |
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
- A forced QP is the other kind of message, and the two do not compose: it is
  the coded slice QP, so the zone offset, the frame-type cascade and the buffer
  are all upstream of it and none of them moves it. An orchestrator that wants
  a curve uses zones; one that is replaying a decision it has already made uses
  forces.
- A force is refused, never approximated. A B frame the lookahead cannot place
  fails the encode with the frame number on stderr; the placements that fail
  are the first frame of the stream, a frame the key-frame interval has
  claimed, the last frame, and one past the B-run limit. A forced anchor on a
  key frame's own slot yields to the key frame, which is the one case where the
  encoder's answer is a superset of what was asked for.
- Parameter sets are repeated per segment; concatenating segments in order is
  a valid stream and equals the unsplit output.
- Nothing here changes the default stream: with no plan and no pinning the
  output is byte-identical to the encoder without these hooks.

## What is deliberately not in the interface

- No resolution switching inside one encode: a rung is one encode of the
  plan at one resolution; the orchestrator owns the scaler and the ladder.
- No quality metric: the encoder reports bits and decisions; the orchestrator
  scores the decode with the metric it is optimising.
- No trial encodes inside the encoder: the plan is the whole message. The
  analysis pre-scan behind the shot table is not a trial encode, but it is a
  second read of the input; see promise 1.

## Checklist for yah265 and yaav1

Shot table from the lookahead; a zone list with keyframe force and QP offset;
per-frame type and absolute-QP forces over the same index space;
per-instance thread pinning and a determinism test like
`scripts/shot_determinism.sh`; per-frame stats in coding order with an
index-to-packet map; per-GOP output with repeated parameter sets. The CLI flag
names above are the shared spelling.
