/*
 * jsonparse.c
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

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <oblibs/types.h>

#include "jsonparse.h"

#define JP_MAX_DEPTH 64 // containers open at once
#define JP_MAX_MEMBERS 64 // members of one object
#define JP_NUM_MAX 64 // bytes of one number

// States and classes of jsonparse.txt that the actions name.
#define JP_KE 3
#define JP_CO 4
#define JP_VA 5
#define JP_DONE 31
#define JP_REJECT 32
#define JP_CLASS_EOF 31
#define JP_STATE_MASK 0x3f
#define U8_REJECT 8

#define JP_MARK 0x000040
#define JP_KEY 0x000080
#define JP_STR_END 0x000100
#define JP_NUM_END 0x000200
#define JP_OBJ_OPEN 0x000400
#define JP_OBJ_CLOSE 0x000800
#define JP_ARR_OPEN 0x001000
#define JP_ARR_CLOSE 0x002000
#define JP_NEXT 0x004000
#define JP_UTF8 0x008000
#define JP_TRUE 0x010000
#define JP_FALSE 0x020000
#define JP_NULL_LIT 0x040000

typedef struct jp_parser_s jp_parser_t ;
struct jp_parser_s {
	char const *s ;
	jp_value *root ;
	jp_value *stack[JP_MAX_DEPTH] ;
	uint8_t depth ;
	char *key ; // decoded key waiting for its value
	size_t mark ;
	uint8_t iskey ;
} ;

#define JP_PARSER_ZERO { NULL, NULL, { NULL }, 0, NULL, 0, 0 }

static uint8_t jp_cclass(unsigned char c)
{
	switch (c) {
		case ' ': return 0 ;
		case '\t': case '\n': case '\r': return 1 ;
		case '{': return 2 ;
		case '}': return 3 ;
		case '[': return 4 ;
		case ']': return 5 ;
		case ':': return 6 ;
		case ',': return 7 ;
		case '"': return 8 ;
		case '\\': return 9 ;
		case '/': return 10 ;
		case '+': return 11 ;
		case '-': return 12 ;
		case '.': return 13 ;
		case '0': return 14 ;
		case '1': case '2': case '3': case '4': case '5':
		case '6': case '7': case '8': case '9': return 15 ;
		case 'a': return 16 ;
		case 'b': return 17 ;
		case 'e': return 18 ;
		case 'f': return 19 ;
		case 'l': return 20 ;
		case 'n': return 21 ;
		case 'r': return 22 ;
		case 's': return 23 ;
		case 't': return 24 ;
		case 'u': return 25 ;
		case 'E': return 26 ;
		case 'c': case 'd': case 'A': case 'B': case 'C': case 'D': case 'F': return 27 ;
		default:
			if (c >= 0x80)
				return 30 ;
			return c < 0x20 ? 29 : 28 ;
	}
}

static uint8_t jp_u8class(unsigned char c)
{
	if (c < 0x80) return 0 ;
	if (c < 0x90) return 1 ;
	if (c < 0xa0) return 2 ;
	if (c < 0xc0) return 3 ;
	if (c < 0xc2) return 11 ;
	if (c < 0xe0) return 4 ;
	if (c == 0xe0) return 5 ;
	if (c == 0xed) return 7 ;
	if (c < 0xf0) return 6 ;
	if (c == 0xf0) return 8 ;
	if (c < 0xf4) return 9 ;
	if (c == 0xf4) return 10 ;
	return 11 ;
}

static unsigned hex4(char const *p)
{
	unsigned v = 0 ;

	for (int i = 0 ; i < 4 ; i++)
		v = (v << 4) | scan_digit((unsigned char)p[i], 16) ;

	return v ;
}

/* Encode a Unicode code point as UTF-8 into w; returns bytes written. */
static size_t utf8_put(char *w, unsigned cp)
{
	if (cp < 0x80) {
		w[0] = (char)cp ;
		return 1 ;
	}

	if (cp < 0x800) {
		w[0] = (char)(0xC0 | (cp >> 6)) ;
		w[1] = (char)(0x80 | (cp & 0x3F)) ;
		return 2 ;
	}

	if (cp < 0x10000) {
		w[0] = (char)(0xE0 | (cp >> 12)) ;
		w[1] = (char)(0x80 | ((cp >> 6) & 0x3F)) ;
		w[2] = (char)(0x80 | (cp & 0x3F)) ;
		return 3 ;
	}

	w[0] = (char)(0xF0 | (cp >> 18)) ;
	w[1] = (char)(0x80 | ((cp >> 12) & 0x3F)) ;
	w[2] = (char)(0x80 | ((cp >> 6) & 0x3F)) ;
	w[3] = (char)(0x80 | (cp & 0x3F)) ;

	return 4 ;
}

/* Decode s[start..end), whose escapes the table already checked for syntax.
 * Returns the heap copy, or NULL with errno EPROTO on \u0000 or a lone
 * surrogate, ENOMEM on allocation failure. Never longer than its source. */
static char *jp_decode(char const *s, size_t start, size_t end, size_t *len)
{
	char *out = malloc(end - start + 1) ;
	if (!out)
		return NULL ;

	char *w = out ;
	size_t i = start ;

	while (i < end) {

		if (s[i] != '\\') {
			*w++ = s[i++] ;
			continue ;
		}

		char e = s[i + 1] ;
		i += 2 ;

		switch (e) {
			case 'b': *w++ = '\b' ; break ;
			case 'f': *w++ = '\f' ; break ;
			case 'n': *w++ = '\n' ; break ;
			case 'r': *w++ = '\r' ; break ;
			case 't': *w++ = '\t' ; break ;
			case 'u': {

				unsigned cp = hex4(s + i) ;
				i += 4 ;

				if (!cp || (cp >= 0xdc00 && cp <= 0xdfff))
					goto reject ;

				if (cp >= 0xd800 && cp <= 0xdbff) {

					if (i + 6 > end || s[i] != '\\' || s[i + 1] != 'u')
						goto reject ;

					unsigned lo = hex4(s + i + 2) ;
					if (lo < 0xdc00 || lo > 0xdfff)
						goto reject ;

					cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00) ;
					i += 6 ;
				}

				w += utf8_put(w, cp) ;
				break ;
			}
			default: *w++ = e ; break ; // '"', '\\' or '/'
		}
	}

	*w = 0 ;
	*len = (size_t)(w - out) ;
	return out ;

	reject:
		free(out) ;
		return (errno = EPROTO, NULL) ;
}

static jp_value *mk(jp_type t)
{
	jp_value *v = calloc(1, sizeof(*v)) ;
	if (v)
		v->type = t ;

	return v ;
}

/* Hang v in the tree: as the root, the next array item, or the value of the
 * pending key. Frees v on failure. */
static int jp_attach(jp_parser_t *p, jp_value *v)
{
	if (!v) return 0 ;
	if (!p->depth) {
		p->root = v ;
		return 1 ;
	}

	jp_value *top = p->stack[p->depth - 1] ;

	if (top->type == JP_ARR) {

		jp_value **items = realloc(top->u.arr.items, (top->u.arr.n + 1) * sizeof(*items)) ;
		if (!items) {
			jp_free(v) ;
			return 0 ;
		}

		top->u.arr.items = items ;
		top->u.arr.items[top->u.arr.n++] = v ;

		return 1 ;
	}

	char **keys = realloc(top->u.obj.keys, (top->u.obj.n + 1) * sizeof(*keys)) ;
	if (!keys) {
		jp_free(v) ; return 0 ;
	}
	top->u.obj.keys = keys ;

	jp_value **vals = realloc(top->u.obj.vals, (top->u.obj.n + 1) * sizeof(*vals)) ;
	if (!vals) {
		jp_free(v) ; return 0 ;
	}
	top->u.obj.vals = vals ;

	top->u.obj.keys[top->u.obj.n] = p->key ;
	top->u.obj.vals[top->u.obj.n] = v ;
	top->u.obj.n++ ;
	p->key = NULL ;

	return 1 ;
}

static int jp_open(jp_parser_t *p, jp_type t)
{
	if (p->depth == JP_MAX_DEPTH)
		return (errno = EPROTO, 0) ;

	jp_value *v = mk(t) ;
	if (!jp_attach(p, v)) return 0 ;
	p->stack[p->depth++] = v ;

	return 1 ;
}

static int jp_close(jp_parser_t *p, jp_type t)
{
	if (!p->depth || p->stack[p->depth - 1]->type != t)
		return (errno = EPROTO, 0) ;

	p->depth-- ;

	return 1 ;
}

static int jp_next(jp_parser_t *p, uint8_t *state)
{
	if (!p->depth)
		return (errno = EPROTO, 0) ;

	*state = p->stack[p->depth - 1]->type == JP_OBJ ? JP_KE : JP_VA ;

	return 1 ;
}

/* The closing quote is at pos, the opening one at p->mark. A key is checked
 * against its object and waits in p->key; a value is attached. */
static int jp_string_end(jp_parser_t *p, size_t pos, uint8_t *state)
{
	size_t len = 0 ;
	char *s = jp_decode(p->s, p->mark + 1, pos, &len) ;
	if (!s) return 0 ;
	if (p->iskey) {

		jp_value const *obj = p->stack[p->depth - 1] ;

		if (obj->u.obj.n == JP_MAX_MEMBERS) {
			free(s) ;
			return (errno = EPROTO, 0) ;
		}

		for (size_t i = 0 ; i < obj->u.obj.n ; i++) {
			if (!strcmp(obj->u.obj.keys[i], s)) {
				free(s) ;
				return (errno = EPROTO, 0) ;
			}
		}

		p->key = s ;
		p->iskey = 0 ;
		*state = JP_CO ;
		return 1 ;
	}

	jp_value *v = mk(JP_STR) ;
	if (!v) {
		free(s) ; return 0 ;
	}
	v->u.str.s = s ;
	v->u.str.len = len ;

	return jp_attach(p, v) ;
}

static int jp_number_end(jp_parser_t *p, size_t pos)
{
	char tmp[JP_NUM_MAX] ;
	size_t n = pos - p->mark ;

	if (n >= JP_NUM_MAX)
		return (errno = EPROTO, 0) ;

	memcpy(tmp, p->s + p->mark, n) ;
	tmp[n] = 0 ;

	jp_value *v = mk(JP_NUM) ;
	if (!v) return 0 ;
	v->u.num = strtod(tmp, NULL) ;

	return jp_attach(p, v) ;
}

static int jp_literal(jp_parser_t *p, jp_type t, int b)
{
	jp_value *v = mk(t) ;
	if (!v) return 0 ;
	v->u.b = b ;

	return jp_attach(p, v) ;
}

/* One step of the automaton of jsonparse.txt: the actions of `what`, in the
 * order the spec gives. Returns 0 with errno set when the step rejects. */
static int jp_act(jp_parser_t *p, uint32_t what, size_t pos, uint8_t *state, uint8_t *u8, unsigned char c)
{
	static uint8_t const utf8[8][12] = {
		{ 0, 8, 8, 8, 1, 4, 2, 5, 6, 3, 7, 8 },
		{ 8, 0, 0, 0, 8, 8, 8, 8, 8, 8, 8, 8 },
		{ 8, 1, 1, 1, 8, 8, 8, 8, 8, 8, 8, 8 },
		{ 8, 2, 2, 2, 8, 8, 8, 8, 8, 8, 8, 8 },
		{ 8, 8, 8, 1, 8, 8, 8, 8, 8, 8, 8, 8 },
		{ 8, 1, 1, 8, 8, 8, 8, 8, 8, 8, 8, 8 },
		{ 8, 8, 2, 2, 8, 8, 8, 8, 8, 8, 8, 8 },
		{ 8, 2, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8 }
	} ;

	if (what & JP_UTF8) {
		*u8 = utf8[*u8][jp_u8class(c)] ;
		if (*u8 == U8_REJECT)
			return (errno = EPROTO, 0) ;
	}

	if (what & JP_MARK)
		p->mark = pos ;

	if (what & JP_KEY)
		p->iskey = 1 ;

	if ((what & JP_STR_END) && !jp_string_end(p, pos, state)) return 0 ;
	if ((what & JP_NUM_END) && !jp_number_end(p, pos)) return 0 ;
	if ((what & JP_TRUE) && !jp_literal(p, JP_BOOL, 1)) return 0 ;
	if ((what & JP_FALSE) && !jp_literal(p, JP_BOOL, 0)) return 0 ;
	if ((what & JP_NULL_LIT) && !jp_literal(p, JP_NULL, 0)) return 0 ;
	if ((what & JP_OBJ_OPEN) && !jp_open(p, JP_OBJ)) return 0 ;
	if ((what & JP_ARR_OPEN) && !jp_open(p, JP_ARR)) return 0 ;
	if ((what & JP_OBJ_CLOSE) && !jp_close(p, JP_OBJ)) return 0 ;
	if ((what & JP_ARR_CLOSE) && !jp_close(p, JP_ARR)) return 0 ;
	if ((what & JP_NEXT) && !jp_next(p, state)) return 0 ;

	return 1 ;
}

jp_value *jp_parse(char const *s, size_t len)
{
	static uint32_t const table[31][32] = {
		/* GO */
		{ 0x000000, 0x000000, 0x000402, 0x000020, 0x001006, 0x000020, 0x000020, 0x000020,
		  0x000047, 0x000020, 0x000020, 0x000020, 0x00004d, 0x000020, 0x00004e, 0x00004f,
		  0x000020, 0x000020, 0x000020, 0x000018, 0x000020, 0x00001c, 0x000020, 0x000020,
		  0x000015, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020 },
		/* OK */
		{ 0x000001, 0x000001, 0x000020, 0x000801, 0x000020, 0x002001, 0x000020, 0x004005,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x00001f },
		/* OB */
		{ 0x000002, 0x000002, 0x000020, 0x000801, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x0000c7, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020 },
		/* KE */
		{ 0x000003, 0x000003, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x0000c7, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020 },
		/* CO */
		{ 0x000004, 0x000004, 0x000020, 0x000020, 0x000020, 0x000020, 0x000005, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020 },
		/* VA */
		{ 0x000005, 0x000005, 0x000402, 0x000020, 0x001006, 0x000020, 0x000020, 0x000020,
		  0x000047, 0x000020, 0x000020, 0x000020, 0x00004d, 0x000020, 0x00004e, 0x00004f,
		  0x000020, 0x000020, 0x000020, 0x000018, 0x000020, 0x00001c, 0x000020, 0x000020,
		  0x000015, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020 },
		/* AR */
		{ 0x000006, 0x000006, 0x000402, 0x000020, 0x001006, 0x002001, 0x000020, 0x000020,
		  0x000047, 0x000020, 0x000020, 0x000020, 0x00004d, 0x000020, 0x00004e, 0x00004f,
		  0x000020, 0x000020, 0x000020, 0x000018, 0x000020, 0x00001c, 0x000020, 0x000020,
		  0x000015, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020 },
		/* ST */
		{ 0x008007, 0x000020, 0x008007, 0x008007, 0x008007, 0x008007, 0x008007, 0x008007,
		  0x008101, 0x008008, 0x008007, 0x008007, 0x008007, 0x008007, 0x008007, 0x008007,
		  0x008007, 0x008007, 0x008007, 0x008007, 0x008007, 0x008007, 0x008007, 0x008007,
		  0x008007, 0x008007, 0x008007, 0x008007, 0x008007, 0x000020, 0x008007, 0x000020 },
		/* ES */
		{ 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000007, 0x000007, 0x000007, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000007, 0x000020, 0x000007, 0x000020, 0x000007, 0x000007, 0x000020,
		  0x000007, 0x000009, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020 },
		/* U1 */
		{ 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x00000a, 0x00000a,
		  0x00000a, 0x00000a, 0x00000a, 0x00000a, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x00000a, 0x00000a, 0x000020, 0x000020, 0x000020, 0x000020 },
		/* U2 */
		{ 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x00000b, 0x00000b,
		  0x00000b, 0x00000b, 0x00000b, 0x00000b, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x00000b, 0x00000b, 0x000020, 0x000020, 0x000020, 0x000020 },
		/* U3 */
		{ 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x00000c, 0x00000c,
		  0x00000c, 0x00000c, 0x00000c, 0x00000c, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x00000c, 0x00000c, 0x000020, 0x000020, 0x000020, 0x000020 },
		/* U4 */
		{ 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000007, 0x000007,
		  0x000007, 0x000007, 0x000007, 0x000007, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000007, 0x000007, 0x000020, 0x000020, 0x000020, 0x000020 },
		/* MI */
		{ 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x00000e, 0x00000f,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020 },
		/* ZE */
		{ 0x000201, 0x000201, 0x000020, 0x000a01, 0x000020, 0x002201, 0x000020, 0x004205,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000010, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000012, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000012, 0x000020, 0x000020, 0x000020, 0x000020, 0x00021f },
		/* IN */
		{ 0x000201, 0x000201, 0x000020, 0x000a01, 0x000020, 0x002201, 0x000020, 0x004205,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000010, 0x00000f, 0x00000f,
		  0x000020, 0x000020, 0x000012, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000012, 0x000020, 0x000020, 0x000020, 0x000020, 0x00021f },
		/* FR */
		{ 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000011, 0x000011,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020 },
		/* FS */
		{ 0x000201, 0x000201, 0x000020, 0x000a01, 0x000020, 0x002201, 0x000020, 0x004205,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000011, 0x000011,
		  0x000020, 0x000020, 0x000012, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000012, 0x000020, 0x000020, 0x000020, 0x000020, 0x00021f },
		/* E1 */
		{ 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000013, 0x000013, 0x000020, 0x000014, 0x000014,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020 },
		/* E2 */
		{ 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000014, 0x000014,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020 },
		/* E3 */
		{ 0x000201, 0x000201, 0x000020, 0x000a01, 0x000020, 0x002201, 0x000020, 0x004205,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000014, 0x000014,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x00021f },
		/* T1 */
		{ 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000016, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020 },
		/* T2 */
		{ 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000017, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020 },
		/* T3 */
		{ 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x010001, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020 },
		/* F1 */
		{ 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000019, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020 },
		/* F2 */
		{ 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x00001a, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020 },
		/* F3 */
		{ 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x00001b,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020 },
		/* F4 */
		{ 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x020001, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020 },
		/* N1 */
		{ 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x00001d, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020 },
		/* N2 */
		{ 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x00001e, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020 },
		/* N3 */
		{ 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x040001, 0x000020, 0x000020, 0x000020,
		  0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020, 0x000020 }
	} ;

	jp_parser_t p = JP_PARSER_ZERO ;
	uint8_t state = 0, u8 = 0 ;
	int err = EPROTO ;

	if (!s)
		return (errno = EINVAL, NULL) ;

	p.s = s ;

	// One pass more than the bytes: the last one feeds the eof class.
	for (size_t pos = 0 ; state < JP_DONE && pos <= len ; pos++) {

		unsigned char c = pos < len ? (unsigned char)s[pos] : 0 ;
		uint32_t what = table[state][pos < len ? jp_cclass(c) : JP_CLASS_EOF] ;

		state = what & JP_STATE_MASK ;

		if (!jp_act(&p, what, pos, &state, &u8, c)) {
			err = errno ;
			state = JP_REJECT ;
			break ;
		}
	}

	if (state != JP_DONE || p.depth) {
		jp_free(p.root) ;
		free(p.key) ;
		return (errno = err, NULL) ;
	}

	return p.root ;
}

void jp_free(jp_value *v)
{
	if (!v) return ;

	switch (v->type) {

		case JP_STR:
			free(v->u.str.s) ;
			break ;

		case JP_ARR:
			for (size_t i = 0 ; i < v->u.arr.n ; i++) jp_free(v->u.arr.items[i]) ;
			free(v->u.arr.items) ;
			break ;

		case JP_OBJ:

			for (size_t i = 0 ; i < v->u.obj.n ; i++) {
				free(v->u.obj.keys[i]) ;
				jp_free(v->u.obj.vals[i]) ;
			}

			free(v->u.obj.keys) ;
			free(v->u.obj.vals) ;
			break ;

		default:
			break ;
	}

	free(v) ;
}

jp_value const *jp_get(jp_value const *v, char const *key)
{
	if (!v || v->type != JP_OBJ || !key)
		return NULL ;

	for (size_t i = 0 ; i < v->u.obj.n ; i++) {
		if (!strcmp(v->u.obj.keys[i], key))
			return v->u.obj.vals[i] ;
	}

	return NULL ;
}

char const *jp_string(jp_value const *v)
{
	return (v && v->type == JP_STR) ? v->u.str.s : NULL ;
}

int jp_is_true(jp_value const *v)
{
	return (v && v->type == JP_BOOL && v->u.b) ? 1 : 0 ;
}

size_t jp_arr_len(jp_value const *v)
{
	return (v && v->type == JP_ARR) ? v->u.arr.n : 0 ;
}

jp_value const *jp_arr_get(jp_value const *v, size_t i)
{
	if (!v || v->type != JP_ARR || i >= v->u.arr.n)
		return NULL ;

	return v->u.arr.items[i] ;
}
