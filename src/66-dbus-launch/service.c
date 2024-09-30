/*
 * service.c
 *
 * Copyright (c) 2024 Eric Vidal <eric@obarun.org>
 *
 * All rights reserved.
 *
 * This file is part of Obarun. It is subject to the license terms in
 * the LICENSE file found in the top-level directory of this
 * distribution.
 * This file may not be copied, modified, propagated, or distributed
 * except according to the terms contained in the LICENSE file./
 */

#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>

#include "launcher.h"
#include "service.h"
#include "dbus.h"
#include "util.h"

#include <oblibs/log.h>
#include <oblibs/string.h>
#include <oblibs/sastr.h>
#include <oblibs/stack.h>
#include <oblibs/environ.h>
#include <oblibs/files.h>

#include <skalibs/types.h>

#include <66/hash.h>
#include <66/utils.h>
#include <66/config.h>
#include <66/constants.h>

#include <66-tools/config.h>

char const *pending_list[] = {
	"DBS_SERVICE_OK",
	"DBS_SERVICE_INSERT",
	"DBS_SERVICE_DELETE",
	0
} ;

void service_hash_free(struct service_s **hservice)
{
	log_flow() ;

	struct service_s *c, *tmp ;
	HASH_ITER(hh, *hservice, c, tmp) {
		HASH_DEL(*hservice, c) ;
		free(c) ;
	}
}

struct service_s *service_search_byname(struct service_s **hservice, const char *name)
{
	log_flow() ;

	struct service_s *h ;
	HASH_FIND_STR(*hservice, name, h) ;
	return h ;
}

struct service_s *service_search_byid(struct service_s **hservice, int id)
{
	log_flow() ;

	struct service_s *c, *tmp ;

	HASH_ITER(hh, *hservice, c, tmp)
		if (c->id == id)
			return c ;

	return NULL ;
}

static void service_mark_delete(struct service_s **hservice)
{
	log_flow() ;

	struct service_s *c, *tmp ;

	HASH_ITER(hh, *hservice, c, tmp)
		c->pending = DBS_SERVICE_DELETE ;
}

void service_add_hash(launcher_t *launcher, struct service_s *service)
{
	log_flow() ;

	struct service_s *h = service_search_byname(launcher->hservice, service->name) ;

	char *name __attribute__((unused)) = service->name ;

	if (h == NULL) {

		service->pending = DBS_SERVICE_INSERT ;
		service->id = launcher->nservice++ ;

		HASH_ADD_STR(*launcher->hservice, name, service) ;

	} else {

		h->pending = DBS_SERVICE_OK ;
	}
}

void service_remove_hash(launcher_t *launcher, const char *name)
{
	log_flow() ;

	struct service_s *h = service_search_byname(launcher->hservice, name) ;

	if (h != NULL) {
		HASH_DEL(*launcher->hservice, h) ;
		free(h) ;
	}
}

int service_environ_filepath(char *store, launcher_t *launcher)
{
	log_flow() ;

	if (launcher->uid > 0) {

		if (!set_ownerhome_stack_byuid(store, launcher->uid))
			log_warnusys_return(LOG_EXIT_ZERO, "set home directory") ;

		auto_string_builder(store, strlen(store), (char const *[]){ SS_ENVIRONMENT_USERDIR, DBS_ENVIRONMENTFILE, NULL }) ;

	} else {

		auto_strings(store, SS_ENVIRONMENT_ADMDIR, DBS_ENVIRONMENTFILE) ;
	}

	return 1 ;
}

int service_collect(launcher_t *launcher)
{
	log_flow() ;

	int r ;
	size_t pos = 0 ;
	_alloc_sa_(sa) ;
	char const *exclude[1] = { 0 } ;
	const char *path = 0 ;

	service_mark_delete(launcher->hservice) ;

	if (launcher->uid > 0) {
		path = SS_TOOLS_DBS_SESSION_SERVICE ;
	} else {
		path = SS_TOOLS_DBS_SYSTEM_SERVICE ;
	}

	if (!sastr_dir_get(&sa, path, exclude, S_IFREG))
        log_warnu_return(DBS_EXIT_WARN, "get services from: ", path) ;

	FOREACH_SASTR(&sa, pos) {

		_alloc_stk_(file, strlen(path) + strlen(sa.s + pos) + 1) ;

		struct service_s *service ;
		service = (struct service_s *)malloc(sizeof(*service)) ;
		if (service == NULL)
			log_warnu_return(DBS_EXIT_FATAL, "malloc") ;

		memset(service, 0, sizeof(*service)) ;

		auto_strings(file.s, path, sa.s + pos) ;

		r = service_parse(service, file.s) ;
		if (!r) {
			log_warnu("parse service: ", file.s) ;
			continue ;
		}

		if (!*service->name || !*service->exec) {
			log_warnu("missing mandatory field at file: ", path) ;
			continue ;
		}

		r = service_write_frontend(launcher, service) ;
		if (!r) {
			log_warnu("write frontend file of service: ", service->name) ;
			continue ;
		}

		service_add_hash(launcher, service) ;
	}

	return 1 ;
}

int service_parse(struct service_s *service, const char *path)
{
	log_flow() ;

	/** We cannot use the environ_parse_file directly
	 * due of the section [D-BUS Service] at start of the file.
	 * Hack it commenting it. */
	ssize_t len = file_get_size(path) ;
	size_t pos = 0 ;
	_alloc_stk_(file, len + 1) ;
	_alloc_stk_(contents, len + 1) ;

	log_info("parsing service: ", path) ;

	if (!stack_read_file(&file, path))
		log_warnu_return(DBS_EXIT_WARN, "read file: ", path) ;

	size_t seclen = str_contain(file.s, DBS_SERVICE_SECTION) ;
	if (seclen < 0)
		log_warnu_return(DBS_EXIT_WARN, "get section " DBS_SERVICE_SECTION) ;

	seclen++ ; // remove '\n' character

	if (!environ_trim(&contents, file.s + seclen))
		log_warnu_return(DBS_EXIT_WARN, "trim environment file: ", path) ;

	FOREACH_STK(&contents, pos) {

		char *line = contents.s + pos ;

		_alloc_stk_(key, strlen(line) + 1) ;
		_alloc_stk_(val, strlen(line) + 1) ;

		if (!environ_get_key(&key, line))
			log_warnu_return(DBS_EXIT_WARN, "get key from line: ", line, " at file: ", path) ;

		if (!environ_get_value(&val, line))
			log_warnu_return(DBS_EXIT_WARN, "get value from line: ", line, " at file: ", path) ;

		if (!strcmp(key.s, "Name")) {
			auto_strings(service->name, val.s) ;
		} else if (!strcmp(key.s, "Exec")) {
			auto_strings(service->exec, val.s) ;
		} else if (!strcmp(key.s, "User")) {
			auto_strings(service->user, val.s) ;
		}
	}
	return 1 ;
}

int service_write_frontend(launcher_t *launcher, struct service_s *service)
{
	log_flow() ;

	_alloc_sa_(sa) ;

	char efile[SS_MAX_PATH_LEN + strlen(SS_ENVIRONMENT_USERDIR) + DBS_ENVIRONMENTFILE_LEN + 1] ;
	const char *suid = 0 ;

	if (launcher->uid) {
		suid = "user" ;
	} else {
		suid = "root" ;
	}

	if (!service_environ_filepath(efile, launcher))
		log_warnu_return(DBS_EXIT_WARN, "get environment file path") ;

	if (!auto_stra(&sa,
		"[Main]\n",
		"Type = classic\n",
		"Description = \"", service->name, " dbus service\"\n",
		"User = ( ", suid, " )\n",
		"Version = 0.0.1\n",
		"InTree = dbus\n",
		"MaxDeath = 5\n"
		"TimeoutStart = 3000\n",
		"TimeoutStop = 3000\n\n",
		"[Start]\n"))

	if (*service->user) {
		if (!auto_stra(&sa, "RunAs = ", service->user,"\n"))
			log_warnu_return(DBS_EXIT_FATAL, "stralloc") ;
	}

	if (!auto_stra(&sa, "Execute = (\n",
		" 	if { printenv }\n"
		"	execl-envfile -l ${ImportFile}\n",
		"	", service->exec, "\n",
		")\n\n",
		"[Environment]\n",
		"ImportFile=", efile, "\n"))
			log_warnu_return(DBS_EXIT_FATAL, "stralloc") ;

	if (launcher->uid > 0) {

		_alloc_stk_(file, SS_MAX_PATH + strlen(SS_SERVICE_USERDIR) + strlen(service->name) + DBS_SERVICE_SUFFIX_LEN + 1) ;
		if (!set_ownerhome_stack_byuid(file.s, launcher->uid))
			log_warnu_return(DBS_EXIT_WARN, "set home directory") ;

		auto_string_builder(file.s, strlen(file.s), (char const *[]){ SS_SERVICE_USERDIR, service->name, DBS_SERVICE_SUFFIX, NULL}) ;

		log_info("writing frontend of service: ", file.s) ;
		if (!file_write_unsafe_g(file.s, sa.s))
			log_warnu_return(DBS_EXIT_WARN, "write file: ", file.s) ;

	} else {

		_alloc_stk_(file, strlen(SS_SERVICE_ADMDIR) + strlen(service->name) + DBS_SERVICE_SUFFIX_LEN + 1) ;
		auto_strings(file.s, SS_SERVICE_ADMDIR, service->name, DBS_SERVICE_SUFFIX) ;

		log_info("writing frontend of service: ", file.s) ;
		if (!file_write_unsafe_g(file.s, sa.s))
			log_warnu_return(DBS_EXIT_WARN, "write file: ", file.s) ;
	}

	return 1 ;
}

void service_sync_dbus_list(launcher_t *launcher)
{
	log_flow() ;

	int r;
	struct service_s *c, *tmp ;

	HASH_ITER(hh, *launcher->hservice, c, tmp) {

		if (c->pending == DBS_SERVICE_OK)
			continue ;

		char fmt[SIZE_FMT] ;
		size_t ilen = size_fmt(fmt, c->id) ;
		fmt[ilen] = 0 ;
		_alloc_stk_(path, strlen("/org/bus1/DBus/Name/") + ilen + 1) ;
		auto_strings(path.s, "/org/bus1/DBus/Name/", fmt) ;

		if (c->pending == DBS_SERVICE_INSERT) {

			r = sd_bus_call_method(launcher->bus_controller, NULL, "/org/bus1/DBus/Broker", "org.bus1.DBus.Broker", "AddName", NULL, NULL, "osu", path.s, c->name, 0) ;
			if (r < 0) {
				errno = -r ;
				log_warnusys("org.bus1.DBus.AddName") ;
				continue ;
			}

			log_info("add service name: ", c->name, " (", path.s, ")") ;

			c->pending = DBS_SERVICE_OK ;

		} else if (c->pending == DBS_SERVICE_DELETE) {

			r = sd_bus_call_method(launcher->bus_controller, NULL, path.s, "org.bus1.DBus.Name", "Release", NULL, NULL, "") ;
			if (r < 0) {
				errno = -r ;
				log_warnusys("org.bus1.DBus.Name.Release") ;
				continue ;
			}

			log_info( "freed service: ", c->name, " (", path.s, ")") ;

			service_discard(launcher, c) ;

		} else {
			/* unreachable */
			log_warn("bug: service ", c->name, " (", path.s, ") in invalid state ", pending_list[c->pending]);
		}
	}

	log_info("service selection synchronized successfully") ;
}

void service_load(launcher_t *launcher)
{
	log_flow() ;

	service_collect(launcher) ;
	service_sync_dbus_list(launcher) ;
}

int service_activate(launcher_t *launcher, int id)
{
	log_flow() ;

	struct service_s *s = service_search_byid(launcher->hservice, id) ;

	_alloc_stk_(name, strlen(s->name) + DBS_SERVICE_SUFFIX_LEN) ;
	auto_strings(name.s, s->name, DBS_SERVICE_SUFFIX) ;

	log_info("activation requested for service: ", name.s) ;

	if (s) {

		char *nargv[] = {
			"66",
			"start",
			name.s,
			0
		} ;

		return sync_spawn(nargv) ;
	}
	log_warnu_return(DBS_EXIT_WARN, "find service: ", name.s, " -- ignoring activation request") ;
}

int service_deactivate(struct service_s *service)
{
	log_flow() ;

	_alloc_stk_(name, strlen(service->name) + DBS_SERVICE_SUFFIX_LEN) ;
	auto_strings(name.s, service->name, DBS_SERVICE_SUFFIX) ;

	log_info("deactivation requested for service: ", name.s) ;

	char *nargv[] = {
		"66",
		"remove",
		name.s,
		0
	} ;

	return sync_spawn(nargv) ;
}

void service_discard(launcher_t *launcher, struct service_s *service)
{
	log_flow() ;

	/** nothing to do with the exit code,
	 * we are on stop process */
	service_deactivate(service) ;
	service_remove_hash(launcher, service->name) ;
}

void service_discard_tree(void)
{
	log_flow() ;

	char *nargv[] = {
		"66",
		"-T3000",
		"tree",
		"free",
		"dbus",
		0
	} ;

	sync_spawn(nargv) ;
}