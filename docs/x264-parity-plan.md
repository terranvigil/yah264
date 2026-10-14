# x264 option parity

## Where it stands

The README says yah264 has roughly the same features and options as x264.
This plan makes that true, or says plainly what we won't build. A census of
x264's full option list found about 60 options yah264 lacked. About half only
needed a flag, because the behavior already existed behind an environment
variable or a fixed value. About 20 were real coding or rate-control features,
and a few were structural: field pictures, slices and runtime bit depth.

Waves 1 to 4 have shipped, plus noise reduction from wave 6. The rest of waves 5 and 6 is open.

The work is done when a fresh census finds every x264 option either present,
present under another name (listed in `docs/options.md`), or on the refusal
list below.

## What we won't build

| x264 option | decision | why |
|---|---|---|
| `--opencl` | refused | No OpenCL target. The GPU path is Metal. |
| MBAFF | refused | Thousands of lines through the hottest code paths. PAFF covers interlaced sources. |
| `--sliced-threads` | refused | Threading is GOP-parallel plus a row wavefront. Slices as a threading tool cost bits and buy nothing here. |
| `--muxer`, `--demuxer`, `--input-fmt`, `--index`, `--vf` | refused | Containers and filters are ffmpeg's job. We take Y4M, raw YUV and ffmpeg frames. |
| `--thread-input` | refused | Avisynth only. |
| `--non-deterministic` | refused | Not needed: one-thread output always repeats, and threaded output may already vary. |
| `--cpu-independent` | always on | Every kernel matches its C reference exactly. |
| `--tcfile-*`, `--timebase`, `--force-cfr`, `--dts-compress` | refused | We write an elementary stream. The ffmpeg wrapper owns timing. |
| `--pulldown` | refused | Telecine flags for broadcast muxing nobody uses here. |
| `--dump-yuv` | renamed | `--dump-recon` does the same thing. |
| `--slow-firstpass` | refused | The first pass already runs the full preset. |
| `--psnr`, `--ssim` | refused | The harness scores externally. In-encoder metrics would slow every encode. |
| `--avcintra-class` | refused | A broadcast intra-only profile with vendor tables. Heavy and niche. |
| `--output-csp` | refused | No conversion stage; ffmpeg converts. `--output-depth` is the one exception. |
| `--lookahead-threads`, `--mvrange-thread` | absorbed | One thread budget covers both. |
| `--input-depth` | automatic | The library is picked from the input's own format tag. |
| `--bluray-compat` | owner decides | It's a bundle of constraints over slices, HRD and strict B-pyramid, all of which now exist. |

## How each item ships

Each item gets a brief, its own worktree and one gated merge. Commits carry the
owner's name only, and nothing that ships names x264's internals.

- **Defaults don't move.** Without the new flag, output must be byte-identical
  to main on the ten board clips at CRF 23, QP 26 and each clip's board
  bitrate, at one thread and at eight.
- **New stream features join conformance.** Each one adds cases to
  `scripts/conformance.sh`, and every case must decode to the encoder's own
  reconstruction with every configured decoder.
- **The standard gate passes:** `make test`, the fast conformance run, the
  thread stress test, the thread sanitizer, the sanitizer matrix, the
  repeatability check and the hygiene and knob audits.
- **Docs move with the code.** Each item adds its row to `docs/options.md` and
  to `docs/what-shipped.md`.
- **Environment knobs still override the new flags.**
- **Heavy quality runs** go one at a time, on a quiet machine.

## Shipped

| wave | what it added |
|---|---|
| 1 | Flags for behavior that already existed (AQ mode, mb-tree, rate-control ratios, deblock, B-pyramid, QP limits, VBV start, MV range, profiles); new SEI and VUI options (`--aud`, `--pic-struct`, frame packing, HDR metadata, overscan); a raw-YUV reader with crop, seek and logging flags; checkers for HRD and levels, and conformance against three decoders (ffmpeg, openh264, JM); and 8-bit and 10-bit in one binary, with 8-bit output unchanged. |
| 2 | Interlaced field coding for I and P fields (`--tff`, `--bff`); `--constrained-intra`; and `--slices N`. |
| 3 | B field pictures, so interlaced coding is complete; HRD signalling (`--nal-hrd vbr|cbr`, `--filler`); and `--open-gop`. |
| 4 | `--crf-max`, `--ratetol` and `--qpfile`; `--weightp 0|1|2`; and `--partitions`. |

## Wave 5

- **Intra refresh.** A moving intra column replaces keyframes, with a
  recovery-point SEI per period and motion vectors kept out of the area not
  yet refreshed. Gate: decoding from mid-stream recovers cleanly, and the cost
  is measured against a plain keyframe interval.
- **Custom quant matrices.** `--cqmfile` in the JM format and the per-list
  `--cqm4iy` family. Gate: a checked-in fixture decodes correctly, and the flat
  and default matrices leave output unchanged.
- **Motion search modes.** `--me esa` and `--me tesa`, `--no-chroma-me` and
  `--no-mixed-refs`. Gate: conformance and repeatability. Their quality cost
  is measured in wave 6.

## Wave 6

- **Noise reduction** has shipped early as `--nr N`, off by default. What's
  left is the quality check: it should help on grainy content without hurting
  elsewhere.
- **Blu-ray compatibility**, if the owner wants it: four slices at 1080p,
  VBR HRD, strict B-pyramid, access-unit delimiters, picture structure and the
  per-level reference and keyframe limits, checked with the level checker.
- **The quality cost of the new motion search modes.**
- **The closing census.** Re-run it against x264's full option list. Zero
  unresolved options ends the program, and the README's parity sentence is
  rewritten from that run.

## Test fixtures

- **Interlaced:** `ffmpeg -f lavfi -i testsrc2=size=320x240:rate=50 -vf
  tinterlace=mode=interleave_top,setfield=tff -frames:v 24 -pix_fmt yuv420p
  syn_tff.y4m`, and the same with `interleave_bottom` and `bff`.
- **10-bit:** `ffmpeg -f lavfi -i testsrc2=size=320x240:rate=30 -frames:v 12
  -pix_fmt yuv420p10le -strict -1 syn_p10.y4m`, and the 4:2:2 and 4:4:4
  variants.
- **Interlaced quality board:** foreman, park_joy and pedestrian run through
  `tinterlace`, compared against x264 at `--tff`.

## After every merge

Run `scripts/regress.py`, the full conformance sweep with every decoder, the
goal board to confirm speed and quality didn't move, and the repeatability
check with each new mode's flags.
