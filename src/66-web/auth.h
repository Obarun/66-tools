/*
 * auth.h
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

#ifndef WEB66_AUTH_H
#define WEB66_AUTH_H

#include <stddef.h>

#define AUTH_USER_MAX 256
#define AUTH_SALT_LEN 16
#define AUTH_HASH_LEN 32

typedef struct auth_cred_s auth_cred_t ;
struct auth_cred_s {
	char user[AUTH_USER_MAX] ;   // NUL-terminated
	unsigned char salt[AUTH_SALT_LEN] ;
	unsigned char hash[AUTH_HASH_LEN] ;
} ;

/**
 * @brief Load the credential file.
 *
 * The file holds exactly one line:
 *
 *     user:<salt_hex(32)>:<hash_hex(64)>\n
 *
 * The trailing newline is optional; anything else after the line is a
 * malformation. Nothing read from the file is ever logged.
 *
 * @param[in]  path Credential file path.
 * @param[out] cred Filled on success, untouched otherwise.
 * @return 1 if a well-formed credential was loaded, 0 if the file is
 *         absent, empty or malformed (errno set on I/O failure).
 */
extern int auth_load(char const *path, auth_cred_t *cred) ;

/**
 * @brief Verify a user/password pair against the loaded credential.
 *
 * The derivation always runs, and both the user name and the digest are
 * compared in constant time, so neither a wrong user nor a wrong password
 * is distinguishable from the other by timing.
 *
 * @param[in] cred Loaded credential.
 * @param[in] user NUL-terminated user name from the request.
 * @param[in] pass NUL-terminated password from the request.
 * @return 1 if both match, 0 otherwise.
 */
extern int auth_check(auth_cred_t const *cred, char const *user, char const *pass) ;

/**
 * @brief Prompt for a user name and password, then write the credential file.
 *
 * Reads "Username", "Password" and "Confirm password" from stdin, one line
 * each. When stdin is a tty, terminal echo is disabled for the two password
 * lines and restored on every exit path; when it is not (a pipe), the tty is
 * left alone. A mismatching confirmation, an empty password, an empty user
 * name or a user name carrying ':' are refused.
 *
 * The salt is 16 bytes from /dev/urandom. The file is written to a temporary
 * name in the same directory then renamed over `path`, so a reader never sees
 * a half-written credential; its mode is 0600, and a missing parent directory
 * is created 0700. The password is never printed nor logged.
 *
 * @param[in] path Credential file path.
 * @return 1 on success, 0 on failure (errno set, message logged).
 */
extern int auth_passwd_interactive(char const *path) ;

/**
 * @brief Test whether a host string names the loopback interface.
 *
 * Recognises "localhost", every 127.0.0.0/8 address in dotted-quad form,
 * and the IPv6 loopback written "::1" or "[::1]".
 *
 * @param[in] host Host part of a listen address.
 * @return 1 if loopback, 0 otherwise.
 */
extern int host_is_loopback(char const *host) ;

#endif
