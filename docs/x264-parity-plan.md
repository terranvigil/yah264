# x264 option parity

The README says yah264 has roughly the same features and options as x264. A
census of x264's full option list found about 60 options yah264 lacked. All
of them have shipped or been refused; `docs/what-shipped.md` has the history.
This page records what we won't build and the census that closed the program.

The work is done when a fresh census finds every x264 option either present,
present under another name (listed in `docs/options.md`), or on the list
below. The census below finds that, so the program is complete.

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
| `--cqm8ic`, `--cqm8pc` | no seat | The 4:4:4 chroma 8x8 lists. 4:4:4 never picks the 8x8 transform here, so they would be read by nothing. |
| `--bluray-compat` | refused | Disc authoring is a niche nobody here targets. Its parts (slices, HRD, strict B-pyramid, access-unit delimiters, picture structure) are each available on their own. |
| `--slice-max-size`, `--slice-max-mbs`, `--slice-min-mbs`, `--slices-max` | refused | Packet-sized slices only matter for RTP-style delivery, which isn't a target. `--slices N` covers the count form. |
| `--asm` | refused | `--no-asm` and `Y264_SIMD_FORCE` cover testing. Every kernel already matches its C reference. |
| `--avcintra-flavor` | refused | It picks the vendor variant of `--avcintra-class`, which is refused. |

## The closing census

The census compares every option in x264's `--fullhelp` against yah264's
parser and this page. Each x264 option is passed to the yah264 CLI, and any
the parser rejects as an unknown argument counts as absent. It finds 165
options, short aliases and the help variants included.

| result | count |
|---|--:|
| present, same flag | 128 |
| renamed, with a row in `docs/options.md` | 3 |
| refused or absorbed, on the list above | 34 |
| unresolved | 0 |

The three renames are `--no-cabac` (`--cavlc`), `--no-8x8dct`
(`--no-transform-8x8`) and `--dump-yuv` (`--dump-recon`). `--video-filter`
counts as refused: it is the long spelling of `--vf`. The `--opencl-clbin`
and `--opencl-device` flags count under `--opencl`, and `--tcfile-in` and
`--tcfile-out` under `--tcfile-*`.

The last round built fifteen of the 21 options the previous census left open
and refused six. The one-letter aliases `-I`, `-i`, `-b`, `-r`, `-f`, `-q`,
`-B`, `-p`, `-A`, `-m` and `-t` are pure aliases for their long flags, and
none of them collides with yah264's own `-o`, `-v` or `-h`. `--longhelp` and
`--fullhelp` print what `--help` prints. `--zones` takes x264's inline syntax
and feeds the zone list `--plan` uses. `--b-bias` leans the adaptive B
placement, and docs/options.md has its measurement. The refused six are the
four slice caps, `--asm` and `--avcintra-flavor`.

`docs/denoise.md` has the `--nr` quality check. `--nr-inter 100` is neutral
to better on the HD band, where no clip reads worse than the per-clip bar
allows. It doesn't help on grain. The noise gate skips both 1924-scan
excerpts. Of the two Nocturne excerpts, one gets better and one gets worse.
So it meets "neutral elsewhere" but not "helps on grain". It stays opt-in.
