# The engine interface: what an orchestrator needs from an encoder

Status: the yah264 side is built, including per-frame forces and the
threaded determinism rule below. The interface is written for all three sibling encoders (yah264,
yah265, yaav1) to implement, so that one orchestration layer in its own
repository can drive any of them. Nothing below names an H.264 concept that the other
two codecs lack.

## Why an interface at all

Per-shot encoding in the Netflix style is a search run above the encoder. It
encodes every shot at several operating points, measures them and builds each
shot's rate-quality curve. Then it picks one point per shot so the title sits
at one quality-per-bit slope, and produces the final stream. The encoder's part
is small and exact. Every stage of the search depends on three promises the
encoder has to make:

1. **It tells the orchestrator where the shots are, and what they cost,
   without a trial encode.** The lookahead already computes per-frame costs.
   The orchestrator must not run its own analysis pass to learn what the
   encoder knows. The table needs no trial encode but is not free either.
   It comes from an analysis pass over the whole input before the encode
   starts. That pass keeps one byte per frame instead of the pictures. It
   costs a second read of the file, and the file's size in memory is spared. Its
   answer can be saved and reused. A pipe cannot be read twice, so this promise
   holds for files only.
2. **It takes a plan.** The plan carries forced keyframes at chosen frames and
   per-frame-range operating-point offsets inside one encode. The orchestrator
   does not have to split the input into files.
3. **A shot encoded alone is byte-identical to that shot inside the full
   encode**, given the same parameters *and the same pinned frame-thread
   count*. The pinning matters. Without it a GOP instance is sized from the
   whole-machine budget and the bytes move. Without the promise, assembling
   the title from the probes of each shot's curve gives a different stream.
   Every partial re-encode then becomes a re-encode of everything.
   *The promise is guaranteed at `--threads 1`. With more
   threads it is best effort, since byte-exact repeatable output is no longer a
   requirement there. `T=1 scripts/shot_determinism.sh` fails on a mismatch,
   and the threaded default only reports it. With threads, an orchestrator must
   treat a re-encoded shot as a valid drop-in segment (each segment has its
   own parameter sets), not as the identical bytes.*

Two conveniences also turn out to matter. One is per-frame decisions (type,
QP) without decoding the stream. The other is output already split at the
plan's keyframes, so a packager or a partial re-encode can address a shot as a
file.

## The contract

| element | what it is | yah264 (CLI / library) |
|---|---|---|
| **Shot table** | `[first, last]` per shot, with the lookahead's mean and peak intra cost and mean inter cost; costs comparable across shots of one input | `--shot-table` (JSON on stderr; implies `--cut-split`); `--scan-out FILE` saves it with the IDR map and `--scan-in FILE` reuses it, refused on any identity mismatch (docs/options.md); `yah264_shotscan_open()` / `_push()` / `_finish()` / `_shots()` / `_idr()` stream it a batch of frames at a time, and `yah264_shotscan_signature()` names what it depends on besides the pixels; `yah264_scan_shots()` takes every frame at once and returns the same table |
| **Plan** | zones over input-frame ranges: `idr` forces a keyframe (and a GOP boundary) at `first`; `qp+N` / `qp-N` offsets every frame's QP in the range on top of whatever the rate control chose, in CRF, CQP and ABR alike; zones may not overlap | `--plan FILE`, one zone per line `first last [idr] [qp+N]`; `yah264_encoder_set_zones()`. `--zones` is x264's inline spelling of the same list, refused alongside `--plan`: `b=F` becomes an offset of -6 log2(F), and `q=N` a zone flagged `YAH264_ZONE_QP`, whose value replaces the rate control's base QP instead of adding to it. The flag is yah264's own and not part of the shared contract |
| **Frame forces** | one record per named input frame, the finest grain of the same plan: a frame type (IDR, anchor, B) and an ABSOLUTE coded QP, either of which may be left to the encoder. A frame no record names is left entirely to the encoder. A force the encoder cannot place is refused by frame number rather than approximated | `--qpfile FILE`, one line per frame `<frame> <type> <qp>`; `yah264_encoder_set_frame_forces()` |
| **Determinism** | encoding frames `[a, b)` alone, with the same parameters and the same pinned frame-thread count, reproduces the bytes the full encode produced for the GOP `[a, b)` | `--gop-threads K` pins every GOP instance; `scripts/shot_determinism.sh` reports it (5/5 GOPs on the CIF sequence, K=2; the 720p sequence at K=3); guaranteed and gated at `T=1` (5/5 on the CIF sequence, 2026-09-25), informational with threads |
| **Frame stats** | per coded frame, in coding order: input index, slice type, keyframe, reference, slice QP, bytes; no decode needed | `--frame-stats FILE` (JSON lines, plus the GOP index and its frame-thread count); `yah264_encoder_frame_stats()` + `yah264_encoder_frame_order()` (bytes come from the NALs the same call returns) |
| **Segment output** | the stream also written as one file per GOP, so a shot is addressable as a file and a re-encoded shot drops into place | `--segment-out PATTERN` (`%d` = GOP index; each segment starts with its own parameter sets) |

Rules the orchestrator can rely on:

- Frame indices count input frames from zero, in input order, everywhere
  (shot table, plan, stats, segments).
- A plan keyframe always starts a new GOP, so segments and zones align. A
  zone's QP offset applies to whole frames and does not require alignment.
- The rate control inside a zone is the encoder's own, CRF's per-frame terms
  included. The offset is added after it. This is what makes a shot's
  rate-quality curve a function of the offset alone.
- A forced QP is the other kind of message. The two do not compose. It is
  the coded slice QP. The zone offset, the frame-type cascade and the buffer
  all sit upstream of it, so none of them moves it. An orchestrator that wants
  a curve uses zones. One that is replaying a decision it has already made uses
  forces.
- A force is refused, never approximated. A B frame the lookahead cannot place
  fails the encode with the frame number on stderr. The placements that fail
  are the first frame of the stream, a frame the key-frame interval has
  claimed, the last frame, and one past the B-run limit. A forced anchor on a
  key frame's own slot yields to the key frame. That is the one case where the
  encoder's answer is a superset of what was asked for.
- Parameter sets are repeated per segment. Concatenating segments in order
  gives a valid stream that equals the unsplit output.
- A library caller creates its parameters with `yah264_param_default()`. Since ABI 3 the struct records its own size, and `yah264_encoder_open()` refuses one whose size is not the library's. An orchestrator built against an older header therefore fails at open instead of encoding with shifted fields.
- Nothing here changes the default stream. With no plan and no pinning, the
  output is byte-identical to the encoder without these hooks.

## What is deliberately not in the interface

- No resolution switching inside one encode. A rung is one encode of the
  plan at one resolution. The orchestrator owns the scaler and the ladder.
- No quality metric. The encoder reports bits and decisions. The
  orchestrator scores the decode with the metric it is optimizing.
- No trial encodes inside the encoder. The plan is the whole message. The
  analysis pre-scan behind the shot table is not a trial encode, but it is a
  second read of the input, as promise 1 says.

## Checklist for yah265 and yaav1

Shot table from the lookahead; a zone list with keyframe force and QP offset;
per-frame type and absolute-QP forces over the same index space;
per-instance thread pinning and a determinism test like
`scripts/shot_determinism.sh`; per-frame stats in coding order with an
index-to-packet map; per-GOP output with repeated parameter sets. The CLI flag
names above are the shared spelling.
