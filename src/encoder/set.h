/*
 * set.h - sequence and picture parameter set serialization
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef YAH264_SET_H
#define YAH264_SET_H

#include "../common/bitstream.h"
#include "../dsp/transform.h"

/* Only the fields yah264 currently sets. Everything else defaults to the
 * values written by the serializers below. */
typedef struct {
    int profile_idc;
    /* Extra constraint_set flags to assert, in the byte's own bit order
 * (constraint_set0 is 0x80). constraint_set0 is asserted by profile_idc 66
 * itself and is not written here. 0 = assert nothing extra, which is what
 * every stream did before --profile existed. */
    int constraints;
    int chroma_format_idc;                       /* 1=4:2:0, 2=4:2:2, 3=4:4:4 */
    int entropy_coding_mode_flag;                /* 1 = CABAC; forbidden in Baseline */
    int level_idc;
    int sps_id;
    int log2_max_frame_num_minus4;
    int pic_order_cnt_type;
    int log2_max_pic_order_cnt_lsb_minus4;       /* only for pic_order_cnt_type 0 */
    int max_num_ref_frames;
    int max_num_reorder_frames;     /* VUI bitstream restriction (output delay) */
    /* VUI bitstream restriction: log2 of the longest motion vector this stream
 * uses, in units of 1/4 luma sample. 0 = write 16, the "no useful bound"
 * value this encoder used until 2026-09-16. The vertical one now follows the
 * range the search is actually clamped to -- the level's Table A-1 MaxVmvR,
 * or a tighter --mvrange. */
    int log2_mv_len_h, log2_mv_len_v;
    /* VUI overscan_info: 0 = not written, 1 = present with
 * overscan_appropriate_flag 0, 2 = present with the flag 1. */
    int overscan;
    int video_format;               /* VUI video_format, 0..5; default 5 */
    int pic_struct_present;         /* VUI pic_struct_present_flag */
    int max_dec_frame_buffering;
    /* VUI timing_info: frame_rate = time_scale / (2 * num_units_in_tick). Signals
 * the framerate so muxers/players don't have to guess. 0 = not present. */
    int vui_timing;
    /* VUI video_signal_type / colour description / chroma location (E.1.1).
 * vs_present 0 = not written (video_format is then 5, unspecified). */
    int vs_present, vs_full_range, vs_primaries, vs_transfer, vs_matrix;
    int chroma_loc;                 /* -1 = not written */
    int num_units_in_tick;
    int time_scale;
    /* VUI hrd_parameters (E.1.2), the Annex C buffer model. 0 = none written,
 * which is every stream that does not ask for --nal-hrd; 1 = cbr_flag 0,
 * 2 = cbr_flag 1. One SchedSelIdx, because the encoder obeys one bucket.
 * The two values are the CODED ones -- rounded onto the scale's grid by the
 * caller -- so the numbers a receiver reads are the numbers the encoder's
 * own schedule was built from, not a rounding of them. */
    int hrd;
    unsigned hrd_bit_rate;          /* BitRate[0] in bit/s, a multiple of 64 */
    unsigned hrd_cpb_size;          /* CpbSize[0] in bits, a multiple of 16 */
    /* VUI aspect_ratio_info: sample aspect ratio W:H, written as Extended_SAR.
 * 0 = not present (square/unspecified). */
    int sar_num;
    int sar_den;
    int width_in_mbs;
    int height_in_map_units;
    int frame_mbs_only_flag;
    int direct_8x8_inference_flag;
    /* Frame cropping, in crop units (2 luma samples each for 4:2:0). */
    int crop_left, crop_right, crop_top, crop_bottom;
    /* Custom quantisation matrices. When cqm != NULL the SPS carries the
 * scaling lists (seq_scaling_matrix_present_flag = 1) and profile_idc is
 * High (>= 100). NULL leaves the flat default. */
    const y264_cqm_t *cqm;
} y264_sps_t;

typedef struct {
    int pps_id;
    int sps_id;
    int entropy_coding_mode_flag;               /* 0 = CAVLC */
    int num_ref_idx_l0_default_active_minus1;
    int num_ref_idx_l1_default_active_minus1;
    int weighted_pred_flag;
    int weighted_bipred_idc;
    int pic_init_qp_minus26;
    int chroma_qp_index_offset;
    int deblocking_filter_control_present_flag;
    int constrained_intra_pred_flag;
    int transform_8x8_mode_flag;
} y264_pps_t;

/* The widths this encoder declares for the HRD delay fields, in bits. They are
 * written once in hrd_parameters and then govern how every buffering_period and
 * pic_timing SEI in the stream is PARSED, so the SPS writer and the SEI writers
 * have to agree on them or the messages read as garbage. Hence one definition,
 * here, rather than a literal in each. 24 bits is 16777215 ticks -- 93 hours at
 * 25 fps -- so the counters cannot wrap inside anything this encoder codes. */
#define Y264_HRD_INIT_DELAY_BITS 24
#define Y264_HRD_CPB_DELAY_BITS  24
#define Y264_HRD_DPB_DELAY_BITS  24

/* Pick the H.264 profile_idc for the enabled tools. CABAC and B-slices are both
 * forbidden in Baseline (66), so either one forces Main (77). */
int y264_profile_idc(int entropy_coding_mode_flag, int bframes);

/* Write the SPS/PPS RBSP (no NAL header, no emulation prevention) into `bs`,
 * ending with rbsp_trailing_bits. */
void y264_sps_write(y264_bs_t *bs, const y264_sps_t *sps);
void y264_pps_write(y264_bs_t *bs, const y264_pps_t *pps);

#endif /* YAH264_SET_H */
