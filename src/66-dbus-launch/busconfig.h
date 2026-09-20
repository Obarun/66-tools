/*
 * busconfig.h
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

#ifndef DBS_BUSCONFIG_H_INCLUDE
#define DBS_BUSCONFIG_H_INCLUDE

#include <stddef.h>

#include "xml.h"

typedef enum busconfig_error_code_e busconfig_error_code_t ;
enum busconfig_error_code_e
{
	BUSCONFIG_ERROR_NONE = 0,
	BUSCONFIG_ERROR_XML,
	BUSCONFIG_ERROR_NOMEM,
	BUSCONFIG_ERROR_HANDLER,
	BUSCONFIG_ERROR_PLACEMENT,
	BUSCONFIG_ERROR_ATTRIBUTE,
	BUSCONFIG_ERROR_VALUE,
	BUSCONFIG_ERROR_MISSING
} ;

typedef struct busconfig_error_s busconfig_error, *busconfig_error_ref ;
struct busconfig_error_s
{
	busconfig_error_code_t code ;
	xml_error xml ;
} ;

extern char const *busconfig_error_str(busconfig_error_code_t code) ;
extern int busconfig_parse(char const *name, char const *s, size_t len, xml_handler const *handler, void *userdata, busconfig_error *err) ;
extern int busconfig_parse_file(char const *path, xml_handler const *handler, void *userdata, busconfig_error *err) ;

#endif
