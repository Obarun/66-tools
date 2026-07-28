/*
 * driver.h
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

#ifndef USERD_DRIVER_H
#define USERD_DRIVER_H

#include <sys/types.h>

#include "userd.h"

/**
 * @brief Fork the per-user guardian.
 *
 * The daemon forks once; the child becomes the guardian. The guardian `setsid`s,
 * builds the per-user environment, then forks the scandir child (which becomes
 * `66-scandir` with the readiness pipe), gates on readiness — an EOF on the pipe
 * or the svscan dying first means the supervisor failed — runs `66 tree start`,
 * then signals @readyfd and supervises the svscan until it dies or a stop is
 * requested via `SIGTERM` (consumed through a signalfd so it is never lost to a
 * race), on which it runs `66 tree stop` then `66 scandir quit` in that order
 * (the trees need the scandir up to be stopped) and exits. The guardian stays a
 * DIRECT child of the daemon (not reparented) so the daemon pidfd-supervises and
 * reaps it; the daemon never blocks on readiness — that lives entirely inside the
 * guardian.
 *
 * This call only performs the single `fork(2)` in the daemon and returns
 * immediately; all the choreography above runs in the child and is not observable
 * through the return value.
 *
 * @param[in] uid UID the guardian brings up.
 * @param[in] readyfd Readiness eventfd, written once the trees are up so the daemon
 *                can promote the user to `USER_STATE_ONLINE`; -1 disables the
 *                notification. The guardian owns its inherited copy and closes it
 *                after writing. A write failure is deliberately NOT fatal: the user
 *                simply stays `USER_STATE_OPENING` until a reconcile re-probes it.
 * @return The guardian pid (> 0) on success.
 * @return -1 on a `fork(2)` error in the daemon (errno set to the `fork(2)` errno,
 *         e.g. EAGAIN or ENOMEM). No errno path other than `fork(2)`'s exists here;
 *         every later failure happens in the child and surfaces only as the
 *         guardian exiting, which the daemon observes via its pidfd.
 */
extern pid_t driver_guardian_spawn(uid_t uid, int readyfd) ;

/**
 * @brief Request an ordered tear-down of a guardian.
 *
 * Sends `SIGTERM` to @guardian_pid. The guardian stops the trees, quits the
 * scandir, then exits; the daemon's pidfd watcher fires on that exit and finishes
 * the cleanup. This call only delivers the signal;
 * it does not wait for the tear-down.
 *
 * @param[in] guardian_pid Pid of the guardian to signal.
 * @return 1 if the `SIGTERM` was sent.
 * @return 0 if @guardian_pid <= 0 (rejected before signalling; errno is NOT set on
 *         this path), or if `kill(2)` failed (errno set to the `kill(2)` errno,
 *         e.g. ESRCH if the guardian is already gone or EPERM).
 */
extern int driver_guardian_stop(pid_t guardian_pid) ;

/**
 * @brief Probe whether @uid's 66 scandir is currently up.
 *
 * Re-derives liveness by building `<SS_LIVE>/scandir/<uid>` (via `set_livescan`,
 * which resolves 66's compiled-default livedir) and calling `svc_scandir_ok` on it,
 * i.e. opening the `66-scandir` control fifo write-only/non-blocking. A
 * pure probe — no fork, no privilege change, no start/stop — so it is safe to call
 * from the daemon itself during restart reconciliation.
 *
 * @param[in] uid UID whose scandir is probed.
 * @return 1 if the scandir is up (the control fifo opened).
 * @return 0 if the scandir is down (`svc_scandir_ok` got ENXIO/ENOENT opening the
 *         control fifo). errno is left as that opening errno, not cleared.
 * @retval -1 on error. This covers two distinct paths, each leaving a different
 *         errno: `set_livescan` returned <= 0 — either `set_livedir` failed/returned
 *         0, or the `auto_strbuf` that appends `scandir/<uid>` failed; this function
 *         does not set errno itself on this path, so errno is whatever that internal
 *         call left; or `svc_scandir_ok` could not open the control fifo for a reason
 *         other than ENXIO/ENOENT (errno is that `open(2)` errno, e.g. EACCES). errno
 *         is not set to a single fixed code; it holds the source-dependent value.
 */
extern int driver_scandir_ok(uid_t uid) ;

/**
 * @brief Start the user's 66 services iff this REGISTER is the first session.
 *
 * POLICY layer. Acts only on the `nsessions` transition 0 -> 1:
 * it MUST be called AFTER the caller incremented `u->nsessions`, so the first
 * session is the one that brought the count to 1. For every later session
 * (`u->nsessions != 1`) it is a no-op, making login idempotent for an already-online
 * user. It is also a no-op when `u->scandir_up` is already set (defensive: never a
 * second guardian for the same user; a re-login during tear-down is absorbed by the
 * dying guardian's death callback). On the actual transition it calls
 * driver_guardian_spawn(); on success (pid > 0) it records the guardian pid in
 * `u->guardian_pid`, sets `u->scandir_up`, and moves `u` to `USER_STATE_OPENING`,
 * after which the daemon arms the guardian's pidfd supervision and waits for
 * @readyfd to promote the user to `USER_STATE_ONLINE`.
 *
 * @param[in,out] u   The user. `u->nsessions` is read; on a successful start
 *                    `u->guardian_pid`, `u->scandir_up` and `u->state` are written.
 * @param[in] readyfd Readiness eventfd handed to the guardian, or -1 for none. It is
 *                    only consumed on the actual transition: when this call is a
 *                    no-op the caller still owns it and must release it, since no
 *                    guardian will ever signal it.
 * @return 1 if a guardian was started, or the call was a no-op (later session, or
 *         scandir already up). No errno is set by this function on the 1 paths.
 * @return 0 if the fork failed — driver_guardian_spawn() returned <= 0.
 *         `u->scandir_up`, `u->guardian_pid` and `u->state` are left untouched
 *         (clear). errno is not set by this function; it holds the `fork(2)` errno.
 */
extern int driver_register(user_t *u, int readyfd) ;

/**
 * @brief Request tear-down of the user's 66 services iff this RELEASE/GC was the
 *        last session.
 *
 * POLICY layer. Acts only on the `nsessions` transition 1 -> 0: it
 * MUST be called AFTER the caller decremented `u->nsessions`. While any session
 * remains (`u->nsessions != 0`) it is a no-op. It is also a no-op when
 * `u->scandir_up` is clear (nothing was ever started, or it is already stopped). On
 * the transition it calls driver_guardian_stop(); on success it moves `u` to
 * `USER_STATE_CLOSING`. `u->scandir_up` is deliberately NOT cleared here — it stays
 * set until the guardian's death is observed, at which point the daemon's pidfd
 * watcher clears it and finishes the cleanup.
 *
 * @param[in,out] u   The user. `u->nsessions`, `u->scandir_up` and
 *                    `u->guardian_pid` are read; on a successful stop `u->state` is
 *                    set to `USER_STATE_CLOSING`.
 * @return 1 if the tear-down was requested, or the call was a no-op (sessions
 *         remain, or scandir not up). No errno is set by this function on the 1
 *         paths.
 * @return 0 if driver_guardian_stop() returned 0 (signal failed or bad guardian
 *         pid). `u->state` is left unchanged. errno is not set by this function; it
 *         holds the `kill(2)` errno, or nothing if the pid was <= 0.
 */
extern int driver_release(user_t *u) ;

#endif
