# What an orchestrator needs from an encoder

All three sibling encoders implement this interface. One orchestration
layer, in its own repository, can then drive yah264, yah265 or yaav1. The yah264
side is built, per-frame forces included. Nothing here names an H.264
concept the other two lack.

## Why an interface at all

Per-shot encoding in the Netflix style is a search above the encoder. It
encodes each shot at several operating points, measures them and builds the
shot's rate-quality curve. It then picks one point per shot so the title
sits at one quality-per-bit slope, and produces the final stream. The search
needs three promises from the encoder:

1. **It tells the orchestrator where the shots are and what they cost,
   without a trial encode.** The orchestrator must not run its own analysis
   pass for costs the lookahead already computes. The shot table comes from
   an analysis pass over the whole input before the encode, keeping one byte
   per frame instead of the pictures. That costs a second read of the file
   but never holds the file in memory. The table can be saved and reused.
   Files only: a pipe can't be read twice.
2. **It takes a plan** of forced keyframes and per-frame-range
   operating-point offsets inside one encode, without splitting the input.
3. **A shot encoded alone is byte-identical to that shot inside the full
   encode**, given the same parameters *and the same pinned frame-thread
   count*. Without a pinned count, a GOP instance is sized from the
   whole-machine budget and the bytes move. Without the promise, a title
   assembled from the shots' probes is a different stream. Each partial
   re-encode would then be a full one.

Promise 3 is guaranteed at `--threads 1` and best effort with more threads,
where byte-exact output isn't required. With threads, an orchestrator must
treat a re-encoded shot as a valid drop-in segment with its own parameter
sets.

Two conveniences also matter: per-frame type and QP without decoding, and
output already split at the plan's keyframes so a packager or partial
re-encode can address a shot as a file.

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

| rule | detail |
|---|---|
| frame indices | count input frames from zero, in input order, everywhere: shot table, plan, stats and segments |
| plan keyframes | always start a new GOP, so segments and zones align. A zone's QP offset applies to whole frames and needs no alignment |
| zone offsets | the encoder runs its own rate control inside a zone, CRF's per-frame terms included, and adds the offset after it. A shot's rate-quality curve is a function of the offset alone |
| forced QP | the coded slice QP; doesn't compose with an offset. The zone offset, the frame-type cascade and the buffer act before it and can't move it. Use zones for a curve, forces to replay a decision already made |
| refused forces | never approximated. A B frame the lookahead can't place fails the encode with the frame number on stderr. Four placements fail: the first frame of the stream, a frame the key-frame interval has claimed, the last frame and one past the B-run limit |
| anchor on a key frame's slot | yields to the key frame, the one case where the answer is a superset of the request |
| segments | repeat the parameter sets. Concatenated in order, they give a valid stream equal to the unsplit output |
| parameter struct | created with `yah264_param_default()`. It records its own size. `yah264_encoder_open()` refuses a size that isn't the library's. A caller built against an older header fails at open instead of encoding with shifted fields |
| default stream | unchanged. With no plan and no pinning, the output is byte-identical to the encoder without these hooks |

## What we left out of the interface on purpose

| left out | why |
|---|---|
| resolution switching inside one encode | a rung is one encode of the plan at one resolution. The orchestrator owns the scaler and the ladder |
| a quality metric | the encoder reports bits and decisions. The orchestrator scores the decode with the metric it's optimizing |
| trial encodes inside the encoder | the plan is the whole message. The pre-scan behind the shot table is a second read of the input and doesn't count as a trial encode |

## Checklist for yah265 and yaav1

- a shot table from the lookahead
- a zone list with keyframe force and QP offset
- per-frame type and absolute-QP forces over the same index space
- per-instance thread pinning and a determinism test like
  `scripts/shot_determinism.sh`
- per-frame stats in coding order with an index-to-packet map
- per-GOP output with repeated parameter sets

The CLI flag names above are the shared spelling.
