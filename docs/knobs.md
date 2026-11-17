# The Y264_* knob census (generated -- edit scripts/knob_census.py OVERRIDES, not this file)

Regenerate: `python3 scripts/knob_census.py`. CI check: `--check`.

These are read only in a build configured with `-Ddev_knobs=true`; a default build reads just the ones the CLI uses to carry its own options (src/common/env.h) and the CLI's two memory bounds, `Y264_MAX_INPUT_MB` and `Y264_STREAM_WINDOW`; its CLI refuses to encode when any other is set.

Tier meanings: **default** = shipped behavior, the env is its escape; **instrument** = measurement machinery (cataloged in docs/instruments.md when it has produced a result); **arm** = default-off gate still being priced. A refuted arm is removed, with its verdict and the removing commit recorded in the local refused index.

## default (64)

| knob | default | reader |
|---|---|---|
| `Y264_2PASS_MT` | 1 | cli/yah264_cli.c:7095 |
| `Y264_ABR_QCOMP` | 0.6 | src/encoder/rate.c:2976 |
| `Y264_ABR_TOL` | = param | src/encoder/encoder.c:5400 |
| `Y264_ABR_VBVOV` | 0.25 | src/encoder/rate.c:2997 |
| `Y264_AQ_D8` | 1 | src/encoder/mbcode.c:321 |
| `Y264_AQ_DARK` | 6.0 | src/encoder/mbcode.c:401 |
| `Y264_AQ_DARK_B0` | 0.08 | src/encoder/mbcode.c:402 |
| `Y264_AQ_DARK_B1` | 0.20 | src/encoder/mbcode.c:403 |
| `Y264_AQ_DARK_M` | 64 | src/encoder/mbcode.c:401 |
| `Y264_AQ_DARK_S0` | 0.35 | src/encoder/mbcode.c:403 |
| `Y264_AQ_DARK_S1` | 0.50 | src/encoder/mbcode.c:404 |
| `Y264_AQ_DARK_V` | 16 | src/encoder/mbcode.c:402 |
| `Y264_AQ_DC` | = rate mode: 1.3 uncapped CRF, 1.04 capped CRF or bitrate target | src/encoder/encoder.c:6259 |
| `Y264_AQ_FADE` | 0.2 (0 = off) | src/encoder/mbcode.c:404 |
| `Y264_AQ_FADE_N` | 6 | src/encoder/mbcode.c:405 |
| `Y264_AQ_MODE` | 2 | src/encoder/mbcode.c:289 |
| `Y264_B8_DIRECT` | 1 | src/encoder/mbcode.c:7412 |
| `Y264_B8_QGATE` | 10 | src/encoder/mbcode.c:7586 |
| `Y264_BAND_ROWS` | 2 | src/encoder/mbcode.c:5863 |
| `Y264_BSKIP_SCREEN` | 1 | src/encoder/encoder.c:4911 |
| `Y264_B_8X8` | 1 | src/encoder/mbcode.c:7403 |
| `Y264_B_SKIP_EXIT` | 1 | src/encoder/mbcode.c:3777 |
| `Y264_CDC_LAM` | 64 | src/encoder/mbcode.c:4086 |
| `Y264_CDC_RD` | 1 | src/encoder/mbcode.c:4092 |
| `Y264_CRF_CPLX` | 1 | src/encoder/rate.c:2967 |
| `Y264_CRF_LEVEL` | 2.3 | src/encoder/rate.c:587 |
| `Y264_DARK_IADMIT` | 12 | src/encoder/encoder.c:4947 |
| `Y264_DARK_SKIPLIFT` | 16 | src/encoder/encoder.c:4963 |
| `Y264_DARK_SKIPLIFT_P` | = Y264_DARK_SKIPLIFT / 2 | src/encoder/encoder.c:4966 |
| `Y264_DARK_SURV` | 1 | src/encoder/encoder.c:4948 |
| `Y264_DCTDEC` | 1 | src/encoder/mbcode.c:4575 |
| `Y264_DIRECT_AUTO` | -1 | src/encoder/encoder.c:548 |
| `Y264_DZ_INTER` | -1 | src/dsp/transform.c:847 |
| `Y264_DZ_INTRA` | -1 | src/dsp/transform.c:846 |
| `Y264_FAST_PSKIP` | 1 | src/encoder/mbcode.c:5760 |
| `Y264_FLAT_SKIP_DC` | 1 | src/encoder/mbcode.c:11365 |
| `Y264_GPQ_CONSUME` | 1 | src/encoder/encoder.c:8647 |
| `Y264_MBT_BLEG_REUSE` | 1 | src/encoder/encoder.c:8637 |
| `Y264_MBT_BREF` | 1 | src/encoder/encoder.c:250 |
| `Y264_MBT_PAIR_SCALE` | 1 | src/encoder/encoder.c:8091 |
| `Y264_ME_AGREE` | 1 | src/encoder/motion.c:242 |
| `Y264_ME_MINW` | 1920 | src/encoder/motion.c:240 |
| `Y264_PART_EARLYTERM` | 4 | src/encoder/mbcode.c:5799 |
| `Y264_PSY_DARK` | 5.0 | src/encoder/mbcode.c:703 |
| `Y264_P_INTRA_CABAC` | 1 | src/encoder/mbcode.c:11926 |
| `Y264_QPEL_SKIPQ` | 96 | src/encoder/motion.c:252 |
| `Y264_RCP_LAG` | 1 | src/encoder/encoder.c:471 |
| `Y264_RC_CARRY` | 1 | cli/yah264_cli.c:1495 |
| `Y264_SKIP_CLIFT` | 1 | src/encoder/mbcode.c:11218 |
| `Y264_STAIR_BDEPTH` | 1 | src/encoder/encoder.c:367 |
| `Y264_STAIR_TDIR` | 1 | src/encoder/encoder.c:603 |
| `Y264_STAIR_WIDE` | 1 | src/encoder/encoder.c:404 |
| `Y264_STQ` | -1 | src/encoder/encoder.c:887 |
| `Y264_TP_CPLXBLUR` | 20.0 | src/encoder/encoder.c:5880 |
| `Y264_TP_IPF` | 1.4 | src/encoder/encoder.c:5883 |
| `Y264_TP_PBF` | 1.3 | src/encoder/encoder.c:5884 |
| `Y264_TREL_SMOOTH_V` | 8 | src/encoder/mbcode.c:1959 |
| `Y264_UMH_RANGE` | 16 | src/encoder/motion.c:777 |
| `Y264_VBV_LOW` | 0.25 | src/encoder/rate.c:580 |
| `Y264_VBV_PLAN` | 1 | src/encoder/rate.c:578 |
| `Y264_VBV_POSTI` | 1.0 | src/encoder/rate.c:579 |
| `Y264_W2` | = pool present | src/encoder/encoder.c:6401 |
| `Y264_WF_CAPK` | -2 | src/encoder/encoder.c:4025 |
| `Y264_WP_FINE` | 1 | src/encoder/encoder.c:1336 |

## instrument (57)

| knob | default | reader |
|---|---|---|
| `Y264_ADME_LOG` | 0 | src/encoder/encoder.c:7181 |
| `Y264_ASM_OFF` | 0 | src/common/cpu.c:218 |
| `Y264_B8_STAT` | 0 | src/encoder/mbcode.c:7476 |
| `Y264_BLATE_STAT` | ? | src/encoder/encoder.c:6118 |
| `Y264_BPROF` | 0 | src/encoder/mbcode.c:8329 |
| `Y264_BPROF2` | 0 | src/encoder/mbcode.c:7785 |
| `Y264_CABAC_TRACE` | ? | src/encoder/cabac.c:51 |
| `Y264_DBG_SEEK_SKIP` | -1 | cli/yah264_cli.c:2095 |
| `Y264_DIAG_COLWATCH` | 0 | src/encoder/mbcode.c:38 |
| `Y264_DIAG_DIRECTOK` | 0 | src/encoder/mbcode.c:39 |
| `Y264_DIAG_NOHPEL` | 0 | src/encoder/motion.c:345 |
| `Y264_DIAG_TDIRLIM` | 0 | src/encoder/mbcode.c:37 |
| `Y264_DIRECT_LRVOTE` | 0 | src/encoder/encoder.c:8227 |
| `Y264_DIRECT_PERMB` | -1 | src/encoder/encoder.c:498 |
| `Y264_DIRECT_SCORE` | 0 | src/encoder/encoder.c:2535 |
| `Y264_DIRECT_WHY` | 0 | src/encoder/encoder.c:2517 |
| `Y264_EST_FROZEN` | 0 | src/encoder/mbcode.c:5702 |
| `Y264_EST_SCRTRACE` | 0 | src/encoder/mbcode.c:4819 |
| `Y264_EST_TWICE` | 0 | src/encoder/mbcode.c:5670 |
| `Y264_GOP_FORCE_G` | ? | cli/yah264_cli.c:4197 |
| `Y264_GOP_FORCE_K` | ? | cli/yah264_cli.c:4198 |
| `Y264_GOP_TRACE` | ? | cli/yah264_cli.c:4102 |
| `Y264_GPQ_WARMUP` | 24 | src/encoder/gpu.c:104 |
| `Y264_GPU_PHASEA` | 0 | src/encoder/gpu.c:44 |
| `Y264_HEX_ORACLE` | 0 | src/encoder/motion.c:751 |
| `Y264_LA_BUF` | ? | src/encoder/encoder.c:924 |
| `Y264_LA_STAT` | ? | src/encoder/encoder.c:6497 |
| `Y264_LRSUB_CENSUS` | 0 | src/encoder/encoder.c:7473 |
| `Y264_LRSUB_PROBE` | 0 | src/encoder/encoder.c:7532 |
| `Y264_MBTREE_OFF` | ? | src/encoder/encoder.c:6045 |
| `Y264_MBTREE_STRENGTH` | = 5*(1-qcomp)*0.7, 1.4 at qcomp 0.6 (fallback path 2.0) | src/encoder/encoder.c:8762 |
| `Y264_MBT_DEPFLOOR` | = param | src/encoder/encoder.c:4976 |
| `Y264_MBT_MISSWHY` | 0 | src/encoder/encoder.c:8665 |
| `Y264_MBT_PLAY` | ? | src/encoder/encoder.c:9928 |
| `Y264_MBT_REC` | ? | src/encoder/encoder.c:9928 |
| `Y264_MBT_SPLIT` | 0 | src/encoder/encoder.c:8654 |
| `Y264_MEREUSE_TRACE` | 0 | src/encoder/mbcode.c:6272 |
| `Y264_ME_ETSTAT` | 0 | src/encoder/motion.c:301 |
| `Y264_NTP_PROF` | 0 | src/common/threadpool.c:61 |
| `Y264_PSY_FLAT_LOG` | 0 | src/encoder/encoder.c:7174 |
| `Y264_QP_TRACE` | 0 | src/encoder/encoder.c:3160 |
| `Y264_RC_PLAY` | ? | src/encoder/rcreplay.h:157 |
| `Y264_RC_PLAY_K` | 1.0 | src/encoder/rcreplay.h:177 |
| `Y264_RC_REC` | ? | src/encoder/rcreplay.h:157 |
| `Y264_SIMD_FORCE` | unset | src/common/cpu.c:213 |
| `Y264_SKIP_ORACLE` | ? | src/encoder/skiporacle.c:35 |
| `Y264_SKIP_ORACLE_AT` | pre | src/encoder/skiporacle.c:47 |
| `Y264_SKIP_ORACLE_SIDE` | ? | src/encoder/skiporacle.c:37 |
| `Y264_THREAD_PROF` | 0 | src/encoder/encoder.c:111 |
| `Y264_TYPE_ORACLE` | ? | src/encoder/encoder.c:10171 |
| `Y264_UNSAFE_NO_EMIT` | 0 | src/encoder/mbcode.c:5657 |
| `Y264_UNSAFE_NO_MBT` | 0 | src/encoder/encoder.c:10126 |
| `Y264_UNSAFE_NO_NAL` | 0 | src/encoder/encoder.c:3349 |
| `Y264_UNSAFE_NO_PREVPWAIT` | 0 | src/encoder/encoder.c:872 |
| `Y264_UNSAFE_NO_REFBWAIT` | 0 | src/encoder/encoder.c:856 |
| `Y264_VBV_ROWBRAKE_TRACE` | 0 | src/encoder/mbcode.c:5641 |
| `Y264_WEIGHTP_STAT` | 0 | src/encoder/encoder.c:896 |

## arm (16)

| knob | default | reader |
|---|---|---|
| `Y264_BAND_PRECOMP` | 0 | src/encoder/mbcode.c:5884 |
| `Y264_B_RECT` | 0 | src/encoder/mbcode.c:7559 |
| `Y264_HW` | off | src/encoder/encoder.c:17959 |
| `Y264_MAX_INPUT_MB` | ? | cli/yah264_cli.c:3294 |
| `Y264_MCSTF` | ? | src/encoder/mcstf.c:56 |
| `Y264_MCSTF_RANGE` | ? | src/encoder/mcstf.c:40 |
| `Y264_ME_HQ` | 0 | src/encoder/motion.c:241 |
| `Y264_NR_INTER` | 0 | src/encoder/encoder.c:2098 |
| `Y264_NR_INTRA` | 0 | src/encoder/encoder.c:2097 |
| `Y264_PROBE_TRELLIS` | 0 | src/encoder/mbcode.c:10707 |
| `Y264_P_PART_RD` | 0 | src/encoder/mbcode.c:11909 |
| `Y264_P_PART_RD_T` | 1 | src/encoder/mbcode.c:11916 |
| `Y264_QPSQ_MINW` | 0 | src/encoder/motion.c:248 |
| `Y264_STREAM_WINDOW` | ? | cli/yah264_cli.c:4145 |
| `Y264_SUB_FAR` | 0 | src/encoder/motion.c:243 |
| `Y264_TREL_SMOOTH` | on (0 = off) | src/encoder/mbcode.c:1959 |
