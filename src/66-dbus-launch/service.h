/*
 * service.h
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

#ifndef DBS_SERVICE_H_INCLUDE
#define DBS_SERVICE_H_INCLUDE

#include <stdint.h>
#include <stddef.h>

#include "launcher.h"

#include <66/hash.h>
#include <66/constants.h>
#include <66/config.h>

#define DBS_ENVIRONMENTFILE "0000-dbus"
#define DBS_ENVIRONMENTFILE_LEN (sizeof DBS_ENVIRONMENTFILE - 1)
#define DBS_SERVICE_SUFFIX ".dbus"
#define DBS_SERVICE_SUFFIX_LEN (sizeof DBS_SERVICE_SUFFIX - 1)
#define DBS_SERVICE_SECTION "[D-BUS Service]"
#define DBS_SERVICE_SECTION_LEN (sizeof DBS_SERVICE_SECTION - 1)

enum {
	DBS_SERVICE_OK = 0,
	DBS_SERVICE_INSERT,
	DBS_SERVICE_DELETE,
	DBS_SERVICE_ENDOFKEY
} ;

struct service_s {
	char name[SS_MAX_SERVICE_NAME + 1] ;
	char exec[1024 + 1] ;
	char user[1024 + 1] ;
	char frontend[SS_MAX_PATH_LEN + 1] ;
	size_t id ;
	uint8_t pending ;
	UT_hash_handle hh ;
} ;

#define SERVICE_ZERO { {0}, {0}, {0}, {0}, 0, DBS_SERVICE_DELETE, NULL }

extern char const *pending_list[] ;

extern void service_hash_free(struct service_s **hservice) ;
extern struct service_s *service_search_byname(struct service_s **hservice, const char *name) ;
extern struct service_s *service_search_byid(struct service_s **hservice, int id) ;
extern void service_add_hash(launcher_t *launcher, struct service_s *service) ;
extern void service_remove_hash(launcher_t *launcher, const char *name) ;
extern int service_environ_owner_path(char *store, launcher_t *launcher) ;
extern int service_environ_file_name(char *store, launcher_t *launcher) ;
extern int service_collect(launcher_t *launcher) ;
extern int service_parse(struct service_s *service, const char *path) ;
extern int service_frontend_path(char *store, launcher_t *launcher, const char *service) ;
extern int service_write_frontend(launcher_t *launcher, struct service_s *service) ;
extern void service_sync_launcher_broker(launcher_t *launcher) ;
extern void service_load(launcher_t *launcher) ;
extern int service_activate(launcher_t *launcher, int id) ;
extern int service_deactivate(struct service_s *service) ;
extern int service_reconfigure(struct service_s *service) ;
extern void service_discard(launcher_t *launcher, struct service_s *service) ;
extern void service_discard_tree(void) ;

#endif

