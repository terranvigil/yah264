# Hardware mode: VideoToolbox H.264 behind the yah264 option surface

Status: STEP 1 AND THE CORE OF STEP 2 BUILT 2026-09-04 (see "Where it
stands" below the step-0 answers). STARTED 2026-09-04. The owner asked for a
mode that uses the Mac's encode hardware when it is present. It maps the yah264
options as far as they go and warns about the rest.

**Step 0 answers (owner, 2026-09-04: "recommendations"):**

1. CRF mapping: (a). `--crf` maps onto the quality key with a fixed monotone
   table and a one-line warning that the scale is not ours. (b), emulation, is
   a follow-up item outside this plan.
2. Default: `--hw off`.
3. Unsupported options: warn once per explicitly set option, in one block at
   open, and stay silent on defaults. `--hw-strict` turns the block into an
   error.
4. Frame-structure hints: scene-cut to ForceKeyFrame, yes. The per-frame QP
   hint comes only after step 2's probe says the H.264 path honors it.
5. ffmpeg exposure: CLI and API only.
6. `auto` fall-back: warn once and fall back to our encoder.

## Where it stands (2026-09-04, first PR)

Built:

- the meson option (`videotoolbox`, auto on darwin, stub elsewhere);
- `src/hw/vt.c` with a hardware-only session (software VideoToolbox refused);
- `yah264_encoder_open_hw` / `yah264_encoder_backend` in the API. The mode is
  passed at open and isn't in the parameter struct, so the ABI is unchanged;
- `Y264_HW`, plus `--hw off|auto|videotoolbox` and `--hw-strict` in the CLI;
- the warnings block for options the user set that the hardware ignores;
- `tools/vtprobe`. On this machine the hardware session opens at 1080p with
  133 supported properties;
- the plumbing. Frames are copied into pooled planar 4:2:0 buffers. AVCC
  samples are converted to Annex B on the callback thread and drained in
  decode order. SPS/PPS go out ahead of the first slice, and the stream
  flushes at its end.

Mapped so far: profile (High), entropy mode, frame reordering, keyint, frame
rate, ABR (AverageBitRate, DataRateLimits from the VBV pair), CRF via the
quality table and CQP via the QP range keys.

First numbers, sunflower_1080p 250f, this machine:

| | wall | CPU (user) | kbit/s | mean Y PSNR (every 10th frame) |
|---|--:|--:|--:|--:|
| yah264 --threads 12 --crf 28 | 1.09 s | 12.6 s | 1347 | 40.97 |
| hardware --crf 28 (quality 0.42) | 0.58 s | 0.15 s | 1050 | 38.26 |
| yah264 --threads 12 --bitrate 1500 | | | 1318 | 40.79 |
| hardware --bitrate 1500 | | | 1574 | 40.30 |

Since then, the same day, we measured the CRF table. Rate-matched over six
clips, crf 28 lands within 16% of our rate, and `Y264_HW_QUALITY` overrides
the key. `scripts/regress.py` now draws `--hw auto` in a quarter of its cells.
It doesn't assert determinism on the hardware because the hardware isn't
byte-stable run to run. Its PSNR floor now aligns frames by index. A raw .264
without VUI timing decodes at a guessed rate, and the psnr filter had paired
the wrong frames.

Later the same day our scene-cut started forcing the hardware's keyframes in
PR #118. It's causal, with no added latency and no flash guard. The option
map became one C table (`src/hw/vt_map.c`, PR #119) that the CLI's warnings
read. The site row is measured on the ten board clips at their ABR targets
(site/results.md, "The hardware mode"). The hardware uses 30-70x less CPU and
runs 2-3x faster in wall time on HD. On eight of ten clips it scores 1-8
VMAF-NEG points below our encoder at the same bitrate.

Then `--tune zerolatency` came to map to the hardware's real-time mode. Frame
delay count is read-only on this hardware, so that's the whole map. The CLI
counts frames as they come back and says so. The QP-hint probe also ran. A
per-frame base-QP option on the H.264 path changes nothing: output is
byte-identical with the hint at 20, 40 or absent, under CRF and under ABR. So
the mb-tree offset hint is not built. Step 0 item 4 is closed, with scene-cut
yes and QP hint no.

Not built: the ffmpeg exposure (owner: not now).

## What this is and is not

A Mac has two "GPU encoders". Only one of them is a mode.

- **Metal compute over our own encoder** is the shared-GPU-library route
  (docs/gpu-acceleration-plan.md, docs/goal3-gpu-reattempt-3.md). The lookahead
  alone has eight refusals. The causes are a 12-17 ms per-process device-init
  floor and synchronous dispatch on the critical path. A whole encoder on Metal is a
  different project and is outside this plan.
- **Apple's fixed-function H.264 encoder** in the media engine, driven through
  the VideoToolbox C API, is a black box we wrap. It runs hundreds of fps at
  1080p on near-zero CPU, with quality well behind x264 medium. This plan
  covers it.

Consequences that hold for the whole plan:

- The output is Apple's stream. Recon-match, conformance, determinism, the
  bands, the ten-clip board and the parity goals don't apply to it. It never
  appears in their rows. It gets its own measured row on the site,
  labeled as hardware.
- ffmpeg already has `h264_videotoolbox`. What we add is the pieces around the
  black box: our option vocabulary and rate-control names, our lookahead's
  scene-cut driving forced keyframes, and any CRF emulation we build. The bare
  wrap alone is a convenience for the CLI and API. It isn't a feature.
- 4:2:0 8-bit only. Every other format the library supports is a hard error
  in forced mode and a fall-back in auto mode.

## Step 0: owner decisions (ask these at kickoff, do not assume)

The session that starts this work puts these to the owner in this order. It
records the answers at the top of this file before touching code, because each
one changes what gets built.

1. **CRF mapping.** VideoToolbox has no CRF. Options:
   (a) map `--crf` onto the Quality key (0..1, what ffmpeg's `-q:v` uses on
   Apple Silicon) with a fixed monotone table and a one-line warning that the
   scale is not ours;
   (b) emulate CRF: run our lookahead, measure per-shot complexity, and set
   AverageBitRate per shot so the result tracks what our CRF would have spent;
   (c) refuse `--crf` in hardware mode with a message naming `--bitrate`.
   (a) ships first and is cheap. (b) is the only form that makes 2-pass mean
   anything on hardware. (c) is honest and small. Recommendation: (a) now, and
   (b) as a follow-up item if the row is wanted for anything beyond
   throughput.
2. **Default.** We recommend `--hw off` as the default. `auto` silently swaps
   in a different encoder, and every gate in this tree assumes ours. The owner
   may want `auto` for the product story. Decide.
3. **Unsupported-option policy.** Either warn once per explicitly set option
   that the hardware ignores, grouped in one block at open and silent on
   defaults, or error on any ignored option (strict). Recommendation: warn,
   with a `--hw-strict` flag that turns the block into an error for scripted
   use.
4. **Frame-structure hints.** Whether to spend the effort on feeding our
   lookahead into the hardware. Scene-cut to ForceKeyFrame is cheap and of
   real value. mb-tree frame offsets could go into the per-frame base QP hint
   if the H.264 path honors it. That needs a probe first (see step 2). The
   recommendation is yes to the first and probe-then-decide on the second.
5. **ffmpeg exposure.** Whether `-c:v libyah264` grows an `hw` private option
   or the hardware mode stays CLI and API only. ffmpeg users already have
   `h264_videotoolbox`. Recommendation: CLI and API only until the lookahead
   coupling exists, since that's the only thing ffmpeg users couldn't get from
   the native wrapper.
6. **Fall-back semantics for `auto`.** When the hardware refuses the session
   (unsupported format, no hardware, another process holding it), either fall
   back to software with a warning or fail. Recommendation: fall back with one
   warning line, and never fall back in forced mode.

Some other behaviors are worth a sentence each when the session asks. Should
`--threads` be silently ignored or warned? Does `--pass 1` in hardware mode
write our stats format at all? That only means something under 1b. Then
there's the CQP form, which sets min and max QP equal, since the hardware may
still move QP inside a frame.

## Step 1: build and detection

- meson option `videotoolbox=auto|enabled|disabled`, same shape as `gpu`.
  `auto` resolves to enabled on darwin and disabled elsewhere. It links
  VideoToolbox, CoreMedia, CoreVideo and CoreFoundation. Off-Apple builds get a
  stub whose open returns NULL, so callers need no ifdefs. That's the shared
  GPU library's convention.
- API: a field on the parameter struct, `hw` = off / auto / videotoolbox.
  (As built, the mode is passed at open through `yah264_encoder_open_hw`
  instead, and the parameter struct is unchanged. See "Where it stands".)
  CLI: `--hw off|auto|videotoolbox`. Env knob `Y264_HW` for the harnesses,
  registered in docs/knobs.md and covered by scripts/env_gate_audit.py.
- Detection: create a compression session with the require-hardware encoder
  specification and read back the hardware-accelerated property. Software
  VideoToolbox is never accepted. If the hardware session fails, we're on our
  own encoder.
- One probe tool, `tools/vtprobe.c`, prints the supported property dictionary
  for H.264 on this machine. That checks the option map below against the OS
  we run on instead of against memory. Several of the keys in the map arrived
  in macOS 13 and 14, and their H.264 behavior differs from HEVC.

## Step 2: plumbing

- Input: y4m I420 is wrapped as a planar 4:2:0 CVPixelBuffer without a copy
  where the pool allows it, with NV12 conversion otherwise. Streaming input
  holds. Frames go in as they arrive, and the session's frame delay is the only
  lead.
- Output: the callback delivers AVCC samples plus SPS/PPS in the format
  description. Convert to Annex B and hand them to the existing writer. Output
  arrives in decode order with PTS. The CLI's frame counting and the
  `--frames` cap must count what came back, since that can differ from what
  went in.
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

The table is a starting point. The probe tool in step 1 makes it true for the
machine it runs on. The map lives in one C table (`src/hw/vt_map.c`) so the
warning text, the docs and the behavior can't drift apart.

## Step 4: warnings

- One block at open, `hw: N option(s) not honoured by the hardware encoder:`
  followed by one line per option that names its class (ignored /
  approximated).
- Only options the user set. A default invocation prints nothing but the
  encoder line naming the hardware.
- Strict flag per step 0 item 3.
- A fall-back from `auto` prints exactly one line naming why.

## Step 5: measurement and the site row

- Speed and BD against x264 medium at the ten-clip board's operating points,
  with hardware mode and our own encoder on the same clips and rate points.
  The row is reported separately from every existing table. A
  rate-controlled row needs its bits quoted alongside.
- Both CPU time and wall, since the whole point is CPU.
- Determinism: run the same clip twice and say whether the hardware is
  byte-stable. Expect it not to be, and say so on the row.
- Add the mode to scripts/regress.py as its own axis so a change to the
  wrapper is caught. The gate is "encodes, decodes with ffmpeg, frame count
  matches", since identity can't hold.

## Effort

Steps 1 and 2 with ABR/CBR/VBV and the fatal-format checks come to roughly a
thousand lines of C in one session. Step 3's full map and step 4 take a second
session. CRF emulation (1b) and the base-QP coupling take a third. That one
happens only if the owner wants the row for more than throughput.

## Related

docs/gpu-acceleration-plan.md, docs/gpu-shared-library-design.md,
docs/ffmpeg-integration-plan.md, docs/knobs.md, docs/instruments.md.
