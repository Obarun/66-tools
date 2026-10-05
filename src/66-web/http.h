/*
 * http.h
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

#ifndef WEB66_HTTP_H
#define WEB66_HTTP_H

#include <stdint.h>

#include <oblibs/sse.h>

#include "auth.h"

/**
 * @brief Arm the HTTP layer with the address it serves and its credential.
 *
 * Must be called once, after the listen watcher is started and before the
 * event loop runs. It records the expected origin so cross-site callers and
 * DNS-rebinding attempts can be told apart from the real UI, the credential
 * every request is checked against, and the listen watcher the accept path
 * pauses and resumes when the process runs out of descriptors. Creating the
 * timer that drives that resume is why this must happen while descriptors
 * are still plentiful.
 *
 * Dies on failure.
 *
 * @param[in] host     Host part of the listen address.
 * @param[in] port     Listen port.
 * @param[in] loopback Non-zero when bound to loopback: the Host
 *                     header is then required to name this server.
 *                     Zero behind a reverse proxy, which owns that
 *                     check instead.
 * @param[in] cred     Credential to authenticate against; must
 *                     outlive the event loop.
 * @param[in] loop     The event loop the server runs on.
 * @param[in] wlisten  The already started listen-socket watcher; must outlive
                       the event loop.
 */
extern void http_init(char const *host, uint16_t port, int loopback, auth_cred_t const *cred, sse_epoll_t *loop, sse_watcher_t *listen_w) ;

/**
 * @brief Accept callback for the listen socket watcher.
 *
 * Drains pending connections from the listen socket. Each accepted
 * client is registered with the event loop and handled asynchronously
 * by an internal read watcher; the listen-socket watcher remains
 * armed for further accepts, save for the brief pause that follows
 * running out of descriptors.
 *
 * Connections beyond the internal cap, and connections that do not
 * complete their header block in time, are closed.
 *
 * @param w      The listen-socket watcher; `w->fd` must be the listen fd.
 * @param cbdata Must be the running `sse_epoll_t *`.
 * @param event  Epoll event flags as reported by the loop.
 */
extern void on_accept(sse_watcher_t *w, void *cbdata, int event) ;

/**
 * @brief Release the connections still standing when the event loop stops.
 *
 * A connection normally closes from one of its own callbacks. Once the loop
 * has returned there is no callback left to run, so the ones still open are
 * closed and freed here instead, request buffer and queued response included.
 *
 * Must be called after the event loop has stopped and before `sse_free()`,
 * which drains the deferred releases this deliberately does not use.
 */
extern void http_shutdown(void) ;

#endif
