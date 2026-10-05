/*
 * action.h
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

#ifndef WEB66_ACTION_H
#define WEB66_ACTION_H

#include <stddef.h>

#include <oblibs/sse.h>

/** Bytes an action line needs, NULs included. The longest one is
 * `66 -t <tree> enable -S <name>`: 3 + 3 + 256 + 7 + 3 + 256 = 528, a tree
 * and a service name passing ::action_name_valid, so shorter than 256 bytes. */
#define ACTION_ARGV_BYTES 1024

/**
 * @brief The command line of one action, held by value.
 *
 * The arguments are packed one after the other in `buf`, each NUL-terminated
 * ("66\0start\0db\0"). No pointer points inside the structure, so it can be
 * copied or embedded freely; ::action_start rebuilds the argv array itself.
 */
typedef struct action_argv_s action_argv_t ;
struct action_argv_s {
	char buf[ACTION_ARGV_BYTES] ;
	size_t len ;
	size_t n ;
} ;

#define ACTION_ARGV_ZERO { { 0 }, 0, 0 }

/**
 * @brief Called once a 66 action has both exited and closed its output.
 *
 * @param[in] ctx        The opaque pointer handed to ::action_start.
 * @param[in] exit_code  The child's WEXITSTATUS, 128+signal if it was
 *                       signaled, or -1 if the status could not be read.
 * @param[in] output     The captured stdout+stderr, NUL-terminated. Never
 *                       NULL: an empty capture is "". Borrowed, and gone the
 *                       moment the callback returns.
 * @param[in] outlen     Length of `output`, excluding the NUL.
 */
typedef void action_done_t(void *ctx, int exit_code, char const *output, size_t outlen) ;

/**
 * @brief Called instead of ::action_done_t when the daemon shuts down on an
 *        action that is still running.
 *
 * There is no result to hand over and nobody left to hand it to: this only
 * tells the owner to release whatever it was holding for this action. Called
 * at most once, and never on a job whose ::action_done_t already ran.
 *
 * @param[in] ctx The opaque pointer handed to ::action_start.
 */
typedef void action_abandon_t(void *ctx) ;

/**
 * @brief Tell whether a name may reach `66` as a service, module, instance or
 *        tree name.
 *
 * The name comes from an HTTP request and ends up on a `66` command line, so
 * this is the guard against argument injection as much as a syntax check:
 * non-empty, shorter than 256 bytes, never starting with '-' (it would be read
 * as an option), free of '/', "..", whitespace and control characters. '@' is
 * accepted: whether it belongs (instance, template) is the caller's rule.
 *
 * Every action_argv_* function applies it to the names it puts on the line.
 *
 * @param[in] s The name, may be NULL.
 * @return 1 if the name is acceptable, 0 otherwise. errno is not set.
 */
extern int action_name_valid(char const *s) ;

/**
 * @brief Spawn the `66` binary and let the event loop wait for it.
 *
 * The verb is executed by spawning the real `66` binary rather than calling
 * lib66 entry points in-process: those set up their own event loop and reuse
 * process-global oblibs singletons the daemon already holds, which a fork
 * inherits in a broken state. A fresh `66` process has clean global state.
 *
 * Nothing blocks: the child is watched through a pidfd and its combined
 * stdout+stderr through a pipe, both on `loop`, so the daemon keeps serving
 * while the command runs. `done` is called exactly once, from the loop, when
 * the child has exited AND its output has reached EOF -- the earliest point
 * at which the whole result exists.
 *
 * A global `-v 2` is injected right after argv[0] so every 66 command run by
 * 66-web surfaces its warnings and errors in the captured output.
 *
 * Exactly one of `done` and `abandon` runs, exactly once.
 *
 * @param[in] loop      Event loop that will own the child and the pipe.
 * @param[in] argv      Command line, first argument "66", as built by one
 *                      of the action_argv_* functions. Only read during the
 *                      call: the exec happens here.
 * @param[in] done      Completion callback, called once the action ended.
 * @param[in] abandon   Called instead of `done` if ::action_shutdown finds
 *                      this action still running.
 * @param[in] ctx       Opaque pointer handed back to `done` or `abandon`.
 *
 * @return 1 once the job is armed and owns the child, 0 on failure with
 *         nothing left running and neither callback ever called.
 *
 * @retval 0 errno is set to:
 *   - `EINVAL` if loop, argv, done or abandon is missing, or argv is empty
 *     or holds fewer bytes than its argument count implies.
 *   - `E2BIG` if argv does not fit the child's argument array.
 *   - `ENOMEM` on allocation failure.
 *   - Whatever pipe2/fork/pidfd_open/epoll_ctl set on system failure.
 */
extern int action_start(sse_epoll_t *loop, action_argv_t const *argv, action_done_t *done, action_abandon_t *abandon, void *ctx) ;

/**
 * @brief Release every action still in flight, at shutdown.
 *
 * The event loop stopped, so no watcher will ever fire again and no job can
 * complete on its own. Each one is told to let its owner go, then unhooked
 * from the loop and freed. Must run before sse_free(): the loop still has the
 * watchers in its table, and they live inside the memory freed here.
 *
 * The `66` children are deliberately left alone rather than killed. They are
 * reparented to init and run to completion: a `66 start` interrupted halfway
 * leaves a service in a worse state than one that finishes unobserved. They
 * lose their output pipe when the daemon's descriptors close, which happens at
 * process exit anyway.
 */
extern void action_shutdown(void) ;

/**
 * @brief Build the command line of a `66 <verb> <service>` action.
 *
 * The verb is checked against the whitelist of service commands the web API
 * may run; the service name must be a plain name. The strings are copied into
 * `out`, so it outlives whatever the arguments pointed at.
 *
 * @param[out] out          Receives the command line.
 * @param[in]  action_name  Verb, whitelisted to: "start", "stop", "restart",
 *                          "reload", "enable", "disable", "reconfigure",
 *                          "free", "remove".
 * @param[in]  service_name Target service name (no path separator).
 *
 * @return 1 on success, 0 on failure with errno set to `EINVAL` for an unknown
 *         verb or an invalid name, `E2BIG` if the line exceeds
 *         ::ACTION_ARGV_BYTES.
 */
extern int action_argv_run(action_argv_t *out, char const *action_name, char const *service_name) ;

/**
 * @brief Build the command line of a `66 tree <verb> <tree>` action.
 *
 * @param[out] out         Receives the command line.
 * @param[in]  action_name Tree verb, whitelisted to: "start", "stop", "free",
 *                         "enable", "disable", "current", "init", "remove".
 * @param[in]  tree_name   Target tree name (no path separator).
 *
 * @return 1 on success, 0 on failure with errno set to `EINVAL` for an unknown
 *         verb or an invalid tree name, `E2BIG` if the line does not fit.
 */
extern int action_argv_run_tree(action_argv_t *out, char const *action_name, char const *tree_name) ;

/**
 * @brief Build the command line of `66 configure -L <service>` (read-only).
 *
 * @param[out] out          Receives the command line.
 * @param[in]  service_name Target service name (no path separator).
 *
 * @return 1 on success, 0 on failure with errno set to `EINVAL` for an invalid
 *         name, `E2BIG` if the line does not fit.
 */
extern int action_argv_config(action_argv_t *out, char const *service_name) ;

/**
 * @brief Build the command line of `66 [-t <tree>] enable [-S] <name>`.
 *
 * Used by service creation to register a freshly-written frontend into a
 * tree (and optionally start it). `enable` parses the frontend first if
 * needed, so a parse error surfaces in the captured output with a non-zero
 * exit.
 *
 * @param[out] out          Receives the command line.
 * @param[in]  service_name Target name (may carry '@'; no path separator).
 * @param[in]  tree_name    Tree to enable into, or NULL to let 66 decide
 *                          (InTree key / current / default tree).
 * @param[in]  start        Non-zero to also start it now (adds `-S`).
 *
 * @return 1 on success, 0 on failure with errno set to `EINVAL` for an invalid
 *         service or tree name, `E2BIG` if the line does not fit.
 */
extern int action_argv_enable(action_argv_t *out, char const *service_name, char const *tree_name, int start) ;

#endif
