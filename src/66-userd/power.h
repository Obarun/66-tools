/*
 * power.h
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

#ifndef USERD_POWER_H
#define USERD_POWER_H

/**
 * @brief The power actions userd can dispatch, in wire order.
 *
 * The numeric value is the index into the internal wire-name / `66-hpr`-flag
 * tables (see power_action_str() and power_action_from()), so the order here is
 * load-bearing and must not be reordered.
 */
typedef enum power_action_e power_action_t ;
enum power_action_e {
    POWER_POWEROFF = 0,     // 66-hpr -p ; writes the 66-shutdownd fifo
    POWER_REBOOT,           // 66-hpr -r ; writes the 66-shutdownd fifo
    POWER_HALT,             // 66-hpr -h ; writes the 66-shutdownd fifo
    POWER_SUSPEND,          // 66-hpr -s ; writes /sys/power/state, blocks until resume
    POWER_HIBERNATE         // 66-hpr -i ; writes /sys/power/state, blocks until resume
} ;

/**
 * @brief Why power_policy() refused, or that it allowed.
 */
typedef enum power_policy_e power_policy_t ;
enum power_policy_e {
    POWER_ALLOW = 0,
    POWER_DENY_NO_SESSION, // caller has no active local session
    POWER_DENY_NOT_ALLOWED, // shutdown.allow exists and the caller is not in it
    POWER_DENY_OTHER_USERS // other users are logged in and --force was not given
} ;

/**
 * @brief Perform @action on the machine by running `66-hpr`.
 *
 * Double-forks and execs `66-hpr <flag> -l <SS_LIVE>` in the grandchild (the inner
 * fork+exec goes through oblibs spawn_path, so a failed exec is caught), reaping
 * only the short-lived intermediate child so userd never blocks and never leaves a
 * zombie. userd never calls reboot(2) itself. Success means "request dispatched",
 * not "completed" (the grandchild may block in /sys/power/state until resume).
 *
 * @param[in] action The action to dispatch.
 * @return 1 when the request was dispatched (the intermediate child exited 0).
 * @return 0 on failure: the intermediate `fork(2)` failed (errno set by fork), or
 *         the grandchild could not be spawned / `66-hpr` could not be exec'd (errno
 *         is NOT propagated to the caller on that second path).
 */
extern int power_trigger(power_action_t action) ;

/**
 * @brief Parse a wire action name into its `power_action_t`.
 *
 * Matches @s, by exact `strcmp`, against the wire names in order: "poweroff",
 * "reboot", "halt", "suspend", "hibernate".
 *
 * @param[in] s Wire name to parse. Must not be NULL (it is passed straight to
 *              `strcmp`; a NULL @s is undefined behaviour, not a handled error).
 * @return The matching `power_action_t` as an int in [0, 4] (POWER_POWEROFF ..
 *         POWER_HIBERNATE).
 * @return -1 if @s matches none of the five names. errno is not set on this path.
 */
extern int power_action_from(char const *s) ;

/**
 * @brief The canonical wire name of @action.
 *
 * @param[in] action Action whose name is wanted.
 * @return A static, NUL-terminated string ("poweroff", "reboot", "halt",
 *         "suspend" or "hibernate") for @action in [0, POWER_HIBERNATE].
 * @return The empty string "" if @action > POWER_HIBERNATE.
 * @note Only the UPPER bound is checked: a negative @action (only reachable by
 *       casting an out-of-range value to the enum) indexes the table out of
 *       bounds and is undefined behaviour, not "". Every value of
 *       `power_action_t` produced by this API is in range, so this is safe for
 *       all defined inputs.
 */
extern char const *power_action_str(power_action_t action) ;

/**
 * @brief Decide whether the caller may power the machine off.
 *
 * Pure and total: every fact is passed in, so the whole decision matrix is
 * unit-testable without a daemon, and it never fails or sets errno. The order is
 * logind-shaped (no D-Bus): root is always allowed; otherwise the caller needs an
 * active LOCAL session; then, if a shutdown.allow whitelist exists, the caller
 * must be in it; finally, if other users are logged in, --force is required.
 *
 * @param[in] is_root           Non-zero iff the caller's uid == 0.
 * @param[in] has_local_session Non-zero iff the caller has an active, non-remote
 *                              session.
 * @param[in] allowed           Non-zero iff the caller passes the whitelist gate
 *                              (listed in shutdown.allow, OR the file does not
 *                              exist = no restriction).
 * @param[in] has_other_users   Non-zero iff some OTHER user has an active session.
 * @param[in] force             Non-zero iff the caller passed --force.
 * @return The verdict, always one of:
 *         - POWER_ALLOW if @is_root, or if every non-root gate passes.
 *         - POWER_DENY_NO_SESSION if not root and !@has_local_session.
 *         - POWER_DENY_NOT_ALLOWED if not root, has a local session, but
 *           !@allowed.
 *         - POWER_DENY_OTHER_USERS if not root, has a local session, is allowed,
 *           but @has_other_users and !@force.
 * @note Never fails; errno is untouched.
 */
extern power_policy_t power_policy(int is_root, int has_local_session, int allowed, int has_other_users, int force) ;

/**
 * @brief Human-readable reason for a verdict (the PROTO_ERROR payload).
 *
 * @param[in] verdict Verdict to describe.
 * @return A static, NUL-terminated string: "no active local session" for
 *         POWER_DENY_NO_SESSION, "not authorized to power off" for
 *         POWER_DENY_NOT_ALLOWED, "other users are logged in (use --force)" for
 *         POWER_DENY_OTHER_USERS, and "allowed" for POWER_ALLOW or any value not
 *         listed above (the `default` arm). Never NULL; never fails.
 */
extern char const *power_policy_str(power_policy_t verdict) ;

#endif
