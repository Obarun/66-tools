/*
 * common.c
 *
 * Copyright (c) 2026 Eric Vidal <eric@obarun.org>
 *
 * All rights reserved.
 *
 * This file is part of Obarun. It is subject to the license terms in
 * the LICENSE file found in the top-level directory of this
 * distribution.
 * This file may not be copied, modified, propagated, or distributed
 * except according to the terms contained in the LICENSE file.
 */

#include <oblibs/environ.h>
#include <oblibs/strbuf.h>
#include <oblibs/types.h>

#include "common.h"

int sf_str(strbuf *sb, char const *key, char const *val)
{
    if (!val || !val[0])
        return 1 ;

    return environ_untrim_kv(sb, key, val) ;
}

int sf_u64(strbuf *sb, char const *key, uint64_t v)
{
    char b[U64_FMT] ;
    b[u64_fmt(b, v)] = 0 ;

    return environ_untrim_kv(sb, key, b) ;
}
