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

#include <dirent.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>

#include "launcher.h"
#include "service.h"
#include "dbus.h"
#include "util.h"

#include <oblibs/directory.h>
#include <oblibs/fd.h>
#include <oblibs/io.h>
#include <oblibs/log.h>
#include <oblibs/string.h>
#include <oblibs/sbl.h>
#include <oblibs/strbuf.h>
#include <oblibs/environ.h>
#include <oblibs/files.h>
#include <oblibs/types.h>
#include <oblibs/hash.h>

#include <66/utils.h>
#include <66/config.h>
#include <66/constants.h>
#include <66/resolve.h>
#include <66/tree.h>

#include <66-tools/config.h>

#define DBS_SERVICE_66_ARGV_MAX 8

void service_hash_free(hash_t *hservice)
{
	log_flow() ;

	struct service_s *c, *tmp ;
	HASH_FOREACH(hservice, c, tmp)
		free(c) ;

	hash_free(hservice) ;
}

struct service_s *service_search_byname(hash_t *hservice, const char *name)
{
	log_flow() ;

	return hash_find(hservice, name, strlen(name)) ;
}

static struct service_s *service_search_byid(hash_t *hservice, int id)
{
	log_flow() ;

	struct service_s *c, *tmp ;

	HASH_FOREACH(hservice, c, tmp)
		if (c->id == id)
			return c ;

	return NULL ;
}

int service_dir_list(strbuf *names, const char *dir, const char *suffix)
{
	log_flow() ;

	DIR *d = opendir(dir) ;

	if (!d) {

		if (errno == ENOENT || errno == ENOTDIR)
			return DBS_SERVICE_DIR_ABSENT ;

		log_warnusys_return(DBS_EXIT_FATAL, "open directory: ", dir) ;
	}

	for (;;) {

		struct dirent *e ;

		errno = 0 ;
		e = readdir(d) ;

		if (!e)
			break ;

		if (e->d_name[0] == '.')
			continue ;

		if (get_rstrlen_until(e->d_name, suffix) <= 0)
			continue ;

		if (!sbl_add(names, e->d_name)) {
			dir_close(d) ;
			log_warnusys_return(DBS_EXIT_FATAL, "list directory: ", dir) ;
		}
	}

	if (errno) {
		dir_close(d) ;
		log_warnusys_return(DBS_EXIT_FATAL, "read directory: ", dir) ;
	}

	dir_close(d) ;

	if (names->len && !sbl_sort(names))
		log_warnusys_return(DBS_EXIT_FATAL, "sort the entries of directory: ", dir) ;

	return DBS_SERVICE_DIR_READ ;
}

int service_get_list(strbuf *sa, launcher_t *launcher)
{
	log_flow() ;

	char const *path = launcher->uid > 0 ? SS_TOOLS_DBS_SESSION_SERVICE : SS_TOOLS_DBS_SYSTEM_SERVICE ;
	int r = service_dir_list(sa, path, DBS_SERVICE_FILE_SUFFIX) ;

	if (r == DBS_SERVICE_DIR_ABSENT)
		log_info("no service directory: ", path, " -- keeping the frontend files untouched") ;

	return r ;
}

int service_add_hash(launcher_t *launcher, struct service_s *service)
{
	log_flow() ;

	if (service_search_byname(launcher->hservice, service->name)) {
		log_warn("ignoring duplicate name: ", service->name) ;
		free(service) ;
		return 0 ;
	}

	service->state = 0 ;
	FLAGS_SET(service->state, DBS_SERVICE_INSERT) ;
	service->id = launcher->nservice++ ;

	if (launcher->nservice > INT_MAX)
		launcher->nservice = DBS_SERVICE_ID_FIRST ;

	if (!hash_add(launcher->hservice, service->name, strlen(service->name), service)) {
		log_warnusys("add service to hash") ;
		free(service) ;
		return 0 ;
	}

	return 1 ;
}

static void service_remove_hash(launcher_t *launcher, const char *name)
{
	log_flow() ;

	struct service_s *h = service_search_byname(launcher->hservice, name) ;

	if (h != NULL) {
		hash_del(launcher->hservice, h) ;
		free(h) ;
	}
}

static int service_read_file(strbuf *sb, const char *path)
{
	log_flow() ;

	struct stat st ;
	ssize_t fd = io_open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC) ;

	if (fd < 0)
		return 0 ;

	if (fstat(fd, &st) < 0) {
		close_fd(fd) ;
		return 0 ;
	}

	if (!S_ISREG(st.st_mode)) {
		close_fd(fd) ;
		return -1 ;
	}

	if (!strbuf_reserve(sb, sb->len + (size_t)st.st_size + 1)) {
		close_fd(fd) ;
		return 0 ;
	}

	if (io_allread(fd, sb->s + sb->len, (size_t)st.st_size) != (size_t)st.st_size) {
		close_fd(fd) ;
		return 0 ;
	}

	sb->len += (size_t)st.st_size ;
	close_fd(fd) ;

	return strbuf_uncounted(sb) ;
}

static int service_write_file(const char *path, const char *s, size_t len)
{
	log_flow() ;

	struct stat st ;
	ssize_t fd = io_open_mode(path, O_WRONLY | O_TRUNC | O_CREAT | O_NONBLOCK | O_CLOEXEC, 0666) ;

	if (fd < 0)
		return 0 ;

	if (fstat(fd, &st) < 0) {
		close_fd(fd) ;
		return 0 ;
	}

	if (!S_ISREG(st.st_mode)) {
		close_fd(fd) ;
		errno = EINVAL ;
		return 0 ;
	}

	return io_writenclose(fd, s, len) ;
}

static int service_group_keys(strbuf *keys, const char *group)
{
	log_flow() ;

	while (*group) {

		const char *end = strchr(group, '\n') ;
		size_t len = end ? (size_t)(end - group) + 1 : strlen(group) ;

		if (memchr(group, '=', len) && !strbuf_catb(keys, group, len))
			return 0 ;

		group += len ;
	}

	return strbuf_uncounted(keys) ;
}

static int service_parse(struct service_s *service, const char *path)
{
	log_flow() ;

	/** We cannot use the environ_parse_file directly
	 * due of the section [D-BUS Service] at start of the file.
	 * Hack it commenting it. */
	size_t pos = 0 ;
	_cleanup_strbuf_ strbuf file = STRBUF_ZERO ;
	_cleanup_strbuf_ strbuf contents = STRBUF_ZERO ;

	log_trace("parsing service: ", path) ;

	if (service_read_file(&file, path) != 1)
		log_warnusys_return(DBS_EXIT_WARN, "read file: ", path) ;

	int seclen = str_contain(file.s, DBS_SERVICE_SECTION) ;
	if (seclen <= 0)
		log_warnu_return(DBS_EXIT_WARN, "get section " DBS_SERVICE_SECTION " at file: ", path) ;

	seclen++ ; // remove '\n' character

	if ((size_t)seclen > file.len)
		log_warn_return(DBS_EXIT_WARN, "ignoring service file: ", path, ": nothing follows " DBS_SERVICE_SECTION) ;

	char *group = strstr(file.s + seclen - 1, DBS_SERVICE_GROUP_START) ;

	if (group)
		group[1] = 0 ;

	_cleanup_strbuf_ strbuf keys = STRBUF_ZERO ;

	if (!service_group_keys(&keys, file.s + seclen))
		log_warnusys_return(DBS_EXIT_WARN, "select the keys of file: ", path) ;

	if (!environ_trim(&contents, keys.s))
		log_warnu_return(DBS_EXIT_WARN, "trim environment file: ", path) ;

	FOREACH_SBL(&contents, pos) {

		char *line = contents.s + pos ;
		char *store = NULL ;
		size_t capacity = 0 ;

		_alloc_strbuf_(key, strlen(line) + 1) ;
		_alloc_strbuf_(val, strlen(line) + 1) ;

		if (!environ_get_key(&key, line))
			log_warnu_return(DBS_EXIT_WARN, "get key from line: ", line, " at file: ", path) ;

		if (!environ_get_value(&val, line))
			log_warnu_return(DBS_EXIT_WARN, "get value from line: ", line, " at file: ", path) ;

		if (!strcmp(key.s, "Name")) {
			store = service->name ;
			capacity = DBS_SERVICE_NAME_MAX + 1 ;
		} else if (!strcmp(key.s, "Exec")) {
			store = service->exec ;
			capacity = sizeof(service->exec) ;
		} else if (!strcmp(key.s, "User")) {
			store = service->user ;
			capacity = sizeof(service->user) ;
		}

		if (!store)
			continue ;

		if (strlen(val.s) >= capacity)
			log_warn_return(DBS_EXIT_WARN, "ignoring service file: ", path, ": oversized ", key.s, " field") ;

		auto_strings(store, val.s) ;
	}
	return 1 ;
}

static int service_owner_dir(char *store, launcher_t *launcher, const char *userdir, const char *sysdir)
{
	if (launcher->uid > 0) {

		if (!set_ownerhome_stack_byuid(store, launcher->uid))
			log_warnu_return(DBS_EXIT_WARN, "set home directory") ;

		auto_string_builder(store, strlen(store), (char const *[]){ userdir, NULL}) ;

	} else {

		auto_strings(store, sysdir) ;
	}

	return 1 ;
}

static int service_front_path(char *store, launcher_t *launcher, const char *service)
{
	if (!service_owner_dir(store, launcher, SS_SERVICE_USERDIR, SS_SERVICE_ADMDIR))
		return DBS_EXIT_WARN ;

	auto_string_builder(store, strlen(store), (char const *[]){ service, DBS_SERVICE_SUFFIX, NULL}) ;

	return 1 ;
}

static int service_write_frontend(launcher_t *launcher, struct service_s *service)
{
	log_flow() ;

	_cleanup_strbuf_ strbuf sa = STRBUF_ZERO ;

	if (!auto_strbuf(&sa,
		"[Main]\n",
		"Type = classic\n",
		"Description = \"", service->name, " dbus service\"\n",
		"InTree = " DBS_SERVICE_TREE "\n\n",
		"[Start]\n"))
			log_warnu_return(DBS_EXIT_FATAL, "build frontend file contents") ;

	if (*service->user) {
		if (!auto_strbuf(&sa, "RunAs = ", service->user, "\n"))
			log_warnu_return(DBS_EXIT_FATAL, "build frontend file contents") ;
	}

	if (!auto_strbuf(&sa,
		"Execute = (\n",
		"	", service->exec, "\n",
		")\n",
		"Timeout = 3000\n",
		"MaxDeath = 5\n\n",
		"[Stop]\n",
		"Timeout = 3000\n"))
			log_warnu_return(DBS_EXIT_FATAL, "build frontend file contents") ;

	if (!service_front_path(service->frontend, launcher, service->name))
		log_warnu_return(DBS_EXIT_FATAL, "get frontend service file of service: ", service->name) ;

	_cleanup_strbuf_ strbuf current = STRBUF_ZERO ;
	int r = service_read_file(&current, service->frontend) ;

	if (r < 0)
		log_warn_return(DBS_EXIT_FATAL, "use the frontend file: ", service->frontend, ": not a regular file") ;

	if (!r && errno != ENOENT)
		log_warnusys("read the frontend file: ", service->frontend, " -- rewriting it") ;

	if (r && current.len == sa.len && !memcmp(current.s, sa.s, sa.len))
		return DBS_SERVICE_FRONTEND_CURRENT ;

	log_trace("write frontend file: ", service->frontend) ;
	if (!service_write_file(service->frontend, sa.s, sa.len))
		log_warnusys_return(DBS_EXIT_FATAL, "write file: ", service->frontend) ;

	return DBS_SERVICE_FRONTEND_WRITTEN ;
}

static int service_name_isvalid(const char *name)
{
	log_flow() ;

	size_t dots = 0 ;
	int start = 1 ;

	for (const char *p = name ; *p ; p++) {

		if (*p == '.') {

			if (start)
				return 0 ;

			dots++ ;
			start = 1 ;
			continue ;
		}

		if (start && *p >= '0' && *p <= '9')
			return 0 ;

		if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-'))
			return 0 ;

		start = 0 ;
	}

	return dots && !start ;
}

static int service_name_matches_file(const char *name, const char *base)
{
	log_flow() ;

	ssize_t stem = get_rstrlen_until(base, DBS_SERVICE_FILE_SUFFIX) ;

	return stem == (ssize_t)strlen(name) && !strncmp(base, name, (size_t)stem) ;
}

static int service_unread(strbuf *unread, const char *file)
{
	log_flow() ;

	if (unread && !strbuf_catb(unread, file, strlen(file) + 1))
		log_warnusys_return(DBS_EXIT_FATAL, "record the unread service file: ", file) ;

	return DBS_EXIT_WARN ;
}

static int service_read(struct service_s **ret, launcher_t *launcher, strbuf *claimed, strbuf *unread, const char *file, const char *base)
{
	log_flow() ;

	int parsed ;
	struct stat st ;
	struct service_s *service ;

	if (stat(file, &st) < 0) {
		log_warnusys("get the status of: ", file, " -- ignoring it") ;
		return service_unread(unread, file) ;
	}

	if (!S_ISREG(st.st_mode)) {
		log_warn("ignoring service file: ", file, ": not a regular file") ;
		return service_unread(unread, file) ;
	}

	service = (struct service_s *)malloc(sizeof(*service)) ;
	if (service == NULL)
		log_warnu_return(DBS_EXIT_FATAL, "malloc") ;

	memset(service, 0, sizeof(*service)) ;

	parsed = service_parse(service, file) ;

	if (*service->name && service_name_isvalid(service->name) && (launcher->uid > 0 || service_name_matches_file(service->name, base))) {

		if (!strbuf_catb(claimed, service->name, strlen(service->name) + 1)) {
			log_warnusys("record the name of service: ", service->name) ;
			free(service) ;
			return DBS_EXIT_FATAL ;
		}
	}

	if (!parsed) {
		free(service) ;
		log_warnu("parse service: ", file) ;
		return service_unread(unread, file) ;
	}

	if (!*service->name) {
		free(service) ;
		log_warn("ignoring service file: ", file, ": missing Name field") ;
		return service_unread(unread, file) ;
	}

	if (!service_name_isvalid(service->name)) {
		log_warn("ignoring service file: ", file, ": invalid D-Bus name: ", service->name) ;
		free(service) ;
		return service_unread(unread, file) ;
	}

	if (!service_name_matches_file(service->name, base)) {

		if (!launcher->uid) {
			log_warn("ignoring service file: ", file, ": it is not named after the D-Bus name: ", service->name) ;
			free(service) ;
			return DBS_EXIT_WARN ;
		}

		log_warn("service file: ", file, " is not named after the D-Bus name: ", service->name, " -- the name prevails") ;
	}

	if (!*service->exec) {
		log_warn("ignoring service: ", service->name, " at file: ", file, ": missing Exec field") ;
		free(service) ;
		return DBS_EXIT_WARN ;
	}

	auto_strings(service->file, base) ;

	*ret = service ;

	return 1 ;
}

static int service_register(launcher_t *launcher, struct service_s *service, const char *file)
{
	log_flow() ;

	if (service_search_byname(launcher->hservice, service->name)) {
		log_warn("ignoring duplicate name: ", service->name, " at file: ", file) ;
		free(service) ;
		return DBS_EXIT_WARN ;
	}

	int w = service_write_frontend(launcher, service) ;

	if (w < 0) {
		log_warnu("write the frontend file of service: ", service->name, " at file: ", file, " -- ignoring it") ;
		free(service) ;
		return DBS_EXIT_WARN ;
	}

	if (!service_add_hash(launcher, service))
		return DBS_EXIT_WARN ;

	if (w == DBS_SERVICE_FRONTEND_WRITTEN)
		FLAGS_SET(service->state, DBS_SERVICE_PARSE) ;

	return 1 ;
}

int service_translate(launcher_t *launcher, strbuf *claimed, strbuf *unread, const char *name)
{
	log_flow() ;

	int r ;
	char const *path = launcher->uid > 0 ? SS_TOOLS_DBS_SESSION_SERVICE : SS_TOOLS_DBS_SYSTEM_SERVICE ;
	struct service_s *service = NULL ;

	_alloc_strbuf_(file, strlen(path) + strlen(name) + 1) ;
	auto_strings(file.s, path, name) ;

	r = service_read(&service, launcher, claimed, unread, file.s, name) ;
	if (r <= 0)
		return r ;

	return service_register(launcher, service, file.s) ;
}

static void service_tree_contents(strbuf *contents, launcher_t *launcher)
{
	log_flow() ;

	char base[SS_MAX_PATH_LEN + 1] ;
	resolve_tree_t tres = RESOLVE_TREE_ZERO ;
	resolve_wrapper_t wres = { .type = DATA_TREE, .obj = &tres } ;

	if (!service_owner_dir(base, launcher, SS_USER_DIR, SS_SYSTEM_DIR))
		return ;

	if (!resolve_check(&wres, base, DBS_SERVICE_TREE)) {

		if (errno != ENOENT)
			log_warnusys("examine the tree: " DBS_SERVICE_TREE " at: ", base, " -- keeping every orphan service") ;

		return ;
	}

	if (resolve_read(&wres, base, DBS_SERVICE_TREE) <= 0 || (tres.ncontents && (tres.contents >= tres.sa.len || !memchr(tres.sa.s + tres.contents, 0, tres.sa.len - tres.contents)))) {
		log_warnu("read the tree: " DBS_SERVICE_TREE " at: ", base, " -- keeping every orphan service") ;
		strbuf_free(&tres.sa) ;
		return ;
	}

	if (tres.ncontents && !sbl_clean_string(contents, tres.sa.s + tres.contents))
		log_warnusys("list the services of the tree: " DBS_SERVICE_TREE " -- keeping every orphan service") ;

	strbuf_free(&tres.sa) ;
}

static int service_frontend_is_removable(const char *path)
{
	log_flow() ;

	struct stat st ;

	if (lstat(path, &st) < 0) {

		if (errno == ENOENT)
			return 1 ;

		log_warnusys_return(DBS_EXIT_WARN, "get the status of the frontend file: ", path, " -- keeping its service") ;
	}

	if (!S_ISREG(st.st_mode))
		log_warn_return(DBS_EXIT_WARN, "ignoring the frontend file: ", path, ": not a regular file -- keeping its service") ;

	return 1 ;
}

static int service_spawn_66(char *option, char **command)
{
	size_t n = 0 ;
	char fmt[I32_FMT] ;
	char *nargv[DBS_SERVICE_66_ARGV_MAX] ;

	fmt[i32_fmt(fmt, VERBOSITY)] = 0 ;

	nargv[n++] = "66" ;

	if (option)
		nargv[n++] = option ;

	nargv[n++] = "-v" ;
	nargv[n++] = fmt ;

	while (*command)
		nargv[n++] = *command++ ;

	nargv[n] = 0 ;

	return sync_spawn(nargv) ;
}

static int service_deactivate(struct service_s *service)
{
	log_flow() ;

	_alloc_strbuf_(name, strlen(service->name) + DBS_SERVICE_SUFFIX_LEN + 1) ;
	auto_strings(name.s, service->name, DBS_SERVICE_SUFFIX) ;

	log_info("deactivation requested for service: ", name.s) ;

	return service_spawn_66(NULL, (char *[]){ "remove", name.s, 0 }) ;
}

static void service_discard(launcher_t *launcher, struct service_s *service)
{
	log_flow() ;

	int r = service_deactivate(service) ;

	if (r) {
		log_warnu("remove the service: ", service->name, " from 66 -- keeping its frontend file") ;
		return ;
	}

	unlink(service->frontend) ;
	service_remove_hash(launcher, service->name) ;
}

static void service_clean_orphans(launcher_t *launcher, strbuf *list, strbuf *claimed, strbuf *unread)
{
	log_flow() ;

	size_t pos = 0 ;
	_cleanup_strbuf_ strbuf contents = STRBUF_ZERO ;

	if (unread->len) {

		FOREACH_SBL(unread, pos)
			log_warn("the service file: ", unread->s + pos, " could not be read entirely -- keeping every orphan service") ;

		return ;
	}

	service_tree_contents(&contents, launcher) ;

	FOREACH_SBL(&contents, pos) {

		struct service_s orphan = SERVICE_ZERO ;
		ssize_t stem = get_rstrlen_until(contents.s + pos, DBS_SERVICE_SUFFIX) ;

		if (stem <= 0 || stem > (ssize_t)DBS_SERVICE_NAME_MAX)
			continue ;

		if (memchr(contents.s + pos, '/', (size_t)stem))
			continue ;

		memcpy(orphan.name, contents.s + pos, (size_t)stem) ;

		if (service_search_byname(launcher->hservice, orphan.name))
			continue ;

		if (sbl_search(claimed, orphan.name) >= 0)
			continue ;

		_alloc_strbuf_(file, (size_t)stem + sizeof(DBS_SERVICE_FILE_SUFFIX)) ;
		auto_strings(file.s, orphan.name, DBS_SERVICE_FILE_SUFFIX) ;

		if (sbl_search(list, file.s) >= 0)
			continue ;

		if (!service_front_path(orphan.frontend, launcher, orphan.name))
			continue ;

		if (!service_frontend_is_removable(orphan.frontend))
			continue ;

		log_info("remove the orphan service: ", orphan.name) ;

		service_discard(launcher, &orphan) ;
	}
}

int service_load(launcher_t *launcher)
{
	log_flow() ;

	int r ;
	size_t pos = 0 ;
	_cleanup_strbuf_ strbuf sa = STRBUF_ZERO ;
	_cleanup_strbuf_ strbuf claimed = STRBUF_ZERO ;
	_cleanup_strbuf_ strbuf unread = STRBUF_ZERO ;

	int dir = service_get_list(&sa, launcher) ;

	if (dir < 0)
		log_warnu_return(1, "list the service files -- starting with no service at all") ;

	FOREACH_SBL(&sa, pos) {

		r = service_translate(launcher, &claimed, &unread, sa.s + pos) ;
		if (r == DBS_EXIT_FATAL)
			return r ;
	}

	if (dir == DBS_SERVICE_DIR_READ)
		service_clean_orphans(launcher, &sa, &claimed, &unread) ;

	return 1 ;
}

static int service_names_from_file(strbuf *names, launcher_t *launcher, const char *file)
{
	log_flow() ;

	struct service_s *c, *tmp ;

	HASH_FOREACH(launcher->hservice, c, tmp)
		if (!strcmp(c->file, file) && !strbuf_catb(names, c->name, strlen(c->name) + 1))
			log_warnusys_return(0, "record the name of service: ", c->name) ;

	return 1 ;
}

void service_handle_state(strbuf *list, launcher_t *launcher)
{
	log_flow() ;

	int r ;
	size_t pos = 0 ;
	char const *path = launcher->uid > 0 ? SS_TOOLS_DBS_SESSION_SERVICE : SS_TOOLS_DBS_SYSTEM_SERVICE ;
	struct service_s *hash ;
	struct service_s *c, *tmp ;
	_cleanup_strbuf_ strbuf names = STRBUF_ZERO ;
	_cleanup_strbuf_ strbuf kept = STRBUF_ZERO ;

	/** compare for service to add */
	FOREACH_SBL(list, pos) {

		struct service_s *service = NULL ;
		_alloc_strbuf_(file, strlen(path) + strlen(list->s + pos) + 1) ;
		auto_strings(file.s, path, list->s + pos) ;

		r = service_read(&service, launcher, &kept, NULL, file.s, list->s + pos) ;
		if (r == DBS_EXIT_FATAL)
			return ;

		if (r == DBS_EXIT_WARN) {

			if (!service_names_from_file(&kept, launcher, list->s + pos))
				return ;

			continue ;
		}

		if (sbl_search(&names, service->name) >= 0) {
			log_warn("ignoring duplicate name: ", service->name, " at file: ", file.s) ;
			free(service) ;
			continue ;
		}

		if (!strbuf_catb(&names, service->name, strlen(service->name) + 1)) {
			log_warnusys("record the name of service: ", service->name) ;
			free(service) ;
			return ;
		}

		hash = service_search_byname(launcher->hservice, service->name) ;

		if (hash == NULL) {
			/** Nothing to do with the exit code */
			service_register(launcher, service, file.s) ;
		} else {

			int w = service_write_frontend(launcher, service) ;

			if (w < 0) {

				log_warnu("update the frontend file of service: ", service->name) ;

			} else {

				auto_strings(hash->file, service->file) ;
				auto_strings(hash->exec, service->exec) ;
				auto_strings(hash->user, service->user) ;

				if (w == DBS_SERVICE_FRONTEND_WRITTEN)
					FLAGS_SET(hash->state, DBS_SERVICE_PARSE) ;
			}

			free(service) ;
		}
	}

	/** compare for service to remove */
	HASH_FOREACH(launcher->hservice, c, tmp) {

		if (sbl_search(&names, c->name) < 0 && sbl_search(&kept, c->name) < 0) {

			FLAGS_SET(c->state, DBS_SERVICE_DELETE) ;

		} else {

			FLAGS_CLEAR(c->state, DBS_SERVICE_DELETE) ;

			if (!FLAGS_ISSET(c->state, DBS_SERVICE_INSERT))
				FLAGS_SET(c->state, DBS_SERVICE_OK) ;
		}
	}
}

static int service_reactivate(struct service_s *service)
{
	log_flow() ;

	_alloc_strbuf_(name, strlen(service->name) + DBS_SERVICE_SUFFIX_LEN + 1) ;
	auto_strings(name.s, service->name, DBS_SERVICE_SUFFIX) ;

	log_info("reactivation requested for service: ", name.s) ;

	return service_spawn_66(NULL, (char *[]){ "parse", "-f", name.s, 0 }) ;
}

static void service_reparse(struct service_s *service)
{
	if (FLAGS_ISSET(service->state, DBS_SERVICE_PARSE) && service_reactivate(service)) {
		log_warnu("parse the frontend file of service: ", service->name) ;
		unlink(service->frontend) ;
	}
}

void service_sync_launcher_broker(launcher_t *launcher)
{
	log_flow() ;

	int r;
	struct service_s *c, *tmp ;

	HASH_FOREACH(launcher->hservice, c, tmp) {

		char fmt[U32_FMT] ;
		size_t ilen = (size_t)u32_fmt(fmt, c->id) ;
		fmt[ilen] = 0 ;
		_alloc_strbuf_(path, strlen("/org/bus1/DBus/Name/") + ilen + 1) ;
		auto_strings(path.s, "/org/bus1/DBus/Name/", fmt) ;

		if (FLAGS_ISSET(c->state, DBS_SERVICE_DELETE)) {

			if (!FLAGS_ISSET(c->state, DBS_SERVICE_OK) && !FLAGS_ISSET(c->state, DBS_SERVICE_SENT)) {
				service_remove_hash(launcher, c->name) ;
				continue ;
			}

			r = odbus_call_method(launcher->bus, path.s, "org.bus1.DBus.Name", "Release", DBS_DBUS_CALL_TIMEOUT_MS, "") ;

			if (!r && FLAGS_ISSET(c->state, DBS_SERVICE_OK)) {
				log_warnusys("org.bus1.DBus.Name.Release") ;
				continue ;
			}

			if (r)
				log_info( "freed service: ", c->name, " (", path.s, ")") ;

			service_remove_hash(launcher, c->name) ;

		} else if (FLAGS_ISSET(c->state, DBS_SERVICE_OK)) {

			service_reparse(c) ;

			FLAGS_CLEAR(c->state, DBS_SERVICE_PARSE) ;

		} else if (FLAGS_ISSET(c->state, DBS_SERVICE_INSERT)) {

			FLAGS_SET(c->state, DBS_SERVICE_SENT) ;

			r = odbus_call_method(launcher->bus, "/org/bus1/DBus/Broker", "org.bus1.DBus.Broker", "AddName", DBS_DBUS_CALL_TIMEOUT_MS, "osu", path.s, c->name, 0) ;
			if (!r) {
				log_warnusys("org.bus1.DBus.AddName") ;
				continue ;
			}

			log_info("add service name: ", c->name, " (", path.s, ")") ;

			service_reparse(c) ;

			c->state = 0 ;
			FLAGS_SET(c->state, DBS_SERVICE_OK) ;

		} else {
			/* unreachable */
			flog_warn("bug: service %s (%s) in invalid state %u", c->name, path.s, c->state) ;
		}
	}

	log_info("service selection synchronized successfully") ;
}

int service_reload(launcher_t *launcher)
{
	log_flow() ;

	_cleanup_strbuf_ strbuf sa = STRBUF_ZERO ;

	if (service_get_list(&sa, launcher) != DBS_SERVICE_DIR_READ)
		log_warnu_return(DBS_EXIT_WARN, "read the service directory -- keeping the service selection untouched") ;

	service_handle_state(&sa, launcher) ;

	service_sync_launcher_broker(launcher) ;

	return 1 ;
}

int service_activate(launcher_t *launcher, int id)
{
	log_flow() ;

	struct service_s *s = service_search_byid(launcher->hservice, id) ;

	if (!s)
		flog_warnu_return(DBS_EXIT_FATAL, "find service with id: %d  -- ignoring activation request", id) ;

	if (service_write_frontend(launcher, s) < 0)
		log_warnu("write the frontend file of service: ", s->name) ;

	_alloc_strbuf_(name, strlen(s->name) + DBS_SERVICE_SUFFIX_LEN + 1) ;
	auto_strings(name.s, s->name, DBS_SERVICE_SUFFIX) ;

	log_info("activation requested for service: ", name.s) ;

	return service_spawn_66(NULL, (char *[]){ "start", name.s, 0 }) ;
}

void service_discard_tree(void)
{
	log_flow() ;

	service_spawn_66("-T3000", (char *[]){ "tree", "free", DBS_SERVICE_TREE, 0 }) ;
}