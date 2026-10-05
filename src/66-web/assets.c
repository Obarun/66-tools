/*
 * assets.c
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

#include <stddef.h>
#include <string.h>

#include <oblibs/string.h>

#include "assets.h"

extern unsigned char index_html[] ; extern unsigned int index_html_len ;
extern unsigned char style_css[] ; extern unsigned int style_css_len  ;
extern unsigned char app_js[] ; extern unsigned int app_js_len     ;

#define HTML_MIME "text/html; charset=utf-8"
#define CSS_MIME "text/css; charset=utf-8"
#define JS_MIME "application/javascript; charset=utf-8"

typedef struct asset_entry_s asset_entry_t ;
struct asset_entry_s {
	char const *path ;
	unsigned char *bytes ;
	unsigned int *len ;
	char const *mime ;
} ;

static asset_entry_t const TABLE[] = {
	{ "/", index_html, &index_html_len, HTML_MIME },
	{ "/index.html", index_html, &index_html_len, HTML_MIME },
	{ "/style.css", style_css, &style_css_len, CSS_MIME  },
	{ "/app.js", app_js, &app_js_len, JS_MIME   },
} ;

#define NTABLE nb_el(TABLE)

int asset_lookup(char const *path, asset_t const **out)
{
	if (!path || !out)
		return 0 ;

	static asset_t hit ;
	for (size_t i = 0 ; i < NTABLE ; i++) {

		if (!strcmp(path, TABLE[i].path)) {

			hit.path = TABLE[i].path ;
			hit.bytes = (char const *)TABLE[i].bytes ;
			hit.len = (size_t)(*TABLE[i].len) ;
			hit.mime = TABLE[i].mime ;
			*out = &hit ;
			return 1 ;
		}
	}
	return 0 ;
}
