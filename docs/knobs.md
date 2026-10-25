# The Y264_* knob census (generated -- edit scripts/knob_census.py OVERRIDES, not this file)

Regenerate: `python3 scripts/knob_census.py`. CI check: `--check`.

These are read only in a build configured with `-Ddev_knobs=true`; a default build reads just the ones the CLI uses to carry its own options (src/common/env.h).

Tier meanings: **default** = shipped behavior, the env is its escape; **instrument** = measurement machinery (cataloged in docs/instruments.md when it has produced a result); **arm** = default-off gate still being priced. A refuted arm is removed, with its verdict and the removing commit recorded in the local refused index.

## default (45)

| knob | default | reader |
|---|---|---|
| `Y264_2PASS_MT` | 1 | cli/yah264_cli.c:5506 |
| `Y264_ABR_QCOMP` | 0.6 | src/encoder/rate.c:2497 |
| `Y264_ABR_TOL` | = param | src/encoder/encoder.c:4837 |
| `Y264_ABR_VBVOV` | 0.25 | src/encoder/rate.c:2518 |
| `Y264_AQ_DC` | = rate mode: 1.3 uncapped CRF, 1.04 capped CRF or bitrate target | src/encoder/encoder.c:5454 |
| `Y264_AQ_MODE` | 2 | src/encoder/mbcode.c:279 |
| `Y264_B8_DIRECT` | 1 | src/encoder/mbcode.c:7020 |
| `Y264_B8_QGATE` | 10 | src/encoder/mbcode.c:7194 |
| `Y264_BAND_ROWS` | 2 | src/encoder/mbcode.c:5471 |
| `Y264_BSKIP_SCREEN` | 1 | src/encoder/encoder.c:4397 |
| `Y264_B_8X8` | 1 | src/encoder/mbcode.c:7011 |
| `Y264_B_SKIP_EXIT` | 1 | src/encoder/mbcode.c:3384 |
| `Y264_CDC_LAM` | 64 | src/encoder/mbcode.c:3693 |
| `Y264_CDC_RD` | 1 | src/encoder/mbcode.c:3699 |
| `Y264_CRF_CPLX` | 1 | src/encoder/rate.c:2488 |
| `Y264_CRF_LEVEL` | 2.3 | src/encoder/rate.c:561 |
| `Y264_DCTDEC` | 1 | src/encoder/mbcode.c:4182 |
| `Y264_DIRECT_AUTO` | -1 | src/encoder/encoder.c:533 |
| `Y264_DZ_INTER` | -1 | src/dsp/transform.c:847 |
| `Y264_DZ_INTRA` | -1 | src/dsp/transform.c:846 |
| `Y264_FAST_PSKIP` | 1 | src/encoder/mbcode.c:5368 |
| `Y264_GPQ_CONSUME` | 1 | src/encoder/encoder.c:7789 |
| `Y264_MBT_BLEG_REUSE` | 1 | src/encoder/encoder.c:7779 |
| `Y264_MBT_BREF` | 1 | src/encoder/encoder.c:247 |
| `Y264_MBT_PAIR_SCALE` | 1 | src/encoder/encoder.c:7235 |
| `Y264_ME_AGREE` | 1 | src/encoder/motion.c:235 |
| `Y264_ME_MINW` | 1920 | src/encoder/motion.c:233 |
| `Y264_PART_EARLYTERM` | 4 | src/encoder/mbcode.c:5407 |
| `Y264_QPEL_SKIPQ` | 96 | src/encoder/motion.c:245 |
| `Y264_RCP_LAG` | 1 | src/encoder/encoder.c:456 |
| `Y264_RC_CARRY` | 1 | cli/yah264_cli.c:1314 |
| `Y264_STAIR_BDEPTH` | 1 | src/encoder/encoder.c:361 |
| `Y264_STAIR_TDIR` | 1 | src/encoder/encoder.c:588 |
| `Y264_STAIR_WIDE` | 1 | src/encoder/encoder.c:398 |
| `Y264_STQ` | -1 | src/encoder/encoder.c:872 |
| `Y264_TP_CPLXBLUR` | 20.0 | src/encoder/encoder.c:5081 |
| `Y264_TP_IPF` | 1.4 | src/encoder/encoder.c:5084 |
| `Y264_TP_PBF` | 1.3 | src/encoder/encoder.c:5085 |
| `Y264_TREL_SMOOTH_V` | 8 | src/encoder/mbcode.c:1606 |
| `Y264_UMH_RANGE` | 16 | src/encoder/motion.c:770 |
| `Y264_VBV_LOW` | 0.25 | src/encoder/rate.c:554 |
| `Y264_VBV_PLAN` | 1 | src/encoder/rate.c:1216 |
| `Y264_VBV_POSTI` | 1.0 | src/encoder/rate.c:553 |
| `Y264_W2` | = pool present | src/encoder/encoder.c:5592 |
| `Y264_WF_CAPK` | -2 | src/encoder/encoder.c:3692 |

## instrument (57)

| knob | default | reader |
|---|---|---|
| `Y264_ADME_LOG` | 0 | src/encoder/encoder.c:6338 |
| `Y264_ASM_OFF` | 0 | src/common/cpu.c:218 |
| `Y264_B8_STAT` | 0 | src/encoder/mbcode.c:7084 |
| `Y264_BLATE_STAT` | ? | src/encoder/encoder.c:5318 |
| `Y264_BPROF` | 0 | src/encoder/mbcode.c:7927 |
| `Y264_BPROF2` | 0 | src/encoder/mbcode.c:7393 |
| `Y264_CABAC_TRACE` | ? | src/encoder/cabac.c:50 |
| `Y264_DIAG_COLWATCH` | 0 | src/encoder/mbcode.c:38 |
| `Y264_DIAG_DIRECTOK` | 0 | src/encoder/mbcode.c:39 |
| `Y264_DIAG_NOHPEL` | 0 | src/encoder/motion.c:338 |
| `Y264_DIAG_TDIRLIM` | 0 | src/encoder/mbcode.c:37 |
| `Y264_DIRECT_LRVOTE` | 0 | src/encoder/encoder.c:7369 |
| `Y264_DIRECT_PERMB` | -1 | src/encoder/encoder.c:483 |
| `Y264_DIRECT_SCORE` | 0 | src/encoder/encoder.c:2422 |
| `Y264_DIRECT_WHY` | 0 | src/encoder/encoder.c:2404 |
| `Y264_EST_FROZEN` | 0 | src/encoder/mbcode.c:5310 |
| `Y264_EST_SCRTRACE` | 0 | src/encoder/mbcode.c:4426 |
| `Y264_EST_TWICE` | 0 | src/encoder/mbcode.c:5278 |
| `Y264_GOP_FORCE_G` | ? | cli/yah264_cli.c:2930 |
| `Y264_GOP_FORCE_K` | ? | cli/yah264_cli.c:2931 |
| `Y264_GOP_TRACE` | ? | cli/yah264_cli.c:2835 |
| `Y264_GPQ_WARMUP` | 24 | src/encoder/gpu.c:104 |
| `Y264_GPU_PHASEA` | 0 | src/encoder/gpu.c:44 |
| `Y264_HEX_ORACLE` | 0 | src/encoder/motion.c:744 |
| `Y264_LA_BUF` | ? | src/encoder/encoder.c:909 |
| `Y264_LA_STAT` | ? | src/encoder/encoder.c:5688 |
| `Y264_LRSUB_CENSUS` | 0 | src/encoder/encoder.c:6617 |
| `Y264_LRSUB_PROBE` | 0 | src/encoder/encoder.c:6676 |
| `Y264_MBTREE_OFF` | ? | src/encoder/encoder.c:5245 |
| `Y264_MBTREE_STRENGTH` | = 5*(1-qcomp)*0.7, 1.4 at qcomp 0.6 (fallback path 2.0) | src/encoder/encoder.c:7904 |
| `Y264_MBT_DEPFLOOR` | = param | src/encoder/encoder.c:4434 |
| `Y264_MBT_MISSWHY` | 0 | src/encoder/encoder.c:7807 |
| `Y264_MBT_PLAY` | ? | src/encoder/encoder.c:9074 |
| `Y264_MBT_REC` | ? | src/encoder/encoder.c:9074 |
| `Y264_MBT_SPLIT` | 0 | src/encoder/encoder.c:7796 |
| `Y264_MEREUSE_TRACE` | 0 | src/encoder/mbcode.c:5880 |
| `Y264_ME_ETSTAT` | 0 | src/encoder/motion.c:294 |
| `Y264_NTP_PROF` | 0 | src/common/threadpool.c:61 |
| `Y264_PROBE_TRELLIS` | 1 | src/encoder/mbcode.c:10303 |
| `Y264_PSY_FLAT_LOG` | 0 | src/encoder/encoder.c:6331 |
| `Y264_QP_TRACE` | 0 | src/encoder/encoder.c:3035 |
| `Y264_RC_PLAY` | ? | src/encoder/rcreplay.h:157 |
| `Y264_RC_PLAY_K` | 1.0 | src/encoder/rcreplay.h:177 |
| `Y264_RC_REC` | ? | src/encoder/rcreplay.h:157 |
| `Y264_SIMD_FORCE` | unset | src/common/cpu.c:213 |
| `Y264_SKIP_ORACLE` | ? | src/encoder/skiporacle.c:35 |
| `Y264_SKIP_ORACLE_AT` | pre | src/encoder/skiporacle.c:47 |
| `Y264_SKIP_ORACLE_SIDE` | ? | src/encoder/skiporacle.c:37 |
| `Y264_THREAD_PROF` | 0 | src/encoder/encoder.c:109 |
| `Y264_TYPE_ORACLE` | ? | src/encoder/encoder.c:9317 |
| `Y264_UNSAFE_NO_EMIT` | 0 | src/encoder/mbcode.c:5265 |
| `Y264_UNSAFE_NO_MBT` | 0 | src/encoder/encoder.c:9272 |
| `Y264_UNSAFE_NO_NAL` | 0 | src/encoder/encoder.c:3226 |
| `Y264_UNSAFE_NO_PREVPWAIT` | 0 | src/encoder/encoder.c:857 |
| `Y264_UNSAFE_NO_REFBWAIT` | 0 | src/encoder/encoder.c:841 |
| `Y264_VBV_ROWBRAKE_TRACE` | 0 | src/encoder/mbcode.c:5249 |
| `Y264_WEIGHTP_STAT` | 0 | src/encoder/encoder.c:881 |

## arm (21)

| knob | default | reader |
|---|---|---|
| `Y264_AQ_DARK` | ? | src/encoder/mbcode.c:386 |
| `Y264_AQ_DARK_B0` | ? | src/encoder/mbcode.c:387 |
| `Y264_AQ_DARK_B1` | ? | src/encoder/mbcode.c:388 |
| `Y264_AQ_DARK_M` | ? | src/encoder/mbcode.c:386 |
| `Y264_AQ_DARK_S0` | ? | src/encoder/mbcode.c:388 |
| `Y264_AQ_DARK_S1` | ? | src/encoder/mbcode.c:389 |
| `Y264_AQ_DARK_V` | ? | src/encoder/mbcode.c:387 |
| `Y264_BAND_PRECOMP` | 0 | src/encoder/mbcode.c:5492 |
| `Y264_B_RECT` | 0 | src/encoder/mbcode.c:7167 |
| `Y264_HW` | off | src/encoder/encoder.c:16768 |
| `Y264_MAX_INPUT_MB` | ? | cli/yah264_cli.c:2165 |
| `Y264_MCSTF` | ? | src/encoder/mcstf.c:56 |
| `Y264_MCSTF_RANGE` | ? | src/encoder/mcstf.c:40 |
| `Y264_ME_HQ` | 0 | src/encoder/motion.c:234 |
| `Y264_NR_INTER` | 0 | src/encoder/encoder.c:1990 |
| `Y264_NR_INTRA` | 0 | src/encoder/encoder.c:1989 |
| `Y264_PSY_DARK` | ? | src/encoder/mbcode.c:449 |
| `Y264_QPSQ_MINW` | 0 | src/encoder/motion.c:241 |
| `Y264_STREAM_WINDOW` | ? | cli/yah264_cli.c:2878 |
| `Y264_SUB_FAR` | 0 | src/encoder/motion.c:236 |
| `Y264_TREL_SMOOTH` | on (0 = off) | src/encoder/mbcode.c:1606 |
