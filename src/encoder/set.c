/*
 * set.c - SPS/PPS serialization per ITU-T H.264 sections 7.3.2.1 / 7.3.2.2
 * Copyright (c) 2026, the next264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "set.h"

/* Zig-zag scan (scan position -> raster index) for delta-coding scaling lists,
 * which the spec transmits in scan order (7.3.2.1.1.1). */
static const int SCAN4[16] = { 0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15 };
static const int SCAN8[64] = {
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

/* Emit scaling_list for one matrix: the scan-order run of se(v) delta_scale.
 * `w` is in raster order; `scan` maps scan position to raster index. Our matrix
 * values are never zero, so nextScale never hits the "use default" sentinel. */
static void write_scaling_list(n264_bs_t *bs, const uint8_t *w,
                               const int *scan, int size)
{
    int last = 8;
    for (int j = 0; j < size; j++) {
        int cur = w[scan[j]];
        n264_bs_write_se(bs, cur - last);            /* delta_scale */
        last = cur;
    }
}

int n264_profile_idc(int entropy_coding_mode_flag, int bframes)
{
    return (entropy_coding_mode_flag || bframes > 0) ? 77 : 66;
}

void n264_sps_write(n264_bs_t *bs, const n264_sps_t *sps)
{
    /* CABAC is forbidden in Baseline; force Main if it's on. B-frames are
 * already reflected in profile_idc by the encoder. */
    int profile_idc = sps->profile_idc;
    if (sps->entropy_coding_mode_flag && profile_idc < 77)
        profile_idc = 77;
    n264_bs_write(bs, 8, profile_idc);
    /* constraint_set0..5_flag + 2 reserved zero bits. Baseline advertises
 * constraint_set0_flag; Main leaves all constraints 0. */
    n264_bs_write(bs, 8, profile_idc == 66 ? 0x80 : 0x00);
    n264_bs_write(bs, 8, sps->level_idc);
    n264_bs_write_ue(bs, sps->sps_id);

    /* profile_idc >= 100 (High and above) carries chroma_format_idc, bit depths,
 * and the scaling-list flag. profile_idc < 100 omits the whole block (and is
 * only reachable at 4:2:0 8-bit flat quant). 4:4:4 adds a
 * separate_colour_plane_flag bit after the format. */
    if (profile_idc >= 100) {
        n264_bs_write_ue(bs, sps->chroma_format_idc);
        if (sps->chroma_format_idc == 3)
            n264_bs_write1(bs, 0);                   /* separate_colour_plane_flag: interleaved */
        n264_bs_write_ue(bs, N264_BIT_DEPTH - 8);    /* bit_depth_luma_minus8 */
        n264_bs_write_ue(bs, N264_BIT_DEPTH - 8);    /* bit_depth_chroma_minus8 */
        n264_bs_write1(bs, 0);                       /* qpprime_y_zero_transform_bypass */
        n264_bs_write1(bs, sps->cqm != NULL);        /* seq_scaling_matrix_present_flag */
        if (sps->cqm) {
            /* 8 lists for 4:2:0 (0-5 = 4x4 Intra/Inter Y/Cb/Cr, 6-7 = 8x8
 * Intra/Inter Y). Chroma shares the luma matrix, matching the
 * encoder's dequant. Each list is transmitted explicitly. */
            const uint8_t *w4[6] = { sps->cqm->w4[0], sps->cqm->w4[0], sps->cqm->w4[0],
                                     sps->cqm->w4[1], sps->cqm->w4[1], sps->cqm->w4[1] };
            for (int i = 0; i < 6; i++) {
                n264_bs_write1(bs, 1);               /* scaling_list_present_flag */
                write_scaling_list(bs, w4[i], SCAN4, 16);
            }
            n264_bs_write1(bs, 1);
            write_scaling_list(bs, sps->cqm->w8[0], SCAN8, 64);
            n264_bs_write1(bs, 1);
            write_scaling_list(bs, sps->cqm->w8[1], SCAN8, 64);
        }
    }

    n264_bs_write_ue(bs, sps->log2_max_frame_num_minus4);
    n264_bs_write_ue(bs, sps->pic_order_cnt_type);
    if (sps->pic_order_cnt_type == 0)
        n264_bs_write_ue(bs, sps->log2_max_pic_order_cnt_lsb_minus4);
    /* pic_order_cnt_type == 2 needs no further POC syntax. */

    n264_bs_write_ue(bs, sps->max_num_ref_frames);
    n264_bs_write1(bs, 0);                       /* gaps_in_frame_num_value_allowed */

    n264_bs_write_ue(bs, sps->width_in_mbs - 1);
    n264_bs_write_ue(bs, sps->height_in_map_units - 1);

    n264_bs_write1(bs, sps->frame_mbs_only_flag);
    /* frame_mbs_only_flag == 1, so no mb_adaptive_frame_field_flag. */
    n264_bs_write1(bs, sps->direct_8x8_inference_flag);

    int crop = sps->crop_left || sps->crop_right || sps->crop_top || sps->crop_bottom;
    n264_bs_write1(bs, crop);
    if (crop) {
        n264_bs_write_ue(bs, sps->crop_left);
        n264_bs_write_ue(bs, sps->crop_right);
        n264_bs_write_ue(bs, sps->crop_top);
        n264_bs_write_ue(bs, sps->crop_bottom);
    }

    /* VUI with only the bitstream restriction: max_num_reorder_frames tells
 * the decoder the exact output delay. Without it, ffmpeg grows its
 * reorder depth adaptively and silently drops a frame the first time a
 * deeper mini-GOP appears after shallower ones (variable B runs). */
    n264_bs_write1(bs, 1);                       /* vui_parameters_present_flag */
    n264_bs_write1(bs, sps->sar_num > 0 && sps->sar_den > 0 ? 1 : 0); /* aspect_ratio_info_present */
    if (sps->sar_num > 0 && sps->sar_den > 0) {
        /* Reduce, then prefer a Table E-1 aspect_ratio_idc over Extended_SAR
 * (same semantics, two bytes shorter, and what x264 emits). */
        int n = sps->sar_num, d = sps->sar_den;
        int a = n, b = d;
        while (b) { int t = a % b; a = b; b = t; }
        n /= a; d /= a;
        static const int E1[][2] = { {0,0}, {1,1}, {12,11}, {10,11}, {16,11},
            {40,33}, {24,11}, {20,11}, {32,11}, {80,33}, {18,11}, {15,11},
            {64,33}, {160,99}, {4,3}, {3,2}, {2,1} };
        int idc = 255;
        for (int i = 1; i <= 16; i++)
            if (E1[i][0] == n && E1[i][1] == d) { idc = i; break; }
        n264_bs_write(bs, 8, (uint32_t)idc);     /* aspect_ratio_idc */
        if (idc == 255) {
            n264_bs_write(bs, 16, (uint32_t)n);  /* sar_width */
            n264_bs_write(bs, 16, (uint32_t)d);  /* sar_height */
        }
    }
    n264_bs_write1(bs, 0);                       /* overscan_info_present */
    n264_bs_write1(bs, 0);                       /* video_signal_type_present */
    n264_bs_write1(bs, 0);                       /* chroma_loc_info_present */
    n264_bs_write1(bs, sps->vui_timing ? 1 : 0); /* timing_info_present */
    if (sps->vui_timing) {
        n264_bs_write(bs, 32, (uint32_t)sps->num_units_in_tick);
        n264_bs_write(bs, 32, (uint32_t)sps->time_scale);
        n264_bs_write1(bs, 1);                   /* fixed_frame_rate_flag */
    }
    n264_bs_write1(bs, 0);                       /* nal_hrd_parameters_present */
    n264_bs_write1(bs, 0);                       /* vcl_hrd_parameters_present */
    n264_bs_write1(bs, 0);                       /* pic_struct_present */
    n264_bs_write1(bs, 1);                       /* bitstream_restriction_flag */
    n264_bs_write1(bs, 1);                       /* motion_vectors_over_pic_boundaries */
    n264_bs_write_ue(bs, 0);                     /* max_bytes_per_pic_denom */
    n264_bs_write_ue(bs, 0);                     /* max_bits_per_mb_denom */
    n264_bs_write_ue(bs, 16);                    /* log2_max_mv_length_horizontal */
    n264_bs_write_ue(bs, 16);                    /* log2_max_mv_length_vertical */
    n264_bs_write_ue(bs, sps->max_num_reorder_frames);
    n264_bs_write_ue(bs, sps->max_dec_frame_buffering);
    n264_bs_rbsp_trailing(bs);
}

void n264_pps_write(n264_bs_t *bs, const n264_pps_t *pps)
{
    n264_bs_write_ue(bs, pps->pps_id);
    n264_bs_write_ue(bs, pps->sps_id);
    n264_bs_write1(bs, pps->entropy_coding_mode_flag);
    n264_bs_write1(bs, 0);                       /* bottom_field_pic_order_present */
    n264_bs_write_ue(bs, 0);                     /* num_slice_groups_minus1 */
    n264_bs_write_ue(bs, pps->num_ref_idx_l0_default_active_minus1);
    n264_bs_write_ue(bs, pps->num_ref_idx_l1_default_active_minus1);
    n264_bs_write1(bs, pps->weighted_pred_flag);
    n264_bs_write(bs, 2, pps->weighted_bipred_idc);
    n264_bs_write_se(bs, pps->pic_init_qp_minus26);
    n264_bs_write_se(bs, 0);                      /* pic_init_qs_minus26 */
    n264_bs_write_se(bs, pps->chroma_qp_index_offset);
    n264_bs_write1(bs, pps->deblocking_filter_control_present_flag);
    n264_bs_write1(bs, pps->constrained_intra_pred_flag);
    n264_bs_write1(bs, 0);                       /* redundant_pic_cnt_present_flag */
    /* The "more RBSP data" tail (transform_8x8_mode_flag onward) is only present
 * when we enable the 8x8 transform; otherwise it is omitted, which is legal
 * and defaults transform_8x8_mode_flag to 0. */
    if (pps->transform_8x8_mode_flag) {
        n264_bs_write1(bs, 1);                   /* transform_8x8_mode_flag */
        n264_bs_write1(bs, 0);                   /* pic_scaling_matrix_present_flag */
        n264_bs_write_se(bs, pps->chroma_qp_index_offset); /* second_chroma_qp_index_offset */
    }
    n264_bs_rbsp_trailing(bs);
}
