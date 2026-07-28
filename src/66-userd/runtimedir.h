/*
 * runtimedir.h
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

#ifndef USERD_RUNTIMEDIR_H
#define USERD_RUNTIMEDIR_H

#include <sys/types.h>
#include "userd.h"

/**
 * @brief Probe whether @uid's runtime directory is currently a mountpoint.
 *
 * Re-derives, without modifying anything, whether `<base>/<uid>` is currently a
 * tmpfs mountpoint by comparing its `st_dev` (via `lstat`) with that of @base.
 * A mountpoint sits on a different device than the directory that
 * holds it; a bare directory shares its parent's device. This device comparison
 * is the correct test even when @base itself lives on a tmpfs (the real `/run`),
 * where a `TMPFS_MAGIC` `statfs()` check would false-positive on a plain
 * `mkdir`. It is a pure probe — no mount, no umount, no filesystem
 * modification — and is therefore safe to call from the daemon itself during
 * restart reconciliation.
 *
 * @param[in] uid  Owner of the runtime directory to probe.
 * @param[in] base Runtime base directory (e.g. `/run/user`); must not be NULL.
 * @return Mountpoint state, or an error:
 *      - 1 if `<base>/<uid>` is a mountpoint (its device differs from @base's)
 *      - 0 if it is not a mountpoint, including when it does not exist (the
 *        `lstat` of `<base>/<uid>` failed with `ENOENT`)
 *      - -1 on error; errno is set to the errno left by the failing `lstat`
 *        (the `lstat` of `<base>/<uid>` for an error other than `ENOENT`, or the
 *        `lstat` of @base)
 */
extern int runtimedir_ismount(uid_t uid, char const *base) ;

/**
 * @brief Mount the user's runtime directory on the first session (0 -> 1).
 *
 * Mounts `<base>/<uid>` (mkdir + private tmpfs, `MS_NOSUID | MS_NODEV`,
 * `mode=0700,uid=<uid>,gid=<gid>,size=SS_TOOLS_USERD_RUNTIME_SIZE`) iff this REGISTER is the first session for
 * the user — i.e. `u->nsessions == 1`. MUST be called AFTER the
 * caller has incremented `u->nsessions`, and BEFORE the user's 66 services start
 * (they need the runtime directory present). For any later session
 * (`u->nsessions != 1`) it is a no-op, so login is idempotent for an
 * already-online user. As a defensive guard it is also a no-op when
 * `u->runtimedir_up` is already set, so the same user is never mounted twice. On
 * a successful mount it sets `u->runtimedir_up`.
 *
 * @param[in,out] u    The user; read for `nsessions`, `runtimedir_up`, `uid`,
 *                     `gid` and `name`, and `runtimedir_up` is set to 1 on a
 *                     successful mount. Must not be NULL.
 * @param[in]     base Runtime base directory (e.g. `/run/user`); must not be NULL.
 * @return Outcome:
 *      - 1 if the directory was mounted, or the call was a no-op (later session,
 *        or `runtimedir_up` already set)
 *      - 0 if the mount failed; a warning is logged and `u->runtimedir_up` is
 *        left clear. errno is not part of this contract: it reflects whatever the
 *        last syscall left and is not set deterministically here.
 *
 * @note This function never fails on its own logic; a 0 return originates solely
 *       from `mkdir`/`mount`.
 * @see runtimedir_release
 */
extern int runtimedir_register(user_t *u, char const *base) ;

/**
 * @brief Unmount the user's runtime directory on the last session (1 -> 0).
 *
 * Unmounts `<base>/<uid>` (`umount2`, falling back to `MNT_DETACH` on `EBUSY`,
 * then removing the now-empty directory) iff this RELEASE/GC was the last
 * session for the user — i.e. `u->nsessions == 0`. MUST be
 * called AFTER the caller has decremented `u->nsessions`, and AFTER the user's 66
 * services have stopped. While any session remains (`u->nsessions != 0`) it is a
 * no-op. It is also a no-op when `u->runtimedir_up` is clear, so a directory
 * that was never mounted (or already unmounted) is not torn down. On a successful
 * unmount it clears `u->runtimedir_up`.
 *
 * @param[in,out] u    The user; read for `nsessions`, `runtimedir_up`, `uid` and
 *                     `name`, and `runtimedir_up` is cleared to 0 on a successful
 *                     unmount. Must not be NULL.
 * @param[in]     base Runtime base directory (e.g. `/run/user`); must not be NULL.
 * @return Outcome:
 *      - 1 if the directory was unmounted, or the call was a no-op (session still
 *        remains, or `runtimedir_up` already clear)
 *      - 0 if the unmount failed; a warning is logged and `u->runtimedir_up` is
 *        left set. errno is not part of this contract: it reflects whatever the
 *        last syscall left and is not set deterministically here.
 *
 * @note This function never fails on its own logic; a 0 return originates solely
 *       from `umount2`/`dir_destroy`.
 * @see runtimedir_register
 */
extern int runtimedir_release(user_t *u, char const *base) ;

#endif
