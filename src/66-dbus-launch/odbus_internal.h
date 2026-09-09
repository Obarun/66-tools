/*
 * odbus_internal.h
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

#ifndef DBS_ODBUS_INTERNAL_H_INCLUDE
#define DBS_ODBUS_INTERNAL_H_INCLUDE

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <oblibs/io_rb.h>
#include <oblibs/strbuf.h>

#include "odbus.h"

/* Wire format, as specified for the classic D-Bus marshalling. */

#define ODBUS_ENDIAN_LE 'l'
#define ODBUS_ENDIAN_BE 'B'
#define ODBUS_PROTOCOL_VERSION 1
#define ODBUS_HEADER_FIXED_LEN 12 // the bytes preceding the header fields array length
#define ODBUS_FLAG_NO_REPLY_EXPECTED 0x01
#define ODBUS_MESSAGE_LEN_MAX (134217728u) // 128 MiB
#define ODBUS_ARRAY_LEN_MAX (67108864u) // 64 MiB
#define ODBUS_SIGNATURE_LEN_MAX 255
#define ODBUS_NAME_LEN_MAX 255
#define ODBUS_DEPTH_MAX 32
#define ODBUS_FDS_MAX 8

enum odbus_msgtype_e
{
	ODBUS_MESSAGE_METHOD_CALL = 1,
	ODBUS_MESSAGE_METHOD_RETURN = 2,
	ODBUS_MESSAGE_ERROR = 3,
	ODBUS_MESSAGE_SIGNAL = 4
} ;

enum odbus_field_e
{
	ODBUS_FIELD_PATH = 1,
	ODBUS_FIELD_INTERFACE = 2,
	ODBUS_FIELD_MEMBER = 3,
	ODBUS_FIELD_ERROR_NAME = 4,
	ODBUS_FIELD_REPLY_SERIAL = 5,
	ODBUS_FIELD_DESTINATION = 6,
	ODBUS_FIELD_SENDER = 7,
	ODBUS_FIELD_SIGNATURE = 8,
	ODBUS_FIELD_UNIX_FDS = 9
} ;

typedef struct odbus_frame_s odbus_frame, *odbus_frame_ref ;
struct odbus_frame_s
{
	int big_endian ;
	uint8_t type ;
	uint8_t flags ;
	uint32_t serial ;
	uint32_t fields_len ;
	size_t body_off ; // where the body starts, a multiple of eight
	size_t body_len ;
	size_t total ; // what the whole message occupies
} ;

typedef struct odbus_container_s odbus_container, *odbus_container_ref ;
struct odbus_container_s
{
	char type ; // 'a', 'r', 'e' or 'v'
	size_t contents_off ;
	size_t contents_len ;
	size_t index ;
	size_t length_off ;
	size_t begin ;
	size_t end ;
} ;

struct odbus_message_s
{
	odbus *bus ; // borrowed; a message must not outlive its bus
	uint8_t type ;
	uint8_t flags ;
	uint8_t sealed ;
	uint8_t big_endian ; // received messages only; we always emit little endian

	strbuf hdr ; // fixed header, header fields array, padding to eight
	strbuf body ;

	char const *path ;
	char const *interface ;
	char const *member ;
	char const *error_name ;
	uint32_t serial ;
	uint32_t reply_serial ;
	uint8_t has_reply_serial ;

	strbuf signature ; // top level signature, built as we append or copied from the wire
	strbuf csignature ; // arena of NUL terminated container content signatures
	size_t root_index ; // index into signature, for reading at depth zero

	odbus_container c[ODBUS_DEPTH_MAX] ;
	unsigned int n_c ;

	size_t rindex ; // read cursor, relative to the start of body

	int fds[ODBUS_FDS_MAX] ; // borrowed from the caller; never closed by us
	unsigned int n_fds ;

	odbus_message *next ; // link in the bus incoming queue
} ;

struct odbus_s
{
	int fd ; // borrowed from the caller; never closed by us
	uint8_t closed ; // the peer went away
	uint32_t serial ;

	strbuf in ;
	size_t in_index ;

	odbus_message *queue_head ;
	odbus_message *queue_tail ;

	odbus_message_handler_t filter ;
	void *filter_userdata ;

	char const *object_path ;
	char const *object_interface ;
	odbus_method const *methods ;
	void *object_userdata ;

	char error_name[ODBUS_NAME_LEN_MAX + 1] ;
	io_rb_t io ;
} ;

extern size_t odbus_align_up(size_t v, size_t align) ;
extern void odbus_st16le(char *d, uint16_t v) ;
extern void odbus_st32le(char *d, uint32_t v) ;
extern void odbus_st64le(char *d, uint64_t v) ;
extern uint16_t odbus_ld16(char const *s, int be) ;
extern uint32_t odbus_ld32(char const *s, int be) ;
extern uint64_t odbus_ld64(char const *s, int be) ;
extern int odbus_type_alignment(char type) ;
extern int odbus_type_is_basic(char type) ;
extern ssize_t odbus_type_len(char const *signature, unsigned int depth) ;
extern int odbus_signature_valid(char const *signature) ;

/* odbus_message.c */

extern int odbus_fail(int e) ;
extern int odbus_err(int e) ;
extern odbus_message *odbus_message_alloc(odbus *bus, uint8_t type) ;
extern int odbus_message_appendv(odbus_message *m, char const *types, va_list *ap) ;
extern int odbus_message_seal(odbus_message *m, uint32_t serial) ;
extern int odbus_frame_parse(char const *buf, size_t avail, odbus_frame *ret) ;
extern int odbus_message_from_wire(odbus *bus, char const *buf, size_t len, odbus_message **ret) ;

/* odbus_socket.c */

extern int odbus_socket_auth(odbus *bus) ;
extern int odbus_socket_recv(odbus *bus, int timeout_ms) ;
extern int odbus_socket_send(odbus *bus, odbus_message *m) ;
extern int odbus_socket_take(odbus *bus, odbus_message **ret) ;

#endif
