/*
 * api.h
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

#ifndef WEB66_API_H
#define WEB66_API_H

#include <stddef.h>
#include <stdint.h>

#include <66/constants.h>

#include "action.h"

/**
 * Result of routing an API request: HTTP status + ready-to-send body.
 *
 * `body` is a heap allocation owned by the caller (must be freed
 * after the response is written). For error statuses with no body
 * payload, `body` is NULL and `bodylen` is 0.
 */
typedef struct api_response_s api_response_t ;
struct api_response_s {
	int status ;
	char *body ;
	size_t bodylen ;
} ;

/** Longest verb an action formatter reports ("enablestart"), NUL included. */
#define API_ACT_VERB_MAX 32

/** Longest path an action formatter reports: the module directory a create
 * route builds, NUL included. */
#define API_ACT_PATH_MAX (SS_MAX_PATH_LEN + 300)

/**
 * Route parameters an action formatter still needs once the command has run.
 * They are copied by value: the request that carried them, and the buffers it
 * was parsed into, are gone by then. A member a route does not use stays "".
 */
typedef struct api_action_ctx_s api_action_ctx_t ;
struct api_action_ctx_s {
	char name[SS_MAX_SERVICE_NAME] ;
	char action[API_ACT_VERB_MAX] ;
	char path[API_ACT_PATH_MAX] ;
} ;

/**
 * @brief Turn the result of a 66 action into the response of its route.
 *
 * Each action route has one of these: the routing layer knows the shape of
 * the JSON and the status mapping, the transport layer knows when the action
 * ended. Called exactly once, from the event loop, long after the request
 * buffer is gone -- hence `ctx`, which carries copies of what the route read
 * from the request.
 *
 * @param[in]  ctx       The route's captured parameters (`api_result_t::act.ctx`).
 * @param[in]  exit_code The 66 command's exit code.
 * @param[in]  output    Its captured stdout+stderr, NUL-terminated, never NULL.
 * @param[in]  outlen    Length of `output`, excluding the NUL.
 * @param[out] resp      Status + heap body, as ::api_route_get would fill it.
 */
typedef void api_action_fmt_t(api_action_ctx_t const *ctx, int exit_code, char const *output, size_t outlen, api_response_t *resp) ;

/**
 * What a route decided: either a response, or a 66 command to run.
 *
 * Running `66` takes as long as the service takes to start or stop, which the
 * single-threaded daemon cannot wait for. So a route that needs one does not
 * run it: it describes it, and the transport layer spawns it, keeps serving,
 * and calls `act.fmt` back when it ends.
 *
 * When `async` is 0, `sync` holds the response and `act` is untouched. When
 * `async` is 1, `act` is filled: the command line, the formatter and its
 * parameters, all held by value. Nothing in it is allocated, nothing is freed.
 */
typedef struct api_result_s api_result_t ;
struct api_result_s {
	uint8_t        async ;
	api_response_t sync ;
	struct {
		action_argv_t     argv ;
		api_action_fmt_t *fmt ;
		api_action_ctx_t  ctx ;
	} act ;
} ;

/**
 * @brief Fill `resp` with an error answer, the JSON body {"error":"<message>"}.
 *
 * @param[in] status HTTP status code of the answer.
 * @param[in] message Error text, JSON-escaped into the body.
 * @param[out] resp Receives `status` and the body, which the caller frees.
 * @return 1 on success. 0 on failure, `resp` left untouched: errno ENOMEM if
 *         the body would not allocate, EINVAL if it did not end up on the heap.
 */
extern int resp_error(int status, char const *message, api_response_t *resp) ;

/**
 * @brief Route a GET request to the matching API handler.
 *
 * Tries each known API endpoint in turn. Returns 1 if the path
 * matched some endpoint (success or known error), 0 if no handler
 * claimed the path (the caller should fall through to its own
 * not-found path).
 *
 * @param[in]  path Request path without query string.
 * @param[out] r    On match, either a response or an action to run.
 * @return 1 if matched, 0 otherwise.
 */
extern int api_route_get(char const *path, api_result_t *r) ;

/**
 * @brief Route a POST request to the matching API handler.
 *
 * Same semantics as `api_route_get` but for POST.
 *
 * @param[in]  path     Request path without query string.
 * @param[in]  body     Request body bytes (may be NULL).
 * @param[in]  body_len Body length (0 if no body).
 * @param[out] r        On match, either a response or an action to run.
 * @return 1 if matched, 0 otherwise.
 */
extern int api_route_post(char const *path, char const *body, size_t body_len,
                           api_result_t *r) ;

#endif
