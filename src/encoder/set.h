/*
 * set.h - sequence and picture parameter set serialization
 * Copyright (c) 2026, the next264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef NEXT264_SET_H
#define NEXT264_SET_H

#include "../common/bitstream.h"
#include "../dsp/transform.h"

/* Only the fields next264 currently sets. Everything else defaults to the
 * values written by the serializers below. */
typedef struct {
    int profile_idc;
    int chroma_format_idc;                       /* 1=4:2:0, 2=4:2:2, 3=4:4:4 */
    int entropy_coding_mode_flag;                /* 1 = CABAC; forbidden in Baseline */
    int level_idc;
    int sps_id;
    int log2_max_frame_num_minus4;
    int pic_order_cnt_type;
    int log2_max_pic_order_cnt_lsb_minus4;       /* only for pic_order_cnt_type 0 */
    int max_num_ref_frames;
    int max_num_reorder_frames;     /* VUI bitstream restriction (output delay) */
    int max_dec_frame_buffering;
    /* VUI timing_info: frame_rate = time_scale / (2 * num_units_in_tick). Signals
 * the framerate so muxers/players don't have to guess. 0 = not present. */
    int vui_timing;
    int num_units_in_tick;
    int time_scale;
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
    const n264_cqm_t *cqm;
} n264_sps_t;

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
} n264_pps_t;

/* Pick the H.264 profile_idc for the enabled tools. CABAC and B-slices are both
 * forbidden in Baseline (66), so either one forces Main (77). */
int n264_profile_idc(int entropy_coding_mode_flag, int bframes);

/* Write the SPS/PPS RBSP (no NAL header, no emulation prevention) into `bs`,
 * ending with rbsp_trailing_bits. */
void n264_sps_write(n264_bs_t *bs, const n264_sps_t *sps);
void n264_pps_write(n264_bs_t *bs, const n264_pps_t *pps);

#endif /* NEXT264_SET_H */
