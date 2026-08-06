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
 * builds the per-user environment, then either adopts an already running scandir or
 * starts one.
 *
 * ADOPTION: a scandir may already be up for @uid — a previous guardian died and left
 * its own behind, or something else started one. `66 scandir start` is idempotent and
 * returns success WITHOUT exec'ing in that case, so forking for it would leave the
 * child with nothing to become. The guardian therefore probes with
 * driver_scandir_ok() first and, when one is up, resolves it with
 * driver_scandir_pidfd(), which hands back a pidfd already bound to the process reading
 * the control fifo, and skips the readiness gate. An adopted scandir is already ready.
 * It refuses to proceed if that reader cannot be identified. An adopted scandir is not
 * a child of the guardian: its death is seen through the pidfd and it is never reaped,
 * since that belongs to init.
 *
 * Otherwise it forks the scandir child (which becomes `66-scandir` with the readiness
 * pipe), gates on readiness — an EOF on the pipe
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
 * @brief Resolve @uid's running 66-scandir and pin it behind a pidfd.
 *
 * A scandir publishes no pidfile, so it is identified by the process that holds its
 * control fifo (`<SS_LIVE>/scandir/<uid>` + `SS_SVSCAN` + `USERD_CONTROL`) open **for
 * reading**: the fifo's `(st_dev, st_ino)` pair is compared against every descriptor of
 * every candidate, and the access mode read from `/proc/<pid>/fdinfo/<n>` settles which
 * side of the pipe it is. Matching on `argv` instead would rely on a command line that
 * is not a contract.
 *
 * The read end is what distinguishes the supervisor from its callers: every client of
 * the control protocol opens the same fifo WRITE-ONLY to talk to it (`svc_scandir_send`,
 * and `svc_scandir_ok` which driver_scandir_ok() itself uses), so a plain `(dev, ino)`
 * match would also return the pid of a `66 scandir` command in flight. The scandir holds
 * both ends — it keeps a writer open so the fifo never reaches EOF — hence every
 * descriptor of a candidate is examined rather than stopping at the first hit.
 *
 * Only processes whose EFFECTIVE uid is @uid are considered, and that test runs before
 * any descriptor is looked at: adopting a scandir owned by another user would hand this
 * user's session over to a foreign process.
 *
 * The returned pidfd is what makes the answer usable: a bare pid would reopen the race
 * the scan just closed, since the process could die and its pid be recycled before the
 * caller signals it. The pid is re-checked behind the pidfd before returning, so the
 * descriptor is bound to the process that really reads the fifo. It is `FD_CLOEXEC` and
 * belongs to the caller, which must `close()` it.
 *
 * A pure probe — no fork, no signal, no privilege change — but it walks `/proc`, so it
 * must run while the caller can still read other processes' descriptors, i.e. before
 * dropping privileges.
 *
 * @param[in] uid UID whose scandir is looked up.
 * @param[out] pid_out Set to the pid on success, 0 otherwise. Never NULL.
 * @param[out] pidfd_out Set to the pidfd on success, -1 otherwise. Never NULL.
 * @return 1 on success; @pid_out and @pidfd_out are set.
 * @return 0 if no process of @uid reads the control fifo, or if the fifo itself is
 *         absent (`ENOENT` from `stat(2)`). A scandir that is up but whose reader
 *         cannot be seen also lands here — the caller must not read this as "nothing is
 *         running", only as "nothing that can be supervised".
 * @retval -1 on error, with errno set in every case: ENOMEM if `set_livescan()` failed,
 *         the `stat(2)` errno if the fifo could not be stat'ed for a reason other than
 *         ENOENT, the `opendir(3)` errno if `/proc` could not be opened, or the
 *         `readdir(3)` errno if the walk itself failed.
 */
extern int driver_scandir_pidfd(uid_t uid, pid_t *pid_out, int *pidfd_out) ;

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
