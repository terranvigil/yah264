# Research: designing an H.264 encoder faster than x264

Claims marked "verified" survived a 3-vote adversarial check against the cited source. The rest are single-source but drawn from primary material (vendor docs, project docs, court opinions, dev mailing lists). Nothing was refuted.

## 1. SIMD, ISA support, and hardware encoders

### What the fast software encoders actually do

The pattern across every fast codec project is the same: plain C for control flow, hand-written assembly for the hot kernels, runtime CPU dispatch. Not intrinsics, not autovectorization.

- dav1d, the reference for "fastest possible codec code," is C99 plus NASM/GAS assembly. By GitHub's language stats the repo is about 78% assembly, 22% C. It ships asm tiers for SSSE3, AVX2, ARMv7 and ARMv8 NEON, and treats AVX-512 as a secondary roadmap target. It decodes 4K60 AV1 at 50 fps on a Pixel 3 without hardware acceleration, against under 10 fps for libgav1.
- x264 gets roughly 2x from its SIMD assembly over its C baseline; x265 gets roughly 5x. x265 validates and cycle-benchmarks every asm kernel against its C primitive with a built-in test bench, a practice worth copying.
- SVT-AV1 enables AVX-512 by default on supported hardware. x264 covers MMX through AVX2 plus a small set of AVX-512 kernels added in 2018; NEON on ARM.

### AVX-512: real gains, workload dependent

Verified: x265's AVX-512 work is over 1,000 hand-written kernels, chosen over auto-vectorization because handwritten asm beat the vectorizing tools. Kernel-level gains averaged 33% over AVX2 for 8-bit and 40% for 10-bit (16-bit pixels double the work per vector). Also verified: those kernel gains did not translate to whole-encoder wins on Skylake-era servers because of AVX-512 frequency throttling; Intel measured an average loss for 1080p 8-bit on dual Xeon 8180 and recommended AVX-512 only for 4K 10-bit at slow presets. That is why x265 ships it disabled behind `--asm avx512`.

The throttling story is dated, though. On desktop Skylake-X the penalty was already minimal and AVX-512 won everywhere. On Rocket Lake it gave about 7.5% whole-encoder speedup at a 28% power cost. On Zen 5 (9950X) there is no meaningful downclocking; x265's AVX-512 benefit concentrates in slow presets and high resolutions, and SVT-AV1's measured benefit was smaller than expected. Zen 4/5 also run AVX-512 well via double-pumped or native 512-bit units, and the 9950X beat an i7-14700F by up to 2x in x265 at lower power.

VNNI: no existing H.264/HEVC/AV1 software encoder was found using AVX-VNNI in production kernels. It is a plausible edge for SAD/SATD-like reductions (dot products on int8/int16), unproven in this domain. Treat as an experiment, not a plan pillar.

### Hardware encoders (H.264)

- NVENC (verified): a dedicated hardware block independent of CUDA cores, but some features (temporal AQ) run CUDA kernels. Lookahead up to 32 frames with adaptive I/B insertion. B-frames usable as references. High 10 (10-bit H.264) encode only arrives with Blackwell; everything earlier is 8-bit only. Seven presets across four tuning modes (HQ, low latency, ultra low latency, lossless), CBR/VBR/CQP/target-quality rate control.
- Intel Quick Sync / oneVPL (verified): AVC encode is 8-bit 4:2:0 only on every platform through Arc; HEVC gets 10-bit and 4:4:4 on newer parts. Intel used to have a hybrid fixed-function-plus-shader AVC path; Arc dropped it, keeping pure fixed function. No iGPU means no Quick Sync at all.
- AMD VCN/AMF reached rough quality parity with NVENC for H.264 only recently (Tom's Hardware testing).
- Apple VideoToolbox exposes hardware H.264 through a session API with limited tunability; quality knobs are coarse compared to any software encoder.
- A Nov 2025 arXiv study (verified) found that for low-latency real-time 4K, hardware encoders on recent GPUs beat software on end-to-end latency and slightly beat them on rate-distortion. In low-latency tuning x264 lost 0.888 dB PSNR / 2.385 VMAF versus about 0.27 dB / 0.168 VMAF for NVENC. Hardware kept latency at or under 12 frames where software needed 41 to 100+. Low-latency tunings on hardware disable B-frames, lookahead, and two-pass.

### GPU-assisted software encoding: a cautionary record

Verified from the x264-devel merge thread: x264's OpenCL path offloads only the lowres lookahead analysis (intra cost, lowres motion search with subpel, bidir costs); MB-tree and all real decisions stay on CPU. Data dependencies forced an iterative GPU motion search that does more total work than the CPU version, so it is neither work efficient nor power efficient, it only wins when spare GPU cycles exist, and it often slightly degrades quality. It does nothing at ultrafast/superfast because those presets have no threaded lookahead. GPU offload of the main motion estimation was considered and abandoned over synchronization problems. Intel independently abandoned its shader-assisted hybrid AVC encode on Arc.

### Implications

Ship C-with-hand-written-asm kernels behind a runtime dispatch table: baseline SSE2, then SSSE3/SSE4.1, AVX2 as the primary tier, AVX-512 as a first-class tier (not an afterthought; Zen 4/5 changed the calculus since x265 made its call), NEON/dotprod for ARM from day one (Apple Silicon and Graviton matter now in ways they didn't when x264's asm was written). Copy x265's checkasm-style kernel test bench. Do not build the encoder around GPU offload; the record says the dependency structure of a quality encoder defeats it. If we touch GPU at all, confine it to the same place x264 did (lookahead pre-analysis) or to whole-frame pre-analysis for content-adaptive features, where quality feedback loops don't exist. Beating x264 in speed will come from architecture (section 4), not from an ISA x264 lacks; AVX-512 and wider parallelism are multipliers on a better architecture.

## 2. API surface and licensing

### The legal landscape

- x264 is GPL-2.0 (explicitly not v3) with a commercial license sold by x264 LLC, which holds exclusive commercial rights and distributes royalties to developers. x264 LLC reads GPL v2 expansively: linking your product to libx264 subjects the whole product to GPL on distribution. They recognize an internal-use safe harbor (their example is Netflix): no distribution, no obligations. This expansive-linking position is exactly the market gap a permissively licensed, API-familiar encoder fills.
- x265 is the precedent: GPL-2 with a commercial option from MulticoreWare, and it openly reuses x264 code and conventions. But x265 did that under GPL. It never had to solve our problem; it inherited x264's license along with its code.
- Google v. Oracle (2021, verified against the slip opinion): the Supreme Court held 6-2 that Google's verbatim copying of about 11,500 lines of Java API declaring code (0.4% of the codebase) was fair use as a matter of law. The Court explicitly declined to decide whether declaring code is copyrightable at all, and drew a sharp distinction between declaring code (names, signatures, organization, farther from the core of copyright) and implementing code. Its reasoning, that copyright cannot be used to lock in programmers' investment in learning an interface, covers our scenario directly.
- Older but still-relevant doctrine: 17 U.S.C. 102(b) excludes methods of operation and systems from protection; Computer Associates v. Altai filtered out parameter lists and elements dictated by compatibility requirements as unprotectable scenes a faire. The 2012 district court in Oracle held the Java API's command structure unprotectable, though that specific holding didn't survive appeal (the Supreme Court resolved on fair use instead).
- Counterweight: a program can be a derivative work with zero literal copying if it takes non-literal structure, sequence, and organization. Matching x264's internal architecture, not just its public header shape, is where the risk lives.
- Patents are orthogonal to all of this. Any H.264 encoder likely needs a license from the Via-LA (formerly MPEG-LA) AVC pool regardless of software license. Note the last AVC patents are expiring; baseline-era patents are largely gone and the pool's remaining coverage thins out over the next couple of years, which is part of why building H.264 now is commercially interesting.

### What this means in practice

Offering an x264-*compatible* API (same conceptual model: params struct, preset/tune/profile application, picture in, NAL payloads out; same parameter *names* on the CLI) with independently written declarations and implementation is well supported legally. Verbatim copying of x264.h text is defensible under Google v. Oracle fair use but needlessly buys a lawsuit-shaped argument. The clean line:

1. Do not copy header text or struct layouts byte-for-byte. Write our own API that mirrors the workflow and vocabulary.
2. Match CLI flag names and semantics (`--preset`, `--crf`, `--tune`, `--bframes`) freely; x265 already normalized this convention and command vocabularies are method-of-operation territory.
3. Anyone who has read x264 source should not write our core encoder files from memory of x264's internals. Reading the spec, papers, and x264's *documentation* is fine; porting its code structure is not.
4. FFmpeg integration needs: a C header, a pkg-config file, an opaque encoder handle, param setup, frame-in/packet-out with proper pts/dts and flush semantics, global header (SPS/PPS) extraction for containers, and reconfiguration. That is the whole contract libx264.c exercises. An FFmpeg wrapper requires the encoder be at least LGPL-compatible for default builds; BSD/MIT makes us linkable everywhere x264 is not.

### Implications

License the encoder BSD-2-Clause or MIT (the dav1d/rav1e position), which is itself the biggest commercial differentiator against x264/x265. Design our own API in x264's *shape* but not its text. Keep CLI compatibility high so x264 users can swap binaries. Budget for a Via-LA patent posture decision before any commercial distribution, and document the clean-room policy in CONTRIBUTING from the first commit.

## 3. Language choice

The rav1d project is the best natural experiment we have: dav1d transpiled to Rust, linking the exact same hand-written assembly.

- Initial transpile: 3.8% slower than C with identical asm. After a $20k optimization bounty and heavy work: still under 6% slower on x86_64 (down from ~11% peak), about 5-9% slower on Apple M3.
- Even with all asm enabled, Rust code is about half of decode runtime, so language overhead is not confined to glue.
- The overhead decomposes into many small costs: bounds checks are only ~2 points of a 7% instruction-count excess; mandatory zero-init of scratch buffers the asm immediately overwrites (1.6% recovered via MaybeUninit); derived PartialEq on a small struct losing to C's compare-as-u32 idiom (0.7%); rustc/LLVM producing more stack traffic than clang on the same code.
- rav1e proves the toolchain side: a Rust encoder linking NASM (x86_64) and GAS (aarch64) kernels, majority of repo bytes being assembly, exposing a C ABI via cargo-c that FFmpeg wraps as librav1e. Compiling the Rust side with target features (avx2, bmi2, fma...) gained 11-13%, showing how much scalar-side codegen matters even when kernels are asm.
- rav1e's performance history is the caution: it never approached x264-class speed for its codec generation and repositioned around safety and simplicity.
- Zig: no fast codec of consequence is written in it; toolchain is capable (can assemble NASM-style via translate or build.zig driving nasm) but 1.0 has not shipped. It buys less than C's ubiquity costs.
- dav1d and every encoder that has ever held the speed crown (x264, x265 kernels, SVT-AV1 core) is C or C++ plus asm.

The honest reading: the language penalty of Rust with asm kernels is now measured at roughly 5%, and a from-scratch idiomatic Rust design (unlike a transpile) can avoid some of it. Against that, our stated #1 goal is performance, the reference competitors are C, and every percent matters when the pitch is "faster than x264."

### Implications

Core encoder in C (C11), kernels in NASM/GAS assembly, exactly the dav1d recipe, with a checkasm-style verification bench. C gives us the zero-cost FFI story, clang codegen, the widest platform and toolchain reach, and no language-overhead asterisk on benchmarks. Two mitigations for C's safety cost: fuzz from week one (decoder-side conformance re-decode, bitstream fuzzing) and consider Rust for the non-hot orchestration layer (CLI, muxers, analysis pass) if we want it, since that boundary is clean. If the team strongly prefers Rust throughout, the measured cost is ~5% and closing; that is a real option, but it spends margin we claim to be competing on.

## 4. H.264 coding tools and where x264 is beatable

### Tools that matter on the speed/quality frontier

- CABAC vs CAVLC is one of the largest single levers: CAVLC costs 10-20% compression efficiency. CABAC is mandatory for competitive quality; CAVLC remains the ultrafast escape hatch. CABAC is also serial per slice, which is the central parallelism constraint of H.264.
- High profile essentials: 8x8 transform, 8x8 intra prediction, custom quant matrices. B-frames with pyramid, weighted prediction, trellis quantization, and psy-RD are the quality tools that separate x264 from every hardware encoder. 10-bit (High 10), 4:2:2/4:4:4, lossless, and MBAFF are checkbox-completeness tools; 10-bit H.264 has a real contribution-workflow niche and almost no hardware competition (NVENC only from Blackwell, Intel never).
- x264's preset ladder is fundamentally a schedule over: reference frame count, motion search method and range (dia/hex/umh/esa/tesa), subme (subpel refinement and RD level), partition depth, B-adapt mode, trellis level, and lookahead depth. That schedule design is not copyrightable and is the right UX to keep.

### x264's own admitted weak spots (from the VideoLAN x264 TODO and dev discussions)

- 1-pass rate control cannot adapt fast enough at 12-24 threads; it was designed for far fewer cores.
- Frame-based threading defaults to 1.5x logical cores and collapses past roughly one thread per 40 lines of vertical resolution (1080p tops out around 27 threads). Slice threading exists for latency but costs quality. There is no SVT-style decoupled pipeline.
- The lookahead (40-frame default rc-lookahead feeding mb-tree and VBV) is single-threaded-ish and B-adapt 1 (used by fast presets) is "substantially improvable" per its own developers.
- VBV rate control adapts per row with quadratic complexity; the devs call the frame/row size predictors weak.
- Trellis for CAVLC is an acknowledged unprincipled workaround.
- GPU offload of core motion estimation was abandoned (synchronization).

### What newer architecture research adds (SVT, verified against the SPIE paper and SVT-AV1 docs)

- SVT-style multidimensional parallelism: decoupled pipeline stages (analysis, mode decision, encode, entropy) with picture-level parallelism for VOD latency budgets and segment-level for low latency, saturating high core counts.
- SVT-AV1's threading is deterministic: bit-identical output at any thread count. That property is worth adopting as a hard requirement; x264 cannot claim it across thread configs.
- SVT deliberately avoids tile-based threading because tiles cost quality; H.264 doesn't have tiles anyway, so our equivalents are frame-level parallelism, segment/slice-level, and wavefront-style macroblock-row parallelism inside a frame (rows can be processed in parallel with a 2-MB horizontal lag; CABAC per slice still serializes final entropy coding, so decouple entropy encode as its own pipeline stage consuming finished MB rows).
- Multi-stage mode decision (cheap classifiers pruning the candidate space before full RD) is how SVT builds a fine-grained speed ladder, versus x264's mostly on/off feature toggles.
- SVT-AV1 scales quality-neutrally to ~16 cores at 1080p mid presets, more at higher resolutions; slowest presets lose parallel efficiency to feature dependencies. Same tradeoff will bind us.

### Implications

The speed win over x264 is architectural: an SVT-style decoupled pipeline with deterministic threading, entropy coding as a pipeline stage, a many-core-aware rate control (fixing x264's admitted 12-24 thread breakdown), and a rebuilt lookahead that is itself parallel. Tool support target: High profile complete (CABAC, 8x8, B-pyramid, weighted pred, trellis, psy-RD) at launch, High 10 soon after (niche with no hardware competition), 4:2:2/4:4:4 and lossless later, MBAFF/interlace last or never unless broadcast customers demand it. Keep x264's preset/tune vocabulary as the UX.

## 5. Streaming I/O

x264's CLI accepts raw YUV, Y4M, Avisynth, and lavf/ffms input (raw on stdin) and muxes only raw Annex-B, MP4, MKV, and FLV. No native MPEG-TS, no fragmented MP4, no RTP. In practice everyone pipes: `ffmpeg -i ... -f yuv4mpegpipe - | x264 --demuxer y4m -` and the output goes to a packager.

What a modern encoder should do:

- Input: Y4M on stdin as the primary contract (self-describing: geometry, rate, chroma, bit depth), raw YUV with explicit flags as fallback. NUT or direct lavf input is optional convenience, not core.
- Output: Annex-B elementary stream on stdout as the primary contract; every packager and ffmpeg consume it. Timestamps sideband via a stats/log channel when needed.
- Worth adding natively where x264 stopped: fragmented MP4/CMAF segment output (the lingua franca of DASH/HLS delivery; emitting CMAF chunks directly enables low-latency HLS/DASH without a separate packager process) and MPEG-TS for the broadcast/contribution path. RTP/SRT/RIST belong in the transport layer, not the encoder; skip them.
- Low latency: support chunked output flushing (encode ahead of segment boundaries, flush partial fragments), zero-latency tuning (no B-frames, no lookahead buffering, slice-based threading), and intra-refresh for the RTC-adjacent crowd.
- The library API matters more than the CLI muxers: frame in, NAL units out with clean pts/dts, SPS/PPS access, and mid-stream reconfig. Given that, ffmpeg and GStreamer wrappers make every container question someone else's problem.

### Implications

Core contract: Y4M/raw in, Annex-B out, both over pipes, byte-exact with x264's conventions so it drops into existing pipelines. First-class CMAF/fMP4 output is a genuine differentiator worth building in; MPEG-TS second; nothing else in-process.

## 6. Content-aware and shot-optimized encoding

### State of the art

- Netflix per-title (2015): choose the bitrate ladder per title by encoding at many resolution/bitrate points and taking the convex hull.
- Netflix Dynamic Optimizer / per-shot (2018, verified): an orchestration layer *outside* the encoder. Split into shots (one camera, near-constant lighting, safe units for independent parallel encoding), encode each shot at multiple resolutions and QPs, score with VMAF, assemble the final stream from convex-hull points. Results: 17.1% average bitrate savings over the best fixed-QP encode, 50%+ over standard 2-pass VBR, roughly 28-38% BD-rate across H.264/HEVC/VP9. Codec-agnostic by design.
- Convex-hull prediction research (ACM TOMM benchmark, 300 UHD shots, x264/x265/VVenC/NVENC): exhaustive hull construction is reliable but expensive; predictors close most of the gap. For x264, an ExtraTrees model on handcrafted features gets within ~2.5% BD-BR (PSNR) / ~4.2% (VMAF) of the exhaustive hull, slightly beating ResNet-50. Lightweight live-grade features extract in under 1.1 s with 2-13 ms CPU inference, versus ~145 s for VoD features and ~273 s for ResNet-50. In-encoder hull prediction is feasible even for low-latency pipelines.
- CAE products: Beamr CABR (closed-loop per-frame perceptual QP search), Harmonic EyeQ, Bitmovin per-title, AWS MediaConvert automated ABR. All are either orchestration layers or closed-loop wrappers around a conventional encoder.
- SVT-AV1 data points: it deliberately does not force keyframes at scene changes (considers that a feature), and treats multi-pass as mainly a VBR-targeting tool, with CRF single-pass as the quality path.
- x264's mb-tree plus AQ is itself early content-aware encoding; CRF is content-adaptive rate control. The bar is what has been added since.

### The build-in vs orchestrate-around split

Per-shot convex hull across *resolutions* inherently needs an orchestration layer (an encoder emits one resolution). But three things the literature treats as external can live in-encoder because we control the architecture:

1. Shot-aware single-pass encoding: real scene-cut segmentation in the lookahead (not just I-frame placement), per-shot complexity classification from lookahead stats, and per-shot rate/QP planning. This is Dynamic-Optimizer thinking collapsed into one pass, no re-encodes.
2. Quality-target rate control (CRF-but-calibrated): a "target VMAF-class score" mode using a cheap learned proxy metric in the loop (real VMAF is too slow per-frame at speed; the convex-hull feature research shows lightweight proxies work). Closed-loop per-shot QP adjustment toward constant perceptual quality is Beamr's pitch, built in rather than wrapped around.
3. Hull-assist outputs: emit per-shot complexity/RD stats and machine-readable shot boundaries as a side channel, plus a segment-parallel encode mode (deterministic re-encode of shot N with parameter overrides). This makes us the best possible engine *under* a Dynamic-Optimizer-style layer, which is what Netflix-shaped customers actually run. Cheap to build, directly monetizes the orchestration trend instead of competing with it.

### Implications

Pick features 1 and 3 for the core roadmap and treat 2 as the flagship differentiator once the encoder is competitive. None of them compromise the speed goal: they all live in the lookahead/analysis stage, which our architecture already rebuilds for parallelism, and shot-segmented encoding is also a parallelism win (shots are independent).

## Sources

Key sources: Intel "Accelerating x265 with AVX-512"; HWCooling Rocket Lake AVX-512 x265 tests; neet.works Zen 5 AVX-512 encoding benchmarks; NVENC Video Codec SDK 13.0 programming guide; Intel oneVPL media capabilities reference; arXiv 2511.18688 (low-latency 4K GPU vs software, Nov 2025); x264-devel OpenCL lookahead merge thread (Apr 2013); VideoLAN x264 TODO wiki; x264 settings reference (MeWiki); x264.org/licensing; x265 4.1 docs introduction; Google v. Oracle slip opinion (593 U.S. 1) and Wikipedia summary; Public Knowledge on GPL and API copyrightability; copyleft.org Comprehensive GPL Guide ch. 4; memorysafety.org rav1d optimization posts and performance bounty; ohadravid.github.io "Making rav1d 1% faster"; xiph/rav1e; videolan/dav1d and Codec Wiki dav1d page; SVT-AV1 SPIE 2020 overview paper; SVT-AV1 CommonQuestions docs; Netflix TechBlog Dynamic Optimizer; ACM TOMM convex-hull prediction benchmark; Tom's Hardware AMD AMF parity testing; HandBrake VideoToolbox docs.
