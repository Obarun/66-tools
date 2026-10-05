/*
 * ctx.h
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

#ifndef WEB66_CTX_H
#define WEB66_CTX_H

#include <stddef.h>
#include <sys/types.h>

#include <66/constants.h>

#define SERVICE_SRC_DIRS_MAX 3

/**
 * Process-wide context: the uid the daemon runs as, and the matching
 * 66 base directory. Resolved once at startup, immutable afterwards,
 * consulted by every API handler that touches lib66.
 */
typedef struct ctx_s ctx_t ;
struct ctx_s {
	uid_t owner ;
	char base[SS_MAX_PATH_LEN + 1] ; // e.g. "/etc/66" or "/home/user/.66"
} ;

/**
 * @brief Initialise the global context from the calling uid.
 *
 * Resolves the base directory according to the running uid (system
 * scope for root, per-user scope otherwise) and stores it for the
 * lifetime of the process.
 *
 * @param[out] c Context to initialise.
 * @return 1 on success, 0 on failure (errno set, message logged).
 */
extern int ctx_init(ctx_t *c) ;

/* Accessor used by handlers that need the running 66 base. */
extern ctx_t const *ctx_get(void) ;

/**
 * @brief Writable frontend source directory for the given uid.
 *
 * root  → "/etc/66/service/" ; user → "$HOME/.66/service/". Trailing
 * slash included, so a service name can be appended directly. This is
 * the directory into which a NEW frontend is written — deliberately
 * distinct from ctx.base (the resolve/state base).
 *
 * @param[in]  owner uid the daemon runs as (0 = root).
 * @param[out] stack Buffer of at least SS_MAX_PATH_LEN + 1 bytes.
 * @return 1 on success, 0 on failure (errno set, e.g. no home dir).
 *         Never exits the process.
 */
extern int service_src_dir(uid_t owner, char *stack) ;

/**
 * @brief Ordered read-precedence list of frontend source directories for the uid.
 *
 * root → { /etc/66/service/, /usr/share/66/service/ } (the "user" subdir is
 * excluded by the caller); user → { $HOME/.66/service/, /etc/66/service/user/,
 * /usr/share/66/service/user/ }. Each entry ends with '/'.
 *
 * The distro dirs (`/usr/share/...`) are package-managed: `removable[i]` is set
 * to 0 for them and 1 for the admin/user dirs. A caller must still confirm write
 * access before deleting — a user cannot unlink in the root-owned admin dir.
 *
 * @param[in]  owner     uid the daemon runs as (0 = root).
 * @param[out] paths     Filled with up to SERVICE_SRC_DIRS_MAX pointers (string
 *                       literals, plus one into `store` for the user home path).
 * @param[out] removable Per-dir flag: 1 if editable (admin/user), 0 if distro.
 * @param[out] n         Number of entries written.
 * @param[out] store     Scratch buffer (>= SS_MAX_PATH_LEN + 1) backing the home path.
 * @return 1 on success, 0 on failure (e.g. no home dir). Never exits.
 */
extern int service_src_dirs(uid_t owner, char const *paths[SERVICE_SRC_DIRS_MAX], int removable[SERVICE_SRC_DIRS_MAX], size_t *n, char *store) ;

#endif
