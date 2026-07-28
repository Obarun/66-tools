/*
 * userenv.h
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

#ifndef USERD_USERENV_H
#define USERD_USERENV_H

#include <sys/types.h>

#include <66-tools/config.h>

/**
 * @brief Inject @uid's stable per-user environment into the calling process's own
 *        environment.
 *
 * Called by the guardian just before it exec()s `66 scandir start` (which exec()s
 * `66-scandir`), so the variables become the inherited environment of the user
 * scandir. The values are SAFE DEFAULTS, not a forced injection: 66 natively
 * merges the user's own `~/.66/environment` on top at scandir start, and that
 * merge WINS, so the user can override any of these freely.
 *
 * The identity fields come from `getpwuid(@uid)`; every variable is set with
 * `setenv(..., 1)` (overwrite), because the guardian's environment is the daemon's
 * (root's) and these MUST replace root's values:
 *      - `HOME`    ← `pw_dir`
 *      - `USER`    ← `pw_name`
 *      - `LOGNAME` ← `pw_name`
 *      - `SHELL`   ← `pw_shell`
 *      - `UID`     ← decimal @uid
 *      - `GID`     ← decimal `pw_gid`
 *      - `XDG_RUNTIME_DIR` ← @runtime_dir
 *      - `PATH`    ← the inherited `PATH` if one is present (left untouched); only
 *        when none is inherited is it set to `SS_TOOLS_USERD_DEFAULT_PATH`
 *      - `DBUS_SESSION_BUS_ADDRESS` ← `SS_TOOLS_USERD_DBUS_ADDR_PREFIX` + @runtime_dir +
 *        `"/bus"` (always set)
 *
 * @param[in] uid          User whose identity is resolved via `getpwuid`; must be a
 *                         user present in the passwd database.
 * @param[in] runtime_dir  Path used verbatim as `XDG_RUNTIME_DIR` and as the base of
 *                         `DBUS_SESSION_BUS_ADDRESS`; must not be NULL.
 *
 * @return Status code:
 *      - 1 on success (all variables set in the caller's environment)
 *      - 0 on failure; errno is set to indicate the error
 *
 * @retval 0 errno is set to ESRCH if `getpwuid(@uid)` fails without setting errno
 *         (i.e. @uid is unknown); if `getpwuid` itself sets errno, that errno is
 *         preserved instead.
 * @retval 0 errno is set to the failing `setenv` errno when any variable cannot be
 *         set (`EINVAL` for an invalid name/value, `ENOMEM` if the environment
 *         cannot be grown).
 *
 * @note Modifies the calling process's environment in place; it is meant to run in
 *       the short-lived guardian child, not in the long-running daemon.
 * @note @runtime_dir is dereferenced unconditionally (no NULL check); passing NULL
 *       is a programming error.
 */
extern int userenv_build(uid_t uid, char const *runtime_dir) ;

#endif
