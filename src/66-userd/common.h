/*
 * common.h
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

#ifndef USERD_COMMON_H
#define USERD_COMMON_H

#include <stdint.h>

#include <oblibs/strbuf.h>

/**
 * @brief Append the envfile field `KEY=value\n` for a string value to @sb.
 *
 * A NULL or empty @val is treated as "field absent": the function appends nothing
 * and reports success. Otherwise the field is appended to @sb (which keeps growing;
 * existing content is preserved).
 *
 * The block is read back through `environ_merge_*` (envfile grammar), so a @val
 * that contains a character the grammar would misparse — a double quote (`"`) or a
 * backslash (`\`) — is emitted quoted: wrapped in double quotes with each `"` and
 * `\` backslash-escaped. A @val with none of those characters is emitted bare
 * (human-readable; the only fields a bare reader — the PAM reply parser — consumes
 * never contain such characters).
 *
 * @param[in,out] sb  Destination buffer. Must not be NULL. Appended to in place;
 *                    left NUL-terminated with `sb->len` excluding the NUL.
 * @param[in] key     Field name written verbatim before `=`. Must not be NULL.
 * @param[in] val     Field value. May be NULL or empty (field is then omitted).
 *
 * @return 1 on success (field appended, or @val NULL/empty and nothing appended).
 * @return 0 on failure to grow @sb. errno is left by the underlying allocator:
 *         ENOMEM when a `malloc`/`realloc` for the buffer fails. (The `strbuf`
 *         size-overflow path that sets errno to ERANGE is unreachable here: the
 *         appended bytes are bounded by a few KEY/value strings.)
 *
 * @note NULL-safe for @val only (NULL @val is a documented no-op). @sb and @key
 *       are dereferenced unconditionally; passing NULL for either is undefined.
 * @see sf_u64
 */
extern int sf_str(strbuf *sb, char const *key, char const *val) ;

/**
 * @brief Append the envfile field `KEY=<decimal>\n` for a 64-bit value to @sb.
 *
 * Formats @v in base 10 (no sign, no padding) into a fixed `U64_FMT`-sized stack
 * buffer that always fits a `uint64_t`, then appends `KEY=<decimal>\n` to @sb.
 * @sb keeps growing; existing content is preserved. Every @v (including 0) yields
 * a field; there is no "absent" case for numeric fields.
 *
 * @param[in,out] sb  Destination buffer. Must not be NULL. Appended to in place;
 *                    left NUL-terminated with `sb->len` excluding the NUL.
 * @param[in] key     Field name written verbatim before `=`. Must not be NULL.
 * @param[in] v       Value formatted as an unsigned base-10 integer.
 *
 * @return 1 on success (field appended).
 * @return 0 on failure to grow @sb. errno is left by the underlying allocator:
 *         ENOMEM when a `malloc`/`realloc` for the buffer fails. (The `strbuf`
 *         size-overflow path that sets errno to ERANGE is unreachable here.)
 *
 * @note @sb and @key are dereferenced unconditionally; passing NULL for either
 *       is undefined.
 * @see sf_str
 */
extern int sf_u64(strbuf *sb, char const *key, uint64_t v) ;

#endif
