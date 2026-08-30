/*
 * ledger.c - op-count ledger storage + exit dump (see ledger.h). Empty TU
 * unless built with -DY264_OP_LEDGER.
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "ledger.h"

#ifdef Y264_OP_LEDGER
#include <stdio.h>

y264_led_t y264_led;
_Thread_local int y264_led_site;

static const char *const g_site[Y264_LED_SITE_N] = {
    "other", "lowres", "p_me", "p_intra", "b_me", "b_intra", "iframe"
};

__attribute__((destructor)) static void y264_led_dump(void)
{
    if (!y264_led.sad_call && !y264_led.satd_call)
        return;
    y264_led_t *L = &y264_led;
    for (int i = 0; i < Y264_LED_SITE_N; i++)
        fprintf(stderr, "Y264LED satdsite_%s %llu\nY264LED satdcall_%s %llu\n",
                g_site[i], (unsigned long long)L->satd_site_pix[i],
                g_site[i], (unsigned long long)L->satd_site_call[i]);
    fprintf(stderr, "Y264LED mb_p %llu\nY264LED mb_p_early %llu\n"
                    "Y264LED mb_b %llu\nY264LED mb_b_early %llu\n"
                    "Y264LED mb_p_skip %llu\nY264LED mb_b_skip %llu\n"
                    "Y264LED intra_admit_try %llu\nY264LED intra_admit_hit %llu\n",
            (unsigned long long)L->mb_p, (unsigned long long)L->mb_p_early,
            (unsigned long long)L->mb_b, (unsigned long long)L->mb_b_early,
            (unsigned long long)L->mb_p_skip, (unsigned long long)L->mb_b_skip,
            (unsigned long long)L->intra_admit_try,
            (unsigned long long)L->intra_admit_hit);
#define D(f) fprintf(stderr, "Y264LED %s %llu\n", #f, (unsigned long long)L->f)
    D(intra_admit_try_b); D(intra_admit_hit_b);
    D(mb_b_try); D(mb_b_conf);
    D(sad_call); D(sad_pix); D(satd_call); D(satd_pix); D(sa8d_call); D(sa8d_pix);
    D(ssd_call); D(ssd_pix); D(var_pix); D(hac_call);
    D(mc_luma_call); D(mc_luma_pix); D(getref_ptr); D(getref_build); D(getref_pix);
    D(mc_chroma_call); D(mc_chroma_pix); D(avg_call); D(avg_pix); D(hpel_call);
    D(q4_blk); D(q8_blk); D(qdc_blk); D(dq4_blk); D(dq8_blk);
    D(dct4_blk); D(dct8_blk); D(idct4_blk); D(idct8_blk); D(idctdc_blk); D(dcxf_blk);
    D(bin_real); D(bin_bypass); D(bin_term); D(bin_est);
    D(trellis4); D(trellis8); D(trellisdc); D(rd_mb); D(rd_part);
    D(probe_int); D(probe_sub); D(cavlc_scratch); D(est_mb);
#undef D
}
#endif
