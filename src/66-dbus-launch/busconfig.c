/*
 * busconfig.c
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

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <oblibs/genbuf.h>
#include <oblibs/log.h>
#include <oblibs/sbl.h>
#include <oblibs/strbuf.h>
#include <oblibs/types.h>

#include "busconfig.h"
#include "xml.h"

#define BUSCONFIG_NOT_APPLIED " is not applied: dbus-broker does not implement it"
#define BUSCONFIG_BROADER ", the effective policy is broader than the file requests"
#define BUSCONFIG_NARROWER ", the effective policy is narrower than the file requests"
#define BUSCONFIG_F_UNAPPLIED 0x01
#define BUSCONFIG_F_REPLY 0x02
#define BUSCONFIG_F_REQUIRED 0x04
#define BUSCONFIG_SBL(array) { .s = (array), .len = sizeof(array), .cap = sizeof(array), .heap = 0 }

typedef enum busconfig_value_e busconfig_value_t ;
enum busconfig_value_e
{
	BUSCONFIG_VALUE_STRING = 0,
	BUSCONFIG_VALUE_INTEGER,
	BUSCONFIG_VALUE_BOOLEAN,
	BUSCONFIG_VALUE_YESNO,
	BUSCONFIG_VALUE_CONTEXT,
	BUSCONFIG_VALUE_MESSAGE_TYPE,
	BUSCONFIG_VALUE_APPARMOR,
	BUSCONFIG_VALUE_LIMIT,
	BUSCONFIG_VALUE_ENDOFKEY
} ;

typedef enum busconfig_element_id_e busconfig_element_id_t ;
enum busconfig_element_id_e
{
	BUSCONFIG_E_BUSCONFIG = 0,
	BUSCONFIG_E_TYPE,
	BUSCONFIG_E_INCLUDE,
	BUSCONFIG_E_INCLUDEDIR,
	BUSCONFIG_E_USER,
	BUSCONFIG_E_FORK,
	BUSCONFIG_E_KEEP_UMASK,
	BUSCONFIG_E_SYSLOG,
	BUSCONFIG_E_PIDFILE,
	BUSCONFIG_E_ALLOW_ANONYMOUS,
	BUSCONFIG_E_LISTEN,
	BUSCONFIG_E_AUTH,
	BUSCONFIG_E_SERVICEDIR,
	BUSCONFIG_E_STANDARD_SESSION_SERVICEDIRS,
	BUSCONFIG_E_STANDARD_SYSTEM_SERVICEDIRS,
	BUSCONFIG_E_SERVICEHELPER,
	BUSCONFIG_E_LIMIT,
	BUSCONFIG_E_POLICY,
	BUSCONFIG_E_SELINUX,
	BUSCONFIG_E_APPARMOR,
	BUSCONFIG_E_ALLOW,
	BUSCONFIG_E_DENY,
	BUSCONFIG_E_ASSOCIATE,
	BUSCONFIG_E_ENDOFKEY
} ;

typedef struct busconfig_values_s busconfig_values, *busconfig_values_ref ;
struct busconfig_values_s
{
	strbuf accepted ;
	strbuf tolerated ;
} ;

typedef struct busconfig_attribute_s busconfig_attribute, *busconfig_attribute_ref ;
struct busconfig_attribute_s
{
	char const *name ;
	busconfig_value_t value ;
	uint8_t flags ;
} ;

typedef struct busconfig_element_s busconfig_element, *busconfig_element_ref ;
struct busconfig_element_s
{
	char const *name ;
	busconfig_element_id_t parent ;
	busconfig_attribute const *attributes ;
	uint8_t flags ;
} ;

typedef struct busconfig_parser_s busconfig_parser, *busconfig_parser_ref ;
struct busconfig_parser_s
{
	char const *name ;
	xml_handler const *handler ;
	void *userdata ;
	busconfig_error_code_t code ;
	genbuf stack ;
	size_t skip ;
	size_t line ;
} ;

static char busconfig_boolean[] = "true\0false" ;
static char busconfig_yesno[] = "yes\0no" ;
static char busconfig_context[] = "default\0mandatory" ;
static char busconfig_message_type[] = "method_call\0method_return\0signal\0error\0*" ;
static char busconfig_apparmor[] = "enabled\0disabled\0required" ;
static char busconfig_limit[] = "max_incoming_bytes\0max_incoming_unix_fds\0max_outgoing_bytes\0max_outgoing_unix_fds\0max_message_size\0max_message_unix_fds\0service_start_timeout\0auth_timeout\0pending_fd_timeout\0max_completed_connections\0max_incomplete_connections\0max_connections_per_user\0max_pending_service_starts\0max_names_per_connection\0max_match_rules_per_connection\0max_replies_per_connection\0reply_timeout" ;
static char busconfig_limit_container[] = "max_containers\0max_containers_per_user\0max_connections_per_container\0max_container_metadata_bytes" ;

static busconfig_values busconfig_value_sets[BUSCONFIG_VALUE_ENDOFKEY] = {
	[BUSCONFIG_VALUE_BOOLEAN] = { .accepted = BUSCONFIG_SBL(busconfig_boolean), .tolerated = STRBUF_ZERO },
	[BUSCONFIG_VALUE_YESNO] = { .accepted = BUSCONFIG_SBL(busconfig_yesno), .tolerated = STRBUF_ZERO },
	[BUSCONFIG_VALUE_CONTEXT] = { .accepted = BUSCONFIG_SBL(busconfig_context), .tolerated = STRBUF_ZERO },
	[BUSCONFIG_VALUE_MESSAGE_TYPE] = { .accepted = BUSCONFIG_SBL(busconfig_message_type), .tolerated = STRBUF_ZERO },
	[BUSCONFIG_VALUE_APPARMOR] = { .accepted = BUSCONFIG_SBL(busconfig_apparmor), .tolerated = STRBUF_ZERO },
	[BUSCONFIG_VALUE_LIMIT] = { .accepted = BUSCONFIG_SBL(busconfig_limit), .tolerated = BUSCONFIG_SBL(busconfig_limit_container) }
} ;

static busconfig_attribute const busconfig_attributes_none[] = {
	{ .name = 0 }
} ;

static busconfig_attribute const busconfig_attributes_include[] = {
	{ .name = "ignore_missing", .value = BUSCONFIG_VALUE_YESNO, .flags = 0 },
	{ .name = "if_selinux_enabled", .value = BUSCONFIG_VALUE_YESNO, .flags = 0 },
	{ .name = "selinux_root_relative", .value = BUSCONFIG_VALUE_YESNO, .flags = 0 },
	{ .name = 0 }
} ;

static busconfig_attribute const busconfig_attributes_limit[] = {
	{ .name = "name", .value = BUSCONFIG_VALUE_LIMIT, .flags = BUSCONFIG_F_REQUIRED },
	{ .name = 0 }
} ;

static busconfig_attribute const busconfig_attributes_policy[] = {
	{ .name = "context", .value = BUSCONFIG_VALUE_CONTEXT, .flags = 0 },
	{ .name = "at_console", .value = BUSCONFIG_VALUE_BOOLEAN, .flags = 0 },
	{ .name = "user", .value = BUSCONFIG_VALUE_STRING, .flags = 0 },
	{ .name = "group", .value = BUSCONFIG_VALUE_STRING, .flags = 0 },
	{ .name = 0 }
} ;

static busconfig_attribute const busconfig_attributes_apparmor[] = {
	{ .name = "mode", .value = BUSCONFIG_VALUE_APPARMOR, .flags = 0 },
	{ .name = 0 }
} ;

static busconfig_attribute const busconfig_attributes_rule[] = {
	{ .name = "send_interface", .value = BUSCONFIG_VALUE_STRING, .flags = 0 },
	{ .name = "send_member", .value = BUSCONFIG_VALUE_STRING, .flags = 0 },
	{ .name = "send_error", .value = BUSCONFIG_VALUE_STRING, .flags = BUSCONFIG_F_UNAPPLIED },
	{ .name = "send_broadcast", .value = BUSCONFIG_VALUE_BOOLEAN, .flags = 0 },
	{ .name = "send_destination", .value = BUSCONFIG_VALUE_STRING, .flags = 0 },
	{ .name = "send_destination_prefix", .value = BUSCONFIG_VALUE_STRING, .flags = BUSCONFIG_F_UNAPPLIED },
	{ .name = "send_type", .value = BUSCONFIG_VALUE_MESSAGE_TYPE, .flags = 0 },
	{ .name = "send_path", .value = BUSCONFIG_VALUE_STRING, .flags = 0 },
	{ .name = "receive_interface", .value = BUSCONFIG_VALUE_STRING, .flags = 0 },
	{ .name = "receive_member", .value = BUSCONFIG_VALUE_STRING, .flags = 0 },
	{ .name = "receive_error", .value = BUSCONFIG_VALUE_STRING, .flags = BUSCONFIG_F_UNAPPLIED },
	{ .name = "receive_sender", .value = BUSCONFIG_VALUE_STRING, .flags = 0 },
	{ .name = "receive_type", .value = BUSCONFIG_VALUE_MESSAGE_TYPE, .flags = 0 },
	{ .name = "receive_path", .value = BUSCONFIG_VALUE_STRING, .flags = 0 },
	{ .name = "send_requested_reply", .value = BUSCONFIG_VALUE_BOOLEAN, .flags = BUSCONFIG_F_REPLY },
	{ .name = "receive_requested_reply", .value = BUSCONFIG_VALUE_BOOLEAN, .flags = BUSCONFIG_F_REPLY },
	{ .name = "eavesdrop", .value = BUSCONFIG_VALUE_BOOLEAN, .flags = 0 },
	{ .name = "own", .value = BUSCONFIG_VALUE_STRING, .flags = 0 },
	{ .name = "own_prefix", .value = BUSCONFIG_VALUE_STRING, .flags = 0 },
	{ .name = "user", .value = BUSCONFIG_VALUE_STRING, .flags = 0 },
	{ .name = "group", .value = BUSCONFIG_VALUE_STRING, .flags = 0 },
	{ .name = "min_fds", .value = BUSCONFIG_VALUE_INTEGER, .flags = 0 },
	{ .name = "max_fds", .value = BUSCONFIG_VALUE_INTEGER, .flags = 0 },
	{ .name = "log", .value = BUSCONFIG_VALUE_BOOLEAN, .flags = BUSCONFIG_F_UNAPPLIED },
	{ .name = 0 }
} ;

static busconfig_attribute const busconfig_attributes_associate[] = {
	{ .name = "own", .value = BUSCONFIG_VALUE_STRING, .flags = BUSCONFIG_F_REQUIRED },
	{ .name = "context", .value = BUSCONFIG_VALUE_STRING, .flags = BUSCONFIG_F_REQUIRED },
	{ .name = 0 }
} ;

static busconfig_element const busconfig_elements[BUSCONFIG_E_ENDOFKEY] = {
	[BUSCONFIG_E_BUSCONFIG] = { .name = "busconfig", .parent = BUSCONFIG_E_ENDOFKEY, .attributes = busconfig_attributes_none, .flags = 0 },
	[BUSCONFIG_E_TYPE] = { .name = "type", .parent = BUSCONFIG_E_BUSCONFIG, .attributes = busconfig_attributes_none, .flags = 0 },
	[BUSCONFIG_E_INCLUDE] = { .name = "include", .parent = BUSCONFIG_E_BUSCONFIG, .attributes = busconfig_attributes_include, .flags = 0 },
	[BUSCONFIG_E_INCLUDEDIR] = { .name = "includedir", .parent = BUSCONFIG_E_BUSCONFIG, .attributes = busconfig_attributes_none, .flags = 0 },
	[BUSCONFIG_E_USER] = { .name = "user", .parent = BUSCONFIG_E_BUSCONFIG, .attributes = busconfig_attributes_none, .flags = 0 },
	[BUSCONFIG_E_FORK] = { .name = "fork", .parent = BUSCONFIG_E_BUSCONFIG, .attributes = busconfig_attributes_none, .flags = 0 },
	[BUSCONFIG_E_KEEP_UMASK] = { .name = "keep_umask", .parent = BUSCONFIG_E_BUSCONFIG, .attributes = busconfig_attributes_none, .flags = 0 },
	[BUSCONFIG_E_SYSLOG] = { .name = "syslog", .parent = BUSCONFIG_E_BUSCONFIG, .attributes = busconfig_attributes_none, .flags = 0 },
	[BUSCONFIG_E_PIDFILE] = { .name = "pidfile", .parent = BUSCONFIG_E_BUSCONFIG, .attributes = busconfig_attributes_none, .flags = 0 },
	[BUSCONFIG_E_ALLOW_ANONYMOUS] = { .name = "allow_anonymous", .parent = BUSCONFIG_E_BUSCONFIG, .attributes = busconfig_attributes_none, .flags = BUSCONFIG_F_UNAPPLIED },
	[BUSCONFIG_E_LISTEN] = { .name = "listen", .parent = BUSCONFIG_E_BUSCONFIG, .attributes = busconfig_attributes_none, .flags = 0 },
	[BUSCONFIG_E_AUTH] = { .name = "auth", .parent = BUSCONFIG_E_BUSCONFIG, .attributes = busconfig_attributes_none, .flags = 0 },
	[BUSCONFIG_E_SERVICEDIR] = { .name = "servicedir", .parent = BUSCONFIG_E_BUSCONFIG, .attributes = busconfig_attributes_none, .flags = 0 },
	[BUSCONFIG_E_STANDARD_SESSION_SERVICEDIRS] = { .name = "standard_session_servicedirs", .parent = BUSCONFIG_E_BUSCONFIG, .attributes = busconfig_attributes_none, .flags = 0 },
	[BUSCONFIG_E_STANDARD_SYSTEM_SERVICEDIRS] = { .name = "standard_system_servicedirs", .parent = BUSCONFIG_E_BUSCONFIG, .attributes = busconfig_attributes_none, .flags = 0 },
	[BUSCONFIG_E_SERVICEHELPER] = { .name = "servicehelper", .parent = BUSCONFIG_E_BUSCONFIG, .attributes = busconfig_attributes_none, .flags = 0 },
	[BUSCONFIG_E_LIMIT] = { .name = "limit", .parent = BUSCONFIG_E_BUSCONFIG, .attributes = busconfig_attributes_limit, .flags = 0 },
	[BUSCONFIG_E_POLICY] = { .name = "policy", .parent = BUSCONFIG_E_BUSCONFIG, .attributes = busconfig_attributes_policy, .flags = 0 },
	[BUSCONFIG_E_SELINUX] = { .name = "selinux", .parent = BUSCONFIG_E_BUSCONFIG, .attributes = busconfig_attributes_none, .flags = 0 },
	[BUSCONFIG_E_APPARMOR] = { .name = "apparmor", .parent = BUSCONFIG_E_BUSCONFIG, .attributes = busconfig_attributes_apparmor, .flags = 0 },
	[BUSCONFIG_E_ALLOW] = { .name = "allow", .parent = BUSCONFIG_E_POLICY, .attributes = busconfig_attributes_rule, .flags = 0 },
	[BUSCONFIG_E_DENY] = { .name = "deny", .parent = BUSCONFIG_E_POLICY, .attributes = busconfig_attributes_rule, .flags = 0 },
	[BUSCONFIG_E_ASSOCIATE] = { .name = "associate", .parent = BUSCONFIG_E_SELINUX, .attributes = busconfig_attributes_associate, .flags = 0 }
} ;

static char const *const busconfig_error_msg[] = {
	[BUSCONFIG_ERROR_NONE] = "no error",
	[BUSCONFIG_ERROR_XML] = "invalid XML",
	[BUSCONFIG_ERROR_NOMEM] = "out of memory",
	[BUSCONFIG_ERROR_HANDLER] = "rejected by the handler",
	[BUSCONFIG_ERROR_PLACEMENT] = "element not allowed at this place",
	[BUSCONFIG_ERROR_ATTRIBUTE] = "unknown attribute",
	[BUSCONFIG_ERROR_VALUE] = "invalid attribute value",
	[BUSCONFIG_ERROR_MISSING] = "missing mandatory attribute"
} ;

char const *busconfig_error_str(busconfig_error_code_t code)
{
	return busconfig_error_msg[code] ;
}

static int busconfig_fail(busconfig_parser *b, busconfig_error_code_t code)
{
	b->code = code ;

	return 0 ;
}

static busconfig_element_id_t busconfig_parent(busconfig_parser const *b)
{
	size_t depth = genbuf_len(uint8_t, &b->stack) ;

	return depth ? (busconfig_element_id_t)genbuf_s(uint8_t, &b->stack)[depth - 1] : BUSCONFIG_E_ENDOFKEY ;
}

static busconfig_element_id_t busconfig_element_find(char const *name)
{
	for (size_t i = 0 ; i < BUSCONFIG_E_ENDOFKEY ; i++)
		if (!strcmp(busconfig_elements[i].name, name))
			return (busconfig_element_id_t)i ;

	return BUSCONFIG_E_ENDOFKEY ;
}

static busconfig_attribute const *busconfig_attribute_find(busconfig_element const *e, char const *name)
{
	for (busconfig_attribute const *d = e->attributes ; d->name ; d++)
		if (!strcmp(d->name, name))
			return d ;

	return 0 ;
}

static int busconfig_invalid(busconfig_parser *b, busconfig_element const *e, xml_attribute const *a)
{
	flog_warn("%s:%zu: invalid value %s=\"%s\" of <%s>", b->name, b->line, a->name, a->value, e->name) ;

	return busconfig_fail(b, BUSCONFIG_ERROR_VALUE) ;
}

static int busconfig_value_check(busconfig_parser *b, busconfig_element const *e, busconfig_attribute const *d, xml_attribute const *a)
{
	busconfig_values *set = &busconfig_value_sets[d->value] ;
	uint64_t u ;

	if (d->value == BUSCONFIG_VALUE_STRING)
		return 1 ;

	if (d->value == BUSCONFIG_VALUE_INTEGER)
		return u64_scan_strict(a->value, &u) || busconfig_invalid(b, e, a) ;

	if (sbl_search(&set->accepted, a->value) >= 0)
		return 1 ;

	if (sbl_search(&set->tolerated, a->value) < 0)
		return busconfig_invalid(b, e, a) ;

	flog_warn("%s:%zu: value %s=\"%s\" of <%s>" BUSCONFIG_NOT_APPLIED, b->name, b->line, a->name, a->value, e->name) ;

	return 1 ;
}

static int busconfig_attribute_check(busconfig_parser *b, busconfig_element_id_t id, xml_attribute const *a)
{
	busconfig_element const *e = &busconfig_elements[id] ;
	busconfig_attribute const *d = busconfig_attribute_find(e, a->name) ;

	if (!d) {
		flog_warn("%s:%zu: unknown attribute %s of <%s>", b->name, b->line, a->name, e->name) ;
		return busconfig_fail(b, BUSCONFIG_ERROR_ATTRIBUTE) ;
	}

	if (!busconfig_value_check(b, e, d, a))
		return 0 ;

	if (d->flags & BUSCONFIG_F_UNAPPLIED)
		flog_warn("%s:%zu: attribute %s of <%s>" BUSCONFIG_NOT_APPLIED, b->name, b->line, a->name, e->name) ;

	if ((d->flags & BUSCONFIG_F_REPLY) && !strcmp(a->value, id == BUSCONFIG_E_DENY ? "true" : "false"))
		flog_warn("%s:%zu: value %s=\"%s\" of <%s>" BUSCONFIG_NOT_APPLIED "%s", b->name, b->line, a->name, a->value, e->name, id == BUSCONFIG_E_DENY ? BUSCONFIG_BROADER : BUSCONFIG_NARROWER) ;

	return 1 ;
}

static int busconfig_required_check(busconfig_parser *b, busconfig_element const *e, xml_attribute const *attributes, size_t nattributes)
{
	for (busconfig_attribute const *d = e->attributes ; d->name ; d++) {

		size_t i = 0 ;

		if (!(d->flags & BUSCONFIG_F_REQUIRED))
			continue ;

		while (i < nattributes && strcmp(attributes[i].name, d->name))
			i++ ;

		if (i == nattributes) {
			flog_warn("%s:%zu: missing attribute %s of <%s>", b->name, b->line, d->name, e->name) ;
			return busconfig_fail(b, BUSCONFIG_ERROR_MISSING) ;
		}
	}

	return 1 ;
}

static int busconfig_open(void *userdata, char const *name, xml_attribute const *attributes, size_t nattributes, size_t line)
{
	busconfig_parser *b = userdata ;
	busconfig_element_id_t id, parent = busconfig_parent(b) ;
	uint8_t push ;

	b->line = line ;

	if (b->skip) {
		b->skip++ ;
		return 1 ;
	}

	id = busconfig_element_find(name) ;

	if (id == BUSCONFIG_E_ENDOFKEY) {
		flog_warn("%s:%zu: unknown element <%s> is ignored with its content", b->name, b->line, name) ;
		b->skip = 1 ;
		return 1 ;
	}

	busconfig_element const *e = &busconfig_elements[id] ;

	if (e->parent != parent && parent == BUSCONFIG_E_ENDOFKEY) {
		flog_warn("%s:%zu: element <%s> is not allowed as the document root", b->name, b->line, name) ;
		return busconfig_fail(b, BUSCONFIG_ERROR_PLACEMENT) ;
	}

	if (e->parent != parent) {
		flog_warn("%s:%zu: element <%s> is not allowed inside <%s>", b->name, b->line, name, busconfig_elements[parent].name) ;
		return busconfig_fail(b, BUSCONFIG_ERROR_PLACEMENT) ;
	}

	for (size_t i = 0 ; i < nattributes ; i++)
		if (!busconfig_attribute_check(b, id, &attributes[i]))
			return 0 ;

	if (!busconfig_required_check(b, e, attributes, nattributes))
		return 0 ;

	if (e->flags & BUSCONFIG_F_UNAPPLIED)
		flog_warn("%s:%zu: element <%s>" BUSCONFIG_NOT_APPLIED, b->name, b->line, name) ;

	push = (uint8_t)id ;

	if (!genbuf_append(uint8_t, &b->stack, &push))
		return busconfig_fail(b, BUSCONFIG_ERROR_NOMEM) ;

	return b->handler->open(b->userdata, name, attributes, nattributes, line) == 1 || busconfig_fail(b, BUSCONFIG_ERROR_HANDLER) ;
}

static int busconfig_text(void *userdata, char const *text, size_t len, size_t line)
{
	busconfig_parser *b = userdata ;

	if (b->skip)
		return 1 ;

	return b->handler->text(b->userdata, text, len, line) == 1 || busconfig_fail(b, BUSCONFIG_ERROR_HANDLER) ;
}

static int busconfig_close(void *userdata, char const *name, size_t line)
{
	busconfig_parser *b = userdata ;

	if (b->skip) {
		b->skip-- ;
		return 1 ;
	}

	b->stack.len -= sizeof(uint8_t) ;

	return b->handler->close(b->userdata, name, line) == 1 || busconfig_fail(b, BUSCONFIG_ERROR_HANDLER) ;
}

static xml_handler const busconfig_handler = {
	.open = busconfig_open,
	.text = busconfig_text,
	.close = busconfig_close
} ;

static int busconfig_finish(busconfig_parser *b, int r, busconfig_error *err)
{
	genbuf_free(&b->stack) ;

	err->code = BUSCONFIG_ERROR_NONE ;

	if (!r)
		err->code = b->code ? b->code : BUSCONFIG_ERROR_XML ;

	return r ;
}

int busconfig_parse(char const *name, char const *s, size_t len, xml_handler const *handler, void *userdata, busconfig_error *err)
{
	log_flow() ;

	busconfig_parser b = { .name = name, .handler = handler, .userdata = userdata, .code = BUSCONFIG_ERROR_NONE, .stack = GENBUF_ZERO, .skip = 0 } ;

	return busconfig_finish(&b, xml_parse(name, s, len, &busconfig_handler, &b, &err->xml), err) ;
}

int busconfig_parse_file(char const *path, xml_handler const *handler, void *userdata, busconfig_error *err)
{
	log_flow() ;

	busconfig_parser b = { .name = path, .handler = handler, .userdata = userdata, .code = BUSCONFIG_ERROR_NONE, .stack = GENBUF_ZERO, .skip = 0 } ;

	return busconfig_finish(&b, xml_parse_file(path, &busconfig_handler, &b, &err->xml), err) ;
}
