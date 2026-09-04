/*
 * set.c - SPS/PPS serialization per ITU-T H.264 sections 7.3.2.1 / 7.3.2.2
 * Copyright (c) 2026, the yah264 authors
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
static void write_scaling_list(y264_bs_t *bs, const uint8_t *w,
                               const int *scan, int size)
{
    int last = 8;
    for (int j = 0; j < size; j++) {
        int cur = w[scan[j]];
        y264_bs_write_se(bs, cur - last);            /* delta_scale */
        last = cur;
    }
}

int y264_profile_idc(int entropy_coding_mode_flag, int bframes)
{
    return (entropy_coding_mode_flag || bframes > 0) ? 77 : 66;
}

void y264_sps_write(y264_bs_t *bs, const y264_sps_t *sps)
{
    /* CABAC is forbidden in Baseline; force Main if it's on. B-frames are
 * already reflected in profile_idc by the encoder. */
    int profile_idc = sps->profile_idc;
    if (sps->entropy_coding_mode_flag && profile_idc < 77)
        profile_idc = 77;
    y264_bs_write(bs, 8, profile_idc);
    /* constraint_set0..5_flag + 2 reserved zero bits. Baseline advertises
 * constraint_set0_flag; Main leaves all constraints 0. */
    y264_bs_write(bs, 8, profile_idc == 66 ? 0x80 : 0x00);
    y264_bs_write(bs, 8, sps->level_idc);
    y264_bs_write_ue(bs, sps->sps_id);

    /* profile_idc >= 100 (High and above) carries chroma_format_idc, bit depths,
 * and the scaling-list flag. profile_idc < 100 omits the whole block (and is
 * only reachable at 4:2:0 8-bit flat quant). 4:4:4 adds a
 * separate_colour_plane_flag bit after the format. */
    if (profile_idc >= 100) {
        y264_bs_write_ue(bs, sps->chroma_format_idc);
        if (sps->chroma_format_idc == 3)
            y264_bs_write1(bs, 0);                   /* separate_colour_plane_flag: interleaved */
        y264_bs_write_ue(bs, Y264_BIT_DEPTH - 8);    /* bit_depth_luma_minus8 */
        y264_bs_write_ue(bs, Y264_BIT_DEPTH - 8);    /* bit_depth_chroma_minus8 */
        y264_bs_write1(bs, 0);                       /* qpprime_y_zero_transform_bypass */
        y264_bs_write1(bs, sps->cqm != NULL);        /* seq_scaling_matrix_present_flag */
        if (sps->cqm) {
            /* The list count is the spec's loop bound: 8 at 4:2:0/4:2:2
 * (0-5 = 4x4 Intra/Inter Y/Cb/Cr, 6-7 = 8x8 Intra/Inter Y) and 12 at
 * 4:4:4, where the 8x8 set expands to Intra/Inter x Y/Cb/Cr
 * (Table 7-2). Writing 8 lists at chroma_format_idc 3 desyncs the
 * whole SPS parse -- found by regress.py cell (--cqm jvt, 4:4:4):
 * the stream was undecodable and the 518-case conformance set never
 * reached the combination. Chroma shares the luma matrix throughout,
 * matching the encoder's dequant, so the extra four lists repeat the
 * luma pair in spec order. */
            const uint8_t *w4[6] = { sps->cqm->w4[0], sps->cqm->w4[0], sps->cqm->w4[0],
                                     sps->cqm->w4[1], sps->cqm->w4[1], sps->cqm->w4[1] };
            for (int i = 0; i < 6; i++) {
                y264_bs_write1(bs, 1);               /* scaling_list_present_flag */
                write_scaling_list(bs, w4[i], SCAN4, 16);
            }
            int n8 = sps->chroma_format_idc == 3 ? 6 : 2;
            for (int i = 0; i < n8; i++) {
                y264_bs_write1(bs, 1);
                write_scaling_list(bs, sps->cqm->w8[i & 1], SCAN8, 64);
            }
        }
    }

    y264_bs_write_ue(bs, sps->log2_max_frame_num_minus4);
    y264_bs_write_ue(bs, sps->pic_order_cnt_type);
    if (sps->pic_order_cnt_type == 0)
        y264_bs_write_ue(bs, sps->log2_max_pic_order_cnt_lsb_minus4);
    /* pic_order_cnt_type == 2 needs no further POC syntax. */

    y264_bs_write_ue(bs, sps->max_num_ref_frames);
    y264_bs_write1(bs, 0);                       /* gaps_in_frame_num_value_allowed */

    y264_bs_write_ue(bs, sps->width_in_mbs - 1);
    y264_bs_write_ue(bs, sps->height_in_map_units - 1);

    y264_bs_write1(bs, sps->frame_mbs_only_flag);
    /* frame_mbs_only_flag == 1, so no mb_adaptive_frame_field_flag. */
    y264_bs_write1(bs, sps->direct_8x8_inference_flag);

    int crop = sps->crop_left || sps->crop_right || sps->crop_top || sps->crop_bottom;
    y264_bs_write1(bs, crop);
    if (crop) {
        y264_bs_write_ue(bs, sps->crop_left);
        y264_bs_write_ue(bs, sps->crop_right);
        y264_bs_write_ue(bs, sps->crop_top);
        y264_bs_write_ue(bs, sps->crop_bottom);
    }

    /* VUI with only the bitstream restriction: max_num_reorder_frames tells
 * the decoder the exact output delay. Without it, ffmpeg grows its
 * reorder depth adaptively and silently drops a frame the first time a
 * deeper mini-GOP appears after shallower ones (variable B runs). */
    y264_bs_write1(bs, 1);                       /* vui_parameters_present_flag */
    y264_bs_write1(bs, sps->sar_num > 0 && sps->sar_den > 0 ? 1 : 0); /* aspect_ratio_info_present */
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
        y264_bs_write(bs, 8, (uint32_t)idc);     /* aspect_ratio_idc */
        if (idc == 255) {
            y264_bs_write(bs, 16, (uint32_t)n);  /* sar_width */
            y264_bs_write(bs, 16, (uint32_t)d);  /* sar_height */
        }
    }
    y264_bs_write1(bs, 0);                       /* overscan_info_present */
    y264_bs_write1(bs, sps->vs_present ? 1 : 0); /* video_signal_type_present */
    if (sps->vs_present) {
        int cd = sps->vs_primaries != 2 || sps->vs_transfer != 2 || sps->vs_matrix != 2;
        y264_bs_write(bs, 3, 5);                 /* video_format: unspecified */
        y264_bs_write1(bs, sps->vs_full_range ? 1 : 0);
        y264_bs_write1(bs, cd);                  /* colour_description_present */
        if (cd) {
            y264_bs_write(bs, 8, (uint32_t)sps->vs_primaries);
            y264_bs_write(bs, 8, (uint32_t)sps->vs_transfer);
            y264_bs_write(bs, 8, (uint32_t)sps->vs_matrix);
        }
    }
    y264_bs_write1(bs, sps->chroma_loc >= 0 ? 1 : 0); /* chroma_loc_info_present */
    if (sps->chroma_loc >= 0) {
        y264_bs_write_ue(bs, (uint32_t)sps->chroma_loc);   /* top field */
        y264_bs_write_ue(bs, (uint32_t)sps->chroma_loc);   /* bottom field */
    }
    y264_bs_write1(bs, sps->vui_timing ? 1 : 0); /* timing_info_present */
    if (sps->vui_timing) {
        y264_bs_write(bs, 32, (uint32_t)sps->num_units_in_tick);
        y264_bs_write(bs, 32, (uint32_t)sps->time_scale);
        y264_bs_write1(bs, 1);                   /* fixed_frame_rate_flag */
    }
    y264_bs_write1(bs, 0);                       /* nal_hrd_parameters_present */
    y264_bs_write1(bs, 0);                       /* vcl_hrd_parameters_present */
    y264_bs_write1(bs, 0);                       /* pic_struct_present */
    y264_bs_write1(bs, 1);                       /* bitstream_restriction_flag */
    y264_bs_write1(bs, 1);                       /* motion_vectors_over_pic_boundaries */
    y264_bs_write_ue(bs, 0);                     /* max_bytes_per_pic_denom */
    y264_bs_write_ue(bs, 0);                     /* max_bits_per_mb_denom */
    y264_bs_write_ue(bs, 16);                    /* log2_max_mv_length_horizontal */
    y264_bs_write_ue(bs, 16);                    /* log2_max_mv_length_vertical */
    y264_bs_write_ue(bs, sps->max_num_reorder_frames);
    y264_bs_write_ue(bs, sps->max_dec_frame_buffering);
    y264_bs_rbsp_trailing(bs);
}

void y264_pps_write(y264_bs_t *bs, const y264_pps_t *pps)
{
    y264_bs_write_ue(bs, pps->pps_id);
    y264_bs_write_ue(bs, pps->sps_id);
    y264_bs_write1(bs, pps->entropy_coding_mode_flag);
    y264_bs_write1(bs, 0);                       /* bottom_field_pic_order_present */
    y264_bs_write_ue(bs, 0);                     /* num_slice_groups_minus1 */
    y264_bs_write_ue(bs, pps->num_ref_idx_l0_default_active_minus1);
    y264_bs_write_ue(bs, pps->num_ref_idx_l1_default_active_minus1);
    y264_bs_write1(bs, pps->weighted_pred_flag);
    y264_bs_write(bs, 2, pps->weighted_bipred_idc);
    y264_bs_write_se(bs, pps->pic_init_qp_minus26);
    y264_bs_write_se(bs, 0);                      /* pic_init_qs_minus26 */
    y264_bs_write_se(bs, pps->chroma_qp_index_offset);
    y264_bs_write1(bs, pps->deblocking_filter_control_present_flag);
    y264_bs_write1(bs, pps->constrained_intra_pred_flag);
    y264_bs_write1(bs, 0);                       /* redundant_pic_cnt_present_flag */
    /* The "more RBSP data" tail (transform_8x8_mode_flag onward) is only present
 * when we enable the 8x8 transform; otherwise it is omitted, which is legal
 * and defaults transform_8x8_mode_flag to 0. */
    if (pps->transform_8x8_mode_flag) {
        y264_bs_write1(bs, 1);                   /* transform_8x8_mode_flag */
        y264_bs_write1(bs, 0);                   /* pic_scaling_matrix_present_flag */
        y264_bs_write_se(bs, pps->chroma_qp_index_offset); /* second_chroma_qp_index_offset */
    }
    y264_bs_rbsp_trailing(bs);
}
