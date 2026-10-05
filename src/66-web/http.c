/*
 * http.c
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
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/sockios.h>

#include <oblibs/fd.h>
#include <oblibs/io.h>
#include <oblibs/log.h>
#include <oblibs/socket.h>
#include <oblibs/sse.h>
#include <oblibs/string.h>
#include <oblibs/types.h>

#include "action.h"
#include "api.h"
#include "assets.h"
#include "auth.h"
#include "events.h"
#include "http.h"

#define REQ_HEADER_MAX (16 * 1024)
#define REQ_BODY_MAX (256 * 1024)
#define PATH_MAX_LEN 1024
#define HEADERS_END "\r\n\r\n"

#define HOSTPORT_MAX 128 // "<host>:<port>"
#define AUTHZ_MAX 2048 // "Basic <base64 of user:pass>"

#define MAX_CONN 256 // connections held at once, request state and all
#define HEADER_TIMEOUT_MS 15000 // to complete the request header block
#define WRITE_TIMEOUT_MS 30000 // without the client taking one more byte
#define FLUSH_REARM_MAX 20 // x WRITE_TIMEOUT_MS: 10 minutes of flushing, ever
#define EMFILE_REARM_MS 100 // accept pause after running out of descriptors

#define RESP_HEAD_MAX 512 // status line, headers, and the short bodies
#define CSRF_HEADER "x-66-web"
#define HEADER_FIELDS_MAX 64 // fields indexed per request; past it, 431

#define SEND_DONE 1 // all of it reached the socket
#define SEND_PENDING 0 // the socket filled up; the loop finishes the rest
#define SEND_FAILED (-1) // the client is gone; nothing more will be sent

typedef enum body_kind_e body_kind_t ;
enum body_kind_e {
	BODY_STATIC, // lives as long as the process: an embedded asset, a literal
	BODY_OWNED, // heap, and the send is what frees it
	BODY_COPY // the caller's stack: copied into the connection
} ;

typedef struct header_field_s header_field_t ;
struct header_field_s {
	uint32_t name ; // offsets into conn_t buf
	uint32_t namelen ;
	uint32_t value ;
	uint32_t valuelen ;
} ;

#define HEADER_FIELD_ZERO { 0, 0, 0, 0 }

typedef struct conn_s conn_t ;
struct conn_s {
	int fd ;
	char *buf ;
	size_t cap ; // allocated capacity of buf
	size_t pos ; // bytes accumulated so far
	size_t hend ; // offset just past "\r\n\r\n" (0 until headers complete)
	size_t need ; // total bytes wanted = hend + body_len (0 until known)
	header_field_t fields[HEADER_FIELDS_MAX] ;
	uint8_t nfields ;
	sse_epoll_t *loop ;
	sse_watcher_t watcher ;
	sse_watcher_t deadline ;
	uint8_t closing ; // cleanup ran; the other watcher may still fire this cycle
	uint8_t async ; // a 66 action owes this connection its response
	/* The response being sent, in two segments: `out` holds the header block
	 * and the short bodies that are copied into it, `body` points at a big one
	 * left where it lies. `sent` walks the two as if they were one. */
	char out[RESP_HEAD_MAX] ;
	size_t outlen ;
	char const *body ;
	size_t bodylen ;
	size_t sent ;
	size_t acked ;
	uint8_t rearms ; // write deadlines this response has been granted
	uint8_t body_owned ; // free(body) when the response is over
	uint8_t flushing ; // the socket filled up; the loop owes the rest
	conn_t *next ;
} ;

typedef struct action_pending_s action_pending_t ;
struct action_pending_s {
	conn_t *conn ;
	api_action_fmt_t *fmt ;
	api_action_ctx_t ctx ; // copied: the router's result dies with the dispatch
} ;

static char g_hostport[HOSTPORT_MAX] ;
static char g_portstr[8] ;
static int g_loopback_enforce ;
static auth_cred_t const *g_cred ;
static sse_watcher_t g_rearm_timer = SSE_WATCHER_ZERO ;
static unsigned int g_nconn ; // live conn_t, never above MAX_CONN
static conn_t *g_conns ;

static void conn_link(conn_t *c)
{
	c->next = g_conns ;
	g_conns = c ;
}

static void conn_unlink(conn_t *c)
{
	conn_t **p = &g_conns ;

	while (*p && *p != c)
		p = &(*p)->next ;

	if (*p)
		*p = c->next ;

	c->next = NULL ;
}

static void conn_free(void *arg)
{
	conn_t *c = (conn_t *)arg ;

	free(c->buf) ;
	free(c) ;
}

static void conn_drop_response(conn_t *c)
{
	if (c->body_owned)
		free((char *)c->body) ;

	c->body = NULL ;
	c->bodylen = 0 ;
	c->body_owned = 0 ;
	c->outlen = 0 ;
	c->sent = 0 ;
	c->acked = 0 ;
	c->rearms = 0 ;
	c->flushing = 0 ;
}

static void conn_cleanup(conn_t *c)
{
	if (c->closing)
		return ;

	c->closing = 1 ;

	conn_drop_response(c) ;
	sse_free_timer(&c->deadline) ;
	sse_free_io(&c->watcher) ;
	g_nconn-- ;

	conn_unlink(c) ;

	if (!sse_defer(c->loop, &conn_free, c)) {
		log_warnusys("defer the release of a connection") ;
		conn_free(c) ;
	}
}

static char const *status_reason(int status)
{
	switch (status) {
		case 200: return "OK" ;
		case 201: return "Created" ;
		case 400: return "Bad Request" ;
		case 404: return "Not Found" ;
		case 409: return "Conflict" ;
		case 413: return "Payload Too Large" ;
		case 500: return "Internal Server Error" ;
		case 501: return "Not Implemented" ;
		case 503: return "Service Unavailable" ;
	}
	return "OK" ;
}

static int write_some(int fd, char const *buf, size_t len, size_t *off)
{
	while (*off < len) {
		ssize_t n = io_write(fd, (char *)buf + *off, len - *off) ;
		if (n < 0)
			return error_isagain(errno) ? SEND_PENDING : SEND_FAILED ;
		*off += (size_t)n ;
	}
	return SEND_DONE ;
}

static int conn_flush(conn_t *c)
{
	size_t off = c->sent ;
	int r = write_some(c->fd, c->out, c->outlen, &off) ;

	c->sent = off ;
	if (r != SEND_DONE)
		return r ;

	off = c->sent - c->outlen ;
	r = write_some(c->fd, c->body, c->bodylen, &off) ;
	c->sent = c->outlen + off ;

	return r ;
}

static ssize_t conn_acked(conn_t const *c)
{
	int outq = 0 ;

	if (ioctl(c->fd, SIOCOUTQ, &outq) < 0)
		log_warnusys_return(LOG_EXIT_LESSONE, "read the send queue of a connection") ;

	return (ssize_t)(c->sent - (size_t)outq) ;
}

static int conn_watch_write(conn_t *c)
{
	if (!sse_modify_io(&c->watcher, c->fd, SSE_WRITE))
		log_warnusys_return(LOG_EXIT_ZERO, "watch a connection for writability") ;

	if (!sse_restart_io(&c->watcher))
		log_warnusys_return(LOG_EXIT_ZERO, "resume a connection to finish its response") ;

	/* Snapshot what the client has taken so far, so the first window asks it
	 * for one byte MORE than it already had rather than granting it for
	 * free: the first flush usually leaves a good deal already acknowledged. */
	ssize_t acked = conn_acked(c) ;
	c->acked = acked < 0 ? 0 : (size_t)acked ;

	if (!sse_modify_timer(&c->deadline, WRITE_TIMEOUT_MS, 0) || !sse_restart_timer(&c->deadline))
		log_warnusys("arm the write deadline of a connection") ;

	return 1 ;
}

static int conn_send(conn_t *c, int status, char const *status_text, char const *extra, char const *mime, char const *body, size_t bodylen, body_kind_t kind)
{
	int r ;

	c->outlen = 0 ;
	c->sent = 0 ;
	c->acked = 0 ;
	c->rearms = 0 ;
	c->body = (kind == BODY_COPY) ? NULL : body ;
	c->bodylen = (kind == BODY_COPY) ? 0 : bodylen ;
	c->body_owned = (kind == BODY_OWNED) ;

	char st[I32_FMT] ;
	st[i32_fmt(st, status)] = 0 ;
	char clen[U64_FMT] ;
	clen[u64_fmt(clen, bodylen)] = 0 ;

	char const *part[] = {
		"HTTP/1.1 ", st, " ", status_text, "\r\n",
		extra ? extra : "",
		"Content-Type: ", mime, "\r\n",
		"Content-Length: ", clen, "\r\n",
		"Connection: close\r\n",
		"\r\n",
		0
	} ;

	size_t hlen = 0 ;
	for (size_t i = 0 ; part[i] ; i++)
		hlen += strlen(part[i]) ;

	if (hlen >= RESP_HEAD_MAX || (kind == BODY_COPY && hlen + bodylen > RESP_HEAD_MAX)) {
		log_warnu("format the header block of a response") ;
		conn_drop_response(c) ;
		return SEND_FAILED ;
	}

	auto_string_builder(c->out, 0, part) ;
	c->outlen = hlen ;
	if (kind == BODY_COPY) {
		memcpy(c->out + c->outlen, body, bodylen) ;
		c->outlen += bodylen ;
	}

	r = conn_flush(c) ;

	if (r == SEND_PENDING && conn_watch_write(c)) {
		c->flushing = 1 ;
		return SEND_PENDING ;
	}

	conn_drop_response(c) ;

	return r == SEND_DONE ? SEND_DONE : SEND_FAILED ;
}

static int conn_send_status(conn_t *c, int status, char const *status_text)
{
	char st[I32_FMT] ;
	st[i32_fmt(st, status)] = 0 ;
	size_t blen = strlen(st) + 1 + strlen(status_text) + 1 ;
	char body[blen + 1] ;

	auto_strings(body, st, " ", status_text, "\n") ;

	return conn_send(c, status, status_text, NULL, "text/plain; charset=utf-8", body, blen, BODY_COPY) ;
}

static int conn_send_unauthorized(conn_t *c)
{
	static char const *BODY = "401 Unauthorized\n" ;

	return conn_send(c, 401, "Unauthorized",
	                  "WWW-Authenticate: Basic realm=\"66-web\", charset=\"UTF-8\"\r\n",
	                  "text/plain; charset=utf-8", BODY, 17, BODY_STATIC) ;
}

static int percent_decode(char *s)
{
	char *r = s, *w = s ;

	while (*r) {

		unsigned char c, hi, lo ;

		if (r[0] == '%' &&
			(hi = scan_digit((unsigned char)r[1], 16)) < 16 &&
			(lo = scan_digit((unsigned char)r[2], 16)) < 16) {

			c = (unsigned char)(hi << 4 | lo) ;
			r += 3 ;

		} else {

			c = (unsigned char)*r++ ;
		}

		/* An encoded NUL would silently truncate the path; no control byte is
		 * valid in a target the router accepts. Refuse the whole request. */
		if (c < 0x20 || c == 0x7f)
			return 0 ;

		*w++ = (char)c ;
	}
	*w = 0 ;
	return 1 ;
}

static int conn_reserve(conn_t *c, size_t want)
{
	if (want <= c->cap)
		return 1 ;

	size_t ncap = c->cap ? c->cap : 4096 ;

	while (ncap < want)
		ncap *= 2 ;

	char *nb = realloc(c->buf, ncap) ;
	if (!nb)
		return 0 ;

	c->buf = nb ;
	c->cap = ncap ;

	return 1 ;
}

static uint8_t header_cclass(unsigned char c)
{
	if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
		return 0 ;

	switch (c) {
		case '!': case '#': case '$': case '%': case '&': case '\'': case '*':
		case '+': case '-': case '.': case '^': case '_': case '`': case '|': case '~':
			return 0 ;
		case ':': return 1 ;
		case ' ': case '\t': return 2 ;
		case '\r': return 3 ;
		case '\n': return 4 ;
		default:
			return (c < 0x20 || c == 0x7f) ? 6 : 5 ;
	}
}

static int header_parse(conn_t *c, size_t start)
{
	static uint16_t const table[6][7] = {
		{ 0x0011, 0x0007, 0x0007, 0x0005, 0x0007, 0x0007, 0x0007 },
		{ 0x0001, 0x0022, 0x0007, 0x0007, 0x0007, 0x0007, 0x0007 },
		{ 0x00c3, 0x00c3, 0x0002, 0x0004, 0x0007, 0x00c3, 0x0007 },
		{ 0x0083, 0x0083, 0x0003, 0x0004, 0x0007, 0x0083, 0x0007 },
		{ 0x0007, 0x0007, 0x0007, 0x0007, 0x0100, 0x0007, 0x0007 },
		{ 0x0007, 0x0007, 0x0007, 0x0007, 0x0006, 0x0007, 0x0007 }
	} ;
	header_field_t f = HEADER_FIELD_ZERO ;
	uint8_t state = 0 ;

	c->nfields = 0 ;

	for (size_t pos = start ; state < 6 && pos < c->hend ; pos++) {

		uint16_t what = table[state][header_cclass((unsigned char)c->buf[pos])] ;
		state = what & 0x0f ;

		if (what & 0x0010)
			f.name = (uint32_t)pos ;

		if (what & 0x0020) {
			f.namelen = (uint32_t)pos - f.name ;
			f.value = (uint32_t)pos + 1 ;
			f.valuelen = 0 ;
		}

		if (what & 0x0040)
			f.value = (uint32_t)pos ;

		if (what & 0x0080)
			f.valuelen = (uint32_t)pos + 1 - f.value ;

		if (what & 0x0100) {

			if (c->nfields == HEADER_FIELDS_MAX)
				return (errno = E2BIG, 0) ;

			c->fields[c->nfields++] = f ;
		}
	}

	return state == 6 ? 1 : (errno = EPROTO, 0) ;
}

static int header_is(conn_t const *c, header_field_t const *f, char const *name)
{
	size_t nlen = strlen(name) ;

	return f->namelen == nlen && !strncasecmp(c->buf + f->name, name, nlen) ;
}

static header_field_t const *header_first(conn_t const *c, char const *name)
{
	for (uint8_t i = 0 ; i < c->nfields ; i++)
		if (header_is(c, &c->fields[i], name))
			return &c->fields[i] ;

	return NULL ;
}

static int header_count(conn_t const *c, char const *name)
{
	int n = 0 ;

	for (uint8_t i = 0 ; i < c->nfields ; i++)
		if (header_is(c, &c->fields[i], name))
			n++ ;

	return n ;
}

static int header_content_length(conn_t const *c, uint64_t *len)
{
	header_field_t const *f = header_first(c, "content-length") ;
	if (!f)
		return 0 ;

	size_t n = u64_scan_base(c->buf + f->value, len, 10) ;
	if (!n)
		return 0 ;

	// u64_scan_base stops short of an overflow: a digit left means too large anyway
	if (scan_digit((unsigned char)c->buf[f->value + n], 10) < 10)
		*len = UINT64_MAX ;

	return 1 ;
}

static int header_value(conn_t const *c, char const *name, char *out, size_t outcap)
{
	header_field_t const *f = header_first(c, name) ;
	if (!f)
		return 0 ;

	if (f->valuelen >= outcap)
		return -1 ;

	memcpy(out, c->buf + f->value, f->valuelen) ;
	out[f->valuelen] = 0 ;

	return 1 ;
}

static int b64decode(char const *in, size_t inlen, unsigned char *out, size_t outcap, size_t *outlen)
{
	static char const ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/" ;
	size_t produced = 0 ;

	if (inlen % 4)
		return 0 ;

	for (size_t i = 0 ; i < inlen ; i += 4) {

		uint32_t quantum = 0 ;
		size_t pad = 0 ;

		for (size_t k = 0 ; k < 4 ; k++) {

			char ch = in[i + k] ;
			char const *hit ;

			if (ch == '=') {
				/* Padding is only ever the last one or two characters. */
				if (i + 4 != inlen || k < 2)
					return 0 ;

				pad++ ;
				quantum <<= 6 ;
				continue ;
			}

			if (pad)
				return 0 ; // data after padding

			hit = memchr(ALPHABET, ch, sizeof(ALPHABET) - 1) ;

			if (!hit)
				return 0 ;

			quantum = (quantum << 6) | (uint32_t)(hit - ALPHABET) ;
		}

		if (produced + (3 - pad) > outcap)
			return 0 ;

		out[produced++] = (unsigned char)(quantum >> 16) ;

		if (pad < 2)
			out[produced++] = (unsigned char)(quantum >> 8) ;

		if (pad < 1)
			out[produced++] = (unsigned char)quantum ;
	}

	*outlen = produced ;
	return 1 ;
}

static int host_matches(char const *value, char const *name)
{
	size_t nlen = strlen(name) ;

	if (strncasecmp(value, name, nlen))
		return 0 ;

	if (!value[nlen])
		return 1 ;

	if (value[nlen] != ':')
		return 0 ;

	return !strcmp(value + nlen + 1, g_portstr) ;
}

static int host_header_ok(char const *value)
{
	static char const *const LOOPBACK_NAMES[] = { "127.0.0.1", "localhost", "[::1]" } ;
	char host[HOSTPORT_MAX] ;
	char const *colon = strrchr(g_hostport, ':') ;
	size_t hlen = (size_t)(colon - g_hostport) ;

	memcpy(host, g_hostport, hlen) ;
	host[hlen] = 0 ;

	if (host_matches(value, host))
		return 1 ;

	for (size_t i = 0 ; i < nb_el(LOOPBACK_NAMES) ; i++) {

		if (host_matches(value, LOOPBACK_NAMES[i]))
			return 1 ;
	}

	return 0 ;
}

static int origin_ok(char const *value)
{
	char const *authority ;

	if (!strncasecmp(value, "http://", 7)) {
		authority = value + 7 ;
	} else if (!strncasecmp(value, "https://", 8)) {
		authority = value + 8 ;
	} else return 0 ;

	return host_header_ok(authority) ;
}

static int authorize(conn_t *c, int is_post)
{
	char value[AUTHZ_MAX] ;
	unsigned char raw[AUTHZ_MAX] ;
	char *pass ;
	size_t rawlen ;

	if (g_loopback_enforce) {

		int host = header_value(c, "host", value, AUTHZ_MAX) ;
		if (host != 1 || !host_header_ok(value)) {
			conn_send_status(c, 403, "Forbidden") ;
			return 0 ;
		}
	}

	if (is_post) {

		if (g_loopback_enforce) {

			int origin = header_value(c, "origin", value, AUTHZ_MAX) ;
			if (origin == -1 || (origin == 1 && !origin_ok(value))) {
				conn_send_status(c, 403, "Forbidden") ;
				return 0 ;
			}
		}

		if (header_value(c, CSRF_HEADER, value, AUTHZ_MAX) != 1) {
			conn_send_status(c, 403, "Forbidden") ;
			return 0 ;
		}
	}

	int authz = header_value(c, "authorization", value, AUTHZ_MAX) ;
	if (authz != 1 || strncasecmp(value, "Basic ", 6)) {
		conn_send_unauthorized(c) ;
		return 0 ;
	}

	char const *b64 = value + 6 ;
	while (*b64 == ' ' || *b64 == '\t')
		b64++ ;

	if (!b64decode(b64, strlen(b64), raw, AUTHZ_MAX - 1, &rawlen) || memchr(raw, 0, rawlen)) {
		conn_send_unauthorized(c) ;
		return 0 ;
	}

	raw[rawlen] = 0 ;

	pass = strchr((char *)raw, ':') ;

	if (!pass) {
		conn_send_unauthorized(c) ;
		return 0 ;
	}

	*pass++ = 0 ;

	if (!auth_check(g_cred, (char *)raw, pass)) {
		conn_send_unauthorized(c) ;
		return 0 ;
	}

	return 1 ;
}

static void conn_send_action_failure(conn_t *c)
{
	api_response_t ar = { 0 } ;

	if (!resp_error(500, "fork or pipe failed", &ar)) {
		log_warnusys("build the failure response of a 66 action") ;
		conn_send_status(c, 500, "Internal Server Error") ;
		return ;
	}

	conn_send(c, ar.status, status_reason(ar.status), NULL, "application/json; charset=utf-8", ar.body, ar.bodylen, BODY_OWNED) ;
}

static void on_action_done(void *ctx, int exit_code, char const *output, size_t outlen)
{
	action_pending_t *p = (action_pending_t *)ctx ;
	conn_t *c = p->conn ;
	api_response_t ar = { 0 } ;
	int sent ;

	/* The command is over, so nothing is owed to it anymore. Saying so before
	 * answering matters: the guards that kept the connection out of everyone
	 * else's way for the duration read this flag, and the deadline the answer
	 * is about to arm is one of them. */
	c->async = 0 ;

	p->fmt(&p->ctx, exit_code, output, outlen, &ar) ;

	if (ar.status) {
		sent = conn_send(c, ar.status, status_reason(ar.status), NULL, "application/json; charset=utf-8",ar.body, ar.bodylen, BODY_OWNED) ;
	} else {
		free(ar.body) ;
		sent = conn_send_status(c, 404, "Not Found") ;
	}

	free(p) ;

	if (sent != SEND_PENDING)
		conn_cleanup(c) ;
}

static void on_action_abandoned(void *ctx)
{
	action_pending_t *p = (action_pending_t *)ctx ;
	conn_t *c = p->conn ;

	free(p) ;

	conn_cleanup(c) ;
}

static void dispatch(conn_t *c)
{
	if (!sse_stop_timer(&c->deadline))
		log_warnusys("disarm the header deadline of a connection") ;

	char *eol = memchr(c->buf, '\n', c->pos) ;
	if (!eol || eol == c->buf) {
		conn_send_status(c, 400, "Bad Request") ;
		return ;
	}

	char *method = c->buf ;
	char *sp1 = memchr(method, ' ', (size_t)(eol - method)) ;
	if (!sp1) {
		conn_send_status(c, 400, "Bad Request") ;
		return ;
	}

	size_t methodlen = (size_t)(sp1 - method) ;
	char *path = sp1 + 1 ;
	char *sp2 = memchr(path, ' ', (size_t)(eol - path)) ;

	if (!sp2) {
		conn_send_status(c, 400, "Bad Request") ;
		return ;
	}

	size_t pathlen = (size_t)(sp2 - path) ;
	if (pathlen >= PATH_MAX_LEN) {
		conn_send_status(c, 414, "URI Too Long") ;
		return ;
	}

	char pathz[PATH_MAX_LEN] ;
	memcpy(pathz, path, pathlen) ;
	pathz[pathlen] = 0 ;

	char *q = strchr(pathz, '?') ;
	if (q)
		*q = 0 ;

	if (!percent_decode(pathz)) {
		conn_send_status(c, 400, "Bad Request") ;
		return ;
	}

	int is_get = (methodlen == 3 && !memcmp(method, "GET",  3)) ;
	int is_post = (methodlen == 4 && !memcmp(method, "POST", 4)) ;

	if (!is_get && !is_post) {
		conn_send_status(c, 405, "Method Not Allowed") ;
		return ;
	}

	if (!authorize(c, is_post))
		return ;

	if (is_get && !strcmp(pathz, "/api/events")) {
		static char const sse_headers[] =
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: text/event-stream\r\n"
			"Cache-Control: no-cache\r\n"
			"Connection: keep-alive\r\n"
			"X-Accel-Buffering: no\r\n"
			"\r\n"
			": connected\n\n" ;

		if (!events_can_accept()) {
			conn_send_status(c, 503, "Service Unavailable") ;
			return ;
		}

		size_t off = 0 ;
		if (write_some(c->fd, sse_headers, sizeof(sse_headers) - 1, &off) != SEND_DONE) {
			/* client already dropped — fall through to normal cleanup */
			return ;
		}

		sse_stop_io(&c->watcher) ;
		c->watcher.fd = -1 ;
		int fd = c->fd ;
		c->fd  = -1 ;

		if (!events_register(fd))
			close_fd(fd) ;

		return ;
	}

	api_result_t r = { 0 } ;
	int routed = is_get ? api_route_get(pathz, &r) : api_route_post(pathz, c->buf + c->hend, c->need - c->hend, &r) ;

	if (routed && !r.async) {
		conn_send(c, r.sync.status, status_reason(r.sync.status), NULL, "application/json; charset=utf-8", r.sync.body, r.sync.bodylen, BODY_OWNED) ;
		return ;
	}

	if (routed) {

		action_pending_t *p = calloc(1, sizeof(*p)) ;
		if (!p) {
			log_warnusys("allocate the state of a pending 66 action") ;
			conn_send_action_failure(c) ;
			return ;
		}

		p->conn = c ;
		p->fmt  = r.act.fmt ;
		p->ctx  = r.act.ctx ;

		if (!action_start(c->loop, &r.act.argv, &on_action_done, &on_action_abandoned, p)) {
			log_warnusys("start a 66 action") ;
			conn_send_action_failure(c) ;
			free(p) ;
			return ;
		}

		if (!sse_stop_io(&c->watcher))
			log_warnusys("pause a connection for the duration of a 66 action") ;

		c->async = 1 ;
		return ;
	}

	if (is_post) {
		conn_send_status(c, 404, "Not Found") ;
		return ;
	}

	asset_t const *a = NULL ;
	if (!asset_lookup(pathz, &a)) {
		conn_send_status(c, 404, "Not Found") ;
		return ;
	}

	conn_send(c, 200, "OK", NULL, a->mime, a->bytes, a->len, BODY_STATIC) ;
}

static void on_client_write(conn_t *c, int event)
{
	if (event & (SSE_ERROR | SSE_HUP)) {
		conn_cleanup(c) ;
		return ;
	}

	if (conn_flush(c) == SEND_PENDING)
		return ;

	conn_cleanup(c) ;
}

static void on_client_io(sse_watcher_t *w, void *cbdata, int event)
{
	(void)w ;
	conn_t *c = (conn_t *)cbdata ;

	if (c->closing)
		return ;

	if (c->flushing) {
		on_client_write(c, event) ;
		return ;
	}

	if (c->async)
		return ;

	if (event & (SSE_ERROR | SSE_HUP | SSE_SHUT)) {
		conn_cleanup(c) ;
		return ;
	}

	size_t target = c->need ? c->need : REQ_HEADER_MAX ;
	if (!conn_reserve(c, target)) {
		conn_cleanup(c) ;
		return ;
	}

	size_t avail = c->cap - c->pos ;
	if (!avail) {
		/* Header cap hit without a complete header block. */
		if (conn_send_status(c, 431, "Request Header Fields Too Large") != SEND_PENDING)
			conn_cleanup(c) ;
		return ;
	}

	ssize_t n = io_read(c->fd, c->buf + c->pos, avail) ;
	if (n == 0) {
		conn_cleanup(c) ;
		return ;
	}

	if (n < 0) {

		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return ;

		conn_cleanup(c) ;
		return ;
	}

	c->pos += (size_t)n ;

	// Phase 1: find the end of headers and size the body.
	if (!c->need) {

		for (size_t i = 0 ; i + 4 <= c->pos ; i++) {

			if (!memcmp(c->buf + i, HEADERS_END, 4)) {
				c->hend = i + 4 ;
				break ;
			}
		}

		if (!c->hend) {

			if (c->pos >= REQ_HEADER_MAX) {

				if (conn_send_status(c, 431, "Request Header Fields Too Large") != SEND_PENDING)
					conn_cleanup(c) ;
			}

			return ; // need more header bytes
		}

		char const *eol = memchr(c->buf, '\n', c->hend) ;

		if (!header_parse(c, (size_t)(eol - c->buf) + 1)) {

			int r = (errno == E2BIG)
				? conn_send_status(c, 431, "Request Header Fields Too Large")
				: conn_send_status(c, 400, "Bad Request") ;

			if (r != SEND_PENDING)
				conn_cleanup(c) ;

			return ;
		}

		/* Ambiguous framing is a request-smuggling vector the moment a
		 * reverse proxy sits in front: we speak neither chunked transfer
		 * nor a repeated Content-Length, so refuse rather than settle on a
		 * body length the proxy may have read differently. */
		if (header_count(c, "transfer-encoding") || header_count(c, "content-length") > 1) {

			if (conn_send_status(c, 400, "Bad Request") != SEND_PENDING)
				conn_cleanup(c) ;

			return ;
		}

		uint64_t cl = 0 ;
		header_content_length(c, &cl) ;

		if (cl > REQ_BODY_MAX) {

			if (conn_send_status(c, 413, "Payload Too Large") != SEND_PENDING)
				conn_cleanup(c) ;

			return ;
		}
		c->need = c->hend + (size_t)cl ;
	}

	// Phase 2: wait until the whole body has arrived.
	if (c->pos < c->need)
		return ;

	dispatch(c) ;

	if (!c->async && !c->flushing)
		conn_cleanup(c) ;
}

static void on_conn_timeout(sse_watcher_t *w, void *cbdata, int event)
{
	(void)w ;
	(void)event ;
	conn_t *c = (conn_t *)cbdata ;

	if (c->closing)
		return ;

	if (c->async)
		return ;

	if (c->flushing) {
		/* Slow is not stalled: a client that took even one more byte than
		 * it had at the last look buys another window, up to the cap on how
		 * many windows one response may ever be granted. Dropped without a
		 * 408 either way -- a status line on top of a response already on
		 * the wire would be a second response. */
		ssize_t acked = conn_acked(c) ;

		if (acked >= 0 && (size_t)acked > c->acked && c->rearms < FLUSH_REARM_MAX) {
			c->acked = (size_t)acked ;
			c->rearms++ ;
			if (!sse_modify_timer(&c->deadline, WRITE_TIMEOUT_MS, 0) || !sse_restart_timer(&c->deadline))
				log_warnusys("arm the write deadline of a connection") ;
			return ;
		}
		conn_cleanup(c) ;
		return ;
	}

	if (conn_send_status(c, 408, "Request Timeout") != SEND_PENDING)
		conn_cleanup(c) ;
}

static void accept_pause(sse_watcher_t *w)
{
	if (!sse_stop_io(w))
		log_warnusys("pause the listen watcher") ;

	if (!sse_modify_timer(&g_rearm_timer, EMFILE_REARM_MS, 0)) {
		log_warnusys("arm the accept retry timer") ;
		if (!sse_restart_io(w))
			log_warnusys("resume the listen watcher") ;
	}
}

static void on_listen_rearm(sse_watcher_t *w, void *cbdata, int event)
{
	(void)w ;
	(void)event ;

	if (!sse_restart_io((sse_watcher_t *)cbdata))
		log_warnusys("resume the listen watcher") ;
}

void http_init(char const *host, uint16_t port, int loopback_enforce, auth_cred_t const *cred, sse_epoll_t *loop, sse_watcher_t *wlisten)
{
	if (!host || !cred || !loop || !wlisten)
		log_die(LOG_EXIT_SYS, "http_init called without a host, a credential, a loop or a listen watcher") ;

	g_portstr[u16_fmt(g_portstr, port)] = 0 ;

	if (strlen(host) + 1 + strlen(g_portstr) + 1 > HOSTPORT_MAX)
		log_die(LOG_EXIT_SYS, "format listen address: ", host) ;

	auto_strings(g_hostport, host, ":", g_portstr) ;

	g_loopback_enforce = loopback_enforce ;
	g_cred = cred ;

	/* A negative timeout leaves the timerfd unarmed: it costs a descriptor
	 * and nothing else until accept_pause() arms it. */
	if (!sse_start_timer(loop, &g_rearm_timer, &on_listen_rearm, wlisten, -1, 0, 0))
		log_dieusys(LOG_EXIT_SYS, "create the accept retry timer") ;
}

void on_accept(sse_watcher_t *w, void *cbdata, int event)
{
	sse_epoll_t *loop = (sse_epoll_t *)cbdata ;

	if (event & SSE_ERROR) {
		log_warnusys("listen socket reported error") ;
		loop->running = false ;
		return ;
	}

	for (;;) {

		int client = socketunix_accept(w->fd, O_NONBLOCK | O_CLOEXEC) ;
		if (client < 0) {

			if (error_isagain(errno))
				return ;

			if (errno == EMFILE || errno == ENFILE) {
				accept_pause(w) ;
				return ;
			}

			log_warnusys("accept incoming connection") ;
			return ;
		}

		/* At the cap, take the connection off the backlog and drop it
		 * rather than leave it there: the listen watcher is level
		 * triggered and would report the same pending connection on
		 * every loop iteration. */
		if (g_nconn >= MAX_CONN) {
			close_fd(client) ;
			continue ;
		}

		conn_t *c = calloc(1, sizeof(*c)) ;
		if (!c) {
			close_fd(client) ;
			log_warnusys("allocate connection state") ;
			continue ;
		}

		c->fd = client ;
		c->loop = loop ;

		if (!sse_start_io(loop, &c->watcher, &on_client_io, c, client, SSE_READ, 0)) {
			log_warnusys("register client fd with event loop") ;
			close_fd(client) ;
			free(c) ;
			continue ;
		}

		if (!sse_start_timer(loop, &c->deadline, &on_conn_timeout, c, HEADER_TIMEOUT_MS, 0, 0)) {
			log_warnusys("arm the header deadline of a connection") ;
			sse_free_io(&c->watcher) ;   /* closes client */
			free(c) ;
			continue ;
		}

		g_nconn++ ;
		conn_link(c) ;
	}
}

void http_shutdown(void)
{
	conn_t *c = g_conns ;

	while (c) {

		conn_t *next = c->next ;

		sse_free_timer(&c->deadline) ;
		sse_free_io(&c->watcher) ;
		conn_drop_response(c) ;
		conn_free(c) ;
		c = next ;
	}

	g_conns = NULL ;
	g_nconn = 0 ;
}
