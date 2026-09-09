/*
 * odbus_socket.c
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

#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

#include <oblibs/io_rb.h>
#include <oblibs/log.h>
#include <oblibs/strbuf.h>

#include "odbus_internal.h"

static char const odbus_auth_request[] = "\0AUTH EXTERNAL\r\nDATA\r\nNEGOTIATE_UNIX_FD\r\nBEGIN\r\n" ;

#define ODBUS_AUTH_REQUEST_LEN (sizeof(odbus_auth_request) - 1)
#define ODBUS_AUTH_ANSWER_MAX 4096

static int wait_for(odbus *bus, short events, int timeout_ms)
{
	struct pollfd pfd = { .fd = bus->fd, .events = events, .revents = 0 } ;

	for (;;) {

		int r = poll(&pfd, 1, timeout_ms) ;

		if (r > 0)
			return 1 ;

		if (!r)
			return 0 ;

		if (errno == EINTR)
			continue ;

		return odbus_err(errno ? errno : EIO) ;
	}
}

static int refuse_fds(odbus *bus)
{
	int fd ;

	if (!io_rb_pending_fdin(&bus->io))
		return 1 ;

	while (io_rb_get_fd(&bus->io, &fd) > 0)
		close(fd) ;

	log_warn("the peer attached file descriptors to a message that carries none") ;

	return odbus_fail(EPROTO) ;
}

static int in_drain(odbus *bus)
{
	size_t n = io_rb_pending_in(&bus->io) ;

	if (!n)
		return 1 ;

	/** Drop what has already been framed before growing any further. */
	if (bus->in_index) {

		if (bus->in_index < bus->in.len)
			memmove(bus->in.s, bus->in.s + bus->in_index, bus->in.len - bus->in_index) ;

		bus->in.len -= bus->in_index ;
		bus->in_index = 0 ;
	}

	if (bus->in.len + n > ODBUS_MESSAGE_LEN_MAX) {

		flog_warn("the peer sent more than the %u bytes a message may hold", (unsigned int)ODBUS_MESSAGE_LEN_MAX) ;

		return odbus_fail(EBADMSG) ;
	}

	if (!strbuf_reserve(&bus->in, bus->in.len + n))
		return odbus_fail(errno ? errno : ENOMEM) ;

	if (io_rb_get(&bus->io, bus->in.s + bus->in.len, n) < 0)
		return odbus_fail(errno ? errno : EIO) ;

	bus->in.len += n ;

	return 1 ;
}

static void in_consume(odbus *bus, size_t n)
{
	bus->in_index += n ;

	if (bus->in_index == bus->in.len) {
		bus->in.len = 0 ;
		bus->in_index = 0 ;
	}
}

int odbus_socket_recv(odbus *bus, int timeout_ms)
{
	if (bus->closed)
		return odbus_err(ECONNRESET) ;

	for (;;) {

		ssize_t r ;

		/** io_rb_read answers 0 both when the read would block and when the peer
		 * is gone. It only reaches the first case after seeing EAGAIN, so a
		 * cleared errno is what tells the two apart. */
		errno = 0 ;
		r = io_rb_read(&bus->io, NULL) ;

		if (r > 0) {

			if (!refuse_fds(bus))
				return -1 ;

			return in_drain(bus) ? 1 : -1 ;
		}

		if (r < 0) {

			/** Settle errno before logging: log_warnusys reports the one in
			 * force, and it must be the one the caller will read back. */
			if (!errno)
				errno = EIO ;

			log_warnusys("read from the bus socket") ;

			return odbus_err(errno) ;
		}

		if (errno != EAGAIN && errno != EWOULDBLOCK) {

			bus->closed = 1 ;

			log_warn("the peer closed the bus connection") ;

			return odbus_err(ECONNRESET) ;
		}

		if (!timeout_ms)
			return 0 ;

		r = wait_for(bus, POLLIN, timeout_ms) ;
		if (r < 0)
			return (int)r ;

		/** Nothing came within the time allowed. */
		if (!r)
			return 0 ;
	}
}

static int drain(odbus *bus)
{
	while (io_rb_pending_out(&bus->io) || io_rb_pending_fdout(&bus->io)) {

		errno = 0 ;

		if (io_rb_flush(&bus->io, NULL) < 0) {

			if (errno == EPIPE || errno == ECONNRESET)
				bus->closed = 1 ;

			if (!errno)
				errno = EIO ;

			log_warnusys("write to the bus socket") ;

			return odbus_fail(errno) ;
		}

		if (!io_rb_pending_out(&bus->io) && !io_rb_pending_fdout(&bus->io))
			break ;

		if (wait_for(bus, POLLOUT, -1) < 0) {

			if (!errno)
				errno = EIO ;

			log_warnusys("wait for the bus socket to accept a write") ;

			return odbus_fail(errno) ;
		}
	}

	return 1 ;
}

static int auth_take_line(odbus *bus, char const **line, size_t *len)
{
	char const *s = bus->in.s + bus->in_index ;
	size_t avail = bus->in.len - bus->in_index ;
	size_t i = 0 ;

	for (; i + 1 < avail ; i++) {

		if (s[i] != '\r' || s[i + 1] != '\n')
			continue ;

		*line = s ;
		*len = i ;
		in_consume(bus, i + 2) ;

		return 1 ;
	}

	return 0 ;
}

int odbus_socket_auth(odbus *bus)
{
	int got_ok = 0, got_fd = 0 ;

	if (io_rb_put(&bus->io, odbus_auth_request, ODBUS_AUTH_REQUEST_LEN, NULL) < 0)
		return odbus_fail(errno ? errno : EIO) ;

	if (!drain(bus))
		return 0 ;

	while (!got_ok || !got_fd) {

		char const *line ;
		size_t len ;

		if (!auth_take_line(bus, &line, &len)) {

			if (bus->in.len - bus->in_index > ODBUS_AUTH_ANSWER_MAX) {

				flog_warn("the peer answered the handshake with more than %u bytes and no line terminator", (unsigned int)ODBUS_AUTH_ANSWER_MAX) ;

				return odbus_fail(EPROTO) ;
			}

			if (odbus_socket_recv(bus, -1) < 0)
				return 0 ;

			continue ;
		}

		if (len >= 2 && !memcmp(line, "OK", 2))
			got_ok = 1 ;
		else if (len == strlen("AGREE_UNIX_FD") && !memcmp(line, "AGREE_UNIX_FD", len))
			got_fd = 1 ;
		else if (len == strlen("DATA") && !memcmp(line, "DATA", len))
			continue ; // the challenge for the EXTERNAL argument we did not send
		else {

			flog_warn("handshake refused by the peer: %.*s", (int)len, line) ;

			return odbus_fail(EPROTO) ;
		}
	}

	return 1 ;
}

int odbus_socket_send(odbus *bus, odbus_message *m)
{
	struct iovec iov[2] ;
	int r ;

	if (bus->closed)
		return odbus_fail(ENOTCONN) ;

	if (!m->sealed)
		return odbus_fail(EINVAL) ;

	iov[0].iov_base = m->hdr.s ;
	iov[0].iov_len = m->hdr.len ;
	iov[1].iov_base = m->body.s ;
	iov[1].iov_len = m->body.len ;

	if (m->n_fds)
		r = io_rb_putv_fd(&bus->io, iov, 2, m->fds, (int)m->n_fds, NULL) ;
	else
		r = io_rb_putv(&bus->io, iov, 2, NULL) ;

	if (r < 0)
		return odbus_fail(errno ? errno : EIO) ;

	return drain(bus) ;
}

int odbus_socket_take(odbus *bus, odbus_message **ret)
{
	char const *p = bus->in.s + bus->in_index ;
	size_t avail = bus->in.len - bus->in_index ;
	odbus_frame f ;
	int r ;

	r = odbus_frame_parse(p, avail, &f) ;
	if (r <= 0)
		return r ;

	if (avail < f.total)
		return 0 ;

	if (!odbus_message_from_wire(bus, p, f.total, ret))
		return -1 ;

	in_consume(bus, f.total) ;

	return 1 ;
}
