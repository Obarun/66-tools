/*
 * odbus_message.c
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
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <oblibs/log.h>
#include <oblibs/strbuf.h>

#include "odbus_internal.h"

static char const odbus_zeros[8] = { 0, 0, 0, 0, 0, 0, 0, 0 } ;

int odbus_fail(int e)
{
	errno = e ;
	return 0 ;
}

int odbus_err(int e)
{
	errno = e ;
	return -1 ;
}

static int strbuf_fail(void)
{
	return odbus_fail(errno ? errno : ENOMEM) ;
}

static int wire_bad(char const *reason)
{
	log_warn("malformed message from the peer: ", reason) ;

	return odbus_fail(EBADMSG) ;
}

static int wire_bad_err(char const *reason)
{
	log_warn("malformed message from the peer: ", reason) ;

	return odbus_err(EBADMSG) ;
}

static int strbuf_pad(strbuf *sb, size_t align)
{
	size_t pad = (align - (sb->len & (align - 1))) & (align - 1) ;

	if (!pad)
		return 1 ;

	return strbuf_catb(sb, odbus_zeros, pad) ;
}

/* Writing a basic value */

static int write_basic(strbuf *sb, char type, void const *p)
{
	char buf[8] ;
	int align = odbus_type_alignment(type) ;

	if (!align)
		return odbus_fail(EINVAL) ;

	if (!strbuf_pad(sb, (size_t)align))
		return strbuf_fail() ;

	switch (type) {

		case 'y' :

			return strbuf_catb(sb, (char const *)p, 1) ? 1 : strbuf_fail() ;

		case 'n' :
		case 'q' :
		{

			uint16_t v ;
			memcpy(&v, p, 2) ;
			odbus_st16le(buf, v) ;
			return strbuf_catb(sb, buf, 2) ? 1 : strbuf_fail() ;
		}

		case 'b' :
		case 'i' :
		case 'u' :
		case 'h' :
		{

			uint32_t v ;
			memcpy(&v, p, 4) ;
			odbus_st32le(buf, v) ;
			return strbuf_catb(sb, buf, 4) ? 1 : strbuf_fail() ;
		}

		case 'x' :
		case 't' :
		case 'd' :
		{

			uint64_t v ;
			memcpy(&v, p, 8) ;
			odbus_st64le(buf, v) ;
			return strbuf_catb(sb, buf, 8) ? 1 : strbuf_fail() ;
		}

		case 's' :
		case 'o' :
		case 'g' :
		{

			char const *v ;
			size_t l ;

			memcpy(&v, p, sizeof(char const *)) ;
			if (!v)
				return odbus_fail(EINVAL) ;

			l = strlen(v) ;

			if (type == 'g') {

				if (l > ODBUS_SIGNATURE_LEN_MAX)
					return odbus_fail(EINVAL) ;

				buf[0] = (char)l ;
				if (!strbuf_catb(sb, buf, 1))
					return strbuf_fail() ;

			} else {

				if (l > UINT32_MAX)
					return odbus_fail(EINVAL) ;

				odbus_st32le(buf, (uint32_t)l) ;
				if (!strbuf_catb(sb, buf, 4))
					return strbuf_fail() ;
			}

			if (!strbuf_catb(sb, v, l))
				return strbuf_fail() ;

			return strbuf_catb(sb, "", 1) ? 1 : strbuf_fail() ;
		}

		default :

			return odbus_fail(EINVAL) ;
	}
}

/* Signature bookkeeping while building */

static char const *csignature_at(odbus_message const *m, size_t off)
{
	return m->csignature.s + off ;
}

static int csignature_push(odbus_message *m, char const *s, size_t len, size_t *off)
{
	*off = m->csignature.len ;

	if (!strbuf_catb(&m->csignature, s, len))
		return strbuf_fail() ;

	if (!strbuf_catb(&m->csignature, "", 1))
		return strbuf_fail() ;

	return 1 ;
}

static int signature_step_write(odbus_message *m, char const *t, size_t tlen)
{
	odbus_container *c ;
	char const *contents ;

	if (!m->n_c) {

		if (m->signature.len + tlen > ODBUS_SIGNATURE_LEN_MAX)
			return odbus_fail(EINVAL) ;

		return strbuf_catb(&m->signature, t, tlen) ? 1 : strbuf_fail() ;
	}

	c = &m->c[m->n_c - 1] ;
	contents = csignature_at(m, c->contents_off) ;

	/** An array repeats a single type, so its cursor never moves. */
	if (c->type == 'a') {

		if (c->contents_len != tlen || memcmp(contents, t, tlen))
			return odbus_fail(EINVAL) ;

		return 1 ;
	}

	if (c->index + tlen > c->contents_len || memcmp(contents + c->index, t, tlen))
		return odbus_fail(EINVAL) ;

	c->index += tlen ;

	return 1 ;
}

/* Containers */

static int container_signature(char type, char const *contents, size_t contents_len, char *out, size_t *outlen, int *align)
{
	size_t len ;

	if (!contents_len || contents_len > ODBUS_SIGNATURE_LEN_MAX)
		return odbus_fail(EINVAL) ;

	switch (type) {

		case 'a' :

			out[0] = 'a' ;
			memcpy(out + 1, contents, contents_len) ;
			len = contents_len + 1 ;
			*align = 4 ;
			break ;

		case 'v' :
		{

			char cbuf[ODBUS_SIGNATURE_LEN_MAX + 1] ;

			/** A variant announces only itself on the wire; the type it holds is
			 * spelled out inside it, so it is checked on its own. */
			memcpy(cbuf, contents, contents_len) ;
			cbuf[contents_len] = 0 ;

			if (odbus_type_len(cbuf, 0) != (ssize_t)contents_len)
				return odbus_fail(EINVAL) ;

			out[0] = 'v' ;
			len = 1 ;
			*align = 1 ;
			break ;
		}

		case 'r' :
		case 'e' :

			out[0] = type == 'r' ? '(' : '{' ;
			memcpy(out + 1, contents, contents_len) ;
			out[contents_len + 1] = type == 'r' ? ')' : '}' ;
			len = contents_len + 2 ;
			*align = 8 ;
			break ;

		default :

			return odbus_fail(EINVAL) ;
	}

	out[len] = 0 ;

	/** Measuring the whole spelling is what enforces an array of exactly one
	 * element type, a struct of at least one field, and a dict entry of exactly
	 * two whose key is basic. */
	if (type != 'v' && odbus_type_len(out, 0) != (ssize_t)len)
		return odbus_fail(EINVAL) ;

	*outlen = len ;

	return 1 ;
}

static int container_open(odbus_message *m, char type, char const *contents, size_t contents_len)
{
	char full[ODBUS_SIGNATURE_LEN_MAX + 3] ;
	odbus_container *c ;
	size_t fulllen ;
	int align ;

	if (m->n_c >= ODBUS_DEPTH_MAX)
		return odbus_fail(EINVAL) ;

	if (!container_signature(type, contents, contents_len, full, &fulllen, &align))
		return 0 ;

	if (!signature_step_write(m, full, fulllen))
		return 0 ;

	if (!strbuf_pad(&m->body, (size_t)align))
		return strbuf_fail() ;

	c = &m->c[m->n_c] ;
	memset(c, 0, sizeof(*c)) ;
	c->type = type ;

	if (type == 'a') {

		int ealign = odbus_type_alignment(contents[0]) ;

		if (!ealign)
			return odbus_fail(EINVAL) ;

		c->length_off = m->body.len ;
		if (!strbuf_catb(&m->body, odbus_zeros, 4) ||
			/** The padding that aligns the first element sits between the length and
			* the elements, and is not counted in that length. An empty array carries
			* it all the same. */
			!strbuf_pad(&m->body, (size_t)ealign))
				return strbuf_fail() ;

		c->begin = m->body.len ;

	} else if (type == 'v') {

		char l = (char)contents_len ;

		if (!strbuf_catb(&m->body, &l, 1) ||
			!strbuf_catb(&m->body, contents, contents_len) ||
			!strbuf_catb(&m->body, "", 1))
				return strbuf_fail() ;

		/** The value of a variant is aligned on its own type, not on the signature
		 * that precedes it. Nothing is padded here because every value pads itself
		 * first, whether it is written by write_basic() or opened as a
		 * container, and a variant always holds exactly one. */
	}

	if (!csignature_push(m, contents, contents_len, &c->contents_off))
		return 0 ;

	c->contents_len = contents_len ;
	m->n_c++ ;

	return 1 ;
}

static int container_close(odbus_message *m)
{
	odbus_container *c ;

	if (!m->n_c)
		return odbus_fail(EINVAL) ;

	c = &m->c[m->n_c - 1] ;

	if (c->type == 'a') {

		size_t len = m->body.len - c->begin ;

		if (len > ODBUS_ARRAY_LEN_MAX)
			return odbus_fail(EINVAL) ;

		odbus_st32le(m->body.s + c->length_off, (uint32_t)len) ;

	} else if (c->index != c->contents_len) {

		return odbus_fail(EINVAL) ;
	}

	m->csignature.len = c->contents_off ;
	m->n_c-- ;

	return 1 ;
}

/* Appending */

static int append_basic(odbus_message *m, char type, void const *p)
{
	if (!signature_step_write(m, &type, 1))
		return 0 ;

	if (type == 'h') {

		uint32_t index ;
		int fd ;

		memcpy(&fd, p, sizeof(int)) ;
		if (fd < 0)
			return odbus_fail(EINVAL) ;

		if (m->n_fds >= ODBUS_FDS_MAX)
			return odbus_fail(EMFILE) ;

		index = m->n_fds ;
		m->fds[m->n_fds++] = fd ;

		return write_basic(&m->body, 'u', &index) ;
	}

	return write_basic(&m->body, type, p) ;
}

static int append_basic_va(odbus_message *m, char type, va_list *ap)
{
	switch (type) {

		case 'y' :
		{
			uint8_t v = (uint8_t)va_arg(*ap, int) ;
			return append_basic(m, type, &v) ;
		}

		case 'b' :
		{
			uint32_t v = va_arg(*ap, int) ? 1 : 0 ;
			return append_basic(m, type, &v) ;
		}

		case 'n' :
		{
			int16_t v = (int16_t)va_arg(*ap, int) ;
			return append_basic(m, type, &v) ;
		}

		case 'q' :
		{
			uint16_t v = (uint16_t)va_arg(*ap, int) ;
			return append_basic(m, type, &v) ;
		}

		case 'i' :
		{
			int32_t v = va_arg(*ap, int32_t) ;
			return append_basic(m, type, &v) ;
		}

		case 'u' :
		{
			uint32_t v = va_arg(*ap, uint32_t) ;
			return append_basic(m, type, &v) ;
		}

		case 'x' :
		{
			int64_t v = va_arg(*ap, int64_t) ;
			return append_basic(m, type, &v) ;
		}

		case 't' :
		{
			uint64_t v = va_arg(*ap, uint64_t) ;
			return append_basic(m, type, &v) ;
		}

		case 'd' :
		{
			double v = va_arg(*ap, double) ;
			return append_basic(m, type, &v) ;
		}

		case 'h' :
		{
			int v = va_arg(*ap, int) ;
			return append_basic(m, type, &v) ;
		}

		case 's' :
		case 'o' :
		case 'g' :
		{
			char const *v = va_arg(*ap, char const *) ;
			return append_basic(m, type, &v) ;
		}

		default :

			return odbus_fail(EINVAL) ;
	}
}

static int message_appendv_n(odbus_message *m, char const *types, size_t len, va_list *ap)
{
	size_t pos = 0 ;

	while (pos < len) {

		char t = types[pos] ;
		ssize_t tlen ;

		if (odbus_type_is_basic(t)) {

			if (!append_basic_va(m, t, ap))
				return 0 ;

			pos++ ;
			continue ;
		}

		/** An array or a variant cannot be inferred from the arguments: its
		 * element type has to be named, which is what open_container is for. */
		if (t != '(' && t != '{')
			return odbus_fail(EOPNOTSUPP) ;

		tlen = odbus_type_len(types + pos, 0) ;
		if (tlen < 2 || (size_t)tlen > len - pos)
			return odbus_fail(EINVAL) ;

		if (!container_open(m, t == '(' ? 'r' : 'e', types + pos + 1, (size_t)tlen - 2))
			return 0 ;

		if (!message_appendv_n(m, types + pos + 1, (size_t)tlen - 2, ap))
			return 0 ;

		if (!container_close(m))
			return 0 ;

		pos += (size_t)tlen ;
	}

	return 1 ;
}

/* Header fields */

static int hdr_field(strbuf *hdr, uint8_t code, char sigtype, void const *p)
{
	char buf[4] ;

	if (!strbuf_pad(hdr, 8))
		return strbuf_fail() ;

	buf[0] = (char)code ;
	buf[1] = 1 ;
	buf[2] = sigtype ;
	buf[3] = 0 ;

	if (!strbuf_catb(hdr, buf, 4))
		return strbuf_fail() ;

	return write_basic(hdr, sigtype, p) ;
}

int odbus_message_seal(odbus_message *m, uint32_t serial)
{
	size_t fields_begin ;

	if (!m)
		return odbus_fail(EINVAL) ;

	if (m->sealed)
		return 1 ;

	if (m->n_c)
		return odbus_fail(EINVAL) ;

	m->serial = serial ;

	// The fixed header and the fields array length, patched once both are known.
	if (!strbuf_copyb(&m->hdr, odbus_zeros, 8) || !strbuf_catb(&m->hdr, odbus_zeros, 8))
		return strbuf_fail() ;

	fields_begin = m->hdr.len ;

	if (m->path) {
		if (!hdr_field(&m->hdr, ODBUS_FIELD_PATH, 'o', &m->path))
			return 0 ;
	}

	if (m->interface) {
		if (!hdr_field(&m->hdr, ODBUS_FIELD_INTERFACE, 's', &m->interface))
			return 0 ;
	}

	if (m->member) {
		if (!hdr_field(&m->hdr, ODBUS_FIELD_MEMBER, 's', &m->member))
			return 0 ;
	}

	if (m->error_name) {
		if (!hdr_field(&m->hdr, ODBUS_FIELD_ERROR_NAME, 's', &m->error_name))
			return 0 ;
	}

	if (m->has_reply_serial) {
		if (!hdr_field(&m->hdr, ODBUS_FIELD_REPLY_SERIAL, 'u', &m->reply_serial))
			return 0 ;
	}

	if (m->signature.len) {

		char const *s ;

		if (!strbuf_uncounted(&m->signature))
			return strbuf_fail() ;

		s = m->signature.s ;
		if (!hdr_field(&m->hdr, ODBUS_FIELD_SIGNATURE, 'g', &s))
			return 0 ;
	}

	if (m->n_fds) {

		uint32_t n = m->n_fds ;

		if (!hdr_field(&m->hdr, ODBUS_FIELD_UNIX_FDS, 'u', &n))
			return 0 ;
	}

	odbus_st32le(m->hdr.s + ODBUS_HEADER_FIXED_LEN, (uint32_t)(m->hdr.len - fields_begin)) ;

	/** The body starts on a multiple of eight, which is why every offset inside
	 * it can be aligned as if it started at zero. */
	if (!strbuf_pad(&m->hdr, 8))
		return strbuf_fail() ;

	if (m->hdr.len + m->body.len > ODBUS_MESSAGE_LEN_MAX)
		return odbus_fail(EMSGSIZE) ;

	m->hdr.s[0] = ODBUS_ENDIAN_LE ;
	m->hdr.s[1] = (char)m->type ;
	m->hdr.s[2] = (char)m->flags ;
	m->hdr.s[3] = ODBUS_PROTOCOL_VERSION ;
	odbus_st32le(m->hdr.s + 4, (uint32_t)m->body.len) ;
	odbus_st32le(m->hdr.s + 8, serial) ;

	m->sealed = 1 ;

	return 1 ;
}

/* Message lifetime */

odbus_message *odbus_message_alloc(odbus *bus, uint8_t type)
{
	odbus_message *m = calloc(1, sizeof(*m)) ;

	if (!m) {
		errno = ENOMEM ;
		return NULL ;
	}

	m->bus = bus ;
	m->type = type ;

	return m ;
}

odbus_message *odbus_message_free(odbus_message *m)
{
	if (!m)
		return NULL ;

	strbuf_free(&m->hdr) ;
	strbuf_free(&m->body) ;
	strbuf_free(&m->signature) ;
	strbuf_free(&m->csignature) ;
	free(m) ;

	return NULL ;
}

/* Public building interface */

int odbus_message_new_method_call(odbus *bus, odbus_message **m, char const *path, char const *interface, char const *member)
{
	odbus_message *msg ;

	if (!bus || !m || !path || !interface || !member)
		return odbus_fail(EINVAL) ;

	msg = odbus_message_alloc(bus, ODBUS_MESSAGE_METHOD_CALL) ;
	if (!msg)
		return odbus_fail(ENOMEM) ;

	msg->path = path ;
	msg->interface = interface ;
	msg->member = member ;

	*m = msg ;

	return 1 ;
}

int odbus_message_appendv(odbus_message *m, char const *types, va_list *ap)
{
	if (!m)
		return odbus_fail(EINVAL) ;

	if (m->sealed)
		return odbus_fail(EPERM) ;

	if (!types || !*types)
		return 1 ;

	return message_appendv_n(m, types, strlen(types), ap) ;
}

int odbus_message_append(odbus_message *m, char const *types, ...)
{
	va_list ap ;
	int r ;

	va_start(ap, types) ;
	r = odbus_message_appendv(m, types, &ap) ;
	va_end(ap) ;

	return r ;
}

int odbus_message_open_container(odbus_message *m, char type, char const *contents)
{
	if (!m || !contents)
		return odbus_fail(EINVAL) ;

	if (m->sealed)
		return odbus_fail(EPERM) ;

	return container_open(m, type, contents, strlen(contents)) ;
}

int odbus_message_close_container(odbus_message *m)
{
	if (!m)
		return odbus_fail(EINVAL) ;

	if (m->sealed)
		return odbus_fail(EPERM) ;

	return container_close(m) ;
}

/* Inspection, valid in both directions */

char const *odbus_message_get_path(odbus_message *m)
{
	return m ? m->path : NULL ;
}

int odbus_message_is_signal(odbus_message *m, char const *interface, char const *member)
{
	if (!m || m->type != ODBUS_MESSAGE_SIGNAL)
		return 0 ;

	if (interface && (!m->interface || strcmp(m->interface, interface)))
		return 0 ;

	if (member && (!m->member || strcmp(m->member, member)))
		return 0 ;

	return 1 ;
}

/* Reading */

static size_t read_end(odbus_message const *m)
{
	unsigned int i = m->n_c ;

	while (i--)
		if (m->c[i].type == 'a')
			return m->c[i].end ;

	return m->body.len ;
}

static int signature_peek_read(odbus_message const *m, char const **t, size_t *tlen)
{
	char const *s ;
	ssize_t l ;

	if (!m->n_c) {

		s = m->signature.s ? m->signature.s + m->root_index : "" ;
		if (!*s)
			return 0 ;

	} else {

		odbus_container const *c = &m->c[m->n_c - 1] ;
		char const *contents = csignature_at(m, c->contents_off) ;

		if (c->type == 'a') {

			if (m->rindex >= c->end)
				return 0 ;

			s = contents ;

		} else {

			if (c->index >= c->contents_len)
				return 0 ;

			s = contents + c->index ;
		}
	}

	l = odbus_type_len(s, 0) ;
	if (!l)
		return odbus_err(EBADMSG) ;

	*t = s ;
	*tlen = (size_t)l ;

	return 1 ;
}

static void signature_advance_read(odbus_message *m, size_t tlen)
{
	if (!m->n_c) {
		m->root_index += tlen ;
		return ;
	}

	if (m->c[m->n_c - 1].type != 'a')
		m->c[m->n_c - 1].index += tlen ;
}

static int read_basic_at(char const *buf, size_t *pos, size_t end, int be, char type, void *p)
{
	int align = odbus_type_alignment(type) ;
	size_t at ;

	if (!align)
		return odbus_fail(EINVAL) ;

	at = odbus_align_up(*pos, (size_t)align) ;

	switch (type) {

		case 'y' :
		{

			uint8_t v ;

			if (at + 1 > end)
				return odbus_fail(EBADMSG) ;

			v = (uint8_t)buf[at] ;
			memcpy(p, &v, 1) ;
			*pos = at + 1 ;
			return 1 ;
		}

		case 'n' :
		case 'q' :
		{

			uint16_t v ;

			if (at + 2 > end)
				return odbus_fail(EBADMSG) ;

			v = odbus_ld16(buf + at, be) ;
			memcpy(p, &v, 2) ;
			*pos = at + 2 ;
			return 1 ;
		}

		case 'b' :
		case 'i' :
		case 'u' :
		{

			uint32_t v ;

			if (at + 4 > end)
				return odbus_fail(EBADMSG) ;

			v = odbus_ld32(buf + at, be) ;

			if (type == 'b' && v > 1)
				return odbus_fail(EBADMSG) ;

			memcpy(p, &v, 4) ;
			*pos = at + 4 ;
			return 1 ;
		}

		case 'x' :
		case 't' :
		case 'd' :
		{

			uint64_t v ;

			if (at + 8 > end)
				return odbus_fail(EBADMSG) ;

			v = odbus_ld64(buf + at, be) ;
			memcpy(p, &v, 8) ;
			*pos = at + 8 ;
			return 1 ;
		}

		case 's' :
		case 'o' :
		case 'g' :
		{

			char const *v ;
			size_t l ;

			if (type == 'g') {

				if (at + 1 > end)
					return odbus_fail(EBADMSG) ;

				l = (size_t)(unsigned char)buf[at] ;
				at += 1 ;

			} else {

				if (at + 4 > end)
					return odbus_fail(EBADMSG) ;

				l = odbus_ld32(buf + at, be) ;
				at += 4 ;
			}

			if (l > end - at || end - at - l < 1)
				return odbus_fail(EBADMSG) ;

			if (buf[at + l])
				return odbus_fail(EBADMSG) ;

			v = buf + at ;
			memcpy(p, &v, sizeof(char const *)) ;
			*pos = at + l + 1 ;
			return 1 ;
		}

		case 'h' :

			/** A file descriptor can only be read out of a message that carries
			 * one, and odbus refuses those on receipt. */
			return odbus_fail(EOPNOTSUPP) ;

		default :

			return odbus_fail(EINVAL) ;
	}
}

/** Read one basic value out of the body of @m, at its read cursor. */
static int read_basic(odbus_message *m, char type, void *p)
{
	return read_basic_at(m->body.s, &m->rindex, read_end(m), m->big_endian, type, p) ;
}

static int read_basic_va(odbus_message *m, char type, va_list *ap)
{
	switch (type) {

		case 'y' :

			return read_basic(m, type, va_arg(*ap, uint8_t *)) ;

		case 'b' :
		{

			int *dest = va_arg(*ap, int *) ;
			uint32_t v ;

			if (!read_basic(m, type, &v))
				return 0 ;

			*dest = (int)v ;
			return 1 ;
		}

		case 'n' :
		case 'q' :

			return read_basic(m, type, va_arg(*ap, uint16_t *)) ;

		case 'i' :
		case 'u' :

			return read_basic(m, type, va_arg(*ap, uint32_t *)) ;

		case 'x' :
		case 't' :

			return read_basic(m, type, va_arg(*ap, uint64_t *)) ;

		case 'd' :

			return read_basic(m, type, va_arg(*ap, double *)) ;

		case 's' :
		case 'o' :
		case 'g' :

			return read_basic(m, type, va_arg(*ap, char const **)) ;

		case 'h' :

			return odbus_fail(EOPNOTSUPP) ;

		default :

			return odbus_fail(EINVAL) ;
	}
}

static int container_enter(odbus_message *m, char type, char const *contents, size_t contents_len)
{
	char expected[ODBUS_SIGNATURE_LEN_MAX + 3] ;
	char const *t ;
	odbus_container *c ;
	size_t tlen, elen, end ;
	int align, r ;

	if (m->n_c >= ODBUS_DEPTH_MAX)
		return odbus_err(EINVAL) ;

	if (!container_signature(type, contents, contents_len, expected, &elen, &align))
		return -1 ;

	r = signature_peek_read(m, &t, &tlen) ;
	if (r < 0)
		return r ;

	/** Nothing left, or something other than the container asked for: say so
	 * without moving, exactly as sd_bus does. */
	if (!r || tlen != elen || memcmp(t, expected, elen))
		return 0 ;

	end = read_end(m) ;
	m->rindex = odbus_align_up(m->rindex, (size_t)align) ;

	c = &m->c[m->n_c] ;
	memset(c, 0, sizeof(*c)) ;
	c->type = type ;

	if (type == 'a') {

		int ealign = odbus_type_alignment(contents[0]) ;
		uint32_t len ;

		if (!ealign || m->rindex + 4 > end)
			return odbus_err(EBADMSG) ;

		len = odbus_ld32(m->body.s + m->rindex, m->big_endian) ;
		m->rindex += 4 ;

		if (len > ODBUS_ARRAY_LEN_MAX)
			return odbus_err(EBADMSG) ;

		/** The padding that aligns the first element is not part of the length. */
		m->rindex = odbus_align_up(m->rindex, (size_t)ealign) ;

		if (m->rindex > end || (size_t)len > end - m->rindex)
			return odbus_err(EBADMSG) ;

		c->end = m->rindex + len ;

	} else if (type == 'v') {

		size_t l ;

		if (m->rindex + 1 > end)
			return odbus_err(EBADMSG) ;

		l = (size_t)(unsigned char)m->body.s[m->rindex] ;
		m->rindex += 1 ;

		if (l > end - m->rindex || end - m->rindex - l < 1)
			return odbus_err(EBADMSG) ;

		if (l != contents_len || memcmp(m->body.s + m->rindex, contents, l) || m->body.s[m->rindex + l])
			return odbus_err(EBADMSG) ;

		m->rindex += l + 1 ;
	}

	if (!csignature_push(m, contents, contents_len, &c->contents_off))
		return -1 ;

	c->contents_len = contents_len ;

	/** Advance the enclosing cursor before the new frame becomes the innermost
	 * one. */
	signature_advance_read(m, tlen) ;
	m->n_c++ ;

	return 1 ;
}

static int container_exit(odbus_message *m)
{
	odbus_container *c ;

	if (!m->n_c)
		return odbus_fail(EINVAL) ;

	c = &m->c[m->n_c - 1] ;

	if (c->type == 'a') {

		if (m->rindex != c->end)
			return odbus_fail(EBUSY) ;

	} else if (c->index != c->contents_len) {

		return odbus_fail(EBUSY) ;
	}

	m->csignature.len = c->contents_off ;
	m->n_c-- ;

	return 1 ;
}

static int message_readv_n(odbus_message *m, char const *types, size_t len, va_list *ap)
{
	size_t pos = 0 ;
	int any = 0 ;

	while (pos < len) {

		char t = types[pos] ;
		char const *et ;
		size_t etlen, tlen ;
		ssize_t l ;
		int r ;

		if (odbus_type_is_basic(t)) {

			r = signature_peek_read(m, &et, &etlen) ;
			if (r < 0)
				return -1 ;

			if (!r)
				return any ? odbus_err(EBADMSG) : 0 ;

			if (etlen != 1 || *et != t)
				return odbus_err(EBADMSG) ;

			if (!read_basic_va(m, t, ap))
				return -1 ;

			signature_advance_read(m, 1) ;
			any = 1 ;
			pos++ ;
			continue ;
		}

		if (t != '(' && t != '{')
			return odbus_err(EOPNOTSUPP) ;

		l = odbus_type_len(types + pos, 0) ;
		if (l < 2 || (size_t)l > len - pos)
			return odbus_err(EINVAL) ;

		tlen = (size_t)l ;

		r = container_enter(m, t == '(' ? 'r' : 'e', types + pos + 1, tlen - 2) ;
		if (r < 0)
			return -1 ;

		if (!r)
			return any ? odbus_err(EBADMSG) : 0 ;

		if (message_readv_n(m, types + pos + 1, tlen - 2, ap) < 0)
			return -1 ;

		if (!container_exit(m))
			return -1 ;

		any = 1 ;
		pos += tlen ;
	}

	return 1 ;
}

/* Public reading interface */

int odbus_message_read(odbus_message *m, char const *types, ...)
{
	va_list ap ;
	int r ;

	if (!m)
		return odbus_err(EINVAL) ;

	if (!m->sealed)
		return odbus_err(EPERM) ;

	if (!types || !*types)
		return 1 ;

	va_start(ap, types) ;
	r = message_readv_n(m, types, strlen(types), &ap) ;
	va_end(ap) ;

	return r ;
}

int odbus_message_enter_container(odbus_message *m, char type, char const *contents)
{
	if (!m || !contents)
		return odbus_err(EINVAL) ;

	if (!m->sealed)
		return odbus_err(EPERM) ;

	return container_enter(m, type, contents, strlen(contents)) ;
}

int odbus_message_exit_container(odbus_message *m)
{
	if (!m)
		return odbus_fail(EINVAL) ;

	if (!m->sealed)
		return odbus_fail(EPERM) ;

	return container_exit(m) ;
}

int odbus_message_at_end(odbus_message *m)
{
	char const *t ;
	size_t tlen ;
	int r ;

	if (!m)
		return odbus_err(EINVAL) ;

	if (!m->sealed)
		return odbus_err(EPERM) ;

	r = signature_peek_read(m, &t, &tlen) ;
	if (r < 0)
		return r ;

	return r ? 0 : 1 ;
}

/* Turning received bytes into a message */

static int hdr_parse_fields(odbus_message *m, odbus_frame const *f)
{
	char const *h = m->hdr.s ;
	size_t end = (size_t)ODBUS_HEADER_FIXED_LEN + 4 + f->fields_len ;
	size_t pos = (size_t)ODBUS_HEADER_FIXED_LEN + 4 ;

	while (pos < end) {

		char const *s = NULL ;
		uint32_t u = 0 ;
		uint8_t code ;
		char sigtype ;

		pos = odbus_align_up(pos, 8) ;
		if (pos >= end)
			break ;

		if (end - pos < 4)
			return wire_bad("a truncated header field") ;

		code = (uint8_t)h[pos++] ;

		if ((size_t)(unsigned char)h[pos++] != 1)
			return wire_bad("a header field signature that is not one letter") ;

		sigtype = h[pos++] ;

		if (h[pos])
			return wire_bad("an unterminated header field signature") ;
		pos++ ;

		/** A header field carrying anything else cannot occur on the controller
		 * connection, and could not be skipped over safely. */
		if (sigtype != 'u' && sigtype != 's' && sigtype != 'o' && sigtype != 'g')
			return wire_bad("a header field of an unknown type") ;

		if (!read_basic_at(h, &pos, end, m->big_endian, sigtype, sigtype == 'u' ? (void *)&u : (void *)&s))
			return wire_bad("a header field value that cannot be read") ;

		switch (code) {

			case ODBUS_FIELD_PATH :

				if (sigtype != 'o')
					return wire_bad("a PATH field that is not an object path") ;
				m->path = s ;
				break ;

			case ODBUS_FIELD_INTERFACE :

				if (sigtype != 's')
					return wire_bad("an INTERFACE field that is not a string") ;
				m->interface = s ;
				break ;

			case ODBUS_FIELD_MEMBER :

				if (sigtype != 's')
					return wire_bad("a MEMBER field that is not a string") ;
				m->member = s ;
				break ;

			case ODBUS_FIELD_ERROR_NAME :

				if (sigtype != 's')
					return wire_bad("an ERROR_NAME field that is not a string") ;
				m->error_name = s ;
				break ;

			case ODBUS_FIELD_REPLY_SERIAL :

				if (sigtype != 'u')
					return wire_bad("a REPLY_SERIAL field that is not an unsigned integer") ;
				m->reply_serial = u ;
				m->has_reply_serial = 1 ;
				break ;

			case ODBUS_FIELD_SIGNATURE :

				if (sigtype != 'g')
					return wire_bad("a SIGNATURE field that is not a signature") ;

				if (!odbus_signature_valid(s))
					return wire_bad("an invalid body signature") ;

				if (!strbuf_copyb(&m->signature, s, strlen(s)) || !strbuf_uncounted(&m->signature))
					return strbuf_fail() ;
				break ;

			case ODBUS_FIELD_UNIX_FDS :

				if (sigtype != 'u')
					return wire_bad("a UNIX_FDS field that is not an unsigned integer") ;

				/** Nothing dbus-broker sends on the controller socket carries a
				 * file descriptor, so one announced here is a protocol error
				 * rather than something to make room for. */
				if (u) {

					flog_warn("the peer announced %u file descriptors on a message that may carry none", (unsigned int)u) ;

					return odbus_fail(EPROTO) ;
				}
				break ;

			default :

				/** DESTINATION, SENDER and anything else: decoded to keep the
				 * cursor honest, then dropped. */
				break ;
		}
	}

	if (pos != end)
		return wire_bad("header fields that do not fill the block they announce") ;

	return 1 ;
}

int odbus_frame_parse(char const *buf, size_t avail, odbus_frame *ret)
{
	char reason[160] ;
	uint32_t body_len, fields_len ;
	int be ;

	if (avail < (size_t)ODBUS_HEADER_FIXED_LEN + 4)
		return 0 ;

	if (buf[0] == ODBUS_ENDIAN_BE)
		be = 1 ;
	else if (buf[0] == ODBUS_ENDIAN_LE)
		be = 0 ;
	else {

		return wire_bad_err(log_fmt(reason, sizeof(reason), "unknown endianness flag 0x%02x", (unsigned int)(uint8_t)buf[0])) ;
	}

	if (buf[3] != ODBUS_PROTOCOL_VERSION) {

		return wire_bad_err(log_fmt(reason, sizeof(reason), "unknown protocol version %u", (unsigned int)(uint8_t)buf[3])) ;
	}

	if ((uint8_t)buf[1] < ODBUS_MESSAGE_METHOD_CALL || (uint8_t)buf[1] > ODBUS_MESSAGE_SIGNAL) {

		return wire_bad_err(log_fmt(reason, sizeof(reason), "unknown message type %u", (unsigned int)(uint8_t)buf[1])) ;
	}

	body_len = odbus_ld32(buf + 4, be) ;
	fields_len = odbus_ld32(buf + ODBUS_HEADER_FIXED_LEN, be) ;

	/** Bound both parts before adding them, so that the total cannot wrap. */
	if (body_len > ODBUS_MESSAGE_LEN_MAX || fields_len > ODBUS_MESSAGE_LEN_MAX) {

		return wire_bad_err(log_fmt(reason, sizeof(reason), "a body of %u bytes and header fields of %u, beyond the %u allowed", (unsigned int)body_len, (unsigned int)fields_len, (unsigned int)ODBUS_MESSAGE_LEN_MAX)) ;
	}

	ret->big_endian = be ;
	ret->type = (uint8_t)buf[1] ;
	ret->flags = (uint8_t)buf[2] ;
	ret->serial = odbus_ld32(buf + 8, be) ;
	ret->fields_len = fields_len ;
	ret->body_off = odbus_align_up((size_t)ODBUS_HEADER_FIXED_LEN + 4 + fields_len, 8) ;
	ret->body_len = body_len ;
	ret->total = ret->body_off + body_len ;

	if (ret->total > ODBUS_MESSAGE_LEN_MAX) {

		return wire_bad_err(log_fmt(reason, sizeof(reason), "a total length of %zu bytes, beyond the %u allowed", ret->total, (unsigned int)ODBUS_MESSAGE_LEN_MAX)) ;
	}

	return 1 ;
}

int odbus_message_from_wire(odbus *bus, char const *buf, size_t len, odbus_message **ret)
{
	odbus_message *m ;
	odbus_frame f ;
	int r ;

	if (!buf || !ret)
		return odbus_fail(EINVAL) ;

	r = odbus_frame_parse(buf, len, &f) ;
	if (r < 0)
		return 0 ;

	if (!r)
		return wire_bad("a header shorter than one whole message") ;

	if (f.total != len)
		return wire_bad("a framing that disagrees with the length it came with") ;

	m = odbus_message_alloc(bus, f.type) ;
	if (!m)
		return odbus_fail(ENOMEM) ;

	m->big_endian = (uint8_t)f.big_endian ;
	m->flags = f.flags ;
	m->serial = f.serial ;
	m->sealed = 1 ;

	if (!strbuf_copyb(&m->hdr, buf, f.body_off) || !strbuf_copyb(&m->body, buf + f.body_off, f.body_len)) {
		int e = errno ;
		odbus_message_free(m) ;
		return odbus_fail(e ? e : ENOMEM) ;
	}

	if (!hdr_parse_fields(m, &f)) {
		int e = errno ;
		odbus_message_free(m) ;
		return odbus_fail(e) ;
	}

	/** A message with a body must say what is in it, and one without must not
	 * claim otherwise. */
	if ((f.body_len && !m->signature.len) || (!f.body_len && m->signature.len)) {
		odbus_message_free(m) ;
		return wire_bad(f.body_len ? "a body with no signature to read it by" : "a signature announcing a body that is not there") ;
	}

	*ret = m ;

	return 1 ;
}
