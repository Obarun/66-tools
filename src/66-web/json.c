/*
 * json.c
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

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <oblibs/types.h>

#include "json.h"

#define JSON_MAX_DEPTH 32

static void put(json_t *j, char const *s, size_t len)
{
	if (j->err)
		return ;
	if (!strbuf_catb(&j->sa, s, len))
		j->err = 1 ;
}

static void putc1(json_t *j, char c)
{
	put(j, &c, 1) ;
}

static void mark_emitted(json_t *j)
{
	if (j->depth == 0)
		return ;
	j->need_comma |= (uint32_t)1 << (j->depth - 1) ;
}

static void maybe_comma(json_t *j)
{
	if (j->depth == 0)
		return ;
	if (j->need_comma & ((uint32_t)1 << (j->depth - 1)))
		putc1(j, ',') ;
}

static void push(json_t *j)
{
	if (j->depth >= JSON_MAX_DEPTH) {
		j->err = 1 ;
		return ;
	}
	maybe_comma(j) ;
	mark_emitted(j) ;
	j->depth++ ;
	j->need_comma &= ~((uint32_t)1 << (j->depth - 1)) ;
}

static void pop(json_t *j)
{
	if (j->depth == 0) {
		j->err = 1 ;
		return ;
	}
	j->depth-- ;
}

static void put_escaped(json_t *j, char const *s, size_t len)
{
	putc1(j, '"') ;

	for (size_t i = 0 ; i < len ; i++) {

		unsigned char c = (unsigned char)s[i] ;
		switch (c) {
			case '"':  put(j, "\\\"", 2) ; break ;
			case '\\': put(j, "\\\\", 2) ; break ;
			case '\b': put(j, "\\b",  2) ; break ;
			case '\f': put(j, "\\f",  2) ; break ;
			case '\n': put(j, "\\n",  2) ; break ;
			case '\r': put(j, "\\r",  2) ; break ;
			case '\t': put(j, "\\t",  2) ; break ;
			default:
				if (c < 0x20) {
					char esc[6] = { '\\', 'u', '0', '0' } ;
					put(j, esc, 4 + u64_fmtpad(esc + 4, c, 2, 16)) ;
				} else {
					putc1(j, (char)c) ;
				}
		}
	}

	putc1(j, '"') ;
}

void json_free(json_t *j)
{
	strbuf_free(&j->sa) ;
	j->need_comma = 0 ;
	j->depth = 0 ;
	j->err = 0 ;
}

void json_obj_open(json_t *j)
{
	push(j) ;
	putc1(j, '{') ;
}

void json_obj_close(json_t *j)
{
	putc1(j, '}') ;
	pop(j) ;
}

void json_arr_open(json_t *j)
{
	push(j) ;
	putc1(j, '[') ;
}

void json_arr_close(json_t *j)
{
	putc1(j, ']') ;
	pop(j) ;
}

void json_key(json_t *j, char const *key)
{
	maybe_comma(j) ;
	put_escaped(j, key, strlen(key)) ;
	putc1(j, ':') ;
	/* The key consumes the separator. Clear need_comma so the value
	 * that follows does not emit a second comma; the value itself
	 * will re-arm need_comma via mark_emitted(). */
	if (j->depth)
		j->need_comma &= ~((uint32_t)1 << (j->depth - 1)) ;
}

void json_str(json_t *j, char const *s)
{
	json_strn(j, s, s ? strlen(s) : 0) ;
}

void json_strn(json_t *j, char const *s, size_t len)
{
	if (!s) {
		json_null(j) ;
		return ;
	}
	maybe_comma(j) ;
	put_escaped(j, s, len) ;
	mark_emitted(j) ;
}

void json_int(json_t *j, int64_t n)
{
	maybe_comma(j) ;
	char buf[I64_FMT] ;
	size_t n2 = i64_fmtbase(buf, n, 10) ;
	put(j, buf, n2) ;
	mark_emitted(j) ;
}

void json_uint(json_t *j, uint64_t n)
{
	maybe_comma(j) ;
	char buf[U64_FMT] ;
	size_t n2 = u64_fmt(buf, n) ;
	put(j, buf, n2) ;
	mark_emitted(j) ;
}

void json_bool(json_t *j, int b)
{
	maybe_comma(j) ;
	if (b) put(j, "true", 4) ;
	else put(j, "false", 5) ;
	mark_emitted(j) ;
}

void json_null(json_t *j)
{
	maybe_comma(j) ;
	put(j, "null", 4) ;
	mark_emitted(j) ;
}

int json_valid(json_t const *j)
{
	return !j->err && j->depth == 0 ;
}
