# The Y264_* knob census (generated -- edit scripts/knob_census.py OVERRIDES, not this file)

Regenerate: `python3 scripts/knob_census.py`. CI check: `--check`.

Tier meanings: **default** = shipped behavior, the env is its escape; **instrument** = measurement machinery (cataloged in docs/instruments.md when it has produced a result); **arm** = default-off gate still being priced. A refuted arm is removed, with its verdict and the removing commit recorded in the local refused index.

## default (41)

| knob | default | reader |
|---|---|---|
| `Y264_2PASS_MT` | 1 | cli/yah264_cli.c:5500 |
| `Y264_ABR_QCOMP` | 0.6 | src/encoder/rate.c:2550 |
| `Y264_ABR_TOL` | = param | src/encoder/encoder.c:5023 |
| `Y264_ABR_VBVOV` | 0.25 | src/encoder/rate.c:2588 |
| `Y264_AQ_DC` | = rate mode: 1.3 uncapped CRF, 1.04 capped CRF or bitrate target | src/encoder/encoder.c:5647 |
| `Y264_AQ_MODE` | 2 | src/encoder/mbcode.c:311 |
| `Y264_B8_DIRECT` | 1 | src/encoder/mbcode.c:7329 |
| `Y264_B8_QGATE` | 10 | src/encoder/mbcode.c:7495 |
| `Y264_BAND_ROWS` | 2 | src/encoder/mbcode.c:5713 |
| `Y264_BSKIP_SCREEN` | 1 | src/encoder/encoder.c:4583 |
| `Y264_B_8X8` | 1 | src/encoder/mbcode.c:7320 |
| `Y264_B_SKIP_EXIT` | 1 | src/encoder/mbcode.c:3398 |
| `Y264_CDC_LAM` | 128 | src/encoder/mbcode.c:3712 |
| `Y264_CDC_RD` | 1 | src/encoder/mbcode.c:3706 |
| `Y264_CRF_CPLX` | 1 | src/encoder/rate.c:2541 |
| `Y264_CRF_LEVEL` | 2.3 | src/encoder/rate.c:560 |
| `Y264_DCTDEC` | 1 | src/encoder/mbcode.c:4187 |
| `Y264_DIRECT_AUTO` | -1 | src/encoder/encoder.c:534 |
| `Y264_DZ_INTER` | -1 | src/dsp/transform.c:846 |
| `Y264_DZ_INTRA` | -1 | src/dsp/transform.c:845 |
| `Y264_FAST_PSKIP` | 1 | src/encoder/mbcode.c:5610 |
| `Y264_GPQ_CONSUME` | 1 | src/encoder/encoder.c:8260 |
| `Y264_MBT_BLEG_REUSE` | 1 | src/encoder/encoder.c:8235 |
| `Y264_MBT_BREF` | 1 | src/encoder/encoder.c:248 |
| `Y264_MBT_PAIR_SCALE` | 1 | src/encoder/encoder.c:7678 |
| `Y264_PART_EARLYTERM` | 4 | src/encoder/mbcode.c:5649 |
| `Y264_RCP_LAG` | 1 | src/encoder/encoder.c:457 |
| `Y264_RC_CARRY` | 1 | cli/yah264_cli.c:1312 |
| `Y264_STAIR_BDEPTH` | 1 | src/encoder/encoder.c:362 |
| `Y264_STAIR_TDIR` | 1 | src/encoder/encoder.c:589 |
| `Y264_STAIR_WIDE` | 1 | src/encoder/encoder.c:399 |
| `Y264_STQ` | -1 | src/encoder/encoder.c:876 |
| `Y264_TP_CPLXBLUR` | 20.0 | src/encoder/encoder.c:5267 |
| `Y264_TP_IPF` | 1.4 | src/encoder/encoder.c:5271 |
| `Y264_TP_PBF` | 1.3 | src/encoder/encoder.c:5272 |
| `Y264_UMH_RANGE` | 16 | src/encoder/motion.c:969 |
| `Y264_VBV_LOW` | 0.25 | src/encoder/rate.c:553 |
| `Y264_VBV_PLAN` | 1 | src/encoder/rate.c:1242 |
| `Y264_VBV_POSTI` | 1.0 | src/encoder/rate.c:552 |
| `Y264_W2` | = pool present | src/encoder/encoder.c:5790 |
| `Y264_WF_CAPK` | -2 | src/encoder/encoder.c:3863 |

## instrument (100)

| knob | default | reader |
|---|---|---|
| `Y264_ADME_LOG` | 0 | src/encoder/encoder.c:6536 |
| `Y264_ASM_OFF` | 0 | src/common/cpu.c:217 |
| `Y264_AUTO_THREADS` | ? | src/encoder/encoder.c:3955 |
| `Y264_AUTO_THREADS_MAX` | ? | src/encoder/encoder.c:3961 |
| `Y264_B8_STAT` | 0 | src/encoder/mbcode.c:7393 |
| `Y264_BDIR_STAT` | 0 | src/encoder/mbcode.c:8358 |
| `Y264_BITSTAT` | 0 | src/encoder/mbcode.c:9936 |
| `Y264_BLATE_STAT` | ? | src/encoder/encoder.c:5511 |
| `Y264_BPROF` | 0 | src/encoder/mbcode.c:8333 |
| `Y264_BPROF2` | 0 | src/encoder/mbcode.c:7694 |
| `Y264_BSKIP_PROBE` | 0 | src/encoder/encoder.c:4576 |
| `Y264_B_REF_ET_STAT` | 0 | src/encoder/mbcode.c:6897 |
| `Y264_CABAC_TRACE` | ? | src/encoder/cabac.c:69 |
| `Y264_CABAC_TRACE_FLAT` | ? | src/encoder/cabac.c:52 |
| `Y264_CUT_SPLIT` | ? | cli/yah264_cli.c:2800 |
| `Y264_CUT_SPLIT_STAT` | ? | cli/yah264_cli.c:2694 |
| `Y264_CZW_STAT` | 0 | src/encoder/encoder.c:3682 |
| `Y264_DBG_CPLX` | ? | src/encoder/rate.c:563 |
| `Y264_DIAG_COLWATCH` | 0 | src/encoder/mbcode.c:45 |
| `Y264_DIAG_DIRECTOK` | 0 | src/encoder/mbcode.c:46 |
| `Y264_DIAG_NOHPEL` | 0 | src/encoder/motion.c:527 |
| `Y264_DIAG_TDIRLIM` | 0 | src/encoder/mbcode.c:44 |
| `Y264_DIRECT_LRVOTE` | 0 | src/encoder/encoder.c:7813 |
| `Y264_DIRECT_PERMB` | -1 | src/encoder/encoder.c:484 |
| `Y264_DIRECT_SCORE` | 0 | src/encoder/encoder.c:2491 |
| `Y264_DIRECT_WHY` | 0 | src/encoder/encoder.c:2473 |
| `Y264_EST_FROZEN` | 0 | src/encoder/mbcode.c:5537 |
| `Y264_EST_PROF` | 0 | src/encoder/mbcode.c:4517 |
| `Y264_EST_SCRTRACE` | 0 | src/encoder/mbcode.c:4528 |
| `Y264_EST_TWICE` | 0 | src/encoder/mbcode.c:5505 |
| `Y264_FLATSKIP_STAT` | 0 | src/encoder/mbcode.c:8140 |
| `Y264_FQP_TRACE` | 0 | src/encoder/encoder.c:1170 |
| `Y264_GOP_FORCE_G` | ? | cli/yah264_cli.c:2939 |
| `Y264_GOP_FORCE_K` | ? | cli/yah264_cli.c:2940 |
| `Y264_GOP_TRACE` | ? | cli/yah264_cli.c:2839 |
| `Y264_GPQ_WARMUP` | 24 | src/encoder/gpu.c:272 |
| `Y264_GPU_PHASEA` | 0 | src/encoder/gpu.c:212 |
| `Y264_HEX_ORACLE` | 0 | src/encoder/motion.c:943 |
| `Y264_HPEL_CENSUS` | 0 | src/encoder/motion.c:206 |
| `Y264_HPEL_PROBE` | 0 | src/encoder/encoder.c:1701 |
| `Y264_IDENT_STAT` | 0 | src/encoder/encoder.c:3115 |
| `Y264_LA_BUF` | ? | src/encoder/encoder.c:927 |
| `Y264_LA_STAT` | ? | src/encoder/encoder.c:5886 |
| `Y264_LRROW_PROF` | 0 | src/encoder/encoder.c:10144 |
| `Y264_LRSUB_CENSUS` | 0 | src/encoder/encoder.c:6814 |
| `Y264_LRSUB_PROBE` | 0 | src/encoder/encoder.c:6873 |
| `Y264_MBTREE_DBG` | ? | src/encoder/encoder.c:9000 |
| `Y264_MBTREE_OFF` | ? | src/encoder/encoder.c:5438 |
| `Y264_MBTREE_STRENGTH` | = 5*(1-qcomp)*0.7, 1.4 at qcomp 0.6 (fallback path 2.0) | src/encoder/encoder.c:8442 |
| `Y264_MBT_DEPFLOOR` | = param | src/encoder/encoder.c:4620 |
| `Y264_MBT_MISSWHY` | 0 | src/encoder/encoder.c:8278 |
| `Y264_MBT_PLAY` | ? | src/encoder/encoder.c:9665 |
| `Y264_MBT_PRE_DBG` | ? | src/encoder/encoder.c:16859 |
| `Y264_MBT_RATIO_DUMP` | ? | src/encoder/encoder.c:8959 |
| `Y264_MBT_REC` | ? | src/encoder/encoder.c:9665 |
| `Y264_MBT_SPLIT` | 0 | src/encoder/encoder.c:8267 |
| `Y264_MBT_UNSAFE_NOSETTLE` | 0 | src/encoder/encoder.c:8250 |
| `Y264_MB_LOG` | 0 | src/encoder/mbcode.c:52 |
| `Y264_MCSTF_TRACE` | ? | src/encoder/mcstf.c:64 |
| `Y264_MEREUSE_TRACE` | 0 | src/encoder/mbcode.c:6148 |
| `Y264_ME_DUMP` | ? | src/encoder/mbcode.c:6120 |
| `Y264_ME_ETSTAT` | 0 | src/encoder/motion.c:477 |
| `Y264_ME_STATS` | 0 | src/encoder/motion.c:441 |
| `Y264_NR_TRACE` | 0 | src/encoder/encoder.c:2057 |
| `Y264_NTP_PROF` | 0 | src/common/threadpool.c:111 |
| `Y264_NTP_STATS` | 0 | src/common/threadpool.c:82 |
| `Y264_PPRUNE_PROBE` | 0 | src/encoder/mbcode.c:8398 |
| `Y264_PROBE_DEADZONE` | 0 | src/encoder/mbcode.c:1096 |
| `Y264_PROBE_TRELLIS` | 1 | src/encoder/mbcode.c:10980 |
| `Y264_PSKIP_CENSUS` | 0 | src/encoder/mbcode.c:8495 |
| `Y264_PSY_FLAT_LOG` | 0 | src/encoder/encoder.c:6529 |
| `Y264_QP_TRACE` | 0 | src/encoder/encoder.c:3193 |
| `Y264_RCP_DBG` | 0 | src/encoder/rate.c:2559 |
| `Y264_RC_PLAY` | ? | src/encoder/rcreplay.h:156 |
| `Y264_RC_PLAY_K` | 1.0 | src/encoder/rcreplay.h:176 |
| `Y264_RC_REC` | ? | src/encoder/rcreplay.h:156 |
| `Y264_RC_TRACE` | ? | src/encoder/rate.c:562 |
| `Y264_RD_ABORT_STAT` | 0 | src/encoder/mbcode.c:7734 |
| `Y264_RESCENSUS` | 0 | src/encoder/mbcode.c:4428 |
| `Y264_RESPROF` | 0 | src/encoder/mbcode.c:4503 |
| `Y264_SIMD_FORCE` | unset | src/common/cpu.c:212 |
| `Y264_SKIP_ORACLE` | ? | src/encoder/skiporacle.c:34 |
| `Y264_SKIP_ORACLE_AT` | pre | src/encoder/skiporacle.c:46 |
| `Y264_SKIP_ORACLE_SIDE` | ? | src/encoder/skiporacle.c:36 |
| `Y264_STAIR_STAT` | 0 | src/encoder/encoder.c:900 |
| `Y264_STREAM_STAT` | ? | cli/yah264_cli.c:3650 |
| `Y264_THREAD_PROF` | 0 | src/encoder/encoder.c:108 |
| `Y264_TP_DBG` | presence | src/encoder/encoder.c:5270 |
| `Y264_TRPROF` | 0 | src/encoder/mbcode.c:4497 |
| `Y264_TYPE_ORACLE` | ? | src/encoder/encoder.c:9908 |
| `Y264_UNSAFE_GATE_AROW` | 0 | src/encoder/encoder.c:13903 |
| `Y264_UNSAFE_NO_EMIT` | 0 | src/encoder/mbcode.c:5492 |
| `Y264_UNSAFE_NO_MBT` | 0 | src/encoder/encoder.c:9863 |
| `Y264_UNSAFE_NO_NAL` | 0 | src/encoder/encoder.c:3385 |
| `Y264_UNSAFE_NO_PREVPWAIT` | 0 | src/encoder/encoder.c:861 |
| `Y264_UNSAFE_NO_REFBWAIT` | 0 | src/encoder/encoder.c:842 |
| `Y264_UNSAFE_NO_ROWGATE` | 0 | src/encoder/encoder.c:13884 |
| `Y264_VBV_ROWBRAKE_TRACE` | 0 | src/encoder/mbcode.c:5476 |
| `Y264_VBV_STAT` | 0 | src/encoder/rate.c:2567 |
| `Y264_WEIGHTP_STAT` | 0 | src/encoder/encoder.c:892 |

## arm (27)

| knob | default | reader |
|---|---|---|
| `Y264_AQ_DARK` | ? | src/encoder/mbcode.c:416 |
| `Y264_AQ_DARK_B0` | ? | src/encoder/mbcode.c:417 |
| `Y264_AQ_DARK_B1` | ? | src/encoder/mbcode.c:418 |
| `Y264_AQ_DARK_M` | ? | src/encoder/mbcode.c:416 |
| `Y264_AQ_DARK_V` | ? | src/encoder/mbcode.c:417 |
| `Y264_BAND_PRECOMP` | 0 | src/encoder/mbcode.c:5734 |
| `Y264_GPU_LOWRES` | 0 | src/encoder/gpu.c:15 |
| `Y264_HW` | off | src/encoder/encoder.c:17772 |
| `Y264_LR_QPEL_AVG` | 0 | src/encoder/encoder.c:7026 |
| `Y264_MAX_INPUT_MB` | ? | cli/yah264_cli.c:2162 |
| `Y264_MCSTF` | ? | src/encoder/mcstf.c:65 |
| `Y264_MCSTF_ADAPT` | ? | src/encoder/mcstf.c:56 |
| `Y264_MCSTF_ADAPT_K` | ? | src/encoder/mcstf.c:59 |
| `Y264_MCSTF_CLEAN` | ? | src/encoder/mcstf.c:55 |
| `Y264_MCSTF_CUT` | ? | src/encoder/mcstf.c:61 |
| `Y264_MCSTF_PERIOD` | ? | src/encoder/mcstf.c:43 |
| `Y264_MCSTF_QADAPT` | ? | src/encoder/mcstf.c:62 |
| `Y264_MCSTF_QP` | ? | src/encoder/mcstf.c:63 |
| `Y264_MCSTF_RANGE` | ? | src/encoder/mcstf.c:40 |
| `Y264_MCSTF_SIGMA` | ? | src/encoder/mcstf.c:47 |
| `Y264_MCSTF_SUBPEL` | ? | src/encoder/mcstf.c:60 |
| `Y264_NR_INTER` | 0 | src/encoder/encoder.c:2051 |
| `Y264_NR_INTRA` | 0 | src/encoder/encoder.c:2050 |
| `Y264_NTP_PARK` | 0 | src/common/threadpool.c:67 |
| `Y264_PSY_DARK` | ? | src/encoder/mbcode.c:456 |
| `Y264_P_SKIP_EXIT` | 0 | src/encoder/mbcode.c:5596 |
| `Y264_STREAM_WINDOW` | ? | cli/yah264_cli.c:2887 |
