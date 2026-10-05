/*
 * events.c
 *
 * Copyright (c) 2026 Eric Vidal <eric@obarun.org>
 *
 * All rights reserved.
 *
 * This file is part of Obarun. It is subject to the license terms in
 * the LICENSE file found in the top-level directory of this
 * distribution.
 * This file may not be copied, modified, propagated, or distributed
 * except according to the terms contained in the LICENSE file./
 */

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>

#include <oblibs/io.h>
#include <oblibs/log.h>
#include <oblibs/sse.h>
#include <oblibs/string.h>
#include <oblibs/types.h>

#include <66/config.h>
#include <66/constants.h>

#include "ctx.h"
#include "events.h"

#define MAX_SUBSCRIBERS 64

struct g_s {
	sse_epoll_t *loop ;
	int fds[MAX_SUBSCRIBERS] ;
	sse_watcher_t w[MAX_SUBSCRIBERS] ;
	int count ;
	sse_watcher_t inotify_w ;
	int inotify_up ;
} ;

static struct g_s g = { .loop = NULL, .count = 0, .inotify_up = 0 } ;

static void on_inotify_event(sse_watcher_t *w, void *cbdata, int event) ;

static void attach_service_dirs(char const *livestate)
{
	DIR *d = opendir(livestate) ;
	if (!d) {
		log_warnusys("events: open ", livestate) ;
		return ;
	}

	uint32_t mask = IN_CLOSE_WRITE | IN_MOVED_TO | IN_MODIFY ;
	struct dirent *e ;

	while ((e = readdir(d))) {

		if (e->d_name[0] == '.')
			continue ;

		char path[strlen(livestate) + 1 + strlen(e->d_name) + SS_STATE_LEN + 1] ;

		auto_strings(path, livestate, "/", e->d_name, SS_STATE) ;

		if (!sse_attach_inotify(&g.inotify_w, path, mask)) {
			// Service may not have a state dir yet; non-fatal.
		}
	}

	closedir(d) ;
}

int events_init(sse_epoll_t *loop)
{
	for (int i = 0 ; i < MAX_SUBSCRIBERS ; i++)
		g.fds[i] = -1 ;

	g.loop = loop ;

	ctx_t const *s = ctx_get() ;
	if (!s)
		log_warn_return(LOG_EXIT_ZERO, "events: context not initialised; SSE will be inert") ;

	char uid[U32_FMT] ;
	uid[u32_fmt(uid, (uint32_t)s->owner)] = 0 ;
	char livestate[sizeof(SS_LIVE) + SS_STATE_LEN + U32_FMT] ;

	auto_strings(livestate, SS_LIVE, SS_STATE + 1, "/", uid) ;

	if (!sse_start_inotify(loop, &g.inotify_w, &on_inotify_event, NULL, 0))
		log_warnusys_return(LOG_EXIT_ZERO, "events: sse_start_inotify failed") ;

	uint32_t topmask = IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM ;
	if (!sse_attach_inotify(&g.inotify_w, livestate, topmask)) {
		log_warnusys("events: attach inotify on ", livestate) ;
		// Soft-fail: keep the watcher up; per-service attach below may still succeed.
	}

	attach_service_dirs(livestate) ;

	g.inotify_up = 1 ;
	log_info("events: watching ", livestate) ;
	return 1 ;
}

int events_can_accept(void)
{
	return g.count < MAX_SUBSCRIBERS ;
}

static void subscriber_drop(int i)
{
	if (g.fds[i] < 0)
		return ;

	sse_free_io(&g.w[i]) ;
	g.fds[i] = -1 ;
	g.count-- ;
}

static void on_subscriber_event(sse_watcher_t *w, void *cbdata, int event)
{
	(void)w ;
	int i = (int)(intptr_t)cbdata ;

	/* The slot may already have been reaped earlier in this very dispatch
	 * cycle, by a broadcast the inotify watcher triggered: sse_dispatch
	 * calls the callback of every queued watcher, released ones included. */
	if (g.fds[i] < 0)
		return ;

	/* And if that freed slot was re-filled with a new subscriber in the same
	 * cycle, this stale event is still harmless: a freshly armed watcher
	 * carries no revents, so the HUP branch below is not taken, and a spurious
	 * readable is absorbed by the read handling that follows. */
	if (event & (SSE_ERROR | SSE_HUP | SSE_SHUT)) {
		subscriber_drop(i) ;
		return ;
	}

	char buf[256] ;
	if (io_read_result(io_read(g.fds[i], buf, sizeof(buf))) < 0)
		subscriber_drop(i) ;
}

int events_register(int fd)
{
	if (!g.loop)
		log_warn_return(LOG_EXIT_ZERO, "events: not initialised; refusing SSE subscriber") ;

	for (int i = 0 ; i < MAX_SUBSCRIBERS ; i++) {

		if (g.fds[i] >= 0)
			continue ;

		if (!sse_start_io(g.loop, &g.w[i], &on_subscriber_event, (void *)(intptr_t)i, fd, SSE_READ, 0))
			log_warnusys_return(LOG_EXIT_ZERO, "events: watch an SSE subscriber") ;

		g.fds[i] = fd ;
		g.count++ ;
		return 1 ;
	}

	return 0 ;
}

void events_shutdown(void)
{
	for (int i = 0 ; i < MAX_SUBSCRIBERS ; i++)
		subscriber_drop(i) ;

	if (g.inotify_up) {
		sse_free_inotify(&g.inotify_w) ;
		g.inotify_up = 0 ;
	}
}

static void broadcast(char const *json)
{
	size_t n = sizeof("data: ") - 1 + strlen(json) + sizeof("\n\n") - 1 ;
	char frame[n + 1] ;
	auto_strings(frame, "data: ", json, "\n\n") ;

	for (int i = 0 ; i < MAX_SUBSCRIBERS ; i++) {
		if (g.fds[i] < 0)
			continue ;
		/* A client whose socket buffer is momentarily full has not gone
		 * away; it just misses this frame. */
		if (io_write_result(io_write(g.fds[i], frame, n)) < 0)
			subscriber_drop(i) ;
	}
}

static void on_inotify_event(sse_watcher_t *w, void *cbdata, int event)
{
	(void)w ; (void)cbdata ; (void)event ;
	/* Any change to the live state tree is treated as a generic
	 * "something moved" signal. Clients re-fetch on receipt. */
	broadcast("{\"type\":\"state_changed\"}") ;
}
