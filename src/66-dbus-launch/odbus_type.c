/*
 * odbus_type.c
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
#include <stddef.h>
#include <string.h>
#include <sys/types.h>

#include <oblibs/types.h>

#include "odbus_internal.h"

int odbus_type_alignment(char type)
{
	switch (type) {

		case 'y' : // byte
		case 'g' : // signature
		case 'v' : // variant
			return 1 ;

		case 'n' : // int16
		case 'q' : // uint16
			return 2 ;

		case 'b' : // boolean, marshalled as uint32
		case 'i' : // int32
		case 'u' : // uint32
		case 'h' : // unix fd, marshalled as an index
		case 's' : // string
		case 'o' : // object path
		case 'a' : // array, aligned on its length prefix
			return 4 ;

		case 'x' : // int64
		case 't' : // uint64
		case 'd' : // double
		case 'r' : // struct, API spelling
		case '(' : // struct, signature spelling
		case 'e' : // dict entry, API spelling
		case '{' : // dict entry, signature spelling
			return 8 ;

		default :
			return 0 ;
	}
}

int odbus_type_is_basic(char type)
{
	return !!memchr("ybnqiuxtdsogh", type, strlen("ybnqiuxtdsogh")) ;
}

ssize_t odbus_type_len(char const *signature, unsigned int depth)
{
	if (depth > ODBUS_DEPTH_MAX)
		return 0 ;

	if (!*signature)
		return 0 ;

	if (odbus_type_is_basic(*signature) || *signature == 'v')
		return 1 ;

	if (*signature == 'a') {

		ssize_t r = odbus_type_len(signature + 1, depth + 1) ;
		if (!r)
			return 0 ;

		return r + 1 ;
	}

	if (*signature == '(' || *signature == '{') {

		char close = *signature == '(' ? ')' : '}' ;
		ssize_t pos = 1 ;
		unsigned int nfield = 0 ;

		while (signature[pos] != close) {

			ssize_t r = odbus_type_len(signature + pos, depth + 1) ;
			if (!r)
				return 0 ;

			pos += r ;
			nfield++ ;
		}

		/** A struct holds at least one field. A dict entry holds exactly two, and
		 * its key must be a basic type. */
		if (close == ')') {

			if (!nfield)
				return 0 ;

		} else {

			if (nfield != 2 || !odbus_type_is_basic(signature[1]))
				return 0 ;
		}

		return pos + 1 ;
	}

	return 0 ;
}

int odbus_signature_valid(char const *signature)
{
	ssize_t pos = 0 ;

	if (strlen(signature) > ODBUS_SIGNATURE_LEN_MAX)
		return 0 ;

	while (signature[pos]) {

		ssize_t r = odbus_type_len(signature + pos, 0) ;
		if (!r)
			return 0 ;

		pos += r ;
	}

	return 1 ;
}

size_t odbus_align_up(size_t v, size_t align)
{
	return (v + align - 1) & ~(align - 1) ;
}

void odbus_st16le(char *d, uint16_t v)
{
	d[0] = (char)(v & 0xff) ;
	d[1] = (char)((v >> 8) & 0xff) ;
}

void odbus_st32le(char *d, uint32_t v)
{
	u32_pack(d, v) ;
}

void odbus_st64le(char *d, uint64_t v)
{
	u32_pack(d, (uint32_t)(v & 0xffffffffu)) ;
	u32_pack(d + 4, (uint32_t)((v >> 32) & 0xffffffffu)) ;
}

uint16_t odbus_ld16(char const *s, int be)
{
	unsigned char const *u = (unsigned char const *)s ;

	if (be)
		return (uint16_t)(((uint16_t)u[0] << 8) | u[1]) ;

	return (uint16_t)(u[0] | ((uint16_t)u[1] << 8)) ;
}

uint32_t odbus_ld32(char const *s, int be)
{
	uint32_t v ;

	if (be)
		u32_unpack_big(s, &v) ;
	else
		u32_unpack(s, &v) ;

	return v ;
}

uint64_t odbus_ld64(char const *s, int be)
{
	uint64_t v ;
	uint32_t lo, hi ;

	if (be) {
		u64_unpack_big(s, &v) ;
		return v ;
	}

	u32_unpack(s, &lo) ;
	u32_unpack(s + 4, &hi) ;

	return lo | ((uint64_t)hi << 32) ;
}
