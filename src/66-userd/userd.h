/*
 * userd.h
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

#ifndef USERD_USERD_H
#define USERD_USERD_H

#include <sys/types.h>
#include <stdint.h>

#include <oblibs/hash.h>
#include <oblibs/sse.h>

#include "constants.h"

/**
 * @brief Lifecycle state of a tracked user, in `user_t.state`.
 *
 * The enumerator order matches the index order of the string table in
 * `session.c`, so `user_state_str()` and `user_state_from()` round-trip these
 * values to/from their textual form.
 */
typedef enum user_state_e {
    USER_STATE_OFFLINE = 0,   // no scandir driven yet (initial state from user_new())
    USER_STATE_OPENING,       // guardian/scandir start initiated
    USER_STATE_ONLINE,        // scandir up, sessions live
    USER_STATE_CLOSING        // last session gone, tear-down in progress
} user_state_e ;

/**
 * @struct user_s
 * @brief Per-user accounting record tracked by the daemon, keyed by uid.
 *
 * One record exists per uid that currently has at least one tracked session.
 * The record is heap-allocated by `user_new()`, inserted into the daemon's
 * hash table on its embedded `node`, and freed by `user_free()`. The address of
 * the record is stable for its whole lifetime (the hash never moves the node),
 * which is why the embedded event-loop watcher `guardianw` can be armed by
 * address.
 *
 * The live session count drives the policy: the first session brings the user
 * `ONLINE` and starts its guardian/scandir; the last session removed triggers
 * tear-down.
 *
 * @param uid
 * The user's numeric id. This is the hash key (set by `user_new()`).
 *
 * @param gid
 * The user's primary group id, as resolved from the password database.
 * Zero-initialized by `user_new()`; populated by the daemon.
 *
 * @param name
 * The user's login name, NUL-terminated, at most `USER_NAME_MAX - 1` bytes.
 * Zero-initialized (empty) by `user_new()`; populated by the daemon.
 *
 * @param nsessions
 * Count of live sessions for this user. Drives the first-start (0 -> 1) and
 * last-stop (1 -> 0) transitions.
 *
 * @param state
 * Lifecycle state (see `user_state_e`). Initialized to `USER_STATE_OFFLINE`
 * by `user_new()`.
 *
 * @param runtimedir_up
 * Boolean flag: non-zero once `/run/user/UID` has been created for this user.
 *
 * @param scandir_up
 * Boolean flag: non-zero while the user's 66 scandir is up. The daemon tracks
 * this via the guardian's liveness.
 *
 * @param guardian_pid
 * Pid of the per-user supervised guardian process, or 0 when none is running.
 *
 * @param guardian_starttime
 * The guardian's process start time in clock ticks (`/proc/<pid>/stat` field
 * 22), used to detect pid reuse across daemon restarts. 0 when unknown.
 *
 * @param guardianfd
 * pidfd opened on `guardian_pid` for supervision in the event loop, or -1 when
 * none. Initialized to -1 by `user_new()`.
 *
 * @param guardianw
 * Event-loop watcher (sse) arming `guardianfd` so the daemon is woken when the
 * guardian dies. Valid only while `guardianfd >= 0`.
 *
 * @param readyw
 * Event-loop watcher (sse) owning the readiness eventfd — its descriptor lives in
 * `readyw.fd`. Armed just before the guardian is forked (which inherits the fd) and
 * freed as soon as the guardian signals it, so it is one-shot: `readyw.fd < 0` means
 * either not armed yet or already consumed. A guardian re-adopted after a daemon
 * restart has no readiness channel at all — the eventfd died with the old daemon —
 * so its state is re-probed with `driver_scandir_ok()` instead.
 *
 * @param timestamp
 * Wall-clock timestamp of the user's most recent state change.
 *
 * @param node
 * Intrusive hash node; the record is indexed by `uid`.
 */
typedef struct user_s user_t, *user_t_ref;
struct user_s {
    uid_t uid ;                  // hash key
    gid_t gid ;
    char name[USER_NAME_MAX] ;   // login name
    uint32_t nsessions ;         // live session count (drives first-start/last-stop)
    user_state_e state ;
    uint8_t runtimedir_up ;     // /run/user/UID created (set from milestone 4)
    uint8_t scandir_up ;         // 66 scandir up, tracked via the guardian's liveness
    pid_t guardian_pid ;         // per-user supervised guardian pid, 0 if none
    unsigned long guardian_starttime ; // guardian start time (ticks, /proc/<pid>/stat field 22)
    int guardianfd ;             // pidfd on the guardian for supervision, -1 if none
    sse_watcher_t guardianw ;    // event-loop watcher arming guardianfd
    sse_watcher_t readyw ;       // one-shot readiness eventfd (descriptor in readyw.fd)
    uint64_t timestamp ;
    hash_node_t node ;           // intrusive hash node: indexed by uid
};

// user.c

/**
 * @brief Allocate and initialize a user record for @uid.
 *
 * Zero-fills a fresh `user_t` (so `gid`, `name`, `nsessions`, flags, guardian
 * pid/start time, timestamp and the hash node all start cleared), then sets
 * `uid` to @uid, `state` to `USER_STATE_OFFLINE`, and both `guardianfd` and
 * `readyw.fd` to -1 (a zero-fill would leave the latter at 0, a valid fd). The
 * record is not inserted into any hash table; the caller owns it and must
 * eventually release it with `user_free()`.
 *
 * @param[in] uid  The user's numeric id to store as the record's key.
 *
 * @return Pointer to the new `user_t` on success.
 * @return NULL on allocation failure; errno is set to ENOMEM.
 */
extern user_t *user_new(uid_t uid) ;

/**
 * @brief Free a user record allocated by `user_new()`.
 *
 * Releases the heap allocation. This does NOT close `guardianfd`, does NOT free
 * the `guardianw` watcher, and does NOT unlink the record from any hash table:
 * the caller is responsible for tearing those down beforehand.
 *
 * @param[in] u  Record to free. May be NULL, in which case the call is a no-op.
 *
 * @note Always succeeds; no return value, errno untouched.
 */
extern void user_free(user_t *u) ;

// String tables (string <-> enum), defined in session.c.

/**
 * @brief Return the textual name of a user state.
 *
 * Maps each `user_state_e` to its lowercase string: `USER_STATE_OFFLINE` ->
 * "offline", `USER_STATE_OPENING` -> "opening", `USER_STATE_ONLINE` ->
 * "online", `USER_STATE_CLOSING` -> "closing". The returned pointer refers to a
 * static, read-only string with program lifetime; the caller must not free or
 * modify it.
 *
 * @param[in] st  A valid `user_state_e` value (0..3).
 *
 * @return Pointer to the corresponding static string.
 *
 * @note Performs no bounds check: passing a value outside the defined enumerator
 *       range reads past the table and is undefined behaviour. Never sets errno.
 */
extern char const *user_state_str(user_state_e st) ;

/**
 * @brief Parse a user state name into its enum value.
 *
 * Linear, case-sensitive match against the same names as `user_state_str()`.
 *
 * @param[in] s  NUL-terminated state name to look up. Must not be NULL (it is
 *               dereferenced by the comparison).
 *
 * @return The matching `user_state_e` on an exact match.
 * @retval USER_STATE_OFFLINE returned for any string that matches no known state
 *         (the unmatched case is mapped to OFFLINE, not reported as an error).
 *
 * @note Never sets errno. There is no distinct error value: a return of
 *       `USER_STATE_OFFLINE` means either an explicit "offline" or an unknown
 *       string.
 * @see user_state_str
 */
extern user_state_e user_state_from(char const *s) ;

/**
 * @brief Run one client transaction against the userd daemon socket.
 *
 * Opens a fresh connection to the Unix socket at @path, writes one request frame
 * (`opcode` + @plen payload bytes), reads exactly one reply frame, and closes the
 * connection. The reply opcode is returned in @rop and the reply payload
 * (never NUL-terminated on the wire) is copied into @rbuf with its byte count in @rlen.
 *
 * @param[in] path    Daemon socket path to connect to.
 * @param[in] opcode  Request opcode (`PROTO_*` client-to-daemon).
 * @param[in] payload Request payload bytes; may be NULL when @plen is 0.
 * @param[in] plen    Number of payload bytes to send.
 * @param[out] rop    Receives the reply opcode.
 * @param[out] rbuf   Receive buffer for the reply payload; must hold @rcap bytes.
 * @param[in] rcap    Capacity of @rbuf. A reply larger than @rcap is rejected.
 * @param[out] rlen   Receives the reply payload byte count.
 *
 * @return 1 on a completed transaction (@rop, @rbuf, @rlen set).
 * @return 0 on any failure (socket create/connect, short read/write, or a reply
 *         exceeding @rcap); the connection is closed and errno is left by the
 *         failing call.
 */
extern int userd_call(char const *path, uint16_t opcode, char const *payload, uint16_t plen, uint16_t *rop, char *rbuf, uint16_t rcap, uint16_t *rlen) ;

#endif
