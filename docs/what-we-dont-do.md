# What we don't do

The feature-parity claim against x264 is read against this list. Every x264
option is present, present under another name (docs/options.md names it), or
listed here with the reason. "Refuse" is a decision. A row marked "owner decides" is
still open.

| x264 option | Recommendation | Reason |
|---|---|---|
| `--opencl*` | Refuse (decided) | No OpenCL target. The GPU path is Metal. |
| MBAFF | Refuse (decided) | 8-12k lines through the macroblock/CABAC/deblock hot paths. PAFF covers field sources. |
| `--sliced-threads` | Refuse | Threading is GOP-parallel plus row wavefront. Slices as a threading vehicle cost bits and buy nothing here. |
| `--muxer` `--demuxer` `--input-fmt` `--index` `--vf` | Refuse | Containers and filters are ffmpeg's job. We take y4m/raw and AVFrames. |
| `--thread-input` | Refuse | Avisynth only. |
| `--non-deterministic` | Refuse | No flag needed. Byte-exact repeatable output isn't required with more than one thread (owner, 2026-09-25; one thread must repeat). A speed win that gives it up ships in the default path instead of behind a switch. |
| `--cpu-independent` | Always on, no flag | Every kernel is checkasm-equal to its C reference. |
| `--tcfile-*` `--timebase` `--force-cfr` `--dts-compress` | Refuse | We output an elementary stream. The ffmpeg wrapper owns timing. |
| `--pulldown` | Refuse (lean) | Telecine flagging for a broadcast mux nobody uses here. |
| `--dump-yuv` | Renamed `--dump-recon` | Same thing. |
| `--slow-firstpass` | Refuse | Pass 1 already runs the full preset. The allocator is the value. |
| `--psnr` `--ssim` | Refuse | The harness scores externally. In-encoder metrics cost wall on every encode. |
| `--avcintra-class` | Refuse | A heavy, niche broadcast intra-only profile with vendor CQM tables and padding. |
| `--output-csp` | Refuse | No conversion stage. ffmpeg converts. `--output-depth` is the one exception (C3). |
| `--lookahead-threads` `--mvrange-thread` | Absorbed | One thread budget. The staircase lag is the MV constraint. |
| `--input-depth` | Auto | The input tag picks the library (C3). |
| `--bluray-compat` | Refuse (decided) | Disc authoring is a niche nobody here targets. Its parts (slices, HRD, strict B-pyramid, access-unit delimiters, picture structure) are each available on their own. |
| `--slice-max-size` `--slice-max-mbs` `--slice-min-mbs` `--slices-max` | Refuse (decided) | Packet-sized slices only matter for RTP-style delivery, which isn't a target. `--slices N` covers the count form. |
| `--asm` | Refuse (decided) | `--no-asm` and `Y264_SIMD_FORCE` cover testing. Every kernel already matches its C reference. |
| `--avcintra-flavor` | Refuse (decided) | It picks the vendor variant of `--avcintra-class`, which is refused. |

Every other x264 option is built, under its own name or the one docs/options.md
gives it. docs/x264-parity-plan.md has the census that checks this.
