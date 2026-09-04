# Hardware mode: VideoToolbox H.264 behind the yah264 option surface

Status: STEP 1 AND THE CORE OF STEP 2 BUILT 2026-09-04 (see "Where it
stands" below the step-0 answers). STARTED 2026-09-04. Owner asked for a mode that uses the Mac's encode
hardware when it is present, with the yah264 options mapped as far as they go
and warnings for the rest.

**Step 0 answers (owner, 2026-09-04: "recommendations"):**

1. CRF mapping: (a), `--crf` maps onto the quality key with a fixed monotone
   table and a one-line warning that the scale is not ours; (b) emulation is
   a follow-up item, not this plan.
2. Default: `--hw off`.
3. Unsupported options: warn once per explicitly set option, in one block at
   open, silent on defaults; `--hw-strict` turns the block into an error.
4. Frame-structure hints: scene-cut to ForceKeyFrame yes; the per-frame QP
   hint only after step 2's probe says the H.264 path honours it.
5. ffmpeg exposure: CLI and API only.
6. `auto` fall-back: warn once and fall back to our encoder.

## Where it stands (2026-09-04, first PR)

Built: the meson option (`videotoolbox`, auto on darwin, stub elsewhere),
`src/hw/vt.c` with a hardware-only session (software VideoToolbox refused),
`yah264_encoder_open_hw` / `yah264_encoder_backend` in the API (the mode is
passed at open, not carried in the parameter struct, so the ABI is
unchanged), `Y264_HW`, `--hw off|auto|videotoolbox` and `--hw-strict` in the
CLI, the warnings block for options the user set that the hardware ignores,
`tools/vtprobe` (this machine: hardware session opens at 1080p, 133
supported properties), and the plumbing: frames copied into pooled planar
4:2:0 buffers, AVCC samples converted to Annex B on the callback thread and
drained in decode order, SPS/PPS emitted ahead of the first slice, flush at
end of stream. Mapped so far: profile (High), entropy mode, frame
reordering, keyint, frame rate, ABR (AverageBitRate, DataRateLimits from
the VBV pair), CRF via the quality table, CQP via the QP range keys.

First numbers, sunflower_1080p 250f, this machine:

| | wall | CPU (user) | kbit/s | mean Y PSNR (every 10th frame) |
|---|--:|--:|--:|--:|
| yah264 --threads 12 --crf 28 | 1.09 s | 12.6 s | 1347 | 40.97 |
| hardware --crf 28 (quality 0.42) | 0.58 s | 0.15 s | 1050 | 38.26 |
| yah264 --threads 12 --bitrate 1500 | | | 1318 | 40.79 |
| hardware --bitrate 1500 | | | 1574 | 40.30 |

Not built: ForceKeyFrame from our scene-cut (step 2, item 4), the QP-hint
probe, the CRF table's calibration (crf 28 lands well below our crf 28),
counting frames that came back rather than went in, the regress axis, the
site row (step 5) and the full option-map table as one C table (the CLI
holds the ignored-option list today).

## What this is and is not

There are two "GPU encoders" on a Mac and only one of them is a mode.

- **Metal compute over our own encoder** is the nextgpu route
  (docs/gpu-acceleration-plan.md, docs/goal3-gpu-reattempt-3.md). Eight
  refusals on the lookahead alone: a 12-17 ms per-process device-init floor
  and synchronous dispatch on the critical path. A whole encoder on Metal is a
  different project. Not this plan.
- **Apple's fixed-function H.264 encoder** in the media engine, driven through
  the VideoToolbox C API. A black box we wrap. Hundreds of fps at 1080p on
  near-zero CPU, quality well behind x264 medium. This plan.

Consequences that hold for the whole plan:

- The output is Apple's stream, not ours. Recon-match, conformance,
  determinism, the bands, the ten-clip board and the parity goals do not
  apply to it and it never appears in their rows. It gets its own measured
  row on the site, labelled as hardware.
- ffmpeg already has `h264_videotoolbox`. The value we add is the pieces around
  the black box: our option vocabulary and rate-control names, our lookahead's
  scene-cut driving forced keyframes, and any CRF emulation we build. The bare
  wrap alone is a convenience for the CLI and API, not a feature.
- 4:2:0 8-bit only. Every other format the library supports is a hard error
  in forced mode and a fall-back in auto mode.

## Step 0: owner decisions (ask these at kickoff, do not assume)

The session that starts this work puts these to the owner, in this order, and
records the answers at the top of this file before touching code. Each one
changes what gets built.

1. **CRF mapping.** VideoToolbox has no CRF. Options:
   (a) map `--crf` onto the Quality key (0..1, what ffmpeg's `-q:v` uses on
   Apple Silicon) with a fixed monotone table and a one-line warning that the
   scale is not ours;
   (b) emulate CRF: run our lookahead, measure per-shot complexity, and set
   AverageBitRate per shot so the result tracks what our CRF would have spent;
   (c) refuse `--crf` in hardware mode with a message naming `--bitrate`.
   (a) ships first and is cheap; (b) is the only form that makes 2-pass mean
   anything on hardware; (c) is honest and small. Recommendation: (a) now,
   (b) as a follow-up item if the row is wanted for anything beyond
   throughput.
2. **Default.** `--hw off` is the recommended default: `auto` swaps in a
   different encoder silently, and every gate in this tree assumes ours. The
   owner may want `auto` for the product story. Decide.
3. **Unsupported-option policy.** Warn once per explicitly set option that the
   hardware ignores, grouped in one block at open, silent on defaults. Or
   error on any ignored option (strict). Recommendation: warn, with a
   `--hw-strict` flag that turns the block into an error for scripted use.
4. **Frame-structure hints.** Whether to spend the effort on feeding our
   lookahead into the hardware: scene-cut to ForceKeyFrame (cheap, real
   value), and mb-tree frame offsets into the per-frame base QP hint if the
   H.264 path honours it (needs a probe first, see step 2). Yes to the first,
   probe-then-decide on the second is the recommendation.
5. **ffmpeg exposure.** Whether `-c:v libyah264` grows an `hw` private option
   or the hardware mode stays CLI and API only. ffmpeg users already have
   `h264_videotoolbox`; recommendation: CLI and API only until the lookahead
   coupling exists, since that is the only thing ffmpeg users could not get
   from the native wrapper.
6. **Fall-back semantics for `auto`.** When the hardware refuses the session
   (unsupported format, no hardware, another process holding it), fall back
   to software with a warning, or fail. Recommendation: fall back with one
   warning line, and never fall back in forced mode.

Other behaviours worth a sentence each when the session asks: whether
`--threads` should be silently ignored or warned; whether `--pass 1` in
hardware mode writes our stats format at all (only meaningful under 1b); and
the CQP form (pin min and max QP equal) since the hardware may still move QP
inside a frame.

## Step 1: build and detection

- meson option `videotoolbox=auto|enabled|disabled`, same shape as `gpu`.
  `auto` resolves to enabled on darwin, disabled elsewhere. Links
  VideoToolbox, CoreMedia, CoreVideo, CoreFoundation. Off-Apple builds get a
  stub whose open returns NULL, so no ifdefs in callers (the nextgpu
  convention).
- API: a field on the parameter struct, `hw` = off / auto / videotoolbox.
  CLI: `--hw off|auto|videotoolbox`. Env knob `Y264_HW` for the harnesses,
  registered in docs/knobs.md and covered by scripts/env_gate_audit.py.
- Detection: create a compression session with the require-hardware encoder
  specification and read back the hardware-accelerated property. Software
  VideoToolbox is never accepted; if the hardware session fails we are on our
  own encoder.
- One probe tool, `tools/vtprobe.c`: prints the supported property
  dictionary for H.264 on this machine so the option map below is checked
  against the OS we run on rather than against memory. Several of the keys in
  the map arrived in macOS 13 and 14 and their H.264 behaviour differs from
  HEVC.

## Step 2: plumbing

- Input: y4m I420 wrapped as a planar 4:2:0 CVPixelBuffer without a copy
  where the pool allows it, NV12 conversion otherwise. Streaming input holds:
  frames go in as they arrive, the session's frame delay is the only lead.
- Output: the callback delivers AVCC samples plus SPS/PPS in the format
  description. Convert to Annex B and hand them to the existing writer. Output
  arrives in decode order with PTS; the CLI's frame counting and the
  `--frames` cap must count what came back, not what went in.
- Flush at end of stream, and propagate the callback's error status.
- Per-frame options: ForceKeyFrame from our scene-cut when step 0 item 4 says
  yes. Base QP hint only after the probe says the H.264 path honours it.

## Step 3: the option map

Three classes. "maps" = same meaning; "approx" = the closest thing the
hardware has, warned once; "no" = ignored with a warning if set explicitly;
"fatal" = cannot encode, error in forced mode, fall back in auto.

| yah264 | VideoToolbox key | Class | Note |
|---|---|---|---|
| profile (from options), --level | ProfileLevel | maps | High is the default; Baseline/Main by cabac and bframes |
| --cabac / --cavlc | H264EntropyMode | maps | |
| --keyint | MaxKeyFrameInterval + Duration | maps | |
| --min-keyint | none | no | |
| --scenecut / --no-scenecut | none; ForceKeyFrame from our lookahead | ours | step 0 item 4 |
| --bframes N | AllowFrameReordering | approx | bool only; count is the hardware's |
| --b-adapt, --direct | none | no | |
| --ref N | ReferenceBufferCount | approx | newer macOS only; ignored with warning elsewhere |
| --bitrate (ABR) | AverageBitRate | maps | |
| --vbv-maxrate / --vbv-bufsize | DataRateLimits (bytes per window) | approx | window = bufsize / maxrate |
| CBR (maxrate == bitrate) | ConstantBitRate | maps | macOS 13+ |
| --crf | Quality, or emulation, or refused | owner | step 0 item 1 |
| --qp (CQP) | MinAllowedFrameQP = MaxAllowedFrameQP | approx | pins the frame QP range |
| --pass / --stats | none for hardware H.264 | no | unless 1b is built |
| --abr-model, --qcomp, --rc-lookahead, --sync-lookahead | none | no | |
| --preset | PrioritizeEncodingSpeedOverQuality, RealTime | approx | ultrafast..fast = speed, medium.. = quality |
| --tune zerolatency | RealTime + AllowFrameReordering off + MaxFrameDelayCount 0 | approx | other tunes: no |
| --sar | PixelAspectRatio | maps | |
| fps from y4m | ExpectedFrameRate | maps | |
| colour primaries / transfer / matrix | Color* keys | maps | when we carry them |
| --threads | ignored | silent or warn | step 0 |
| --aq-strength, --psy-rd, --psy-trellis, --trellis, --deadzone-*, --cqm | none | no | |
| --me, --merange, --subme, --subpel | none | no | |
| --transform-8x8 / --no-transform-8x8 | implied by profile | no | |
| --no-sei | our SEI only | maps | the hardware writes none of ours |
| --dump-recon | none | fatal | no reconstruction to dump |
| 4:2:2, 4:4:4, bit depth > 8 | none | fatal | |
| interlace | FieldCount / FieldDetail | fatal until probed | |

The table is a starting point. The probe tool in step 1 is what makes it
true for the machine it runs on, and the map lives in one C table
(`src/hw/vt_map.c`) so the warning text, the docs and the behaviour cannot
drift apart.

## Step 4: warnings

- One block at open, `hw: N option(s) not honoured by the hardware encoder:`
  followed by one line per option, class named (ignored / approximated).
- Only options the user set. A default invocation prints nothing but the
  encoder line naming the hardware.
- Strict flag per step 0 item 3.
- A fall-back from `auto` prints exactly one line naming why.

## Step 5: measurement and the site row

- Speed and BD against x264 medium at the ten-clip board's operating points,
  hardware mode vs our own encoder, same clips, same rate points. The row is
  reported separately from every existing table (see [[abr-is-not-a-speed-board]]
  for why a rate-controlled row needs its bits quoted alongside).
- CPU time and wall both, since the whole point is CPU.
- Determinism: run the same clip twice and say whether the hardware is
  byte-stable. Expect it not to be; say so on the row.
- Add the mode to scripts/regress.py as its own axis so a change to the
  wrapper is caught, gated on "encodes, decodes with ffmpeg, frame count
  matches" rather than on identity.

## Effort

Step 1 and 2 with ABR/CBR/VBV and the fatal-format checks: roughly a
thousand lines of C, one session. Step 3's full map and step 4: a second
session. CRF emulation (1b) and the base-QP coupling: a third, only if the
owner wants the row for more than throughput.

## Related

docs/gpu-acceleration-plan.md, docs/gpu-shared-library-design.md,
docs/ffmpeg-integration-plan.md, docs/knobs.md, docs/instruments.md.
