/*
 * xml.c
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
#include <stdint.h>
#include <string.h>
#include <strings.h>

#include <oblibs/genbuf.h>
#include <oblibs/log.h>
#include <oblibs/sbl.h>
#include <oblibs/strbuf.h>

#include "xml.h"

#define XML_BOM_UTF8 "\xef\xbb\xbf"
#define XML_BOM_UTF16_BE "\xfe\xff"
#define XML_BOM_UTF16_LE "\xff\xfe"
#define XML_BOM_UTF32_BE "\0\0\xfe\xff"
#define XML_CRLF "\r\n"
#define XML_LINE_FIRST 1
#define XML_ASCII_MAX 0x7f
#define XML_UTF8_CONTINUATION_MASK 0xc0
#define XML_UTF8_CONTINUATION 0x80
#define XML_UTF8_PAYLOAD_BITS 6
#define XML_UTF8_LEAD_WIDTH 1
#define XML_UTF8_SHORTEST_SEQUENCE 2
#define XML_UTF8_2_BYTES_MASK 0xe0
#define XML_UTF8_2_BYTES_LEAD 0xc0
#define XML_UTF8_2_BYTES_MIN 0x80
#define XML_UTF8_3_BYTES_MASK 0xf0
#define XML_UTF8_3_BYTES_LEAD 0xe0
#define XML_UTF8_3_BYTES_MIN 0x800
#define XML_UTF8_4_BYTES_MASK 0xf8
#define XML_UTF8_4_BYTES_LEAD 0xf0
#define XML_UTF8_4_BYTES_MIN 0x10000
#define XML_SURROGATE_MIN 0xd800
#define XML_SURROGATE_MAX 0xdfff
#define XML_CHAR_BMP_MAX 0xfffd
#define XML_CHAR_SUPPLEMENTARY_MIN 0x10000
#define XML_CHAR_MAX 0x10ffff
#define XML_SPACES " \t\n"
#define XML_PUBID_CHARS "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 \n-'()+,./:=?;!*#@$_%"
#define XML_DECLARATION_TARGET "xml"
#define XML_DOCTYPE_KEYWORD "DOCTYPE"
#define XML_DOCTYPE_PUBLIC "PUBLIC"
#define XML_DOCTYPE_SYSTEM "SYSTEM"
#define XML_DOCTYPE_PUBLIC_LITERALS 2
#define XML_DOCTYPE_SYSTEM_LITERALS 1
#define XML_VERSION "1.0"
#define XML_ENCODING "UTF-8"
#define XML_DECLARATION_VERSION "version"
#define XML_DECLARATION_ENCODING "encoding"
#define XML_DECLARATION_STANDALONE "standalone"
#define XML_STANDALONE_YES "yes"
#define XML_STANDALONE_NO "no"
#define XML_DECLARATION_VERSION_RANK 1
#define XML_DECLARATION_ENCODING_RANK 2
#define XML_STATE_BITS 7
#define XML_STATE_MASK ((UINT32_C(1) << XML_STATE_BITS) - 1)
#define XML_A_MARK (UINT32_C(1) << XML_ACTION_MARK)
#define XML_A_MARK_CLOSE (UINT32_C(1) << XML_ACTION_MARK_CLOSE)
#define XML_A_TEXT (UINT32_C(1) << XML_ACTION_TEXT)
#define XML_A_STAG_BEGIN (UINT32_C(1) << XML_ACTION_STAG_BEGIN)
#define XML_A_SCRATCH_RESET (UINT32_C(1) << XML_ACTION_SCRATCH_RESET)
#define XML_A_SCRATCH_CAT (UINT32_C(1) << XML_ACTION_SCRATCH_CAT)
#define XML_A_STAG_NAME_END (UINT32_C(1) << XML_ACTION_STAG_NAME_END)
#define XML_A_ATT_CAT (UINT32_C(1) << XML_ACTION_ATT_CAT)
#define XML_A_ATT_NAME_END (UINT32_C(1) << XML_ACTION_ATT_NAME_END)
#define XML_A_ATT_VALUE_END (UINT32_C(1) << XML_ACTION_ATT_VALUE_END)
#define XML_A_DECL_VALUE (UINT32_C(1) << XML_ACTION_DECL_VALUE)
#define XML_A_STAG_END (UINT32_C(1) << XML_ACTION_STAG_END)
#define XML_A_ETAG_END (UINT32_C(1) << XML_ACTION_ETAG_END)
#define XML_A_CLOSE (UINT32_C(1) << XML_ACTION_CLOSE)
#define XML_A_DOCTYPE_BEGIN (UINT32_C(1) << XML_ACTION_DOCTYPE_BEGIN)
#define XML_A_DT_KEYWORD (UINT32_C(1) << XML_ACTION_DT_KEYWORD)
#define XML_A_DT_LIT_BEGIN (UINT32_C(1) << XML_ACTION_DT_LIT_BEGIN)
#define XML_A_DT_LIT_END (UINT32_C(1) << XML_ACTION_DT_LIT_END)
#define XML_A_DT_END (UINT32_C(1) << XML_ACTION_DT_END)
#define XML_A_DECL_BEGIN (UINT32_C(1) << XML_ACTION_DECL_BEGIN)
#define XML_A_DECL_END (UINT32_C(1) << XML_ACTION_DECL_END)
#define XML_A_FINISH (UINT32_C(1) << XML_ACTION_FINISH)

enum xml_action_e
{
	XML_ACTION_MARK = XML_STATE_BITS,
	XML_ACTION_MARK_CLOSE,
	XML_ACTION_TEXT,
	XML_ACTION_STAG_BEGIN,
	XML_ACTION_SCRATCH_RESET,
	XML_ACTION_SCRATCH_CAT,
	XML_ACTION_STAG_NAME_END,
	XML_ACTION_ATT_CAT,
	XML_ACTION_ATT_NAME_END,
	XML_ACTION_ATT_VALUE_END,
	XML_ACTION_DECL_VALUE,
	XML_ACTION_STAG_END,
	XML_ACTION_ETAG_END,
	XML_ACTION_CLOSE,
	XML_ACTION_DOCTYPE_BEGIN,
	XML_ACTION_DT_KEYWORD,
	XML_ACTION_DT_LIT_BEGIN,
	XML_ACTION_DT_LIT_END,
	XML_ACTION_DT_END,
	XML_ACTION_DECL_BEGIN,
	XML_ACTION_DECL_END,
	XML_ACTION_FINISH
} ;

enum xml_state_e
{
	XML_S_CONTENT = 0,
	XML_S_CONTENT_RB,
	XML_S_CONTENT_RBRB,
	XML_S_LT,
	XML_S_BANG,
	XML_S_COMMENT_OPEN,
	XML_S_COMMENT,
	XML_S_COMMENT_DASH,
	XML_S_COMMENT_DASH2,
	XML_S_DECL_KW,
	XML_S_DT_S,
	XML_S_DT_NAME,
	XML_S_DT_S2,
	XML_S_DT_KW,
	XML_S_DT_DQ,
	XML_S_DT_SQ,
	XML_S_DT_LIT_AFTER,
	XML_S_PI_START,
	XML_S_PI_X,
	XML_S_PI_XM,
	XML_S_PI_XML,
	XML_S_PI_TARGET,
	XML_S_PI_TARGET_QM,
	XML_S_PI_BODY,
	XML_S_PI_QM,
	XML_S_DECL_S,
	XML_S_DECL_NAME,
	XML_S_DECL_EQ,
	XML_S_DECL_VS,
	XML_S_DECL_DQ,
	XML_S_DECL_SQ,
	XML_S_DECL_AFTER,
	XML_S_DECL_QM,
	XML_S_TAG_NAME,
	XML_S_TAG_S,
	XML_S_TAG_AFTER,
	XML_S_ATT_NAME,
	XML_S_ATT_EQ,
	XML_S_ATT_VS,
	XML_S_ATT_DQ,
	XML_S_ATT_SQ,
	XML_S_EMPTY,
	XML_S_ETAG_START,
	XML_S_ETAG_NAME,
	XML_S_ETAG_S,
	XML_S_DONE
} ;

enum xml_class_e
{
	XML_C_END = 0,
	XML_C_SPACE,
	XML_C_LT,
	XML_C_GT,
	XML_C_AMP,
	XML_C_BANG,
	XML_C_QUEST,
	XML_C_SLASH,
	XML_C_EQ,
	XML_C_DQ,
	XML_C_SQ,
	XML_C_DASH,
	XML_C_LBRACK,
	XML_C_RBRACK,
	XML_C_X,
	XML_C_M,
	XML_C_L,
	XML_C_START,
	XML_C_NAME,
	XML_C_OTHER,
	XML_C_ENDOFKEY
} ;

typedef struct xml_utf8_form_s xml_utf8_form, *xml_utf8_form_ref ;
struct xml_utf8_form_s
{
	unsigned char mask ;
	unsigned char lead ;
	uint32_t min ;
} ;

static xml_utf8_form const xml_utf8_forms[] = {
	{ .mask = XML_UTF8_2_BYTES_MASK, .lead = XML_UTF8_2_BYTES_LEAD, .min = XML_UTF8_2_BYTES_MIN },
	{ .mask = XML_UTF8_3_BYTES_MASK, .lead = XML_UTF8_3_BYTES_LEAD, .min = XML_UTF8_3_BYTES_MIN },
	{ .mask = XML_UTF8_4_BYTES_MASK, .lead = XML_UTF8_4_BYTES_LEAD, .min = XML_UTF8_4_BYTES_MIN }
} ;

typedef struct xml_element_s xml_element, *xml_element_ref ;
struct xml_element_s
{
	size_t name ;
	size_t text ;
} ;

typedef struct xml_parser_s xml_parser, *xml_parser_ref ;
struct xml_parser_s
{
	size_t line ;
	size_t open ;
	size_t close ;
	xml_handler const *handler ;
	void *userdata ;
	xml_error *err ;
	strbuf names ;
	strbuf text ;
	strbuf scratch ;
	strbuf attrnames ;
	strbuf attrvalues ;
	genbuf attrs ;
	genbuf stack ;
	xml_element element ;
	size_t value ;
	size_t dt_required ;
	size_t dt_count ;
	unsigned int declaration ;
	unsigned int doctype ;
	unsigned int root ;
} ;

static char const *const xml_error_msg[] = {
	[XML_ERROR_NONE] = "no error",
	[XML_ERROR_READ] = "unable to read the file",
	[XML_ERROR_NOMEM] = "out of memory",
	[XML_ERROR_HANDLER] = "rejected by the handler",
	[XML_ERROR_NUL] = "NUL byte",
	[XML_ERROR_BOM] = "byte order mark of an encoding other than UTF-8",
	[XML_ERROR_CHAR] = "control character",
	[XML_ERROR_UTF8] = "invalid UTF-8 sequence",
	[XML_ERROR_DECLARATION] = "invalid, repeated or misplaced XML declaration",
	[XML_ERROR_ENCODING] = "declared encoding other than UTF-8",
	[XML_ERROR_DOCTYPE] = "invalid document type declaration",
	[XML_ERROR_DOCTYPE_SUBSET] = "internal subset in the document type declaration",
	[XML_ERROR_PI] = "invalid processing instruction",
	[XML_ERROR_COMMENT] = "double hyphen inside a comment",
	[XML_ERROR_CDATA] = "CDATA section",
	[XML_ERROR_ENTITY] = "entity or character reference",
	[XML_ERROR_NAME] = "invalid name",
	[XML_ERROR_ATTRIBUTE] = "attribute without a quoted value",
	[XML_ERROR_DUPLICATE] = "duplicate attribute",
	[XML_ERROR_MISMATCH] = "closing tag does not match the open element",
	[XML_ERROR_TEXT] = "text outside the root element",
	[XML_ERROR_ROOT] = "missing or second root element",
	[XML_ERROR_SYNTAX] = "syntax error",
	[XML_ERROR_EOF] = "unexpected end of file"
} ;

char const *xml_error_str(xml_error_code_t code)
{
	return xml_error_msg[code] ;
}

static int xml_fail_at(xml_parser *p, xml_error_code_t code, size_t line)
{
	p->err->code = code ;
	p->err->line = line ;

	return 0 ;
}

static int xml_fail(xml_parser *p, xml_error_code_t code)
{
	return xml_fail_at(p, code, p->line) ;
}

static int xml_utf8(xml_parser *p, char const *s, size_t len, size_t *width)
{
	unsigned char c = (unsigned char)s[0] ;
	size_t nforms = sizeof(xml_utf8_forms) / sizeof(xml_utf8_forms[0]) ;

	for (size_t f = 0 ; f < nforms ; f++) {

		xml_utf8_form const *form = &xml_utf8_forms[f] ;
		uint32_t cp = c & (unsigned char)~form->mask ;
		size_t n = f + XML_UTF8_SHORTEST_SEQUENCE ;

		if ((c & form->mask) != form->lead)
			continue ;

		for (size_t i = XML_UTF8_LEAD_WIDTH ; i < n ; i++) {

			unsigned char b ;

			if (i == len)
				return xml_fail(p, XML_ERROR_UTF8) ;

			b = (unsigned char)s[i] ;

			if (!b)
				return xml_fail(p, XML_ERROR_NUL) ;

			if ((b & XML_UTF8_CONTINUATION_MASK) != XML_UTF8_CONTINUATION)
				return xml_fail(p, XML_ERROR_UTF8) ;

			cp = (cp << XML_UTF8_PAYLOAD_BITS) | (b & (unsigned char)~XML_UTF8_CONTINUATION_MASK) ;
		}

		if (cp < form->min || cp > XML_CHAR_MAX ||
			(cp >= XML_SURROGATE_MIN && cp <= XML_SURROGATE_MAX) ||
			(cp > XML_CHAR_BMP_MAX && cp < XML_CHAR_SUPPLEMENTARY_MIN))
				return xml_fail(p, XML_ERROR_UTF8) ;

		*width = n ;

		return 1 ;
	}

	return xml_fail(p, XML_ERROR_UTF8) ;
}

static int xml_normalize(xml_parser *p, strbuf *doc, char const *s, size_t len)
{
	size_t i = 0 ;

	if ((len >= sizeof(XML_BOM_UTF32_BE) - 1 && !memcmp(s, XML_BOM_UTF32_BE, sizeof(XML_BOM_UTF32_BE) - 1)) ||
		(len >= sizeof(XML_BOM_UTF16_BE) - 1 && !memcmp(s, XML_BOM_UTF16_BE, sizeof(XML_BOM_UTF16_BE) - 1)) ||
		(len >= sizeof(XML_BOM_UTF16_LE) - 1 && !memcmp(s, XML_BOM_UTF16_LE, sizeof(XML_BOM_UTF16_LE) - 1)))
			return xml_fail(p, XML_ERROR_BOM) ;

	if (len >= sizeof(XML_BOM_UTF8) - 1 && !memcmp(s, XML_BOM_UTF8, sizeof(XML_BOM_UTF8) - 1))
		i = sizeof(XML_BOM_UTF8) - 1 ;

	if (!strbuf_reserve(doc, len + 1))
		return xml_fail(p, XML_ERROR_NOMEM) ;

	while (i < len) {

		unsigned char c = (unsigned char)s[i] ;
		char out = (char)c ;
		size_t width = sizeof(out) ;

		if (!c)
			return xml_fail(p, XML_ERROR_NUL) ;

		if (c > XML_ASCII_MAX) {

			if (!xml_utf8(p, s + i, len - i, &width))
				return 0 ;

			if (!strbuf_catb(doc, s + i, width))
				return xml_fail(p, XML_ERROR_NOMEM) ;

			i += width ;

			continue ;
		}

		if (c < ' ' && c != '\t' && c != '\n' && c != '\r')
			return xml_fail(p, XML_ERROR_CHAR) ;

		if (c == '\r') {

			out = '\n' ;
			if (len - i >= sizeof(XML_CRLF) - 1 && !memcmp(s + i, XML_CRLF, sizeof(XML_CRLF) - 1))
				width = sizeof(XML_CRLF) - 1 ;
		}

		if (!strbuf_catb(doc, &out, sizeof(out)))
			return xml_fail(p, XML_ERROR_NOMEM) ;

		if (out == '\n')
			p->line++ ;

		i += width ;
	}

	p->line = XML_LINE_FIRST ;

	return 1 ;
}

static inline uint8_t xml_cclass(char c)
{
	switch (c) {

		case 0 : return XML_C_END ;
		case ' ' :
		case '\t' :
		case '\n' : return XML_C_SPACE ;
		case '<' : return XML_C_LT ;
		case '>' : return XML_C_GT ;
		case '&' : return XML_C_AMP ;
		case '!' : return XML_C_BANG ;
		case '?' : return XML_C_QUEST ;
		case '/' : return XML_C_SLASH ;
		case '=' : return XML_C_EQ ;
		case '"' : return XML_C_DQ ;
		case '\'' : return XML_C_SQ ;
		case '-' : return XML_C_DASH ;
		case '[' : return XML_C_LBRACK ;
		case ']' : return XML_C_RBRACK ;
		case 'x' :
		case 'X' : return XML_C_X ;
		case 'm' :
		case 'M' : return XML_C_M ;
		case 'l' :
		case 'L' : return XML_C_L ;
		case '_' :
		case ':' : return XML_C_START ;
		case '.' : return XML_C_NAME ;
		default :
			if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
				return XML_C_START ;
			if (c >= '0' && c <= '9')
				return XML_C_NAME ;
			return XML_C_OTHER ;
	}
}

static int xml_cat(xml_parser *p, strbuf *sb, char c)
{
	return strbuf_catb(sb, &c, sizeof(c)) || xml_fail(p, XML_ERROR_NOMEM) ;
}

static int xml_terminate(xml_parser *p, strbuf *sb)
{
	return strbuf_terminate(sb) || xml_fail(p, XML_ERROR_NOMEM) ;
}

static int xml_scratch_is(xml_parser const *p, char const *literal)
{
	return p->scratch.len == strlen(literal) && !memcmp(p->scratch.s, literal, p->scratch.len) ;
}

static size_t xml_depth(xml_parser const *p)
{
	return genbuf_len(xml_element, &p->stack) ;
}

static void xml_attributes_reset(xml_parser *p)
{
	p->attrnames.len = 0 ;
	p->attrvalues.len = 0 ;
}

static int xml_text(xml_parser *p, char c, uint8_t cls)
{
	if (!xml_depth(p))
		return cls == XML_C_SPACE || xml_fail(p, XML_ERROR_TEXT) ;

	return xml_cat(p, &p->text, c) ;
}

static int xml_stag_name_end(xml_parser *p)
{
	p->element.name = p->names.len ;
	p->element.text = p->text.len ;
	xml_attributes_reset(p) ;

	return sbl_addb(&p->names, p->scratch.s, p->scratch.len) || xml_fail(p, XML_ERROR_NOMEM) ;
}

static int xml_att_cat(xml_parser *p, char c, uint8_t cls)
{
	return xml_cat(p, &p->attrvalues, cls == XML_C_SPACE ? ' ' : c) ;
}

static int xml_att_name_end(xml_parser *p)
{
	if (!strbuf_uncounted(&p->scratch))
		return xml_fail(p, XML_ERROR_NOMEM) ;

	if (sbl_search(&p->attrnames, p->scratch.s) >= 0)
		return xml_fail(p, XML_ERROR_DUPLICATE) ;

	p->value = p->attrvalues.len ;

	return sbl_addb(&p->attrnames, p->scratch.s, p->scratch.len) || xml_fail(p, XML_ERROR_NOMEM) ;
}

static int xml_decl_value(xml_parser *p)
{
	char const *name = p->scratch.s, *value ;
	size_t rank = sbl_count(&p->attrnames) ;

	if (!xml_terminate(p, &p->attrvalues))
		return 0 ;

	value = p->attrvalues.s + p->value ;

	if (!strcmp(name, XML_DECLARATION_VERSION) && rank == XML_DECLARATION_VERSION_RANK)
		return !strcmp(value, XML_VERSION) || xml_fail(p, XML_ERROR_DECLARATION) ;

	if (!strcmp(name, XML_DECLARATION_ENCODING) && rank == XML_DECLARATION_ENCODING_RANK)
		return !strcasecmp(value, XML_ENCODING) || xml_fail(p, XML_ERROR_ENCODING) ;

	if (!strcmp(name, XML_DECLARATION_STANDALONE) && rank > XML_DECLARATION_VERSION_RANK)
		return !strcmp(value, XML_STANDALONE_YES) || !strcmp(value, XML_STANDALONE_NO) || xml_fail(p, XML_ERROR_DECLARATION) ;

	return xml_fail(p, XML_ERROR_DECLARATION) ;
}

static int xml_attributes_names(xml_parser *p)
{
	size_t pos = 0 ;

	p->attrs.len = 0 ;

	FOREACH_SBL(&p->attrnames, pos) {

		xml_attribute const a = { .name = p->attrnames.s + pos, .value = 0 } ;

		if (!genbuf_append(xml_attribute, &p->attrs, &a))
			return xml_fail(p, XML_ERROR_NOMEM) ;
	}

	return 1 ;
}

static void xml_attributes_values(xml_parser *p)
{
	size_t pos = 0, i = 0 ;

	FOREACH_SBL(&p->attrvalues, pos)
		genbuf_s(xml_attribute, &p->attrs)[i++].value = p->attrvalues.s + pos ;
}

static int xml_stag_end(xml_parser *p)
{
	if (!xml_attributes_names(p))
		return 0 ;

	xml_attributes_values(p) ;

	if (!genbuf_append(xml_element, &p->stack, &p->element))
		return xml_fail(p, XML_ERROR_NOMEM) ;

	p->root = 1 ;

	if (p->handler->open(p->userdata, p->names.s + p->element.name, genbuf_s(xml_attribute, &p->attrs), genbuf_len(xml_attribute, &p->attrs), p->open) != 1)
		return xml_fail_at(p, XML_ERROR_HANDLER, p->open) ;

	return 1 ;
}

static int xml_etag_end(xml_parser *p)
{
	size_t depth = xml_depth(p) ;

	if (!depth || !xml_scratch_is(p, p->names.s + genbuf_s(xml_element, &p->stack)[depth - 1].name))
		return xml_fail_at(p, XML_ERROR_MISMATCH, p->open) ;

	return 1 ;
}

static int xml_close(xml_parser *p)
{
	xml_element const e = genbuf_s(xml_element, &p->stack)[xml_depth(p) - 1] ;
	size_t start = e.text, end = p->text.len ;

	while (start < end && memchr(XML_SPACES, p->text.s[start], sizeof(XML_SPACES) - 1))
		start++ ;

	while (end > start && memchr(XML_SPACES, p->text.s[end - 1], sizeof(XML_SPACES) - 1))
		end-- ;

	if (end > start) {

		if (!strbuf_uncounted(&p->text))
			return xml_fail(p, XML_ERROR_NOMEM) ;

		p->text.s[end] = 0 ;

		if (p->handler->text(p->userdata, p->text.s + start, end - start, p->close) != 1)
			return xml_fail_at(p, XML_ERROR_HANDLER, p->close) ;
	}

	if (p->handler->close(p->userdata, p->names.s + e.name, p->close) != 1)
		return xml_fail_at(p, XML_ERROR_HANDLER, p->close) ;

	p->text.len = e.text ;
	p->names.len = e.name ;
	p->stack.len -= sizeof(xml_element) ;

	return 1 ;
}

static int xml_doctype_begin(xml_parser *p)
{
	if (!xml_scratch_is(p, XML_DOCTYPE_KEYWORD))
		return xml_fail(p, XML_ERROR_SYNTAX) ;

	if (p->doctype || p->root)
		return xml_fail(p, XML_ERROR_DOCTYPE) ;

	p->doctype = 1 ;
	p->dt_required = 0 ;
	p->dt_count = 0 ;

	return 1 ;
}

static int xml_dt_keyword(xml_parser *p)
{
	if (p->dt_required)
		return xml_fail(p, XML_ERROR_DOCTYPE) ;

	if (xml_scratch_is(p, XML_DOCTYPE_PUBLIC))
		p->dt_required = XML_DOCTYPE_PUBLIC_LITERALS ;
	else if (xml_scratch_is(p, XML_DOCTYPE_SYSTEM))
		p->dt_required = XML_DOCTYPE_SYSTEM_LITERALS ;
	else
		return xml_fail(p, XML_ERROR_DOCTYPE) ;

	return 1 ;
}

static int xml_dt_lit_end(xml_parser *p)
{
	if (p->dt_required == XML_DOCTYPE_PUBLIC_LITERALS && !p->dt_count) {

		if (!strbuf_uncounted(&p->scratch))
			return xml_fail(p, XML_ERROR_NOMEM) ;

		if (strspn(p->scratch.s, XML_PUBID_CHARS) != p->scratch.len)
			return xml_fail(p, XML_ERROR_DOCTYPE) ;
	}

	p->dt_count++ ;

	return 1 ;
}

static int xml_decl_begin(xml_parser *p)
{
	if (!xml_scratch_is(p, XML_DECLARATION_TARGET))
		return xml_fail(p, XML_ERROR_PI) ;

	if (p->declaration || p->root)
		return xml_fail(p, XML_ERROR_DECLARATION) ;

	p->declaration = 1 ;
	xml_attributes_reset(p) ;

	return 1 ;
}

static int xml_finish(xml_parser *p)
{
	if (xml_depth(p))
		return xml_fail(p, XML_ERROR_EOF) ;

	return p->root || xml_fail(p, XML_ERROR_ROOT) ;
}

static int xml_document(xml_parser *p, char const *s, size_t len)
{
	static uint32_t const table[XML_S_DONE][XML_C_ENDOFKEY] =
	{
		{ 0x1000002d, 0x00000200, 0x00000083, 0x00000200, 0x0000003c, 0x00000200, 0x00000200, 0x00000200, 0x00000200, 0x00000200, 0x00000200, 0x00000200, 0x00000200, 0x00000201, 0x00000200, 0x00000200, 0x00000200, 0x00000200, 0x00000200, 0x00000200 },
		{ 0x1000002d, 0x00000200, 0x00000083, 0x00000200, 0x0000003c, 0x00000200, 0x00000200, 0x00000200, 0x00000200, 0x00000200, 0x00000200, 0x00000200, 0x00000200, 0x00000202, 0x00000200, 0x00000200, 0x00000200, 0x00000200, 0x00000200, 0x00000200 },
		{ 0x1000002d, 0x00000200, 0x00000083, 0x00000043, 0x0000003c, 0x00000200, 0x00000200, 0x00000200, 0x00000200, 0x00000200, 0x00000200, 0x00000200, 0x00000200, 0x00000202, 0x00000200, 0x00000200, 0x00000200, 0x00000200, 0x00000200, 0x00000200 },
		{ 0x00000044, 0x0000003d, 0x0000003d, 0x0000003d, 0x0000003d, 0x00000004, 0x00000011, 0x0000002a, 0x0000003d, 0x0000003d, 0x0000003d, 0x0000003d, 0x0000003d, 0x0000003d, 0x00001c21, 0x00001c21, 0x00001c21, 0x00001c21, 0x0000003d, 0x0000003d },
		{ 0x00000044, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000005, 0x0000003b, 0x00000043, 0x00001809, 0x00001809, 0x00001809, 0x00001809, 0x00000043, 0x00000043 },
		{ 0x00000044, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000006, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043 },
		{ 0x00000044, 0x00000006, 0x00000006, 0x00000006, 0x00000006, 0x00000006, 0x00000006, 0x00000006, 0x00000006, 0x00000006, 0x00000006, 0x00000007, 0x00000006, 0x00000006, 0x00000006, 0x00000006, 0x00000006, 0x00000006, 0x00000006, 0x00000006 },
		{ 0x00000044, 0x00000006, 0x00000006, 0x00000006, 0x00000006, 0x00000006, 0x00000006, 0x00000006, 0x00000006, 0x00000006, 0x00000006, 0x00000008, 0x00000006, 0x00000006, 0x00000006, 0x00000006, 0x00000006, 0x00000006, 0x00000006, 0x00000006 },
		{ 0x00000044, 0x0000003a, 0x0000003a, 0x00000000, 0x0000003a, 0x0000003a, 0x0000003a, 0x0000003a, 0x0000003a, 0x0000003a, 0x0000003a, 0x0000003a, 0x0000003a, 0x0000003a, 0x0000003a, 0x0000003a, 0x0000003a, 0x0000003a, 0x0000003a, 0x0000003a },
		{ 0x00000044, 0x0020000a, 0x00200037, 0x00200037, 0x00200037, 0x00200037, 0x00200037, 0x00200037, 0x00200037, 0x00200037, 0x00200037, 0x00001009, 0x00200037, 0x00200037, 0x00001009, 0x00001009, 0x00001009, 0x00001009, 0x00001009, 0x00200037 },
		{ 0x00000044, 0x0000000a, 0x0000003d, 0x0000003d, 0x0000003d, 0x0000003d, 0x0000003d, 0x0000003d, 0x0000003d, 0x0000003d, 0x0000003d, 0x0000003d, 0x0000003d, 0x0000003d, 0x0000000b, 0x0000000b, 0x0000000b, 0x0000000b, 0x0000003d, 0x0000003d },
		{ 0x00000044, 0x0000000c, 0x00000037, 0x02000000, 0x00000037, 0x00000037, 0x00000037, 0x00000037, 0x00000037, 0x00000037, 0x00000037, 0x0000000b, 0x00000038, 0x00000037, 0x0000000b, 0x0000000b, 0x0000000b, 0x0000000b, 0x0000000b, 0x00000037 },
		{ 0x00000044, 0x0000000c, 0x00000037, 0x02000000, 0x00000037, 0x00000037, 0x00000037, 0x00000037, 0x00000037, 0x0080080e, 0x0080080f, 0x00000037, 0x00000038, 0x00000037, 0x0000180d, 0x0000180d, 0x0000180d, 0x0000180d, 0x00000037, 0x00000037 },
		{ 0x00000044, 0x0040000c, 0x00000037, 0x00000037, 0x00000037, 0x00000037, 0x00000037, 0x00000037, 0x00000037, 0x00000037, 0x00000037, 0x0000100d, 0x00000037, 0x00000037, 0x0000100d, 0x0000100d, 0x0000100d, 0x0000100d, 0x0000100d, 0x00000037 },
		{ 0x00000044, 0x0000100e, 0x0000100e, 0x0000100e, 0x0000100e, 0x0000100e, 0x0000100e, 0x0000100e, 0x0000100e, 0x01000010, 0x0000100e, 0x0000100e, 0x0000100e, 0x0000100e, 0x0000100e, 0x0000100e, 0x0000100e, 0x0000100e, 0x0000100e, 0x0000100e },
		{ 0x00000044, 0x0000100f, 0x0000100f, 0x0000100f, 0x0000100f, 0x0000100f, 0x0000100f, 0x0000100f, 0x0000100f, 0x0000100f, 0x01000010, 0x0000100f, 0x0000100f, 0x0000100f, 0x0000100f, 0x0000100f, 0x0000100f, 0x0000100f, 0x0000100f, 0x0000100f },
		{ 0x00000044, 0x0000000c, 0x00000037, 0x02000000, 0x00000037, 0x00000037, 0x00000037, 0x00000037, 0x00000037, 0x00000037, 0x00000037, 0x00000037, 0x00000038, 0x00000037, 0x00000037, 0x00000037, 0x00000037, 0x00000037, 0x00000037, 0x00000037 },
		{ 0x00000044, 0x0000003d, 0x0000003d, 0x0000003d, 0x0000003d, 0x0000003d, 0x0000003d, 0x0000003d, 0x0000003d, 0x0000003d, 0x0000003d, 0x0000003d, 0x0000003d, 0x0000003d, 0x00001812, 0x00000015, 0x00000015, 0x00000015, 0x0000003d, 0x0000003d },
		{ 0x00000044, 0x00000017, 0x00000039, 0x00000039, 0x00000039, 0x00000039, 0x00000016, 0x00000039, 0x00000039, 0x00000039, 0x00000039, 0x00000015, 0x00000039, 0x00000039, 0x00000015, 0x00001013, 0x00000015, 0x00000015, 0x00000015, 0x00000039 },
		{ 0x00000044, 0x00000017, 0x00000039, 0x00000039, 0x00000039, 0x00000039, 0x00000016, 0x00000039, 0x00000039, 0x00000039, 0x00000039, 0x00000015, 0x00000039, 0x00000039, 0x00000015, 0x00000015, 0x00001014, 0x00000015, 0x00000015, 0x00000039 },
		{ 0x00000044, 0x04000019, 0x00000039, 0x00000039, 0x00000039, 0x00000039, 0x04000020, 0x00000039, 0x00000039, 0x00000039, 0x00000039, 0x00000015, 0x00000039, 0x00000039, 0x00000015, 0x00000015, 0x00000015, 0x00000015, 0x00000015, 0x00000039 },
		{ 0x00000044, 0x00000017, 0x00000039, 0x00000039, 0x00000039, 0x00000039, 0x00000016, 0x00000039, 0x00000039, 0x00000039, 0x00000039, 0x00000015, 0x00000039, 0x00000039, 0x00000015, 0x00000015, 0x00000015, 0x00000015, 0x00000015, 0x00000039 },
		{ 0x00000044, 0x00000039, 0x00000039, 0x00000000, 0x00000039, 0x00000039, 0x00000039, 0x00000039, 0x00000039, 0x00000039, 0x00000039, 0x00000039, 0x00000039, 0x00000039, 0x00000039, 0x00000039, 0x00000039, 0x00000039, 0x00000039, 0x00000039 },
		{ 0x00000044, 0x00000017, 0x00000017, 0x00000017, 0x00000017, 0x00000017, 0x00000018, 0x00000017, 0x00000017, 0x00000017, 0x00000017, 0x00000017, 0x00000017, 0x00000017, 0x00000017, 0x00000017, 0x00000017, 0x00000017, 0x00000017, 0x00000017 },
		{ 0x00000044, 0x00000017, 0x00000017, 0x00000000, 0x00000017, 0x00000017, 0x00000018, 0x00000017, 0x00000017, 0x00000017, 0x00000017, 0x00000017, 0x00000017, 0x00000017, 0x00000017, 0x00000017, 0x00000017, 0x00000017, 0x00000017, 0x00000017 },
		{ 0x00000044, 0x00000019, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000020, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x0000181a, 0x0000181a, 0x0000181a, 0x0000181a, 0x00000035, 0x00000035 },
		{ 0x00008044, 0x0000801b, 0x00008035, 0x00008035, 0x00008035, 0x00008035, 0x00008035, 0x00008035, 0x0000801c, 0x00008035, 0x00008035, 0x0000101a, 0x00008035, 0x00008035, 0x0000101a, 0x0000101a, 0x0000101a, 0x0000101a, 0x0000101a, 0x00008035 },
		{ 0x00000044, 0x0000001b, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x0000001c, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035 },
		{ 0x00000044, 0x0000001c, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x0000001d, 0x0000001e, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035 },
		{ 0x00000044, 0x0000401d, 0x0000401d, 0x0000401d, 0x0000401d, 0x0000401d, 0x0000401d, 0x0000401d, 0x0000401d, 0x0002001f, 0x0000401d, 0x0000401d, 0x0000401d, 0x0000401d, 0x0000401d, 0x0000401d, 0x0000401d, 0x0000401d, 0x0000401d, 0x0000401d },
		{ 0x00000044, 0x0000401e, 0x0000401e, 0x0000401e, 0x0000401e, 0x0000401e, 0x0000401e, 0x0000401e, 0x0000401e, 0x0000401e, 0x0002001f, 0x0000401e, 0x0000401e, 0x0000401e, 0x0000401e, 0x0000401e, 0x0000401e, 0x0000401e, 0x0000401e, 0x0000401e },
		{ 0x00000044, 0x00000019, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000020, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035 },
		{ 0x00000044, 0x00000035, 0x00000035, 0x08000000, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x00000035 },
		{ 0x00000044, 0x00002022, 0x00000043, 0x00042000, 0x00000043, 0x00000043, 0x00000043, 0x00002129, 0x00000043, 0x00000043, 0x00000043, 0x00001021, 0x00000043, 0x00000043, 0x00001021, 0x00001021, 0x00001021, 0x00001021, 0x00001021, 0x00000043 },
		{ 0x00000044, 0x00000022, 0x0000003d, 0x00040000, 0x0000003d, 0x0000003d, 0x0000003d, 0x00000129, 0x0000003d, 0x0000003d, 0x0000003d, 0x0000003d, 0x0000003d, 0x0000003d, 0x00001824, 0x00001824, 0x00001824, 0x00001824, 0x0000003d, 0x0000003d },
		{ 0x00000044, 0x00000022, 0x00000043, 0x00040000, 0x00000043, 0x00000043, 0x00000043, 0x00000129, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043 },
		{ 0x00008044, 0x00008025, 0x0000803e, 0x0000803e, 0x0000803e, 0x0000803e, 0x0000803e, 0x0000803e, 0x00008026, 0x0000803e, 0x0000803e, 0x00001024, 0x0000803e, 0x0000803e, 0x00001024, 0x00001024, 0x00001024, 0x00001024, 0x00001024, 0x0000803e },
		{ 0x00000044, 0x00000025, 0x0000003e, 0x0000003e, 0x0000003e, 0x0000003e, 0x0000003e, 0x0000003e, 0x00000026, 0x0000003e, 0x0000003e, 0x0000003e, 0x0000003e, 0x0000003e, 0x0000003e, 0x0000003e, 0x0000003e, 0x0000003e, 0x0000003e, 0x0000003e },
		{ 0x00000044, 0x00000026, 0x0000003e, 0x0000003e, 0x0000003e, 0x0000003e, 0x0000003e, 0x0000003e, 0x0000003e, 0x00000027, 0x00000028, 0x0000003e, 0x0000003e, 0x0000003e, 0x0000003e, 0x0000003e, 0x0000003e, 0x0000003e, 0x0000003e, 0x0000003e },
		{ 0x00000044, 0x00004027, 0x00000043, 0x00004027, 0x0000003c, 0x00004027, 0x00004027, 0x00004027, 0x00004027, 0x00010023, 0x00004027, 0x00004027, 0x00004027, 0x00004027, 0x00004027, 0x00004027, 0x00004027, 0x00004027, 0x00004027, 0x00004027 },
		{ 0x00000044, 0x00004028, 0x00000043, 0x00004028, 0x0000003c, 0x00004028, 0x00004028, 0x00004028, 0x00004028, 0x00004028, 0x00010023, 0x00004028, 0x00004028, 0x00004028, 0x00004028, 0x00004028, 0x00004028, 0x00004028, 0x00004028, 0x00004028 },
		{ 0x00000044, 0x00000043, 0x00000043, 0x00140000, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043 },
		{ 0x00000044, 0x0000003d, 0x0000003d, 0x0000003d, 0x0000003d, 0x0000003d, 0x0000003d, 0x0000003d, 0x0000003d, 0x0000003d, 0x0000003d, 0x0000003d, 0x0000003d, 0x0000003d, 0x0000182b, 0x0000182b, 0x0000182b, 0x0000182b, 0x0000003d, 0x0000003d },
		{ 0x00000044, 0x0000002c, 0x00000043, 0x00180000, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x0000102b, 0x00000043, 0x00000043, 0x0000102b, 0x0000102b, 0x0000102b, 0x0000102b, 0x0000102b, 0x00000043 },
		{ 0x00000044, 0x0000002c, 0x00000043, 0x00180000, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043, 0x00000043 }
	} ;
	size_t pos = 0 ;
	uint8_t state = XML_S_CONTENT ;

	while (state < XML_S_DONE) {

		char c = pos < len ? s[pos] : 0 ;
		uint8_t cls = xml_cclass(c) ;
		uint32_t what = table[state][cls] ;
		state = what & XML_STATE_MASK ;

		if (what & XML_A_MARK) p->open = p->close = p->line ;
		if (what & XML_A_MARK_CLOSE) p->close = p->line ;
		if ((what & XML_A_TEXT) && !xml_text(p, c, cls)) return 0 ;
		if ((what & XML_A_STAG_BEGIN) && p->root && !xml_depth(p)) return xml_fail(p, XML_ERROR_ROOT) ;
		if (what & XML_A_SCRATCH_RESET) p->scratch.len = 0 ;
		if ((what & XML_A_SCRATCH_CAT) && !xml_cat(p, &p->scratch, c)) return 0 ;
		if ((what & XML_A_STAG_NAME_END) && !xml_stag_name_end(p)) return 0 ;
		if ((what & XML_A_ATT_CAT) && !xml_att_cat(p, c, cls)) return 0 ;
		if ((what & XML_A_ATT_NAME_END) && !xml_att_name_end(p)) return 0 ;
		if ((what & XML_A_ATT_VALUE_END) && !xml_terminate(p, &p->attrvalues)) return 0 ;
		if ((what & XML_A_DECL_VALUE) && !xml_decl_value(p)) return 0 ;
		if ((what & XML_A_STAG_END) && !xml_stag_end(p)) return 0 ;
		if ((what & XML_A_ETAG_END) && !xml_etag_end(p)) return 0 ;
		if ((what & XML_A_CLOSE) && !xml_close(p)) return 0 ;
		if ((what & XML_A_DOCTYPE_BEGIN) && !xml_doctype_begin(p)) return 0 ;
		if ((what & XML_A_DT_KEYWORD) && !xml_dt_keyword(p)) return 0 ;
		if ((what & XML_A_DT_LIT_BEGIN) && p->dt_count == p->dt_required) return xml_fail(p, XML_ERROR_DOCTYPE) ;
		if ((what & XML_A_DT_LIT_END) && !xml_dt_lit_end(p)) return 0 ;
		if ((what & XML_A_DT_END) && p->dt_count != p->dt_required) return xml_fail(p, XML_ERROR_DOCTYPE) ;
		if ((what & XML_A_DECL_BEGIN) && !xml_decl_begin(p)) return 0 ;
		if ((what & XML_A_DECL_END) && !p->attrnames.len) return xml_fail(p, XML_ERROR_DECLARATION) ;
		if ((what & XML_A_FINISH) && !xml_finish(p)) return 0 ;
		if (state > XML_S_DONE) break ;
		if (c == '\n') p->line++ ;
		pos++ ;
	}

	if (state > XML_S_DONE)
		return xml_fail(p, (xml_error_code_t)(state - XML_S_DONE)) ;

	return 1 ;
}

int xml_parse(char const *name, char const *s, size_t len, xml_handler const *handler, void *userdata, xml_error *err)
{
	log_flow() ;

	int r ;
	_cleanup_strbuf_ strbuf doc = STRBUF_ZERO ;
	xml_parser p = {
		.line = XML_LINE_FIRST,
		.open = 0,
		.close = 0,
		.handler = handler,
		.userdata = userdata,
		.err = err,
		.names = SBL_ZERO,
		.text = STRBUF_ZERO,
		.scratch = STRBUF_ZERO,
		.attrnames = SBL_ZERO,
		.attrvalues = SBL_ZERO,
		.attrs = GENBUF_ZERO,
		.stack = GENBUF_ZERO,
		.element = { .name = 0, .text = 0 },
		.value = 0,
		.dt_required = 0,
		.dt_count = 0,
		.declaration = 0,
		.doctype = 0,
		.root = 0
	} ;

	err->code = XML_ERROR_NONE ;
	err->line = 0 ;

	r = xml_normalize(&p, &doc, s, len) && xml_document(&p, doc.s, doc.len) ;

	strbuf_free(&p.names) ;
	strbuf_free(&p.text) ;
	strbuf_free(&p.scratch) ;
	strbuf_free(&p.attrnames) ;
	strbuf_free(&p.attrvalues) ;
	genbuf_free(&p.attrs) ;
	genbuf_free(&p.stack) ;

	if (!r && err->code != XML_ERROR_HANDLER)
		flog_warn("invalid XML at line %zu of %s: %s", err->line, name, xml_error_str(err->code)) ;

	return r ;
}

int xml_parse_file(char const *path, xml_handler const *handler, void *userdata, xml_error *err)
{
	log_flow() ;

	_cleanup_strbuf_ strbuf sa = STRBUF_ZERO ;

	if (!strbuf_read_file(&sa, path)) {

		err->code = XML_ERROR_READ ;
		err->line = 0 ;

		log_warnusys_return(LOG_EXIT_ZERO, "read file: ", path) ;
	}

	return xml_parse(path, sa.s, sa.len, handler, userdata, err) ;
}
