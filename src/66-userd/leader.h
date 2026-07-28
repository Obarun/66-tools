/*
 * leader.h
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

#ifndef USERD_LEADER_H
#define USERD_LEADER_H

#include <sys/types.h>

/**
 * @brief Read @pid's start time from `/proc/<pid>/stat` (field 22, clock ticks
 * since boot).
 *
 * Opens `/proc/<pid>/stat`, reads it once, and extracts the 22nd field (start
 * time). The comm field (2nd field) is parenthesized and may itself contain
 * spaces and parentheses, so the numeric fields are located from the LAST `)`
 * in the line (`strrchr`), never the first; start time is the 20th
 * space-separated value after that `)`. A short or empty read is not treated as
 * a distinct success: it surfaces as a parse failure (see `EIO` below).
 *
 * @param[in]  pid Process id to inspect.
 * @param[out] out Receives the start time in clock ticks. Written ONLY on
 *             success (value 1); left untouched on every 0 or -1 return.
 *
 * @return Status code:
 *      - 1 on success; @out is set to the start time
 *      - 0 if the process is absent: `/proc/<pid>/stat` cannot be opened with
 *        `errno == ENOENT` (the pid is gone). @out is untouched.
 *      - -1 on a system error; @out is untouched. errno is set as follows:
 *
 * @retval -1 errno is the underlying `open(2)` errno (forwarded verbatim) when
 *         opening `/proc/<pid>/stat` fails for any reason other than `ENOENT`
 *         (e.g. `EACCES`, `ENFILE`, `EMFILE`).
 * @retval -1 errno is set to `EIO` when the stat line cannot be parsed: no `)`
 *         is present, a field runs off the end of the buffer before field 22,
 *         or field 22 does not scan as a base-10 unsigned integer.
 *
 * @note No check is made that @pid is positive or that it names a real process
 *       beyond the open; a non-existent pid is reported as 0 via the `ENOENT`
 *       path. The function does not distinguish a process that died between the
 *       open and the read.
 */
extern int leader_starttime(pid_t pid, unsigned long *out) ;

/**
 * @brief Reconciliation identity test for a persisted session leader.
 *
 * Opens a pidfd on @pid FIRST, then re-reads the current start time (via
 * `leader_starttime`) and compares it to @persisted: a mismatch means the
 * original leader died and its PID was reused by another process. The pidfd is
 * handed back only when the leader is confirmed alive and identical, ready to
 * be armed in the event loop; in every other case any pidfd opened internally
 * is closed.
 *
 * @param[in]  pid       Process id of the persisted session leader.
 * @param[in]  persisted Start time captured at REGISTER, in the same clock-tick
 *             units as `leader_starttime` produces.
 * @param[out] pidfd_out Set to -1 on entry, before anything else. Receives the
 *             open pidfd ONLY when the function returns 1 (live, matching
 *             leader); left at -1 on every 0 or -1 return. The caller owns the
 *             returned fd and must close it.
 *
 * @return Status code:
 *      - 1 if the leader is alive and identical; @pidfd_out holds an open pidfd
 *      - 0 if the leader is dead or its PID was reused; @pidfd_out is -1
 *      - -1 on a system error; @pidfd_out is -1
 *
 * @retval 0 when `pidfd_open(@pid, 0)` fails with `errno == ESRCH` (the pid is
 *         gone), OR when the pidfd opens but the current start time differs
 *         from @persisted (original leader gone, PID reused), OR when
 *         `leader_starttime` reports the process became absent (its 0 path).
 * @retval -1 errno is the underlying `pidfd_open(2)` errno (forwarded verbatim)
 *         when it fails for any reason other than `ESRCH` (e.g. `EMFILE`,
 *         `ENFILE`, `ENOMEM`, `EINVAL`).
 * @retval -1 errno is whatever `leader_starttime` left on its -1 path (the
 *         `open(2)` errno of `/proc/<pid>/stat`, or `EIO` on a parse failure)
 *         when the pidfd opened but the start-time re-read failed; the pidfd is
 *         closed before returning.
 *
 * @see leader_starttime
 */
extern int leader_check(pid_t pid, unsigned long persisted, int *pidfd_out) ;

#endif
