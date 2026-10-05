/*
 * assets.h
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

#ifndef WEB66_ASSETS_H
#define WEB66_ASSETS_H

#include <stddef.h>

typedef struct asset_s asset_t ;
struct asset_s {
	char const *path ;
	char const *bytes ;
	size_t      len ;
	char const *mime ;
} ;

/**
 * @brief Look up an embedded asset by HTTP request path.
 *
 * Performs exact-match lookup on the asset table. The returned pointer
 * is valid for the lifetime of the process.
 *
 * @param[in]  path Null-terminated request path (e.g. "/", "/index.html").
 * @param[out] out  On success, filled with a pointer to the matching entry.
 * @return 1 if a matching asset is found, 0 otherwise.
 */
extern int asset_lookup(char const *path, asset_t const **out) ;

#endif
