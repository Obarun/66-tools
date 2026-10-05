/*
 * events.h
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

#ifndef WEB66_EVENTS_H
#define WEB66_EVENTS_H

#include <oblibs/sse.h>

/**
 * @brief Initialise the events subsystem.
 *
 * Attaches an inotify watcher to the uid's 66 live-state directory
 * (`<SS_LIVE>state/<uid>/`) and every existing `<service>/state/`
 * subdir. Failure is non-fatal: a
 * warning is logged and the subsystem stays online for client
 * registration but will never emit events.
 *
 * @param[in] loop  The event loop the inotify watcher attaches to.
 * @return 1 if inotify came up, 0 if it could not (warning logged).
 */
extern int events_init(sse_epoll_t *loop) ;

/**
 * @brief Report whether one more subscriber can be taken.
 *
 * Lets the HTTP layer refuse a stream before it commits to a 200
 * response block, which is the only way to answer a full table with
 * a single response.
 *
 * @return 1 if a slot is free, 0 if the subscriber table is full.
 */
extern int events_can_accept(void) ;

/**
 * @brief Take ownership of a client fd as an SSE subscriber.
 *
 * The HTTP layer is expected to have already written the SSE
 * response headers and removed the fd from its own per-connection
 * watcher (so cleanup will neither close it nor leave it registered
 * with epoll). The events module thereafter owns the fd: it puts a
 * read watcher on it to notice a disconnect, writes broadcasts, and
 * closes the fd when the peer leaves or at shutdown.
 *
 * @param[in] fd  Client TCP socket.
 * @return 1 on success, 0 if the subscriber table is full or the
 *         watcher could not be installed (caller must close `fd`
 *         themselves in that case).
 */
extern int events_register(int fd) ;

/**
 * @brief Close every subscriber fd and tear down the inotify watcher.
 *
 * Called from main() before sse_free(loop).
 */
extern void events_shutdown(void) ;

#endif
