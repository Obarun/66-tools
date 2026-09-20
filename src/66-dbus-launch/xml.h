/*
 * xml.h
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

#ifndef DBS_XML_H_INCLUDE
#define DBS_XML_H_INCLUDE

#include <stddef.h>

typedef enum xml_error_code_e xml_error_code_t ;
enum xml_error_code_e
{
	XML_ERROR_NONE = 0,
	XML_ERROR_READ,
	XML_ERROR_NOMEM,
	XML_ERROR_HANDLER,
	XML_ERROR_NUL,
	XML_ERROR_BOM,
	XML_ERROR_CHAR,
	XML_ERROR_UTF8,
	XML_ERROR_DECLARATION,
	XML_ERROR_ENCODING,
	XML_ERROR_DOCTYPE,
	XML_ERROR_DOCTYPE_SUBSET,
	XML_ERROR_PI,
	XML_ERROR_COMMENT,
	XML_ERROR_CDATA,
	XML_ERROR_ENTITY,
	XML_ERROR_NAME,
	XML_ERROR_ATTRIBUTE,
	XML_ERROR_DUPLICATE,
	XML_ERROR_MISMATCH,
	XML_ERROR_TEXT,
	XML_ERROR_ROOT,
	XML_ERROR_SYNTAX,
	XML_ERROR_EOF
} ;

typedef struct xml_error_s xml_error, *xml_error_ref ;
struct xml_error_s
{
	xml_error_code_t code ;
	size_t line ;
} ;

typedef struct xml_attribute_s xml_attribute, *xml_attribute_ref ;
struct xml_attribute_s
{
	char const *name ;
	char const *value ;
} ;

typedef int (*xml_open_t)(void *userdata, char const *name, xml_attribute const *attributes, size_t nattributes, size_t line) ;
typedef int (*xml_text_t)(void *userdata, char const *text, size_t len, size_t line) ;
typedef int (*xml_close_t)(void *userdata, char const *name, size_t line) ;

typedef struct xml_handler_s xml_handler, *xml_handler_ref ;
struct xml_handler_s
{
	xml_open_t open ;
	xml_text_t text ;
	xml_close_t close ;
} ;

extern char const *xml_error_str(xml_error_code_t code) ;
extern int xml_parse(char const *name, char const *s, size_t len, xml_handler const *handler, void *userdata, xml_error *err) ;
extern int xml_parse_file(char const *path, xml_handler const *handler, void *userdata, xml_error *err) ;

#endif
