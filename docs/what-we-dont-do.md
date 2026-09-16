# What we don't do

The feature-parity claim against x264 is read against this list. Every x264
option is either present, present under another name (docs/options.md names
it), or here with the reason. "Refuse" is a decision; "owner decides" is
still open.

| x264 option | Recommendation | Reason |
|---|---|---|
| `--opencl*` | Refuse (decided) | No OpenCL target; GPU path is Metal. |
| MBAFF | Refuse (decided) | 8-12k lines through the macroblock/CABAC/deblock hot paths; PAFF covers field sources. |
| `--sliced-threads` | Refuse | Threading is GOP-parallel plus row wavefront; slices as a threading vehicle cost bits and buy nothing here. |
| `--muxer` `--demuxer` `--input-fmt` `--index` `--vf` | Refuse | Containers and filters are ffmpeg's job; we take y4m/raw and AVFrames. |
| `--thread-input` | Refuse | Avisynth only. |
| `--non-deterministic` | Refuse | Determinism is a published guarantee. |
| `--cpu-independent` | Always on, no flag | Every kernel is checkasm-equal to its C reference. |
| `--tcfile-*` `--timebase` `--force-cfr` `--dts-compress` | Refuse | Elementary-stream output; the ffmpeg wrapper owns timing. |
| `--pulldown` | Refuse (lean) | Telecine flagging for a broadcast mux nobody uses here. |
| `--dump-yuv` | Renamed `--dump-recon` | Same thing. |
| `--slow-firstpass` | Refuse | Pass 1 already runs the full preset; the allocator is the value. |
| `--psnr` `--ssim` | Refuse | Scored externally by the harness; in-encoder metrics cost wall on every encode. |
| `--avcintra-class` | Refuse | Broadcast intra-only profile with vendor CQM tables and padding; heavy, niche. |
| `--output-csp` | Refuse | No conversion stage; ffmpeg converts. `--output-depth` is the one exception (C3). |
| `--lookahead-threads` `--mvrange-thread` | Absorbed | One thread budget; the staircase lag is the MV constraint. |
| `--input-depth` | Auto | The library is picked from the input tag (C3). |
| `--bluray-compat` | Owner decides after slices + HRD + `--b-pyramid strict` ship | It is a constraint bundle over those three. |

Everything else absent is built below.

The programme that closes everything not on this list is docs/x264-parity-plan.md.
