/*
 * config.c
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

#include <dirent.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "busconfig.h"
#include "config.h"
#include "odbus_internal.h"
#include "policy.h"
#include "xml.h"

#include <oblibs/account.h>
#include <oblibs/directory.h>
#include <oblibs/genbuf.h>
#include <oblibs/log.h>
#include <oblibs/sbl.h>
#include <oblibs/strbuf.h>
#include <oblibs/string.h>
#include <oblibs/types.h>

#define CONFIG_SUFFIX ".conf"
#define CONFIG_WILDCARD "*"
#define CONFIG_YES "yes"

typedef enum config_element_e config_element_t ;
enum config_element_e
{
	CONFIG_ELEMENT_OTHER = 0,
	CONFIG_ELEMENT_INCLUDE,
	CONFIG_ELEMENT_INCLUDEDIR
} ;

typedef struct config_file_id_s config_file_id, *config_file_id_ref ;
struct config_file_id_s
{
	dev_t dev ;
	ino_t ino ;
} ;

typedef struct config_state_s config_state, *config_state_ref ;
struct config_state_s
{
	policy_t *policy ;
	genbuf files ;
} ;

typedef struct config_frame_s config_frame, *config_frame_ref ;
struct config_frame_s
{
	config_state *state ;
	char const *path ;
	char const *dir ;
	policy_context_t context ;
	uint32_t id ;
	config_element_t element ;
	uint8_t ignore_missing ;
	uint8_t if_selinux_enabled ;
	size_t line ;
	strbuf text ;
} ;

typedef struct config_value_s config_value, *config_value_ref ;
struct config_value_s
{
	char const *name ;
	uint32_t value ;
} ;

static config_value const config_message_types[] = {
	{ .name = "method_call", .value = ODBUS_MESSAGE_METHOD_CALL },
	{ .name = "method_return", .value = ODBUS_MESSAGE_METHOD_RETURN },
	{ .name = "error", .value = ODBUS_MESSAGE_ERROR },
	{ .name = "signal", .value = ODBUS_MESSAGE_SIGNAL },
	{ .name = CONFIG_WILDCARD, .value = POLICY_TYPE_ANY },
	{ .name = 0 }
} ;

static config_value const config_booleans[] = {
	{ .name = "true", .value = POLICY_TRISTATE_YES },
	{ .name = "false", .value = POLICY_TRISTATE_NO },
	{ .name = 0 }
} ;

static config_value const config_policy_contexts[] = {
	{ .name = "default", .value = POLICY_CONTEXT_DEFAULT },
	{ .name = "mandatory", .value = POLICY_CONTEXT_MANDATORY },
	{ .name = 0 }
} ;

static config_value const config_consoles[] = {
	{ .name = "true", .value = POLICY_CONTEXT_AT_CONSOLE },
	{ .name = "false", .value = POLICY_CONTEXT_NO_CONSOLE },
	{ .name = 0 }
} ;

static int config_file(config_state *s, char const *path, struct stat const *st) ;

static int config_value_find(config_value const *set, char const *name, uint32_t *value)
{
	for (; set->name ; set++)
		if (!strcmp(set->name, name)) {
			*value = set->value ;
			return 1 ;
		}

	return 0 ;
}

static int config_account_absent_or_error(void)
{
	return errno == ENOENT || errno == ESRCH || errno == EBADF || errno == EPERM ? 0 : -1 ;
}

static int config_uid(char const *name, uint32_t *id)
{
	_cleanup_strbuf_ strbuf account = STRBUF_ZERO ;
	uint32_t u ;
	uid_t uid ;

	if (u32_scan_strict(name, &u) && u != POLICY_ID_NONE) {

		if (!get_namebyuid((uid_t)u, &account))
			return config_account_absent_or_error() ;

		*id = u ;
		return 1 ;
	}

	if (get_uidbyname(name, &uid) < 0)
		return config_account_absent_or_error() ;

	*id = (uint32_t)uid ;
	return 1 ;
}

static int config_gid(char const *name, uint32_t *id)
{
	_cleanup_strbuf_ strbuf account = STRBUF_ZERO ;
	uint32_t u ;
	gid_t gid ;

	if (u32_scan_strict(name, &u) && u != POLICY_ID_NONE) {

		if (!get_groupbygid((gid_t)u, &account))
			return config_account_absent_or_error() ;

		*id = u ;
		return 1 ;
	}

	if (get_gidbygroup(name, &gid) < 0)
		return config_account_absent_or_error() ;

	*id = (uint32_t)gid ;
	return 1 ;
}

static int config_policy(config_frame *f, xml_attribute const *attributes, size_t nattributes, size_t line)
{
	f->context = POLICY_CONTEXT_NONE ;
	f->id = POLICY_ID_NONE ;

	for (size_t i = 0 ; i < nattributes ; i++) {

		char const *k = attributes[i].name, *v = attributes[i].value ;
		uint32_t context ;

		if (f->context != POLICY_CONTEXT_NONE)
			flog_warn("%s:%zu: <policy> has conflicting attributes, %s=\"%s\" wins", f->path, line, k, v) ;

		if (!strcmp(k, "user") || !strcmp(k, "group")) {

			int found ;

			f->context = !strcmp(k, "user") ? POLICY_CONTEXT_USER : POLICY_CONTEXT_GROUP ;
			found = f->context == POLICY_CONTEXT_USER ? config_uid(v, &f->id) : config_gid(v, &f->id) ;

			if (found < 0)
				flog_warnusys_return(LOG_EXIT_ZERO, "look up the %s %s of <policy> at %s:%zu", k, v, f->path, line) ;

			if (!found) {
				flog_warn("%s:%zu: unknown %s %s, the rules of its <policy> are ignored", f->path, line, k, v) ;
				f->id = POLICY_ID_NONE ;
			}

		} else if (!strcmp(k, "context") && config_value_find(config_policy_contexts, v, &context)) {

			f->context = (policy_context_t)context ;

		} else if (!strcmp(k, "at_console") && config_value_find(config_consoles, v, &context)) {

			f->context = (policy_context_t)context ;

		} else {

			flog_warn("%s:%zu: unexpected attribute %s=\"%s\" of <policy>", f->path, line, k, v) ;
			return 0 ;
		}
	}

	if (f->context == POLICY_CONTEXT_NONE)
		flog_warn("%s:%zu: <policy> has no context attribute, its rules are ignored", f->path, line) ;

	return 1 ;
}

static int config_rule_account(config_frame const *f, policy_rule *r, char const *k, char const *v)
{
	uint8_t user = !strcmp(k, "user") ;
	uint32_t *id = user ? &r->uid : &r->gid ;
	uint8_t *set = user ? &r->user : &r->group ;
	int found ;

	if (!strcmp(v, CONFIG_WILDCARD)) {
		*id = POLICY_ID_NONE ;
		*set = 1 ;
		return 1 ;
	}

	found = user ? config_uid(v, id) : config_gid(v, id) ;

	if (found < 0)
		flog_warnusys_return(LOG_EXIT_ZERO, "look up the %s %s at %s:%zu", k, v, f->path, r->line) ;

	if (found)
		*set = 1 ;
	else if (user)
		r->user_unknown = v ;
	else
		r->group_unknown = v ;

	return 1 ;
}

static int config_rule_attribute(config_frame const *f, policy_rule *r, char const *k, char const *v)
{
	if (!strcmp(k, "user") || !strcmp(k, "group"))
		return config_rule_account(f, r, k, v) ;

	if (!strcmp(k, "send_type")) {
		r->send_typed = 1 ;
		return config_value_find(config_message_types, v, &r->send_type) ;
	}

	if (!strcmp(k, "receive_type")) {
		r->recv_typed = 1 ;
		return config_value_find(config_message_types, v, &r->recv_type) ;
	}

	if (!strcmp(k, "send_broadcast"))
		return config_value_find(config_booleans, v, &r->send_broadcast) ;

	if (!strcmp(k, "send_requested_reply"))
		return config_value_find(config_booleans, v, &r->send_requested_reply) ;

	if (!strcmp(k, "receive_requested_reply"))
		return config_value_find(config_booleans, v, &r->recv_requested_reply) ;

	if (!strcmp(k, "eavesdrop"))
		return config_value_find(config_booleans, v, &r->eavesdrop) ;

	if (!strcmp(k, "min_fds"))
		return u64_scan_strict(v, &r->min_fds) > 0 ;

	if (!strcmp(k, "max_fds"))
		return u64_scan_strict(v, &r->max_fds) > 0 ;

	if (!strcmp(k, "log"))
		return 1 ;

	if (!strcmp(k, "own"))
		r->own = v ;
	else if (!strcmp(k, "own_prefix"))
		r->own_prefix = v ;
	else if (!strcmp(k, "send_interface"))
		r->send_interface = v ;
	else if (!strcmp(k, "send_member"))
		r->send_member = v ;
	else if (!strcmp(k, "send_error"))
		r->send_error = v ;
	else if (!strcmp(k, "send_destination"))
		r->send_destination = v ;
	else if (!strcmp(k, "send_destination_prefix"))
		r->send_destination_prefix = v ;
	else if (!strcmp(k, "send_path"))
		r->send_path = v ;
	else if (!strcmp(k, "receive_interface"))
		r->recv_interface = v ;
	else if (!strcmp(k, "receive_member"))
		r->recv_member = v ;
	else if (!strcmp(k, "receive_error"))
		r->recv_error = v ;
	else if (!strcmp(k, "receive_sender"))
		r->recv_sender = v ;
	else if (!strcmp(k, "receive_path"))
		r->recv_path = v ;
	else
		return 0 ;

	return 1 ;
}

static int config_rule(config_frame *f, char const *name, xml_attribute const *attributes, size_t nattributes, size_t line)
{
	policy_rule r = {
		.file = f->path,
		.line = line,
		.allow = !strcmp(name, "allow"),
		.context = f->context,
		.id = f->id,
		.uid = POLICY_ID_NONE,
		.gid = POLICY_ID_NONE,
		.max_fds = UINT64_MAX
	} ;

	for (size_t i = 0 ; i < nattributes ; i++)
		if (!config_rule_attribute(f, &r, attributes[i].name, attributes[i].value)) {
			flog_warn("%s:%zu: unexpected attribute %s=\"%s\" of <%s>", f->path, line, attributes[i].name, attributes[i].value, name) ;
			return 0 ;
		}

	if (!policy_import(f->state->policy, &r))
		flog_warnusys_return(LOG_EXIT_ZERO, "import the rule at %s:%zu", f->path, line) ;

	return 1 ;
}

static void config_include_open(config_frame *f, xml_attribute const *attributes, size_t nattributes, size_t line)
{
	f->element = CONFIG_ELEMENT_INCLUDE ;
	f->ignore_missing = 0 ;
	f->if_selinux_enabled = 0 ;
	f->line = line ;
	f->text.len = 0 ;

	for (size_t i = 0 ; i < nattributes ; i++) {

		if (!strcmp(attributes[i].name, "ignore_missing"))
			f->ignore_missing = !strcmp(attributes[i].value, CONFIG_YES) ;
		else if (!strcmp(attributes[i].name, "if_selinux_enabled"))
			f->if_selinux_enabled = !strcmp(attributes[i].value, CONFIG_YES) ;
	}
}

static int config_resolve(config_frame const *f, char const *element, strbuf *path)
{
	if (!f->text.len)
		flog_warn_return(LOG_EXIT_ZERO, "%s:%zu: <%s> names nothing", f->path, f->line, element) ;

	if (f->text.s[0] == '/')
		return auto_strbuf(path, f->text.s) ;

	return auto_strbuf(path, f->dir, f->text.s) ;
}

static int config_include_once(config_frame const *f, char const *path, struct stat const *st)
{
	size_t n = genbuf_len(config_file_id, &f->state->files) ;

	for (size_t i = 0 ; i < n ; i++) {

		config_file_id const *id = &genbuf_s(config_file_id, &f->state->files)[i] ;

		if (id->dev == st->st_dev && id->ino == st->st_ino) {
			flog_warn("%s:%zu: skip %s: it is already being read, the inclusion is recursive", f->path, f->line, path) ;
			return 1 ;
		}
	}

	return config_file(f->state, path, st) ;
}

static int config_include_close(config_frame *f)
{
	_cleanup_strbuf_ strbuf path = STRBUF_ZERO ;
	struct stat st ;

	if (f->if_selinux_enabled)
		return 1 ;

	if (!config_resolve(f, "include", &path))
		return 0 ;

	if (stat(path.s, &st) < 0) {

		if ((errno == ENOENT || errno == ENOTDIR) && f->ignore_missing)
			return 1 ;

		flog_warnsys_return(LOG_EXIT_ZERO, "%s:%zu: cannot include %s", f->path, f->line, path.s) ;
	}

	if (!S_ISREG(st.st_mode))
		flog_warn_return(LOG_EXIT_ZERO, "%s:%zu: cannot include %s: not a regular file", f->path, f->line, path.s) ;

	return config_include_once(f, path.s, &st) ;
}

static int config_dir_list(strbuf *names, char const *dir)
{
	DIR *d = opendir(dir) ;

	if (!d) {

		if (errno == ENOENT || errno == ENOTDIR)
			return 1 ;

		log_warnusys_return(LOG_EXIT_ZERO, "open directory: ", dir) ;
	}

	for (;;) {

		struct dirent *e ;

		errno = 0 ;
		e = readdir(d) ;

		if (!e)
			break ;

		if (get_rstrlen_until(e->d_name, CONFIG_SUFFIX) <= 0)
			continue ;

		if (!sbl_add(names, e->d_name)) {
			dir_close(d) ;
			log_warnusys_return(LOG_EXIT_ZERO, "list directory: ", dir) ;
		}
	}

	if (errno) {
		dir_close(d) ;
		log_warnusys_return(LOG_EXIT_ZERO, "read directory: ", dir) ;
	}

	dir_close(d) ;

	return 1 ;
}

static int config_dir_entry(config_frame const *f, char const *dir, char const *name)
{
	_cleanup_strbuf_ strbuf path = STRBUF_ZERO ;
	struct stat st ;

	if (!auto_strbuf(&path, dir, "/", name))
		log_warnusys_return(LOG_EXIT_ZERO, "build the path of ", name, " in ", dir) ;

	if (stat(path.s, &st) < 0) {
		flog_warnsys("%s:%zu: skip %s", f->path, f->line, path.s) ;
		return 1 ;
	}

	if (!S_ISREG(st.st_mode)) {
		flog_warn("%s:%zu: skip %s: not a regular file", f->path, f->line, path.s) ;
		return 1 ;
	}

	return config_include_once(f, path.s, &st) ;
}

static int config_includedir_close(config_frame *f)
{
	_cleanup_strbuf_ strbuf dir = STRBUF_ZERO ;
	_cleanup_sbl_ strbuf names = SBL_ZERO ;
	size_t pos = 0 ;

	if (!config_resolve(f, "includedir", &dir) || !config_dir_list(&names, dir.s))
		return 0 ;

	if (names.len && !sbl_sort(&names))
		log_warnusys_return(LOG_EXIT_ZERO, "sort the entries of directory: ", dir.s) ;

	FOREACH_SBL(&names, pos)
		if (!config_dir_entry(f, dir.s, names.s + pos))
			return 0 ;

	return 1 ;
}

static int config_open(void *userdata, char const *name, xml_attribute const *attributes, size_t nattributes, size_t line)
{
	config_frame *f = userdata ;

	f->element = CONFIG_ELEMENT_OTHER ;

	if (!strcmp(name, "policy"))
		return config_policy(f, attributes, nattributes, line) ;

	if (!strcmp(name, "allow") || !strcmp(name, "deny"))
		return config_rule(f, name, attributes, nattributes, line) ;

	if (!strcmp(name, "include")) {
		config_include_open(f, attributes, nattributes, line) ;
		return 1 ;
	}

	if (!strcmp(name, "includedir")) {
		f->element = CONFIG_ELEMENT_INCLUDEDIR ;
		f->line = line ;
		f->text.len = 0 ;
	}

	return 1 ;
}

static int config_text(void *userdata, char const *text, size_t len, size_t line)
{
	config_frame *f = userdata ;

	(void)line ;

	if (f->element == CONFIG_ELEMENT_OTHER)
		return 1 ;

	return strbuf_copyb(&f->text, text, len) && strbuf_uncounted(&f->text) ;
}

static int config_close(void *userdata, char const *name, size_t line)
{
	config_frame *f = userdata ;
	config_element_t element = f->element ;

	(void)name ;
	(void)line ;

	f->element = CONFIG_ELEMENT_OTHER ;

	if (element == CONFIG_ELEMENT_INCLUDE)
		return config_include_close(f) ;

	if (element == CONFIG_ELEMENT_INCLUDEDIR)
		return config_includedir_close(f) ;

	return 1 ;
}

static xml_handler const config_handler = {
	.open = config_open,
	.text = config_text,
	.close = config_close
} ;

static int config_parse(config_state *s, char const *path)
{
	char dir[strlen(path) + 1] ;
	config_frame f = {
		.state = s,
		.path = path,
		.dir = dir,
		.context = POLICY_CONTEXT_NONE,
		.id = POLICY_ID_NONE,
		.element = CONFIG_ELEMENT_OTHER,
		.ignore_missing = 0,
		.if_selinux_enabled = 0,
		.line = 0,
		.text = STRBUF_ZERO
	} ;
	busconfig_error err ;
	int r ;

	if (!ob_dirname(dir, path))
		log_warnusys_return(LOG_EXIT_ZERO, "get the directory of: ", path) ;

	r = busconfig_parse_file(path, &config_handler, &f, &err) ;

	strbuf_free(&f.text) ;

	if (!r)
		log_warnu_return(LOG_EXIT_ZERO, "load configuration file: ", path, ": ", err.code == BUSCONFIG_ERROR_XML ? xml_error_str(err.xml.code) : busconfig_error_str(err.code)) ;

	return 1 ;
}

static int config_file(config_state *s, char const *path, struct stat const *st)
{
	config_file_id const id = { .dev = st->st_dev, .ino = st->st_ino } ;
	int r ;

	if (!genbuf_append(config_file_id, &s->files, &id))
		log_warnusys_return(LOG_EXIT_ZERO, "record the configuration file: ", path) ;

	r = config_parse(s, path) ;

	s->files.len -= sizeof(config_file_id) ;

	return r ;
}

int config_load(policy_t *p, char const *path)
{
	log_flow() ;

	config_state s = { .policy = p, .files = GENBUF_ZERO } ;
	struct stat st ;
	int r ;

	if (stat(path, &st) < 0)
		log_warnusys_return(LOG_EXIT_ZERO, "load configuration file: ", path) ;

	if (!S_ISREG(st.st_mode))
		log_warn_return(LOG_EXIT_ZERO, "configuration file ", path, " is not a regular file") ;

	r = config_file(&s, path, &st) ;

	genbuf_free(&s.files) ;

	return r ;
}
