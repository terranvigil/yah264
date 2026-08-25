# next264 developer Makefile -- thin wrappers over the meson/ninja build.
# Real build config lives in meson.build; this just gives short, memorable targets.
#
#   make            build the binary
#   make test       fast unit tests
#   make repro      encode the repro clips and check they match tests/.golden
#   make golden     (re)generate the golden references (after an intended change)
#   make vmaf       encode a clip and print its VMAF / VMAF-NEG scores
#   make bench      wall-clock the wavefront (serial vs threaded)
#   make perf-comp  vs x264, as-shipped (SIMD + threads)
#   make perf-comp-purec  vs x264, single-CPU pure-C, matched preset (algo gap)
#   make perf-comp-set    the pure-C gap over the whole clip set, as one table
#                         (the gap is clip-dependent -- quote the set, not a clip)
#   make perf-comp-purec-threaded  same, but both encoders multi-threaded
#   make parity-status  the 3-goal scoreboard: pure-C 1t / pure-C MT / SIMD MT (ABR, fast)
#   make parity-status-crf  the same 3 goals in CRF at a matched operating point
#   make parity-modes   the RC-mode matrix: CRF/CQP/ABR/CBR/capped-VBR/2-pass (slow)
#                       x264-speed multiples (QUICK=1 for a 2-clip fast read)
#   make cvbr-compliance  capped-VBR VBV gate, 36 cells vs x264's 18
#   make conformance  recon-match gate (decode == encoder reconstruction)
#   make clean      remove the build dir
#
# Common overrides:  make repro FRAMES1=450 ENCCFG='--crf 26 --threads 1'
#   make perf-comp-purec PUREC_CLIP=tests/corpus/bus_cif.y4m PUREC_BITRATE=2500
#   make perf-comp-purec OUTDIR=/tmp/cmp   # keep next264/x264/source .mp4 for VLC

BUILD    ?= build
CLI      := $(BUILD)/cli/next264
NINJA    := ninja -C $(BUILD)
CORPUS   := tests/corpus
GOLDEN   := tests/.golden

# Reproducibility: one clip at two lengths -- a ~15s and a ~30s encode (at
# --threads 1 on 720p, ~0.43s/frame). Encoded --threads 1 so the golden is the
# exact serial/max-quality bitstream, deterministic and portable. Tunable:
#   make repro FRAMES1=60 FRAMES2=120  (or CLIP=..., ENCCFG=...)
CLIP     ?= $(CORPUS)/ducks_720p.y4m
FRAMES1  ?= 35
FRAMES2  ?= 70
ENCCFG   ?= --crf 28 --cabac --bframes 2 --threads 1

# VMAF target defaults
VMAF_CLIP   ?= $(CORPUS)/foreman_cif.y4m
VMAF_CRF    ?= 28
VMAF_FRAMES ?= 120

# bench target defaults
BENCH_CLIP   ?= $(CORPUS)/ducks_720p.y4m
BENCH_FRAMES ?= 30

# perf-comp target defaults (next264 vs x264 head-to-head)
PERF_CLIP    ?= $(CORPUS)/ducks_720p.y4m
PERF_CRF     ?= 25
PERF_SECONDS ?= 15

# perf-comp-purec defaults: single-CPU, both scalar, matched preset + bitrate --
# the honest algorithmic speed number. Match the bitrate to the clip so both
# encoders code the same bits (720p ~12000, CIF ~2500); matched-CRF confounds the
# ratio. Override e.g. make perf-comp-purec PUREC_CLIP=tests/corpus/bus_cif.y4m PUREC_BITRATE=2500
# Default clip: ducks_720p, the set's mid-range clip and the same default as
# perf-comp. The old default (samsung_720p) is the WORST clip of the set, so its
# single-clip ratio kept being misread as the campaign number. There is no
# representative single clip -- the gap is clip-dependent (see
# purec-gap-is-clip-dependent) -- so any one-clip run is labeled with its clip
# and the campaign number comes from `make perf-comp-set` only.
#
# 2026-08-12: this target's config drifted from the scoreboard's and the two
# disagreed by 0.15-0.20 with nothing saying why. Three causes, all fixed here:
# the bitrate was 12000, which is the target the corpus recalibration REJECTED
# for ducks (it sits at VMAF 75.5, below any deployment point -- 25000 lands it
# in the 88-94 band where the metric discriminates); and the args forced
# `--ref 1 --bframes 2`, which the scoreboard dropped for the preset defaults.
# That last one is not cosmetic: at --ref 1 the frame-concurrency machinery
# ENGAGES, and at ref 3 two predicates refuse it (docs/rc-parallel-design.md
# session 17), so the threaded variant was measuring a different encoder.
# Aligned with scripts/perf-comp-set.sh so the numbers are comparable; it stays
# single-clip for iteration speed, which is the one difference that remains.
PUREC_CLIP    ?= $(CORPUS)/ducks_720p.y4m
PUREC_BITRATE ?= 25000
PUREC_PRESET  ?= medium
PUREC_SECONDS ?= 6      # 180 frames -- stable numbers, ~1.5min/run; clip holds 10s
# Threads for perf-comp-purec-threaded (both encoders). Default = all online CPUs.
PUREC_THREADS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)

# Where perf-comp / perf-comp-purec drop the comparison encodes (next264.mp4,
# x264.mp4, source.mp4 for VLC A/B). Set OUTDIR= (empty) to skip saving them.
OUTDIR        ?= /tmp/cmp

.DEFAULT_GOAL := build
.PHONY: build configure test repro golden vmaf bench perf-comp perf-comp-purec perf-comp-purec-threaded parity-status parity-status-crf parity-modes cvbr-compliance conformance clean help

help:
	@sed -n '3,20p' Makefile | sed 's/^#\s\{0,1\}//'

configure:
	@test -f $(BUILD)/build.ninja || meson setup $(BUILD)

build: configure
	@$(NINJA)

# Fast unit tests (all of them run in well under a second each).
test: build
	@meson test -C $(BUILD)
	@python3 scripts/knob_census.py --check

$(GOLDEN):
	@mkdir -p $(GOLDEN)

golden: build | $(GOLDEN)
	@echo ">> generating golden references [$(notdir $(CLIP)) $(ENCCFG)]"
	@$(CLI) --input-y4m $(CLIP) --frames $(FRAMES1) $(ENCCFG) -o $(GOLDEN)/short.264 2>/dev/null
	@$(CLI) --input-y4m $(CLIP) --frames $(FRAMES2) $(ENCCFG) -o $(GOLDEN)/long.264 2>/dev/null
	@ls -l $(GOLDEN)/*.264 | awk '{printf "   %-28s %s bytes\n", $$NF, $$5}'
	@echo ">> golden updated -- 'make repro' checks against these."

# Encode again and confirm the output is byte-identical to the golden: proves the
# encoder is reproducible (run-to-run deterministic) AND flags any accidental
# output change. Regenerate the golden with 'make golden' after an intended change.
repro: build
	@test -f $(GOLDEN)/short.264 || { echo ">> no golden yet -- run 'make golden' first"; exit 1; }
	@echo ">> encoding + comparing to golden [$(ENCCFG)]"
	@$(CLI) --input-y4m $(CLIP) --frames $(FRAMES1) $(ENCCFG) -o $(BUILD)/repro-short.264 2>/dev/null
	@$(CLI) --input-y4m $(CLIP) --frames $(FRAMES2) $(ENCCFG) -o $(BUILD)/repro-long.264 2>/dev/null
	@fail=0; \
	  if cmp -s $(BUILD)/repro-short.264 $(GOLDEN)/short.264; then \
	    echo "   short ($(FRAMES1)f $(notdir $(CLIP))): REPRODUCIBLE"; \
	  else echo "   short: MISMATCH vs golden"; fail=1; fi; \
	  if cmp -s $(BUILD)/repro-long.264 $(GOLDEN)/long.264; then \
	    echo "   long  ($(FRAMES2)f $(notdir $(CLIP))): REPRODUCIBLE"; \
	  else echo "   long: MISMATCH vs golden"; fail=1; fi; \
	  if [ $$fail -eq 0 ]; then echo ">> repro OK (byte-identical to golden)"; \
	  else echo ">> repro FAILED -- output changed; run 'make golden' if intended"; exit 1; fi

vmaf: build
	@VMAF="$(VMAF)" NEXT264_VMAF_MODEL="$(NEXT264_VMAF_MODEL)" NEXT264="$(CLI)" \
	  scripts/vmaf.sh $(VMAF_CLIP) $(VMAF_CRF) $(VMAF_FRAMES)

# Wall-clock the in-frame wavefront: serial vs threaded on the same clip.
bench: build
	@echo ">> serial (--threads 1):"
	@/usr/bin/time -p $(CLI) --input-y4m $(BENCH_CLIP) --frames $(BENCH_FRAMES) --crf 30 --cabac --bframes 2 --threads 1 -o /dev/null 2>&1 | awk '/real/{print "   "$$2"s"}'
	@echo ">> threaded (--threads $$(getconf _NPROCESSORS_ONLN)):"
	@/usr/bin/time -p $(CLI) --input-y4m $(BENCH_CLIP) --frames $(BENCH_FRAMES) --crf 30 --cabac --bframes 2 --threads $$(getconf _NPROCESSORS_ONLN) -o /dev/null 2>&1 | awk '/real/{print "   "$$2"s"}'

parity-status: build
	@VMAF="$(VMAF)" NEXT264="$(CLI)" scripts/parity-status.sh $(if $(QUICK),quick,)

# The same three goals in CRF, at a MATCHED OPERATING POINT rather than a
# matched CRF number: each encoder is swept over CRF and solved onto a common
# achieved bitrate, then timed there (scripts/crf-solve.py). CRF is the mode
# most users actually run, so this is the one to headline -- but its numbers are
# NOT comparable to the ABR board's, so the two are separate targets rather than
# a flag on one.
#
# It is opt-in because it costs a calibration sweep (~3 encodes per encoder per
# clip) on top of the timed runs. That cost is paid once: the solve is cached in
# tests/.crfcache and keyed on the binaries, so repeat runs are about as quick as
# the ABR board. `make parity-status` stays the fast default.
#
#   make parity-status-crf              matched achieved BITRATE (headline)
#   make parity-status-crf POINT=vmaf   matched achieved VMAF instead
#   make parity-status-crf QUICK=1      2-clip read
parity-status-crf: build
	@VMAF="$(VMAF)" NEXT264="$(CLI)" PARITY_RC=crf POINT="$(if $(POINT),$(POINT),kbps)" \
	  scripts/parity-status.sh $(if $(QUICK),quick,)

# The rate-control mode matrix: CRF / CQP / ABR / CBR / capped VBR / 2-pass,
# per clip, one table per mode. SLOW (tens of minutes) and deliberately kept out
# of parity-status, which stays an ABR-only scoreboard that is quick enough to
# run routinely. Quote a mode, never a cross-mode average -- the modes take
# structurally different paths through the encoder and 2-pass in particular
# ignores --threads entirely. See docs/rc-mode-matrix.md.
#   make parity-modes                       pure-C, all online CPUs
#   make parity-modes MODE=asm THREADS=1    as-shipped SIMD, single thread
#   make parity-modes MODES="cbr 2pass"     just those rows
parity-modes: build
	@NEXT264="$(CLI)" VMAF="$(VMAF)" \
	  SET_THREADS="$(if $(THREADS),$(THREADS),$(shell getconf _NPROCESSORS_ONLN))" \
	  $(if $(MODES),MODES="$(MODES)",) \
	  scripts/perf-comp-modes.sh $(if $(MODE),$(MODE),pure)

# The capped-VBR VBV compliance gate: 36 next264 cells (6 clips x 3 caps x both
# N264_RC_PIPE_VBV paths) against 18 x264 reference cells. The cvbr row of
# parity-modes samples one cap on one path; this is the whole surface. Exits
# non-zero unless every cell is clean. Frame rate is never passed in -- it comes
# from each clip's Y4M header. See docs/archive/capped-vbr-cap-overshoot.md.
#   make cvbr-compliance                     6s windows (crosses a keyint at 50fps)
#   make cvbr-compliance CVBR_FRAMES=180     one GOP per clip, the historical number
cvbr-compliance: build
	@NEXT264="$(CLI)" scripts/cvbr_compliance.sh

conformance: build
	@NEXT264_CONF_FAST=1 scripts/conformance.sh --fast $(CLI)

# Head-to-head vs x264: speed (fps), quality (VMAF @ ~5fps sampling), size.
perf-comp: build
	@OUTDIR="$(OUTDIR)" VMAF="$(VMAF)" NEXT264="$(CLI)" scripts/perf-comp.sh $(PERF_CLIP) $(PERF_CRF) $(PERF_SECONDS)

# Single-CPU, pure-C (both scalar), matched --preset + bitrate: the honest
# algorithmic speed gap vs x264 (no SIMD, no threads, matched analysis tier).
# The plain `perf-comp` default leaves next264 at subme-10 vs x264-medium AND
# uses SIMD+threads -- this target fixes all three. Prints "x264 is N.Nx faster".
perf-comp-purec: build
	@THREADS=1 OUTDIR="$(OUTDIR)" VMAF="$(VMAF)" NEXT264="$(CLI)" \
	  NEXT264_ARGS="--preset $(PUREC_PRESET) --cabac --transform-8x8 --bitrate $(PUREC_BITRATE)" \
	  X264_ARGS="--preset $(PUREC_PRESET) --bitrate $(PUREC_BITRATE)" \
	  scripts/perf-comp-purec.sh $(PUREC_CLIP) 30 $(PUREC_SECONDS)

# Same pure-C (both scalar), matched preset + bitrate, but both encoders run
# multi-threaded (PUREC_THREADS, default = all online CPUs). This measures the
# algorithmic gap WITH each encoder's threading efficiency stacked on top -- a
# distinct benchmark from the 1-thread perf-comp-purec, not a substitute for it.
perf-comp-purec-threaded: build
	@THREADS=$(PUREC_THREADS) OUTDIR="$(OUTDIR)" VMAF="$(VMAF)" NEXT264="$(CLI)" \
	  NEXT264_ARGS="--preset $(PUREC_PRESET) --cabac --transform-8x8 --bitrate $(PUREC_BITRATE)" \
	  X264_ARGS="--preset $(PUREC_PRESET) --bitrate $(PUREC_BITRATE)" \
	  scripts/perf-comp-purec.sh $(PUREC_CLIP) 30 $(PUREC_SECONDS)

# The pure-C gap over a CLIP SET, as one table. There is no single "the" gap --
# it is ~2.5x on CIF and 2.7-3.6x on 720p at the same matched config -- so a
# single-clip run is not a campaign number. Use this before quoting one.
#   make perf-comp-set            pure-C (default)
#   make perf-comp-set MODE=asm   as-shipped SIMD
perf-comp-set: build
	@NEXT264="$(CLI)" VMAF="$(VMAF)" scripts/perf-comp-set.sh $(MODE)

clean:
	@rm -rf $(BUILD)
