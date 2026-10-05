/*
 * api.c
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <oblibs/clock.h>
#include <oblibs/directory.h>
#include <oblibs/files.h>
#include <oblibs/log.h>
#include <oblibs/sbl.h>
#include <oblibs/strbuf.h>
#include <oblibs/string.h>

#include <66/config.h>
#include <66/constants.h>
#include <66/enum_parser.h>
#include <66/instance.h>
#include <66/log.h>
#include <66/module.h>
#include <66/parse.h>
#include <66/resolve.h>
#include <66/service.h>
#include <66/status.h>
#include <66/svc.h>
#include <66/tree.h>

#include "action.h"
#include "api.h"
#include "ctx.h"
#include "json.h"
#include "jsonparse.h"

#define API_PREFIX "/api"
#define API_PREFIX_LEN (sizeof(API_PREFIX) - 1)
#define LOG_TAIL_LINES 5

static char const *const TYPE_NAMES[] = { "classic", "oneshot", "module", "event" } ;
#define NTYPES nb_el(TYPE_NAMES)

static int resp_from_json(json_t *j, int status, api_response_t *resp)
{
	if (!json_valid(j)) {
		json_free(j) ;
		return (errno = ENOMEM, 0) ;
	}

	if (!j->sa.heap) {
		json_free(j) ;
		return (errno = EINVAL, 0) ;
	}

	resp->status  = status ;
	resp->body    = j->sa.s ;
	resp->bodylen = j->sa.len ;
	*j = (json_t)JSON_ZERO ;
	return 1 ;
}

int resp_error(int status, char const *message, api_response_t *resp)
{
	json_t j = JSON_ZERO ;
	json_obj_open(&j) ;
	json_key(&j, "error") ;
	json_str(&j, message) ;
	json_obj_close(&j) ;
	return resp_from_json(&j, status, resp) ;
}

static void fmt_emit(json_t *j, int status, api_response_t *resp)
{
	if (!resp_from_json(j, status, resp))
		log_warnusys("build the response of a 66 action") ;
}

static int ctx_field(char *dst, size_t cap, char const *src)
{
	if (!src) src = "" ;
	size_t len = strlen(src) ;
	if (len >= cap)
		return (errno = E2BIG, 0) ;
	memcpy(dst, src, len + 1) ;
	return 1 ;
}

static int act_ctx_set(api_action_ctx_t *c, char const *name, char const *action, char const *path)
{
	return ctx_field(c->name, SS_MAX_SERVICE_NAME, name) &&
		   ctx_field(c->action, API_ACT_VERB_MAX, action) &&
		   ctx_field(c->path, API_ACT_PATH_MAX, path) ;
}

static int result_action(api_result_t *r, int built, api_action_fmt_t *fmt)
{
	if (!built)
		return resp_error(500, "fork or pipe failed",&r->sync) ;

	r->async   = 1 ;
	r->act.fmt = fmt ;

	return 1 ;
}

static int read_master(ctx_t const *s, resolve_tree_master_t *mres)
{
	resolve_wrapper_t_ref wres = resolve_set_struct(DATA_TREE_MASTER, mres) ;
	if (!wres)
		return 0 ;
	int r = resolve_read(wres, s->base, SS_MASTER + 1) ;
	free(wres) ;
	return r > 0 ;
}

static int read_tree(ctx_t const *s, char const *name, resolve_tree_t *tres)
{
	resolve_wrapper_t_ref wres = resolve_set_struct(DATA_TREE, tres) ;
	if (!wres)
		return 0 ;
	int r = resolve_read(wres, s->base, name) ;
	free(wres) ;
	return r > 0 ;
}

static int read_service(ctx_t const *s, char const *name, resolve_service_t *sres)
{
	resolve_wrapper_t_ref wres = resolve_set_struct(DATA_SERVICE, sres) ;
	if (!wres)
		return 0 ;
	int r = resolve_read(wres, s->base, name) ;
	free(wres) ;
	return r > 0 ;
}

static char const *type_name(uint32_t t)
{
	return t < NTYPES ? TYPE_NAMES[t] : "unknown" ;
}

static void emit_service_status(json_t *j, resolve_service_t *res)
{
	service_status_t st = STATUS_ZERO ;
	svc_status(res, &st) ;
	json_key(j, "status") ; json_str(j, status_state_to_string(st.state)) ;
	json_key(j, "pid") ; json_uint(j, st.pid) ;
	json_key(j, "who") ; json_str(j, status_who_to_string(st.who)) ;
	json_key(j, "since") ;

	if (st.stamp.tv_sec > 0) {
		json_uint(j, (uint64_t)st.stamp.tv_sec) ;
	} else {
		json_null(j) ;
	}
}

static void emit_name_array(json_t *j, char const *list)
{
	json_arr_open(j) ;
	if (list && *list) {

		_cleanup_strbuf_ strbuf sb = STRBUF_ZERO ;
		if (sbl_clean_string(&sb, list)) {
			size_t pos = 0 ;
			FOREACH_SBL(&sb, pos)
				json_str(j, sb.s + pos) ;
		}
	}
	json_arr_close(j) ;
}

static void emit_service_deps(json_t *j, resolve_service_t *res)
{
	resolve_service_addon_dependencies_t dep = RESOLVE_SERVICE_ADDON_DEPENDENCIES_ZERO ;
	resolve_wrapper_t_ref w = NULL ;
	char const *depends = "" ;
	char const *requiredby = "" ;
	char const *contents = "" ;
	char const *provide = "" ;
	char const *conflict = "" ;

	if (res->has_dependencies) {

		w = resolve_set_struct(DATA_SERVICE_DEPENDENCIES, &dep) ;
		if (w && resolve_read(w, res->sa.s + res->path.home, res->sa.s + res->name) > 0) {
			depends = dep.sa.s + dep.depends ;
			requiredby = dep.sa.s + dep.requiredby ;
			contents = dep.sa.s + dep.contents ;   /* services a module bundles */
			provide = dep.sa.s + dep.provide ;
			conflict = dep.sa.s + dep.conflict ;
		} else if (w) {
			resolve_free(w) ;
			w = NULL ;
		}
	}

	json_key(j, "depends") ; emit_name_array(j, depends) ;
	json_key(j, "requiredby") ; emit_name_array(j, requiredby) ;
	json_key(j, "contents") ; emit_name_array(j, contents) ;
	json_key(j, "provide") ; emit_name_array(j, provide) ;
	json_key(j, "conflict") ; emit_name_array(j, conflict) ;

	if (w)
		resolve_free(w) ;
}

static void emit_service_links(json_t *j, resolve_service_t const *res)
{
	json_key(j, "inns") ;

	if (res->inns) {
		json_str(j, res->sa.s + res->inns) ;
	} else json_null(j) ;

	json_key(j, "logger") ;

	if (res->logger) {

		char const *name = res->sa.s + res->name ;
		char logname[strlen(name) + SS_LOG_SUFFIX_LEN + 1] ;
		auto_strings(logname, name, SS_LOG_SUFFIX) ;

		json_str(j, logname) ;

	} else json_null(j) ;
}

static void emit_tree(json_t *j, char const *name, resolve_tree_t const *t, char const *current)
{
	json_obj_open(j) ;
	json_key(j, "name") ; json_str(j, name) ;
	json_key(j, "enabled") ; json_bool(j, t->enabled != 0) ;
	json_key(j, "current") ; json_bool(j, current && *current && !strcmp(name, current)) ;
	json_key(j, "init") ; json_bool(j, t->init != 0) ;
	json_key(j, "supervised") ; json_bool(j, t->supervised != 0) ;
	json_key(j, "group") ; json_str(j, t->sa.s + t->groups) ;
	json_key(j, "depends") ; emit_name_array(j, t->sa.s + t->depends) ;
	json_key(j, "requiredby") ; emit_name_array(j, t->sa.s + t->requiredby) ;
	json_key(j, "allow") ; emit_name_array(j, t->sa.s + t->allow) ;
	json_obj_close(j) ;
}

static int handle_trees(api_response_t *resp)
{
	ctx_t const *s = ctx_get() ;
	if (!s)
		return resp_error(500, "66-web state not initialized", resp) ;

	resolve_tree_master_t mres = RESOLVE_TREE_MASTER_ZERO ;
	int have_master = read_master(s, &mres) ;

	json_t j = JSON_ZERO ;
	json_obj_open(&j) ;
	json_key(&j, "base") ; json_str(&j, s->base) ;
	json_key(&j, "trees") ; json_arr_open(&j) ;

	if (have_master && mres.ncontents) {

		char const *current = mres.sa.s + mres.current ;
		_cleanup_strbuf_ strbuf names = STRBUF_ZERO ;

		if (sbl_clean_string(&names, mres.sa.s + mres.contents)) {

			size_t pos = 0 ;
			FOREACH_SBL(&names, pos) {

				char const *tname = names.s + pos ;
				resolve_tree_t tres = RESOLVE_TREE_ZERO ;

				if (read_tree(s, tname, &tres)) {
					emit_tree(&j, tname, &tres, current) ;
					strbuf_free(&tres.sa) ;
				}
			}
		}
	}

	json_arr_close(&j) ;
	json_obj_close(&j) ;

	if (have_master)
		strbuf_free(&mres.sa) ;

	return resp_from_json(&j, 200, resp) ;
}

static int handle_services(api_response_t *resp)
{
	ctx_t const *s = ctx_get() ;
	if (!s)
		return resp_error(500, "66-web state not initialized", resp) ;

	resolve_tree_master_t mres = RESOLVE_TREE_MASTER_ZERO ;
	_cleanup_strbuf_ strbuf trees = STRBUF_ZERO ;

	int have_master = read_master(s, &mres) ;
	if (have_master && mres.ncontents) {

		if (!sbl_clean_string(&trees, mres.sa.s + mres.contents)) {
			strbuf_free(&mres.sa) ;
			return resp_error(500, "parse master tree contents", resp) ;
		}
	}

	json_t j = JSON_ZERO ;
	json_obj_open(&j) ;
	json_key(&j, "base") ; json_str(&j, s->base) ;
	json_key(&j, "services") ; json_arr_open(&j) ;

	size_t tpos = 0 ;
	FOREACH_SBL(&trees, tpos) {

		char const *tname = trees.s + tpos ;
		resolve_tree_t tres = RESOLVE_TREE_ZERO ;

		if (read_tree(s, tname, &tres)) {

			if (tres.ncontents) {

				_cleanup_strbuf_ strbuf services = STRBUF_ZERO ;
				if (sbl_clean_string(&services, tres.sa.s + tres.contents)) {

					size_t spos = 0 ;
					FOREACH_SBL(&services, spos) {

						char const *sname = services.s + spos ;

						json_obj_open(&j) ;
						json_key(&j, "name") ; json_str(&j, sname) ;
						json_key(&j, "tree") ; json_str(&j, tname) ;

						resolve_service_t sres = RESOLVE_SERVICE_ZERO ;
						if (read_service(s, sname, &sres)) {
							json_key(&j, "type") ; json_uint(&j, sres.type) ;
							json_key(&j, "typestr") ; json_str(&j, type_name(sres.type)) ;
							emit_service_links(&j, &sres) ;
							emit_service_status(&j, &sres) ;
							emit_service_deps(&j, &sres) ;
							strbuf_free(&sres.sa) ;
						} else {
							json_key(&j, "type") ; json_null(&j) ;
							json_key(&j, "typestr") ; json_null(&j) ;
							json_key(&j, "inns") ; json_null(&j) ;
							json_key(&j, "logger") ; json_null(&j) ;
							json_key(&j, "status") ; json_null(&j) ;
							json_key(&j, "pid") ; json_uint(&j, 0) ;
							json_key(&j, "since") ; json_null(&j) ;
							json_key(&j, "depends") ; json_arr_open(&j) ; json_arr_close(&j) ;
							json_key(&j, "requiredby") ; json_arr_open(&j) ; json_arr_close(&j) ;
							json_key(&j, "contents") ; json_arr_open(&j) ; json_arr_close(&j) ;
							json_key(&j, "provide") ; json_arr_open(&j) ; json_arr_close(&j) ;
							json_key(&j, "conflict") ; json_arr_open(&j) ; json_arr_close(&j) ;
						}

						json_obj_close(&j) ;
					}
				}
			}
			strbuf_free(&tres.sa) ;
		}
	}

	json_arr_close(&j) ;
	json_obj_close(&j) ;

	strbuf_free(&mres.sa) ;

	return resp_from_json(&j, 200, resp) ;
}

static int handle_service_detail(char const *name, api_response_t *resp)
{
	ctx_t const *s = ctx_get() ;
	if (!s)
		return resp_error(500, "66-web state not initialized", resp) ;

	if (!name || !*name)
		return resp_error(404, "service name required", resp) ;

	// Reject any further path component; e.g. /api/services/foo/bar.
	if (strchr(name, '/'))
		return resp_error(404, "service not found", resp) ;

	resolve_service_t sres = RESOLVE_SERVICE_ZERO ;
	if (!read_service(s, name, &sres)) {
		strbuf_free(&sres.sa) ;
		return resp_error(404, "service not found", resp) ;
	}

	json_t j = JSON_ZERO ;
	json_obj_open(&j) ;
	json_key(&j, "name") ; json_str(&j, sres.sa.s + sres.name) ;
	json_key(&j, "description") ; json_str(&j, sres.sa.s + sres.description) ;
	json_key(&j, "version") ; json_str(&j, sres.sa.s + sres.version) ;
	json_key(&j, "type") ; json_uint(&j, sres.type) ;
	json_key(&j, "typestr") ; json_str(&j, type_name(sres.type)) ;
	json_key(&j, "source") ; json_str(&j, sres.sa.s + sres.path.frontend) ;
	json_key(&j, "intree") ; json_str(&j, sres.sa.s + sres.intree) ;
	json_key(&j, "treename") ; json_str(&j, sres.sa.s + sres.treename) ;
	json_key(&j, "enabled") ; json_bool(&j, sres.enabled != 0) ;
	json_key(&j, "islog") ; json_bool(&j, sres.islog   != 0) ;
	json_key(&j, "earlier") ; json_bool(&j, sres.earlier != 0) ;
	json_key(&j, "config") ; json_bool(&j, sres.has_environ != 0) ;

	emit_service_status(&j, &sres) ;
	emit_service_deps(&j, &sres) ;
	json_obj_close(&j) ;

	strbuf_free(&sres.sa) ;
	return resp_from_json(&j, 200, resp) ;
}

static int log_tail_append(strbuf *sb, log_source_t const *src, log_line_t const *pl)
{
	char const *line = src->data.s + pl->off ;

	if (pl->type == LOG_STAMP_TAI64N) {

		char local[CLOCK_LOCAL_LEN + 1] ;
		size_t ll = clock_local_fmt(local, &pl->stamp) ;

		if (!strbuf_catb(sb, local, ll) ||
			!strbuf_catb(sb, " ", 1) ||
			!strbuf_catb(sb, line + pl->msgoff, pl->len - pl->msgoff))
				return 0 ;
	} else {

		if (!strbuf_catb(sb, line, pl->len))
			return 0 ;
	}

	return strbuf_catb(sb, "\n", 1) ;
}

static int handle_service_log(char const *name, api_response_t *resp)
{
	ctx_t const *s = ctx_get() ;
	if (!s)
		return resp_error(500, "66-web state not initialized", resp) ;

	if (!action_name_valid(name))
		return resp_error(400, "invalid service name", resp) ;

	resolve_service_t sres = RESOLVE_SERVICE_ZERO ;
	if (!read_service(s, name, &sres)) {
		strbuf_free(&sres.sa) ;
		return resp_error(404, "service not found", resp) ;
	}

	_cleanup_strbuf_ strbuf log = STRBUF_ZERO ;
	log_source_t src = LOG_SOURCE_ZERO ;
	int failed = 0 ;

	if (sres.has_io) {

		resolve_service_addon_io_t io = RESOLVE_SERVICE_ADDON_IO_ZERO ;
		resolve_wrapper_t_ref wio = resolve_set_struct(DATA_SERVICE_IO, &io) ;

		if (wio && resolve_read(wio, sres.sa.s + sres.path.home, sres.sa.s + sres.name) > 0 && io.fdout.destination) {

			char const *dest = io.sa.s + io.fdout.destination ;
			int loaded = 0 ;

			// The logdir/file only exists once the service has run at least once
			if (io.fdout.type == E_PARSER_IO_TYPE_66LOG && scan_mode(dest, S_IFDIR) == 1) {

				loaded = log_source_logdir(&src, name, dest) ;

			} else if (io.fdout.type == E_PARSER_IO_TYPE_FILE && scan_mode(dest, S_IFREG) == 1) {

				loaded = log_source_file(&src, name, dest) ;
			}

			if (loaded) {

				size_t first = src.nline > LOG_TAIL_LINES ? src.nline - LOG_TAIL_LINES : 0 ;
				for (size_t i = first ; i < src.nline && !failed ; i++)
					failed = !log_tail_append(&log, &src, &src.line[i]) ;
			}
		}

		if (wio)
			free(wio) ;

		strbuf_free(&io.sa) ;
	}

	log_source_free(&src) ;
	strbuf_free(&sres.sa) ;

	if (failed)
		return resp_error(500, "assemble log tail", resp) ;

	json_t j = JSON_ZERO ;
	json_obj_open(&j) ;
	json_key(&j, "name") ; json_str(&j, name) ;
	json_key(&j, "log") ; json_strn(&j, log.len ? log.s : "", log.len) ;
	json_obj_close(&j) ;

	return resp_from_json(&j, 200, resp) ;
}

static int handle_service_frontend(char const *name, api_response_t *resp)
{
	_cleanup_strbuf_ strbuf sb = STRBUF_ZERO ;
	resolve_service_t sres = RESOLVE_SERVICE_ZERO ;
	ctx_t const *s = ctx_get() ;

	if (!s)
		return resp_error(500, "66-web state not initialized", resp) ;

	if (!name || !*name || strchr(name, '/'))
		return resp_error(404, "service not found", resp) ;

	if (!read_service(s, name, &sres)) {
		strbuf_free(&sres.sa) ;
		return resp_error(404, "service not found", resp) ;
	}

	// Copy the frontend path out of the resolve buffer, then free it.
	size_t plen = strlen(sres.sa.s + sres.path.frontend) ;
	char path[plen + 2] ;

	memcpy(path, sres.sa.s + sres.path.frontend, plen + 1) ;
	strbuf_free(&sres.sa) ;

	// An unreadable frontend is still answered, with a note in place of its content
	json_t j = JSON_ZERO ;
	json_obj_open(&j) ;
	json_key(&j, "name") ; json_str(&j, name) ;
	json_key(&j, "path") ; json_str(&j, path) ;
	json_key(&j, "content") ;

	if (strbuf_read_file(&sb, path)) {

		json_strn(&j, sb.s, sb.len) ;

	} else {

		json_str(&j, "(unable to read frontend)") ;
	}

	json_obj_close(&j) ;

	return resp_from_json(&j, 200, resp) ;
}

static void fmt_service_config(api_action_ctx_t const *c, int exit_code, char const *output, size_t outlen, api_response_t *resp)
{
	(void)exit_code ;

	json_t j = JSON_ZERO ;
	json_obj_open(&j) ;
	json_key(&j, "name") ; json_str(&j, c->name) ;
	json_key(&j, "config") ; json_strn(&j, output, outlen) ;
	json_obj_close(&j) ;

	fmt_emit(&j, 200, resp) ;
}

static int handle_service_config(char const *name, api_result_t *r)
{
	if (!action_name_valid(name))
		return resp_error(400, "invalid service name", &r->sync) ;

	int built = action_argv_config(&r->act.argv, name) ;

	if (!built && errno == EINVAL)
		return resp_error(400, "invalid service name", &r->sync) ;

	return result_action(r, built && act_ctx_set(&r->act.ctx, name, NULL, NULL), &fmt_service_config) ;
}

static void fmt_action(api_action_ctx_t const *c, int exit_code, char const *output, size_t outlen, api_response_t *resp)
{

	json_t j = JSON_ZERO ;
	json_obj_open(&j) ;
	json_key(&j, "name") ; json_str(&j, c->name) ;
	json_key(&j, "action") ; json_str(&j, c->action) ;
	json_key(&j, "exit") ; json_int(&j, exit_code) ;
	json_key(&j, "output") ; json_strn(&j, output, outlen) ;
	json_obj_close(&j) ;

	fmt_emit(&j, exit_code == 0 ? 200 : 503, resp) ;
}

static int handle_action(char const *name, char const *action, api_result_t *r)
{
	if (!action_name_valid(name))
		return resp_error(400, "invalid service name", &r->sync) ;

	int built = action_argv_run(&r->act.argv, action, name) ;

	if (!built && errno == EINVAL)
		return resp_error(400, "unknown action or invalid service name", &r->sync) ;

	return result_action(r, built && act_ctx_set(&r->act.ctx, name, action, NULL), &fmt_action) ;
}

static void fmt_tree_action(api_action_ctx_t const *c, int exit_code, char const *output, size_t outlen, api_response_t *resp)
{

	json_t j = JSON_ZERO ;
	json_obj_open(&j) ;
	json_key(&j, "tree") ; json_str(&j, c->name) ;
	json_key(&j, "action") ; json_str(&j, c->action) ;
	json_key(&j, "exit") ; json_int(&j, exit_code) ;
	json_key(&j, "output") ; json_strn(&j, output, outlen) ;
	json_obj_close(&j) ;

	fmt_emit(&j, exit_code == 0 ? 200 : 503, resp) ;
}

static int handle_tree_action(char const *name, char const *action, api_result_t *r)
{
	if (!action_name_valid(name))
		return resp_error(400, "invalid tree name", &r->sync) ;

	int built = action_argv_run_tree(&r->act.argv, action, name) ;

	if (!built && errno == EINVAL)
		return resp_error(400, "unknown tree action or invalid tree name", &r->sync) ;

	return result_action(r, built && act_ctx_set(&r->act.ctx, name, action, NULL), &fmt_tree_action) ;
}

static void fmt_create(api_action_ctx_t const *c, int exit_code, char const *output, size_t outlen, api_response_t *resp)
{

	json_t j = JSON_ZERO ;
	json_obj_open(&j) ;
	json_key(&j, "name") ; json_str(&j, c->name) ;
	json_key(&j, "path") ; json_str(&j, c->path) ;
	json_key(&j, "action") ; json_str(&j, c->action) ;
	json_key(&j, "exit") ; json_int(&j, exit_code) ;
	json_key(&j, "output") ; json_strn(&j, output, outlen) ;
	json_obj_close(&j) ;

	/* 201 written (+ any action ok); 503 if the 66 action itself failed. */
	fmt_emit(&j, exit_code ? 503 : 201, resp) ;
}

static int created_only(char const *name, char const *path, char const *action, api_response_t *resp)
{
	json_t j = JSON_ZERO ;
	json_obj_open(&j) ;
	json_key(&j, "name") ; json_str(&j, name) ;
	json_key(&j, "path") ; json_str(&j, path) ;
	json_key(&j, "action") ; json_str(&j, action) ;
	json_key(&j, "exit") ; json_int(&j, 0) ;
	json_key(&j, "output") ; json_strn(&j, "", 0) ;
	json_obj_close(&j) ;

	return resp_from_json(&j, 201, resp) ;
}

static int handle_create_service(char const *body, size_t len, api_result_t *r)
{
	api_response_t *resp = &r->sync ;

	if (!body || !len)
		return resp_error(400, "missing request body", resp) ;

	jp_value *doc = jp_parse(body, len) ;
	if (!doc)
		return resp_error(400, "invalid JSON body", resp) ;

	char const *name = jp_string(jp_get(doc, "name")) ;
	char const *content = jp_string(jp_get(doc, "content")) ;
	char const *action = jp_string(jp_get(doc, "action")) ;
	char const *tree = jp_string(jp_get(doc, "tree")) ;
	int overwrite = jp_is_true(jp_get(doc, "overwrite")) ;

	// '@' (instance/template) is allowed for a service name.
	if (!action_name_valid(name)) {
		jp_free(doc) ;
		return resp_error(400, "invalid service name (no spaces, '/', '..' or control chars)", resp) ;
	}

	if (!content || !*content) {
		jp_free(doc) ;
		return resp_error(400, "missing frontend content", resp) ;
	}

	if (!action)
		action = "none" ;

	ctx_t const *ctx = ctx_get() ;
	if (!ctx) {
		jp_free(doc) ;
		return resp_error(500, "context unavailable", resp) ;
	}

	char dir[SS_MAX_PATH_LEN + 1] ;
	if (!service_src_dir(ctx->owner, dir)) {
		jp_free(doc) ;
		return resp_error(500, "resolve service source directory", resp) ;
	}

	char path[strlen(dir) + strlen(name) + 1] ;
	auto_strings(path, dir, name) ;

	// Never overwrite silently; the client must opt in with "overwrite".
	if (!overwrite && access(path, F_OK) == 0) {
		jp_free(doc) ;
		return resp_error(409, "a service with this name already exists", resp) ;
	}

	// The user's source dir ($HOME/.66/service) may not exist yet.
	if (!dir_create_parent(dir, 0755)) {
		jp_free(doc) ;
		return resp_error(500, "create service source directory", resp) ;
	}

	if (!file_write_atomic(path, content, strlen(content))) {
		jp_free(doc) ;
		return resp_error(500, "write frontend file", resp) ;
	}

	/* Run the requested action. `enable` parses the frontend first, so a
	 * parse error surfaces with a non-zero exit; the file is kept. */
	int built = 0 ;

	if (!strcmp(action, "enable") || !strcmp(action, "enablestart")) {

		char const *t = (tree && *tree) ? tree : NULL ;
		built = action_argv_enable(&r->act.argv, name, t, !strcmp(action, "enablestart")) ;

	} else if (!strcmp(action, "start")) {

		built = action_argv_run(&r->act.argv, "start", name) ;

	} else {

		// "none" (or anything else) -- written only
		int ret = created_only(name, path, action, resp) ;
		jp_free(doc) ;
		return ret ;
	}

	if (!built && errno == EINVAL) {
		jp_free(doc) ;
		return resp_error(400, "invalid action or name", resp) ;
	}

	built = built && act_ctx_set(&r->act.ctx, name, action, path) ;
	jp_free(doc) ;

	return result_action(r, built, &fmt_create) ;
}

static int mod_write(char const *path, char const *content)
{
	return file_write_atomic(path, content ? content : "", content ? strlen(content) : 0) ;
}

static int handle_create_module(char const *body, size_t len, api_result_t *r)
{
	api_response_t *resp = &r->sync ;

	if (!body || !len)
		return resp_error(400, "missing request body", resp) ;

	jp_value *doc = jp_parse(body, len) ;
	if (!doc)
		return resp_error(400, "invalid JSON body", resp) ;

	char const *name = jp_string(jp_get(doc, "name")) ;
	char const *content = jp_string(jp_get(doc, "content")) ;
	char const *configure = jp_string(jp_get(doc, "configure")) ;
	char const *action = jp_string(jp_get(doc, "action")) ;
	char const *tree = jp_string(jp_get(doc, "tree")) ;
	char const *instance = jp_string(jp_get(doc, "instance")) ;
	int overwrite = jp_is_true(jp_get(doc, "overwrite")) ;
	jp_value const *members   = jp_get(doc, "members") ;
	jp_value const *activated = jp_get(doc, "activated") ;

	/* The module directory is named plainly (no '@'); the frontend inside
	 * it carries the '@'. */
	if (!action_name_valid(name) || instance_check(name) >= 0) {
		jp_free(doc) ;
		return resp_error(400, "invalid module name (no spaces, '@', '/', '..' or control chars)", resp) ;
	}

	if (!content || !*content) {
		jp_free(doc) ;
		return resp_error(400, "missing module frontend content", resp) ;
	}

	if (!action)
		action = "none" ;

	// Validate every contained-service local name up front.
	size_t nmem = jp_arr_len(members) ;
	for (size_t i = 0 ; i < nmem ; i++) {
		char const *local = jp_string(jp_get(jp_arr_get(members, i), "local")) ;
		if (!action_name_valid(local)) {
			jp_free(doc) ;
			return resp_error(400, "invalid contained-service name (no spaces, '/', '..' or control chars)", resp) ;
		}
	}

	ctx_t const *ctx = ctx_get() ;
	if (!ctx) {
		jp_free(doc) ;
		return resp_error(500, "context unavailable", resp) ;
	}

	char dir[SS_MAX_PATH_LEN + 1] ;
	if (!service_src_dir(ctx->owner, dir)) {
		jp_free(doc) ;
		return resp_error(500, "resolve service source directory", resp) ;
	}

	char moddir[strlen(dir) + strlen(name) + 1] ;
	auto_strings(moddir, dir, name) ;

	if (access(moddir, F_OK) == 0) {

		if (!overwrite) {
			jp_free(doc) ;
			return resp_error(409, "a module with this name already exists", resp) ;
		}

		if (!dir_destroy(moddir)) {
			jp_free(doc) ;
			return resp_error(500, "remove existing module directory", resp) ;
		}
	}

	// Build the whole directory; roll it back on any failure.
	int ok = dir_create_parent(moddir, 0755) ;
	/* The longest entry is activated/requiredby/<name>, and every name written
	 * below passed action_name_valid: shorter than 256 bytes. */
	char path[strlen(moddir) + sizeof("/activated/requiredby/") + SS_MAX_SERVICE_NAME] ;

	if (ok) {
		auto_strings(path, moddir, "/", name, "@") ;
		ok = mod_write(path, content) ;
	}

	if (ok) {
		auto_strings(path, moddir, "/frontend") ;
		ok = dir_create_parent(path, 0755) ;
	}

	for (size_t i = 0 ; ok && i < nmem ; i++) {
		jp_value const *m = jp_arr_get(members, i) ;
		auto_strings(path, moddir, "/frontend/", jp_string(jp_get(m, "local"))) ;
		ok = mod_write(path, jp_string(jp_get(m, "content"))) ;
	}

	if (ok && configure && *configure) {

		auto_strings(path, moddir, "/configure") ;

		ok = dir_create_parent(path, 0755) ;
		if (ok) {

			auto_strings(path, moddir, "/configure/configure") ;

			ok = mod_write(path, configure) ;
			if (ok)
				chmod(path, 0755) ;
		}
	}

	jp_value const *depends = jp_get(doc, "depends") ;
	jp_value const *requiredby = jp_get(doc, "requiredby") ;
	size_t nact = jp_arr_len(activated), ndep = jp_arr_len(depends), nreq = jp_arr_len(requiredby) ;

	if (ok && (nact || ndep || nreq)) {
		auto_strings(path, moddir, "/activated") ;
		ok = dir_create_parent(path, 0755) ;
	}

	// always-on members
	for (size_t i = 0 ; ok && i < nact ; i++) {

		char const *a = jp_string(jp_arr_get(activated, i)) ;
		if (!action_name_valid(a))
			continue ;

		auto_strings(path, moddir, "/activated/", a) ;

		ok = mod_write(path, "") ;
	}

	// outside dependencies -- activated/depends/<svc>
	if (ok && ndep) {
		auto_strings(path, moddir, "/activated/depends") ;
		ok = dir_create_parent(path, 0755) ;
	}

	for (size_t i = 0 ; ok && i < ndep ; i++) {

		char const *d = jp_string(jp_arr_get(depends, i)) ;
		if (!action_name_valid(d))
			continue ;

		auto_strings(path, moddir, "/activated/depends/", d) ;

		ok = mod_write(path, "") ;
	}

	// outside services requiring this module -- activated/requiredby/<svc>
	if (ok && nreq) {
		auto_strings(path, moddir, "/activated/requiredby") ;
		ok = dir_create_parent(path, 0755) ;
	}

	for (size_t i = 0 ; ok && i < nreq ; i++) {
		char const *rq = jp_string(jp_arr_get(requiredby, i)) ;
		if (!action_name_valid(rq))
			continue ;
		auto_strings(path, moddir, "/activated/requiredby/", rq) ;
		ok = mod_write(path, "") ;
	}

	if (!ok) {
		dir_destroy(moddir) ;
		jp_free(doc) ;
		return resp_error(500, "write module directory", resp) ;
	}

	// Enable/start the requested instance (name@instance).
	int built = 0 ;
	int ran = 0 ;

	if (strcmp(action, "none")) {

		if (!instance || !*instance) {
			jp_free(doc) ;
			return resp_error(400, "an instance name is required to enable or start a module", resp) ;
		}

		if (!action_name_valid(instance) || instance_check(instance) >= 0) {
			jp_free(doc) ;
			return resp_error(400, "invalid instance name (no spaces, '@', '/' or control chars)", resp) ;
		}

		char full[strlen(name) + 1 + strlen(instance) + 1] ;
		auto_strings(full, name, "@", instance) ;

		if (!strcmp(action, "enable") || !strcmp(action, "enablestart")) {
			ran = 1 ;
			char const *t = (tree && *tree) ? tree : NULL ;
			built = action_argv_enable(&r->act.argv, full, t, !strcmp(action, "enablestart")) ;
		} else if (!strcmp(action, "start")) {
			ran = 1 ;
			built = action_argv_run(&r->act.argv, "start", full) ;
		}
	}

	if (!ran) {
		int ret = created_only(name, moddir, action, resp) ;
		jp_free(doc) ;
		return ret ;
	}

	if (!built && errno == EINVAL) {
		jp_free(doc) ;
		return resp_error(400, "invalid action or instance", resp) ;
	}

	built = built && act_ctx_set(&r->act.ctx, name, action, moddir) ;
	jp_free(doc) ;

	return result_action(r, built, &fmt_create) ;
}

static void frontend_main_keys(char const *path, strbuf *type, strbuf *desc)
{
	_cleanup_strbuf_ strbuf text = STRBUF_ZERO ;
	parse_store_t st ;
	char const *v ;
	size_t len ;

	if (!strbuf_read_file(&text, path))
		return ;

	if (parse_store_build(&st, text.s)) {

		v = parse_store_get(&st, E_PARSER_SECTION_MAIN, E_PARSER_SECTION_MAIN_TYPE, &len) ;
		if (v && !strbuf_catb(type, v, len))
			log_warnusys("store the type of frontend: ", path) ;

		v = parse_store_get(&st, E_PARSER_SECTION_MAIN, E_PARSER_SECTION_MAIN_DESCRIPTION, &len) ;
		if (v && !strbuf_catb(desc, v, len))
			log_warnusys("store the description of frontend: ", path) ;
	}

	parse_store_free(&st) ;
}


static void available_emit(json_t *j, char const *dir, char const *name, int isdir, int removable)
{
	size_t nlen = strlen(name) ;

	char path[strlen(dir) + 2 * nlen + sizeof("/@")] ;

	if (isdir) {
		auto_strings(path, dir, name, "/", name, "@") ;
	} else auto_strings(path, dir, name) ;

	_cleanup_strbuf_ strbuf type = STRBUF_ZERO ;
	_cleanup_strbuf_ strbuf desc = STRBUF_ZERO ;
	frontend_main_keys(path, &type, &desc) ;

	json_obj_open(j) ;
	json_key(j, "name") ; json_str(j, name) ;
	json_key(j, "kind") ; json_str(j, isdir ? "module" : "service") ;
	json_key(j, "type") ;
	if (isdir) json_str(j, "module") ;
	else json_strn(j, type.len ? type.s : "", type.len) ;
	json_key(j, "description") ; json_strn(j, desc.len ? desc.s : "", desc.len) ;
	json_key(j, "instanced") ; json_bool(j, isdir || (nlen && name[nlen - 1] == '@')) ;
	json_key(j, "source") ; json_str(j, dir) ;
	json_key(j, "removable") ; json_bool(j, removable) ;
	json_obj_close(j) ;
}

static int available_add(json_t *j, strbuf *parsed, strbuf *seen, char const *dir, strbuf *names, int isdir, int removable)
{
	size_t pos = 0 ;

	FOREACH_SBL(names, pos) {

		char const *name = names->s + pos ;

		if (sbl_search(parsed, name) >= 0 || sbl_search(seen, name) >= 0)
			continue ;

		if (!sbl_add(seen, name))
			return 0 ;

		available_emit(j, dir, name, isdir, removable) ;
	}
	return 1 ;
}

static int handle_available(api_response_t *resp)
{
	char const *dirs[SERVICE_SRC_DIRS_MAX] ;
	int editable[SERVICE_SRC_DIRS_MAX] ;
	size_t ndirs = 0 ;
	char home[SS_MAX_PATH_LEN + 1] ;
	char const *exclude_root[] = { "user", 0 } ;
	ctx_t const *ctx = ctx_get() ;
	if (!ctx)
		return resp_error(500, "context unavailable", resp) ;

	// already-parsed names live as symlinks in <base>system/.resolve/service
	char const *noexc[] = { 0 } ;

	char rdir[strlen(ctx->base) + SS_SYSTEM_LEN + SS_RESOLVE_LEN + SS_SERVICE_LEN + 1] ;

	auto_strings(rdir, ctx->base, SS_SYSTEM, SS_RESOLVE, SS_SERVICE) ;

	_cleanup_strbuf_ strbuf parsed = STRBUF_ZERO ;
	if (!sbl_dir_get_recursive(&parsed, rdir, noexc, S_IFLNK, 0) && errno != ENOENT) // ENOENT: a fresh base
		return resp_error(500, "list the parsed services", resp) ;

	if (!service_src_dirs(ctx->owner, dirs, editable, &ndirs, home))
		return resp_error(500, "resolve source directories", resp) ;

	char const **exclude = ctx->owner ? noexc : exclude_root ;

	json_t j = JSON_ZERO ;
	json_obj_open(&j) ;
	json_key(&j, "base") ; json_str(&j, ctx->base) ;
	json_key(&j, "available") ; json_arr_open(&j) ;

	_cleanup_strbuf_ strbuf seen = STRBUF_ZERO ;

	for (size_t di = 0 ; di < ndirs ; di++) {

		if (access(dirs[di], F_OK))
			continue ;

		// removable only if the dir is editable (not distro) AND we can unlink in it
		int removable = editable[di] && access(dirs[di], W_OK) == 0 ;

		/* One entry that cannot be examined (a dangling link) fails the whole
		 * directory: say so once, and list the others. */
		_cleanup_strbuf_ strbuf files = STRBUF_ZERO ;
		_cleanup_strbuf_ strbuf modules = STRBUF_ZERO ;
		if (!sbl_dir_get(&files, dirs[di], exclude, S_IFREG) ||
			!sbl_dir_get(&modules, dirs[di], exclude, S_IFDIR)) {
				log_warnusys("list the frontends in ", dirs[di]) ;
				continue ;
		}

		if (!available_add(&j, &parsed, &seen, dirs[di], &files, 0, removable) ||
			!available_add(&j, &parsed, &seen, dirs[di], &modules, 1, removable)) {
				json_free(&j) ;
				return resp_error(500, "list the available frontends", resp) ;
		}
	}

	json_arr_close(&j) ;
	json_obj_close(&j) ;

	return resp_from_json(&j, 200, resp) ;
}

static void fmt_enable(api_action_ctx_t const *c, int exit_code, char const *output, size_t outlen, api_response_t *resp)
{

	json_t j = JSON_ZERO ;
	json_obj_open(&j) ;
	json_key(&j, "name") ; json_str(&j, c->name) ;
	json_key(&j, "exit") ; json_int(&j, exit_code) ;
	json_key(&j, "output") ; json_strn(&j, output, outlen) ;
	json_obj_close(&j) ;

	fmt_emit(&j, exit_code == 0 ? 200 : 503, resp) ;
}

static int handle_enable(char const *body, size_t len, api_result_t *r)
{
	api_response_t *resp = &r->sync ;

	if (!body || !len)
		return resp_error(400, "missing request body", resp) ;

	jp_value *doc = jp_parse(body, len) ;
	if (!doc)
		return resp_error(400, "invalid JSON body", resp) ;

	char const *name = jp_string(jp_get(doc, "name")) ;
	char const *tree = jp_string(jp_get(doc, "tree")) ;
	char const *action = jp_string(jp_get(doc, "action")) ;

	if (!action)
		action = "enable" ;

	if (!action_name_valid(name)) {
		jp_free(doc) ;
		return resp_error(400, "invalid service name (no spaces, '/', '..' or control chars)", resp) ;
	}

	char const *t = (tree && *tree) ? tree : NULL ;

	int built = !strcmp(action, "start")
				? action_argv_run(&r->act.argv, "start", name)
				: action_argv_enable(&r->act.argv, name, t, !strcmp(action, "enablestart")) ;

	if (!built && errno == EINVAL) {
		jp_free(doc) ;
		return resp_error(400, "invalid service or tree name", resp) ;
	}

	built = built && act_ctx_set(&r->act.ctx, name, NULL, NULL) ;
	jp_free(doc) ;

	return result_action(r, built, &fmt_enable) ;
}

static int frontend_locate(strbuf *path, char const *dir, char const *name, int insta, char const **exclude)
{
	_cleanup_strbuf_ strbuf found = STRBUF_ZERO ;
	struct stat st ;
	char direct[strlen(dir) + strlen(name) + 1] ;
	auto_strings(direct, dir, name) ;

	if (!insta)
		return stat(direct, &st) < 0 ? 0 : auto_strbuf(path, direct) ? 1 : -1 ;

	if (insta < 0 && !stat(direct, &st) && S_ISDIR(st.st_mode))
		return auto_strbuf(path, direct, "/", name, "@") ? 1 : -1 ;

	int r = service_frontend_src(&found, name, dir, exclude) ;
	if (r <= 0)
		return r ;

	/* For an instance, 66 answers <dir>/<name@instance>, which does not
	 * exist: the content is the template's, <dir>/<name@>. */
	size_t len = strlen(found.s) ;
	if (insta > 0)
		len -= strlen(name) - insta - 1 ;

	return strbuf_catb(path, found.s, len) && strbuf_uncounted(path) ? 1 : -1 ;
}

static int handle_frontend_read(char const *name, api_response_t *resp)
{
	if (!action_name_valid(name))
		return resp_error(400, "invalid name", resp) ;

	ctx_t const *ctx = ctx_get() ;
	if (!ctx)
		return resp_error(500, "context unavailable", resp) ;

	size_t ndirs = 0 ;
	char const *dirs[SERVICE_SRC_DIRS_MAX] ;
	int editable[SERVICE_SRC_DIRS_MAX] ;
	char home[SS_MAX_PATH_LEN + 1] ;

	if (!service_src_dirs(ctx->owner, dirs, editable, &ndirs, home))
		return resp_error(500, "resolve source directories", resp) ;

	int insta = instance_check(name) ;
	char const *exclude[] = { SS_MODULE_ACTIVATED + 1, SS_MODULE_FRONTEND + 1,
	                          SS_MODULE_CONFIG_DIR + 1, ctx->owner ? 0 : "user", 0 } ;

	for (size_t i = 0 ; i < ndirs ; i++) {

		_cleanup_strbuf_ strbuf path = STRBUF_ZERO ;

		int r = frontend_locate(&path, dirs[i], name, insta, exclude) ;
		if (r < 0) {
			log_warnusys("look for the frontend ", name, " in ", dirs[i]) ;
			continue ;
		}

		if (!r)
			continue ;

		_cleanup_strbuf_ strbuf sb = STRBUF_ZERO ;

		if (!strbuf_read_file(&sb, path.s))
			return resp_error(500, "read frontend", resp) ;

		json_t j = JSON_ZERO ;
		json_obj_open(&j) ;
		json_key(&j, "name") ;    json_str(&j, name) ;
		json_key(&j, "source") ;  json_str(&j, dirs[i]) ;
		json_key(&j, "content") ; json_strn(&j, sb.s, sb.len) ;
		json_obj_close(&j) ;

		return resp_from_json(&j, 200, resp) ;
	}

	return resp_error(404, "frontend not found", resp) ;
}

static int handle_remove(char const *name, api_response_t *resp)
{
	if (!action_name_valid(name))
		return resp_error(400, "invalid name", resp) ;

	ctx_t const *ctx = ctx_get() ;
	if (!ctx) return resp_error(500, "context unavailable", resp) ;

	char const *dirs[SERVICE_SRC_DIRS_MAX] ;
	int editable[SERVICE_SRC_DIRS_MAX] ;
	size_t ndirs = 0 ;
	char home[SS_MAX_PATH_LEN + 1] ;
	struct stat st ;

	if (!service_src_dirs(ctx->owner, dirs, editable, &ndirs, home))
		return resp_error(500, "resolve source directories", resp) ;

	for (size_t i = 0 ; i < ndirs ; i++) {

		if (!editable[i] || access(dirs[i], W_OK))
			continue ;

		char path[strlen(dirs[i]) + strlen(name) + 1] ;
		auto_strings(path, dirs[i], name) ;

		if (lstat(path, &st) < 0)
			continue ;

		int ok = S_ISDIR(st.st_mode) ? dir_destroy(path) : (unlink(path) == 0) ;
		if (!ok)
			return resp_error(500, "remove frontend", resp) ;

		json_t j = JSON_ZERO ;
		json_obj_open(&j) ;
		json_key(&j, "name") ; json_str(&j, name) ;
		json_key(&j, "source") ; json_str(&j, dirs[i]) ;
		json_key(&j, "removed") ; json_bool(&j, 1) ;
		json_obj_close(&j) ;

		return resp_from_json(&j, 200, resp) ;
	}

	return resp_error(404, "frontend not found in an editable source directory", resp) ;
}

int api_route_get(char const *path, api_result_t *r)
{
	api_response_t *resp = &r->sync ;

	if (strncmp(path, API_PREFIX, API_PREFIX_LEN) != 0)
		return 0 ;

	char const *tail = path + API_PREFIX_LEN ;

	if (!strcmp(tail, "/trees"))
		return handle_trees(resp) ;

	if (!strcmp(tail, "/services"))
		return handle_services(resp) ;

	if (!strcmp(tail, "/available"))
		return handle_available(resp) ;

	if (!strncmp(tail, "/frontend/", 10))
		return handle_frontend_read(tail + 10, resp) ;

	if (!strncmp(tail, "/services/", 10)) {

		char const *p = tail + 10 ;
		char const *slash = strchr(p, '/') ;

		if (!slash)
			return handle_service_detail(p, resp) ;

		size_t namelen = (size_t)(slash - p) ;
		char sname[SS_MAX_SERVICE_NAME] ;

		if (namelen >= SS_MAX_SERVICE_NAME)
			return resp_error(400, "service name too long", resp) ;

		memcpy(sname, p, namelen) ;
		sname[namelen] = 0 ;

		if (!strcmp(slash, "/log"))
			return handle_service_log(sname, resp) ;
		if (!strcmp(slash, "/frontend"))
			return handle_service_frontend(sname, resp) ;
		if (!strcmp(slash, "/config"))
			return handle_service_config(sname, r) ;

		return 0 ; // unknown subresource
	}

	// /api/<unknown> falls through to the http layer's generic 404.
	return 0 ;
}

int api_route_post(char const *path, char const *body, size_t body_len, api_result_t *r)
{
	api_response_t *resp = &r->sync ;

	if (strncmp(path, API_PREFIX, API_PREFIX_LEN) != 0)
		return 0 ;

	char const *tail = path + API_PREFIX_LEN ;

	// /api/services (exact) — create; must precede the "/services/" prefix.
	if (!strcmp(tail, "/services"))
		return handle_create_service(body, body_len, r) ;

	// /api/modules (exact) — create a module directory.
	if (!strcmp(tail, "/modules"))
		return handle_create_module(body, body_len, r) ;

	// /api/enable (exact) — parse+enable an existing frontend.
	if (!strcmp(tail, "/enable"))
		return handle_enable(body, body_len, r) ;

	// /api/remove/<name> — delete an unparsed frontend from an editable dir.
	if (!strncmp(tail, "/remove/", 8))
		return handle_remove(tail + 8, resp) ;

	/* /api/services/<name>/<action> */
	if (!strncmp(tail, "/services/", 10)) {

		char const *p = tail + 10 ;
		char const *slash = strchr(p, '/') ;

		if (!slash || slash == p)
			return resp_error(400, "expected /api/services/<name>/<action>", resp) ;

		size_t namelen = (size_t)(slash - p) ;
		char const *action = slash + 1 ;
		if (!*action || strchr(action, '/'))
			return resp_error(400, "expected /api/services/<name>/<action>", resp) ;

		char name[SS_MAX_SERVICE_NAME] ;
		if (namelen >= SS_MAX_SERVICE_NAME)
			return resp_error(400, "service name too long", resp) ;

		memcpy(name, p, namelen) ;
		name[namelen] = 0 ;

		return handle_action(name, action, r) ;
	}

	/* /api/trees/<name>/<action> */
	if (!strncmp(tail, "/trees/", 7)) {
		char const *p = tail + 7 ;
		char const *slash = strchr(p, '/') ;

		if (!slash || slash == p)
			return resp_error(400, "expected /api/trees/<name>/<action>", resp) ;

		size_t namelen = (size_t)(slash - p) ;
		char const *action = slash + 1 ;

		if (!*action || strchr(action, '/'))
			return resp_error(400, "expected /api/trees/<name>/<action>", resp) ;

		char name[SS_MAX_SERVICE_NAME] ;
		if (namelen >= SS_MAX_SERVICE_NAME)
			return resp_error(400, "tree name too long", resp) ;

		memcpy(name, p, namelen) ;
		name[namelen] = 0 ;

		return handle_tree_action(name, action, r) ;
	}

	return 0 ;
}
