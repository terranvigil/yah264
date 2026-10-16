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
| `--cqm8ic`, `--cqm8pc` | no seat | The 4:4:4 chroma 8x8 lists. 4:4:4 never picks the 8x8 transform here, so they would be read by nothing. |
| `--bluray-compat` | refused | Disc authoring is a niche nobody here targets. Its parts (slices, HRD, strict B-pyramid, access-unit delimiters, picture structure) are each available on their own. |

## The closing census

The census compared every option in x264's `--fullhelp` against yah264's
`--help` and this page. Each x264 option was passed to the yah264 CLI, and
any the parser rejected as an unknown argument counts as absent. It found
165 options, short aliases and the help variants included.

| result | count |
|---|--:|
| present, same flag | 113 |
| renamed, with a row in `docs/options.md` | 3 |
| refused or absorbed, on the list above | 28 |
| unresolved | 21 |

The three renames are `--no-cabac` (`--cavlc`), `--no-8x8dct`
(`--no-transform-8x8`) and `--dump-yuv` (`--dump-recon`). `--video-filter`
counts as refused: it is the long spelling of `--vf`. The `--opencl-clbin`
and `--opencl-device` flags count under `--opencl`, and `--tcfile-in` and
`--tcfile-out` under `--tcfile-*`.

The program isn't complete. These need an owner decision:

| x264 option | what it does | suggestion |
|---|---|---|
| `-I`, `-i`, `-b`, `-r`, `-f`, `-q`, `-B`, `-p`, `-A`, `-m`, `-t` | Single-letter aliases for `--keyint`, `--min-keyint`, `--bframes`, `--ref`, `--deblock`, `--qp`, `--bitrate`, `--pass`, `--partitions`, `--subme` and `--trellis`. | Build. Each is an alias of a flag that is already present, so a ported command line works unchanged. |
| `--longhelp`, `--fullhelp` | Longer help listings. | Rename: point both at `--help`, which already lists everything. |
| `--zones` | Inline per-range QP force or bitrate multiplier. | Build it as a front end to the zone interface `--plan` uses. `--plan` takes QP offsets from a file, which is not the same thing, so it can't count as a rename yet. |
| `--b-bias` | Biases the adaptive B-frame decision. | Build. It is one term in a decision that already exists. Refuse if the owner wants no unmeasured knobs. |
| `--slice-max-size`, `--slice-max-mbs`, `--slice-min-mbs`, `--slices-max` | Cap slices by bytes or macroblocks. | Refuse unless someone asks for packetized delivery. `--slices` covers the count form. A byte cap would need the slice writer to cut mid-row. |
| `--asm` | Overrides CPU feature detection. | Refuse. `--no-asm` covers the test need. Every kernel already matches its C reference. |
| `--avcintra-flavor` | Picks the vendor variant of `--avcintra-class`. | Refuse along with `--avcintra-class`. |

`docs/denoise.md` has the `--nr` quality check. `--nr-inter 100` is neutral
to better on the HD band, where no clip reads worse than the per-clip bar
allows. It doesn't help on grain. The noise gate skips both 1924-scan
excerpts. Of the two Nocturne excerpts, one gets better and one gets worse.
So it meets "neutral elsewhere" but not "helps on grain". It stays opt-in.
