# The Y264_* knob census (generated -- edit scripts/knob_census.py OVERRIDES, not this file)

Regenerate: `python3 scripts/knob_census.py`. CI check: `--check`.

Tier meanings: **default** = shipped behavior, the env is its escape; **instrument** = measurement machinery (cataloged in docs/instruments.md when it has produced a result); **arm** = default-off gate still being priced. A refuted arm is removed, with its verdict and the removing commit recorded in the local refused index.

## default (42)

| knob | default | reader |
|---|---|---|
| `Y264_2PASS_MT` | 1 | cli/yah264_cli.c:5503 |
| `Y264_ABR_QCOMP` | 0.6 | src/encoder/rate.c:2551 |
| `Y264_ABR_TOL` | = param | src/encoder/encoder.c:5026 |
| `Y264_ABR_VBVOV` | 0.25 | src/encoder/rate.c:2589 |
| `Y264_AQ_DC` | = rate mode: 1.3 uncapped CRF, 1.04 capped CRF or bitrate target | src/encoder/encoder.c:5647 |
| `Y264_AQ_MODE` | 2 | src/encoder/mbcode.c:312 |
| `Y264_B8_DIRECT` | 1 | src/encoder/mbcode.c:7373 |
| `Y264_B8_QGATE` | 10 | src/encoder/mbcode.c:7547 |
| `Y264_BAND_ROWS` | 2 | src/encoder/mbcode.c:5757 |
| `Y264_BSKIP_SCREEN` | 1 | src/encoder/encoder.c:4586 |
| `Y264_B_8X8` | 1 | src/encoder/mbcode.c:7364 |
| `Y264_B_SKIP_EXIT` | 1 | src/encoder/mbcode.c:3433 |
| `Y264_CDC_LAM` | 64 | src/encoder/mbcode.c:3742 |
| `Y264_CDC_RD` | 1 | src/encoder/mbcode.c:3748 |
| `Y264_CRF_CPLX` | 1 | src/encoder/rate.c:2542 |
| `Y264_CRF_LEVEL` | 2.3 | src/encoder/rate.c:561 |
| `Y264_DCTDEC` | 1 | src/encoder/mbcode.c:4231 |
| `Y264_DIRECT_AUTO` | -1 | src/encoder/encoder.c:535 |
| `Y264_DZ_INTER` | -1 | src/dsp/transform.c:847 |
| `Y264_DZ_INTRA` | -1 | src/dsp/transform.c:846 |
| `Y264_FAST_PSKIP` | 1 | src/encoder/mbcode.c:5654 |
| `Y264_GPQ_CONSUME` | 1 | src/encoder/encoder.c:8260 |
| `Y264_MBT_BLEG_REUSE` | 1 | src/encoder/encoder.c:8235 |
| `Y264_MBT_BREF` | 1 | src/encoder/encoder.c:249 |
| `Y264_MBT_PAIR_SCALE` | 1 | src/encoder/encoder.c:7678 |
| `Y264_PART_EARLYTERM` | 4 | src/encoder/mbcode.c:5693 |
| `Y264_RCP_LAG` | 1 | src/encoder/encoder.c:458 |
| `Y264_RC_CARRY` | 1 | cli/yah264_cli.c:1315 |
| `Y264_STAIR_BDEPTH` | 1 | src/encoder/encoder.c:363 |
| `Y264_STAIR_TDIR` | 1 | src/encoder/encoder.c:590 |
| `Y264_STAIR_WIDE` | 1 | src/encoder/encoder.c:400 |
| `Y264_STQ` | -1 | src/encoder/encoder.c:877 |
| `Y264_TP_CPLXBLUR` | 20.0 | src/encoder/encoder.c:5270 |
| `Y264_TP_IPF` | 1.4 | src/encoder/encoder.c:5274 |
| `Y264_TP_PBF` | 1.3 | src/encoder/encoder.c:5275 |
| `Y264_TREL_SMOOTH_V` | 8 | src/encoder/mbcode.c:1655 |
| `Y264_UMH_RANGE` | 16 | src/encoder/motion.c:970 |
| `Y264_VBV_LOW` | 0.25 | src/encoder/rate.c:554 |
| `Y264_VBV_PLAN` | 1 | src/encoder/rate.c:1243 |
| `Y264_VBV_POSTI` | 1.0 | src/encoder/rate.c:553 |
| `Y264_W2` | = pool present | src/encoder/encoder.c:5790 |
| `Y264_WF_CAPK` | -2 | src/encoder/encoder.c:3859 |

## instrument (99)

| knob | default | reader |
|---|---|---|
| `Y264_ADME_LOG` | 0 | src/encoder/encoder.c:6536 |
| `Y264_ASM_OFF` | 0 | src/common/cpu.c:218 |
| `Y264_AUTO_THREADS` | ? | src/encoder/encoder.c:3951 |
| `Y264_AUTO_THREADS_MAX` | ? | src/encoder/encoder.c:3957 |
| `Y264_B8_STAT` | 0 | src/encoder/mbcode.c:7437 |
| `Y264_BDIR_STAT` | 0 | src/encoder/mbcode.c:8410 |
| `Y264_BITSTAT` | 0 | src/encoder/mbcode.c:10018 |
| `Y264_BLATE_STAT` | ? | src/encoder/encoder.c:5511 |
| `Y264_BPROF` | 0 | src/encoder/mbcode.c:8385 |
| `Y264_BPROF2` | 0 | src/encoder/mbcode.c:7746 |
| `Y264_BSKIP_PROBE` | 0 | src/encoder/encoder.c:4579 |
| `Y264_B_REF_ET_STAT` | 0 | src/encoder/mbcode.c:6941 |
| `Y264_CABAC_TRACE` | ? | src/encoder/cabac.c:70 |
| `Y264_CABAC_TRACE_FLAT` | ? | src/encoder/cabac.c:53 |
| `Y264_CUT_SPLIT` | ? | cli/yah264_cli.c:2803 |
| `Y264_CUT_SPLIT_STAT` | ? | cli/yah264_cli.c:2697 |
| `Y264_CZW_STAT` | 0 | src/encoder/encoder.c:3678 |
| `Y264_DBG_CPLX` | ? | src/encoder/rate.c:564 |
| `Y264_DIAG_COLWATCH` | 0 | src/encoder/mbcode.c:46 |
| `Y264_DIAG_DIRECTOK` | 0 | src/encoder/mbcode.c:47 |
| `Y264_DIAG_NOHPEL` | 0 | src/encoder/motion.c:528 |
| `Y264_DIAG_TDIRLIM` | 0 | src/encoder/mbcode.c:45 |
| `Y264_DIRECT_LRVOTE` | 0 | src/encoder/encoder.c:7813 |
| `Y264_DIRECT_PERMB` | -1 | src/encoder/encoder.c:485 |
| `Y264_DIRECT_SCORE` | 0 | src/encoder/encoder.c:2487 |
| `Y264_DIRECT_WHY` | 0 | src/encoder/encoder.c:2469 |
| `Y264_EST_FROZEN` | 0 | src/encoder/mbcode.c:5581 |
| `Y264_EST_PROF` | 0 | src/encoder/mbcode.c:4561 |
| `Y264_EST_SCRTRACE` | 0 | src/encoder/mbcode.c:4572 |
| `Y264_EST_TWICE` | 0 | src/encoder/mbcode.c:5549 |
| `Y264_FLATSKIP_STAT` | 0 | src/encoder/mbcode.c:8192 |
| `Y264_FQP_TRACE` | 0 | src/encoder/encoder.c:1171 |
| `Y264_GOP_FORCE_G` | ? | cli/yah264_cli.c:2942 |
| `Y264_GOP_FORCE_K` | ? | cli/yah264_cli.c:2943 |
| `Y264_GOP_TRACE` | ? | cli/yah264_cli.c:2842 |
| `Y264_GPQ_WARMUP` | 24 | src/encoder/gpu.c:273 |
| `Y264_GPU_PHASEA` | 0 | src/encoder/gpu.c:213 |
| `Y264_HEX_ORACLE` | 0 | src/encoder/motion.c:944 |
| `Y264_HPEL_CENSUS` | 0 | src/encoder/motion.c:207 |
| `Y264_HPEL_PROBE` | 0 | src/encoder/encoder.c:1697 |
| `Y264_IDENT_STAT` | 0 | src/encoder/encoder.c:3111 |
| `Y264_LA_BUF` | ? | src/encoder/encoder.c:928 |
| `Y264_LA_STAT` | ? | src/encoder/encoder.c:5886 |
| `Y264_LRROW_PROF` | 0 | src/encoder/encoder.c:10144 |
| `Y264_LRSUB_CENSUS` | 0 | src/encoder/encoder.c:6814 |
| `Y264_LRSUB_PROBE` | 0 | src/encoder/encoder.c:6873 |
| `Y264_MBTREE_DBG` | ? | src/encoder/encoder.c:9000 |
| `Y264_MBTREE_OFF` | ? | src/encoder/encoder.c:5438 |
| `Y264_MBTREE_STRENGTH` | = 5*(1-qcomp)*0.7, 1.4 at qcomp 0.6 (fallback path 2.0) | src/encoder/encoder.c:8442 |
| `Y264_MBT_DEPFLOOR` | = param | src/encoder/encoder.c:4623 |
| `Y264_MBT_MISSWHY` | 0 | src/encoder/encoder.c:8278 |
| `Y264_MBT_PLAY` | ? | src/encoder/encoder.c:9665 |
| `Y264_MBT_PRE_DBG` | ? | src/encoder/encoder.c:16858 |
| `Y264_MBT_RATIO_DUMP` | ? | src/encoder/encoder.c:8959 |
| `Y264_MBT_REC` | ? | src/encoder/encoder.c:9665 |
| `Y264_MBT_SPLIT` | 0 | src/encoder/encoder.c:8267 |
| `Y264_MBT_UNSAFE_NOSETTLE` | 0 | src/encoder/encoder.c:8250 |
| `Y264_MB_LOG` | 0 | src/encoder/mbcode.c:53 |
| `Y264_MEREUSE_TRACE` | 0 | src/encoder/mbcode.c:6192 |
| `Y264_ME_DUMP` | ? | src/encoder/mbcode.c:6164 |
| `Y264_ME_ETSTAT` | 0 | src/encoder/motion.c:478 |
| `Y264_ME_STATS` | 0 | src/encoder/motion.c:442 |
| `Y264_NR_TRACE` | 0 | src/encoder/encoder.c:2053 |
| `Y264_NTP_PROF` | 0 | src/common/threadpool.c:112 |
| `Y264_NTP_STATS` | 0 | src/common/threadpool.c:83 |
| `Y264_PPRUNE_PROBE` | 0 | src/encoder/mbcode.c:8450 |
| `Y264_PROBE_DEADZONE` | 0 | src/encoder/mbcode.c:1097 |
| `Y264_PROBE_TRELLIS` | 1 | src/encoder/mbcode.c:11062 |
| `Y264_PSKIP_CENSUS` | 0 | src/encoder/mbcode.c:8547 |
| `Y264_PSY_FLAT_LOG` | 0 | src/encoder/encoder.c:6529 |
| `Y264_QP_TRACE` | 0 | src/encoder/encoder.c:3189 |
| `Y264_RCP_DBG` | 0 | src/encoder/rate.c:2560 |
| `Y264_RC_PLAY` | ? | src/encoder/rcreplay.h:157 |
| `Y264_RC_PLAY_K` | 1.0 | src/encoder/rcreplay.h:177 |
| `Y264_RC_REC` | ? | src/encoder/rcreplay.h:157 |
| `Y264_RC_TRACE` | ? | src/encoder/rate.c:563 |
| `Y264_RD_ABORT_STAT` | 0 | src/encoder/mbcode.c:7786 |
| `Y264_RESCENSUS` | 0 | src/encoder/mbcode.c:4472 |
| `Y264_RESPROF` | 0 | src/encoder/mbcode.c:4547 |
| `Y264_SIMD_FORCE` | unset | src/common/cpu.c:213 |
| `Y264_SKIP_ORACLE` | ? | src/encoder/skiporacle.c:35 |
| `Y264_SKIP_ORACLE_AT` | pre | src/encoder/skiporacle.c:47 |
| `Y264_SKIP_ORACLE_SIDE` | ? | src/encoder/skiporacle.c:37 |
| `Y264_STAIR_STAT` | 0 | src/encoder/encoder.c:901 |
| `Y264_STREAM_STAT` | ? | cli/yah264_cli.c:3653 |
| `Y264_THREAD_PROF` | 0 | src/encoder/encoder.c:109 |
| `Y264_TP_DBG` | presence | src/encoder/encoder.c:5273 |
| `Y264_TRPROF` | 0 | src/encoder/mbcode.c:4541 |
| `Y264_TYPE_ORACLE` | ? | src/encoder/encoder.c:9908 |
| `Y264_UNSAFE_GATE_AROW` | 0 | src/encoder/encoder.c:13903 |
| `Y264_UNSAFE_NO_EMIT` | 0 | src/encoder/mbcode.c:5536 |
| `Y264_UNSAFE_NO_MBT` | 0 | src/encoder/encoder.c:9863 |
| `Y264_UNSAFE_NO_NAL` | 0 | src/encoder/encoder.c:3381 |
| `Y264_UNSAFE_NO_PREVPWAIT` | 0 | src/encoder/encoder.c:862 |
| `Y264_UNSAFE_NO_REFBWAIT` | 0 | src/encoder/encoder.c:843 |
| `Y264_UNSAFE_NO_ROWGATE` | 0 | src/encoder/encoder.c:13884 |
| `Y264_VBV_ROWBRAKE_TRACE` | 0 | src/encoder/mbcode.c:5520 |
| `Y264_VBV_STAT` | 0 | src/encoder/rate.c:2568 |
| `Y264_WEIGHTP_STAT` | 0 | src/encoder/encoder.c:893 |

## arm (20)

| knob | default | reader |
|---|---|---|
| `Y264_AQ_DARK` | ? | src/encoder/mbcode.c:417 |
| `Y264_AQ_DARK_B0` | ? | src/encoder/mbcode.c:418 |
| `Y264_AQ_DARK_B1` | ? | src/encoder/mbcode.c:419 |
| `Y264_AQ_DARK_M` | ? | src/encoder/mbcode.c:417 |
| `Y264_AQ_DARK_V` | ? | src/encoder/mbcode.c:418 |
| `Y264_BAND_PRECOMP` | 0 | src/encoder/mbcode.c:5778 |
| `Y264_B_RECT` | 0 | src/encoder/mbcode.c:7520 |
| `Y264_GPU_LOWRES` | 0 | src/encoder/gpu.c:16 |
| `Y264_HW` | off | src/encoder/encoder.c:17771 |
| `Y264_LR_QPEL_AVG` | 0 | src/encoder/encoder.c:7026 |
| `Y264_MAX_INPUT_MB` | ? | cli/yah264_cli.c:2165 |
| `Y264_MCSTF` | ? | src/encoder/mcstf.c:56 |
| `Y264_MCSTF_RANGE` | ? | src/encoder/mcstf.c:40 |
| `Y264_NR_INTER` | 0 | src/encoder/encoder.c:2047 |
| `Y264_NR_INTRA` | 0 | src/encoder/encoder.c:2046 |
| `Y264_NTP_PARK` | 0 | src/common/threadpool.c:68 |
| `Y264_PSY_DARK` | ? | src/encoder/mbcode.c:457 |
| `Y264_P_SKIP_EXIT` | 0 | src/encoder/mbcode.c:5640 |
| `Y264_STREAM_WINDOW` | ? | cli/yah264_cli.c:2890 |
| `Y264_TREL_SMOOTH` | on (0 = off) | src/encoder/mbcode.c:1655 |
