/*
 * json.h
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

#ifndef WEB66_JSON_H
#define WEB66_JSON_H

#include <stddef.h>
#include <stdint.h>

#include <oblibs/strbuf.h>

typedef struct json_s json_t ;
struct json_s {
	strbuf sa ;
	uint32_t need_comma ; // bitmap: bit N = needs comma at nesting depth N
	uint8_t depth ;
	int err ;
} ;

#define JSON_ZERO { STRBUF_ZERO, 0, 0, 0 }

extern void json_free(json_t *j) ;

extern void json_obj_open(json_t *j) ;
extern void json_obj_close(json_t *j) ;
extern void json_arr_open(json_t *j) ;
extern void json_arr_close(json_t *j) ;

extern void json_key(json_t *j, char const *key) ;
extern void json_str(json_t *j, char const *s) ;
extern void json_strn(json_t *j, char const *s, size_t len) ;

/**
 * Emit a bare JSON number.
 *
 * JSON has no integer type, and every browser parses numbers as IEEE-754
 * doubles: past 2^53 (9007199254740992) `JSON.parse` rounds the value and
 * reports no error, so the client silently reads something other than what
 * was sent. Anything that can exceed that magnitude — a 64-bit hash, a
 * nanosecond timestamp, a 64-bit identifier — MUST go out through
 * `json_str()` as a decimal string, and the API contract must say so.
 *
 * Numbers known to stay below the limit (pids, epoch seconds, enum values)
 * are emitted here.
 */
extern void json_int(json_t *j, int64_t n) ;
extern void json_uint(json_t *j, uint64_t n) ;
extern void json_bool(json_t *j, int b) ;
extern void json_null(json_t *j) ;

/* 1 if no errors so far and depth is balanced; 0 otherwise. */
extern int json_valid(json_t const *j) ;

#endif
