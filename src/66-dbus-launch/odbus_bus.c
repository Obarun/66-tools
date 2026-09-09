/*
 * odbus_bus.c
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
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <oblibs/strbuf.h>

#include "odbus_internal.h"

#define ODBUS_METHODS_MAX 64

/* Lifetime */

int odbus_open(odbus **ret, int fd)
{
	odbus *bus ;

	if (!ret || fd < 0)
		return odbus_fail(EINVAL) ;

	bus = calloc(1, sizeof(*bus)) ;
	if (!bus)
		return odbus_fail(ENOMEM) ;

	bus->fd = fd ;

	if (!io_rb_init(&bus->io, NULL, fd)) {
		free(bus) ;
		return odbus_fail(errno ? errno : EIO) ;
	}

	if (!odbus_socket_auth(bus)) {
		io_rb_close_fds(&bus->io.fdin.buf) ;
		strbuf_free(&bus->in) ;
		free(bus) ;
		return 0 ;
	}

	*ret = bus ;

	return 1 ;
}

odbus *odbus_free(odbus *bus)
{
	if (!bus)
		return NULL ;

	while (bus->queue_head) {

		odbus_message *m = bus->queue_head ;

		bus->queue_head = m->next ;
		odbus_message_free(m) ;
	}

	io_rb_close_fds(&bus->io.fdin.buf) ;
	strbuf_free(&bus->in) ;
	free(bus) ;

	return NULL ;
}

/* Registration */

int odbus_set_filter(odbus *bus, odbus_message_handler_t callback, void *userdata)
{
	if (!bus || !callback)
		return odbus_fail(EINVAL) ;

	if (bus->filter)
		return odbus_fail(EEXIST) ;

	bus->filter = callback ;
	bus->filter_userdata = userdata ;

	return 1 ;
}

int odbus_set_object(odbus *bus, char const *path, char const *interface, odbus_method const *methods, void *userdata)
{
	unsigned int i ;

	if (!bus || !path || !interface || !methods)
		return odbus_fail(EINVAL) ;

	if (bus->methods)
		return odbus_fail(EEXIST) ;

	for (i = 0 ; i < ODBUS_METHODS_MAX ; i++) {

		if (!methods[i].member && !methods[i].handler)
			break ;

		if (!methods[i].member || !methods[i].handler)
			return odbus_fail(EINVAL) ;
	}

	/** An empty table would register an object that answers nothing, and a table
	 * that never terminates would have run off the end. */
	if (!i || i == ODBUS_METHODS_MAX)
		return odbus_fail(EINVAL) ;

	bus->methods = methods ;
	bus->object_path = path ;
	bus->object_interface = interface ;
	bus->object_userdata = userdata ;

	return 1 ;
}

/* Serials and the incoming queue */

static uint32_t serial_next(odbus *bus)
{
	if (!++bus->serial)
		bus->serial = 1 ;

	return bus->serial ;
}

static void queue_push(odbus *bus, odbus_message *m)
{
	m->next = NULL ;

	if (bus->queue_tail)
		bus->queue_tail->next = m ;
	else
		bus->queue_head = m ;

	bus->queue_tail = m ;
}

static odbus_message *queue_pop(odbus *bus)
{
	odbus_message *m = bus->queue_head ;

	if (!m)
		return NULL ;

	bus->queue_head = m->next ;

	if (!bus->queue_head)
		bus->queue_tail = NULL ;

	m->next = NULL ;

	return m ;
}

/* Dispatch */

// read from the beginning.
static void message_rewind(odbus_message *m)
{
	m->rindex = 0 ;
	m->root_index = 0 ;
	m->n_c = 0 ;
	m->csignature.len = 0 ;
}

static int dispatch(odbus *bus, odbus_message *m)
{
	unsigned int i ;
	int r ;

	if (bus->filter) {

		message_rewind(m) ;

		r = bus->filter(m, bus->filter_userdata) ;
		if (r < 0)
			return r ;

		if (r > 0)
			return 1 ;
	}

	if (m->type != ODBUS_MESSAGE_METHOD_CALL || !bus->methods || !m->member || !m->path)
		return 1 ;

	if (strcmp(m->path, bus->object_path))
		return 1 ;

	if (m->interface && strcmp(m->interface, bus->object_interface))
		return 1 ;

	/** The table was checked at registration: it ends within ODBUS_METHODS_MAX
	 * entries and every entry it holds is complete. */
	for (i = 0 ; bus->methods[i].member ; i++) {

		if (strcmp(bus->methods[i].member, m->member))
			continue ;

		message_rewind(m) ;

		r = bus->methods[i].handler(m, bus->object_userdata) ;
		if (r < 0)
			return r ;

		return 1 ;
	}

	return 1 ;
}

int odbus_process(odbus *bus)
{
	odbus_message *m ;
	int r ;

	if (!bus)
		return odbus_err(EINVAL) ;

	if (bus->closed)
		return odbus_err(ENOTCONN) ;

	// Whatever odbus_call() set aside while it was waiting comes first.
	m = queue_pop(bus) ;

	if (!m) {

		r = odbus_socket_take(bus, &m) ;
		if (r < 0)
			return -1 ;

		if (!r) {

			r = odbus_socket_recv(bus, 0) ;
			if (r < 0)
				return -1 ;

			if (!r)
				return 0 ;

			r = odbus_socket_take(bus, &m) ;
			if (r < 0)
				return -1 ;

			// Only part of a message has arrived; the rest will come.
			if (!r)
				return 0 ;
		}
	}

	r = dispatch(bus, m) ;
	odbus_message_free(m) ;

	if (r < 0)
		return -1 ;

	return 1 ;
}

/* Calls */

/** Keep the name of an ERROR reply in the storage the bus owns, so that the
 * caller gets a string it never has to free. */
static void error_capture(odbus *bus, odbus_message *m)
{
	size_t l ;

	bus->error_name[0] = 0 ;

	if (!m->error_name)
		return ;

	l = strlen(m->error_name) ;

	if (l >= sizeof(bus->error_name))
		l = sizeof(bus->error_name) - 1 ;

	memcpy(bus->error_name, m->error_name, l) ;
	bus->error_name[l] = 0 ;
}

char const *odbus_error_name(odbus const *bus)
{
	if (!bus || !bus->error_name[0])
		return NULL ;

	return bus->error_name ;
}

// Milliseconds on a clock that does not step, to bound a wait across several
static int64_t now_ms(void)
{
	struct timespec ts ;

	clock_gettime(CLOCK_MONOTONIC, &ts) ;

	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000 ;
}

int odbus_call(odbus *bus, odbus_message *m, unsigned int timeout_ms)
{
	int64_t deadline ;
	uint32_t serial ;
	int r ;

	if (!bus || !m || !timeout_ms)
		return odbus_fail(EINVAL) ;

	if (bus->closed)
		return odbus_fail(ENOTCONN) ;

	if (!odbus_message_seal(m, serial_next(bus)))
		return 0 ;

	/** Sealing a message that already was one leaves its serial alone, so read
	 * back what it actually carries rather than what was just handed out. */
	serial = m->serial ;

	if (!odbus_socket_send(bus, m))
		return 0 ;

	deadline = now_ms() + (int64_t)timeout_ms ;

	for (;;) {

		odbus_message *in = NULL ;

		r = odbus_socket_take(bus, &in) ;
		if (r < 0)
			return 0 ;

		if (!r) {

			int64_t left = deadline - now_ms() ;

			if (left <= 0)
				return odbus_fail(ETIMEDOUT) ;

			/** poll() counts its timeout in an int, and @timeout_ms is unsigned:
			 * a caller asking for more than about twenty-five days would wrap
			 * into a negative, which poll() reads as "wait for ever". Clamping
			 * costs nothing and the loop simply comes back for the rest. */
			if (left > INT_MAX)
				left = INT_MAX ;

			r = odbus_socket_recv(bus, (int)left) ;
			if (r < 0)
				return 0 ;

			if (!r)
				return odbus_fail(ETIMEDOUT) ;

			continue ;
		}

		/** Serials handed out are never zero, so a message that carries no reply
		 * serial reads back as zero and cannot match; the field test is there to
		 * say so rather than to add a condition. */
		if ((in->type == ODBUS_MESSAGE_METHOD_RETURN || in->type == ODBUS_MESSAGE_ERROR) &&
			in->has_reply_serial && in->reply_serial == serial) {

			int failed = in->type == ODBUS_MESSAGE_ERROR ;

			if (failed)
				error_capture(bus, in) ;

			odbus_message_free(in) ;

			return failed ? odbus_fail(EIO) : 1 ;
		}

		queue_push(bus, in) ;
	}
}

int odbus_call_method(odbus *bus, char const *path, char const *interface, char const *member, unsigned int timeout_ms, char const *types, ...)
{
	odbus_message *m = NULL ;
	va_list ap ;
	int r ;

	if (!bus)
		return odbus_fail(EINVAL) ;

	if (!odbus_message_new_method_call(bus, &m, path, interface, member))
		return 0 ;

	if (types && *types) {

		va_start(ap, types) ;
		r = odbus_message_appendv(m, types, &ap) ;
		va_end(ap) ;

		if (!r) {
			odbus_message_free(m) ;
			return 0 ;
		}
	}

	r = odbus_call(bus, m, timeout_ms) ;
	odbus_message_free(m) ;

	return r ;
}

int odbus_reply_method_return(odbus_message *call, char const *types, ...)
{
	odbus_message *m ;
	va_list ap ;
	int r ;

	if (!call || call->type != ODBUS_MESSAGE_METHOD_CALL || !call->bus)
		return odbus_err(EINVAL) ;

	if (call->flags & ODBUS_FLAG_NO_REPLY_EXPECTED)
		return 0 ;

	if (call->bus->closed)
		return odbus_err(ENOTCONN) ;

	m = odbus_message_alloc(call->bus, ODBUS_MESSAGE_METHOD_RETURN) ;
	if (!m)
		return odbus_err(ENOMEM) ;

	m->flags = ODBUS_FLAG_NO_REPLY_EXPECTED ;
	m->reply_serial = call->serial ;
	m->has_reply_serial = 1 ;

	if (types && *types) {

		va_start(ap, types) ;
		r = odbus_message_appendv(m, types, &ap) ;
		va_end(ap) ;

		if (!r) {
			odbus_message_free(m) ;
			return -1 ;
		}
	}

	if (!odbus_message_seal(m, serial_next(call->bus))) {
		odbus_message_free(m) ;
		return -1 ;
	}

	r = odbus_socket_send(call->bus, m) ;
	odbus_message_free(m) ;

	return r ? 1 : -1 ;
}
