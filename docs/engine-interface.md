# What an orchestrator needs from an encoder

This is the engine interface all three sibling encoders implement. With it,
one orchestration layer in its own repository can drive yah264, yah265 or
yaav1. The yah264 side is built, per-frame forces included. Nothing below
names an H.264 concept the other two codecs lack.

## Why an interface at all

Per-shot encoding in the Netflix style is a search that runs above the
encoder. It encodes every shot at several operating points, measures them
and builds each shot's rate-quality curve. Then it picks one point per shot
so the whole title sits at one quality-per-bit slope before it produces the
final stream. The encoder's part is small and exact. The search depends on
three promises:

1. **It tells the orchestrator where the shots are and what they cost,
   without a trial encode.** The orchestrator must not run its own analysis
   pass to learn costs the encoder's lookahead already computes. The shot
   table comes from an analysis pass over the whole input before the encode
   starts. Because that pass keeps one byte per frame instead of the
   pictures, it costs a second read of the file but never holds the whole
   file in memory. Its answer can be saved and reused. The promise holds for
   files only, since a pipe can't be read twice.
2. **It takes a plan.** The plan holds forced keyframes and per-frame-range
   operating-point offsets inside one encode. The orchestrator doesn't have
   to split the input into files.
3. **A shot encoded alone is byte-identical to that shot inside the full
   encode**, given the same parameters *and the same pinned frame-thread
   count*. Without a pinned count, a GOP instance is sized from the
   whole-machine budget and the bytes move. Without the promise, a title
   assembled from each shot's probes is a different stream. Every partial
   re-encode would then be a full one.

The third promise is guaranteed at `--threads 1`. With more threads it's best
effort, because byte-exact repeatable output isn't required there.
`T=1 scripts/shot_determinism.sh` fails on a mismatch, while the threaded
default only reports one. With threads, an orchestrator must treat a
re-encoded shot as a valid drop-in segment with its own parameter sets, not
as identical bytes.

Two conveniences also matter. One is per-frame decisions such as type and QP
without decoding the stream. The other is output already split at the plan's
keyframes, which lets a packager or a partial re-encode address a shot as a
file.

## The contract

| element | what it is |
|---|---|
| **Shot table** | `[first, last]` per shot, with the lookahead's mean and peak intra cost and mean inter cost. Costs compare across shots of one input |
| **Plan** | zones over input-frame ranges. `idr` forces a keyframe and a GOP boundary at `first`. `qp+N` / `qp-N` offsets every frame's QP in the range on top of the rate control's choice, in CRF, CQP and ABR alike. Zones may not overlap |
| **Frame forces** | one record per named input frame, the plan's finest grain: a frame type (IDR, anchor, B) and an ABSOLUTE coded QP, either of which may be left to the encoder. A frame no record names is left entirely to the encoder. A force the encoder can't place is refused by frame number, never approximated |
| **Determinism** | encoding frames `[a, b)` alone reproduces the bytes the full encode produced for the GOP `[a, b)`, given the same parameters and the same pinned frame-thread count |
| **Frame stats** | per coded frame, in coding order: input index, slice type, keyframe, reference, slice QP, bytes. No decode needed |
| **Segment output** | the stream also written as one file per GOP. A shot is addressable as a file. A re-encoded shot drops into place |

yah264 implements the contract like this:

| element | CLI | library |
|---|---|---|
| Shot table | `--shot-table` writes JSON on stderr and implies `--cut-split`. `--scan-out FILE` saves it with the IDR map. `--scan-in FILE` reuses it and refuses it on any identity mismatch ([options](options.md)) | `yah264_shotscan_open()` / `_push()` / `_finish()` / `_shots()` / `_idr()` stream it a batch of frames at a time. `yah264_shotscan_signature()` names the inputs besides the pixels that the table comes from. `yah264_scan_shots()` takes every frame at once and returns the same table |
| Plan | `--plan FILE`, one zone per line: `first last [idr] [qp+N]`. `--zones` takes the same list in x264's public inline syntax and is refused alongside `--plan`. There `b=F` becomes an offset of -6 log2(F), and `q=N` a zone flagged `YAH264_ZONE_QP` whose value replaces the rate control's base QP instead of adding to it. The flag is yah264's own, not part of the shared contract | `yah264_encoder_set_zones()` |
| Frame forces | `--qpfile FILE`, one line per frame: `<frame> <type> <qp>` | `yah264_encoder_set_frame_forces()` |
| Determinism | `--gop-threads K` pins every GOP instance. `scripts/shot_determinism.sh` checks it: guaranteed and gated at `T=1`, informational with threads | |
| Frame stats | `--frame-stats FILE`, JSON lines that add the GOP index and its frame-thread count | `yah264_encoder_frame_stats()` + `yah264_encoder_frame_order()`; the bytes come from the NALs the same call returns |
| Segment output | `--segment-out PATTERN`, where `%d` is the GOP index. Each segment starts with its own parameter sets | |

Rules the orchestrator can rely on:

- Frame indices count input frames from zero, in input order, everywhere:
  shot table, plan, stats and segments.
- A plan keyframe always starts a new GOP, so segments and zones align. A
  zone's QP offset applies to whole frames and needs no alignment.
- Inside a zone the encoder runs its own rate control, CRF's per-frame terms
  included, and adds the offset after it. A shot's rate-quality curve is then
  a function of the offset alone.
- A forced QP is the other kind of message and doesn't compose with an
  offset. It's the coded slice QP. The zone offset, the frame-type
  cascade and the buffer all act before it and can't move it. An orchestrator
  that wants a curve uses zones. One replaying a decision it already made
  uses forces.
- A force is refused, never approximated. A B frame the lookahead can't place
  fails the encode with the frame number on stderr. Four placements fail:
  the first frame of the stream, a frame the key-frame interval has claimed,
  the last frame and one past the B-run limit. A forced anchor on a key
  frame's own slot yields to the key frame. That's the one case where the
  encoder's answer is a superset of what was asked for.
- Parameter sets repeat per segment. Concatenating the segments in order
  gives a valid stream equal to the unsplit output.
- A library caller creates its parameters with `yah264_param_default()`. The
  struct records its own size, and `yah264_encoder_open()` refuses one whose
  size isn't the library's. An orchestrator built against an older header
  fails at open instead of encoding with shifted fields.
- Nothing here changes the default stream. With no plan and no pinning, the
  output is byte-identical to the encoder without these hooks.

## What we left out of the interface on purpose

- Resolution switching inside one encode. A rung is one encode of the plan at
  one resolution. The orchestrator owns the scaler and the ladder.
- A quality metric. The encoder reports bits and decisions. The orchestrator
  scores the decode with the metric it's optimizing.
- Trial encodes inside the encoder. The plan is the whole message. The
  pre-scan behind the shot table is a second read of the input, as promise 1
  says, and doesn't count as a trial encode.

## Checklist for yah265 and yaav1

- a shot table from the lookahead
- a zone list with keyframe force and QP offset
- per-frame type and absolute-QP forces over the same index space
- per-instance thread pinning and a determinism test like
  `scripts/shot_determinism.sh`
- per-frame stats in coding order with an index-to-packet map
- per-GOP output with repeated parameter sets

The CLI flag names above are the shared spelling.
