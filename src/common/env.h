/*
 * env.h - the one place the library reads its environment
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef YAH264_ENV_H
#define YAH264_ENV_H

#include <stdlib.h>
#include <string.h>

/* The Y264_* tuning and measurement knobs are read only in a build configured
 * with -Ddev_knobs=true, so a program linking a release library can't have
 * its output changed by a stray environment variable. The exception is the
 * short list below: the CLI carries some of its options to the library
 * through these until they become parameters, so they are read in every
 * build. */
static inline const char *y264_env(const char *name)
{
#if Y264_DEV_KNOBS
    return getenv(name);
#else
    static const char *const cli_backed[] = {
        "Y264_NR_INTRA", "Y264_NR_INTER", "Y264_MCSTF", "Y264_MCSTF_RANGE",
        "Y264_UMH_RANGE", "Y264_AQ_MODE", "Y264_MBTREE_OFF", "Y264_DCTDEC",
        "Y264_FAST_PSKIP", "YAH264_NO_ASM", "Y264_TP_IPF", "Y264_TP_PBF",
        "Y264_TP_CPLXBLUR", "Y264_ABR_QCOMP", "Y264_DZ_INTER", "Y264_DZ_INTRA",
    };
    for (size_t i = 0; i < sizeof cli_backed / sizeof *cli_backed; i++)
        if (!strcmp(name, cli_backed[i]))
            return getenv(name);
    return NULL;
#endif
}

#endif
