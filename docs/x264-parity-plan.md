# x264 option parity

The README says yah264 has roughly the same features and options as x264. A
census of x264's full option list found about 60 options yah264 lacked. Most
have shipped; `docs/what-shipped.md` has the history. This page lists what's
left and what we won't build.

The work is done when a fresh census finds every x264 option either present,
present under another name (listed in `docs/options.md`), or on the list
below.

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

## What's left

- **Intra refresh.** A moving intra column replaces keyframes, with a
  recovery-point SEI per period and motion vectors kept out of the area not
  yet refreshed. Gate: decoding from mid-stream recovers cleanly, and the cost
  is measured against a plain keyframe interval.
- **Custom quant matrices.** `--cqmfile` in the JM format and the per-list
  `--cqm4iy` family. Gate: a checked-in fixture decodes correctly, and the flat
  and default matrices leave output unchanged.
- **Motion search modes.** `--me esa` and `--me tesa`, `--no-chroma-me` and
  `--no-mixed-refs`. Gate: conformance and repeatability. Their quality cost
  gets measured too.
- **Noise reduction** has shipped as `--nr N`, off by default. It still needs
  its quality check: it should help on grainy content without hurting
  elsewhere.
- **Blu-ray compatibility**, if the owner wants it: four slices at 1080p,
  VBR HRD, strict B-pyramid, access-unit delimiters, picture structure and the
  per-level reference and keyframe limits, checked with the level checker.
- **The closing census.** Re-run it against x264's full option list. Zero
  unresolved options ends the program, and the README's parity sentence is
  rewritten from that run.
