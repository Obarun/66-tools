/*
 * jsonparse.h
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

#ifndef WEB66_JSONPARSE_H
#define WEB66_JSONPARSE_H

#include <stddef.h>

typedef enum jp_type_e {
	JP_NULL, JP_BOOL, JP_NUM, JP_STR, JP_ARR, JP_OBJ
} jp_type ;

typedef struct jp_value_s jp_value ;
struct jp_value_s {
	jp_type type ;
	union {
		int b ; // JP_BOOL
		double num ; // JP_NUM
		struct { char *s ; size_t len ; } str ; // JP_STR (unescaped, NUL-terminated)
		struct { jp_value **items ; size_t n ; } arr ;
		struct { char **keys ; jp_value **vals ; size_t n ; } obj ;
	} u ;
} ;

/**
 * @brief Parse a JSON document.
 * @param[in] s   Bytes to parse (need not be NUL-terminated).
 * @param[in] len Number of bytes.
 * @return Owned value tree on success (free with ::jp_free).
 * @return NULL with errno EPROTO when the document is not accepted: a syntax
 *         error, \u0000 or a lone surrogate in a string, invalid UTF-8, a
 *         duplicate key, more than 64 nested containers, more than 64
 *         members in one object, or a number of 64 bytes or more.
 * @return NULL with errno ENOMEM on allocation failure, EINVAL if s is NULL.
 */
extern jp_value *jp_parse(char const *s, size_t len) ;

/** @brief Free a value tree returned by ::jp_parse (NULL-safe). */
extern void jp_free(jp_value *v) ;

/** @brief Object member lookup; NULL if v is not an object or key is absent. */
extern jp_value const *jp_get(jp_value const *v, char const *key) ;

/** @brief String value, or NULL if v is not a JP_STR. */
extern char const *jp_string(jp_value const *v) ;

/** @brief 1 if v is a JP_BOOL set to true, else 0. */
extern int jp_is_true(jp_value const *v) ;

/** @brief Array length, or 0 if v is not a JP_ARR. */
extern size_t jp_arr_len(jp_value const *v) ;

/** @brief Array element i, or NULL if out of range / not an array. */
extern jp_value const *jp_arr_get(jp_value const *v, size_t i) ;

#endif
