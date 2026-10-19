# What we don't do

The feature-parity claim against x264 is read against this list. Every x264
option is present, present under another name (docs/options.md names it), or
listed here with the reason. "Refused" is a decision.

| x264 option | what yah264 does | reason |
|---|---|---|
| `--opencl*` | Refused | No OpenCL target. The GPU path is Metal (docs/gpu.md). |
| MBAFF | Refused | 8-12k lines through the macroblock, CABAC and deblock hot paths. PAFF covers field sources. |
| `--sliced-threads` | Refused | Threading is GOP-parallel plus row wavefront (docs/threading.md). Slices as a threading vehicle cost bits and buy nothing here. |
| `--muxer` `--demuxer` `--input-fmt` `--index` `--vf` | Refused | Containers and filters are ffmpeg's job. We take y4m/raw and AVFrames. |
| `--thread-input` | Refused | Avisynth only. |
| `--non-deterministic` | Refused | No flag needed. Byte-exact repeatable output isn't required with more than one thread (one thread must repeat). A speed win that gives it up ships in the default path instead of behind a switch. |
| `--cpu-independent` | Always on, no flag | Every kernel is checkasm-equal to its C reference. |
| `--tcfile-*` `--timebase` `--force-cfr` `--dts-compress` | Refused | We output an elementary stream. The ffmpeg wrapper owns timing. |
| `--pulldown` | Refused | Telecine flagging for a broadcast mux nobody uses here. |
| `--dump-yuv` | Renamed `--dump-recon` | Same thing. |
| `--slow-firstpass` | Refused | Pass 1 already runs the full preset. The allocator is the value. |
| `--psnr` `--ssim` | Refused | The harness scores externally. In-encoder metrics cost wall on every encode. (`--tune psnr` and `--tune ssim` exist.) |
| `--avcintra-class` | Refused | A heavy, niche broadcast intra-only profile with vendor CQM tables and padding. |
| `--avcintra-flavor` | Refused | It picks the vendor variant of `--avcintra-class`, which is refused. |
| `--output-csp` | Refused | No conversion stage. ffmpeg converts. `--output-depth` is the one exception (docs/formats.md). |
| `--lookahead-threads` `--mvrange-thread` | Absorbed | One thread budget. The staircase lag is the MV constraint. |
| `--input-depth` | Automatic | The input's own format tag picks the bit depth (docs/formats.md). |
| `--bluray-compat` | Refused | Disc authoring is a niche nobody here targets. Its parts (slices, HRD, access-unit delimiters, picture structure) are each available on their own, except the strict B-pyramid, which is refused. |
| `--slice-max-size` `--slice-max-mbs` `--slice-min-mbs` `--slices-max` | Refused | Packet-sized slices only matter for RTP-style delivery, which isn't a target. `--slices N` covers the count form. |
| `--asm` | Refused | `--no-asm` and `Y264_SIMD_FORCE` cover testing. Every kernel already matches its C reference. |

Every other x264 option is built, under its own name or the one docs/options.md
gives it.
