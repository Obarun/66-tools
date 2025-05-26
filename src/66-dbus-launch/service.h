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

#include <oblibs/hash.h>

#include <skalibs/stralloc.h>

#include <66/constants.h>
#include <66/config.h>

#define DBS_ENVIRONMENTFILE "0000-dbus"
#define DBS_ENVIRONMENTFILE_LEN (sizeof DBS_ENVIRONMENTFILE - 1)
#define DBS_SERVICE_SUFFIX ".dbus"
#define DBS_SERVICE_SUFFIX_LEN (sizeof DBS_SERVICE_SUFFIX - 1)
#define DBS_SERVICE_SECTION "[D-BUS Service]"
#define DBS_SERVICE_SECTION_LEN (sizeof DBS_SERVICE_SECTION - 1)

#define DBS_SERVICE_OK (1 << 1)
#define DBS_SERVICE_INSERT (1 << 2)
#define DBS_SERVICE_PARSE (1 << 3)
#define DBS_SERVICE_DELETE (1 << 4)

struct service_s {
	char name[SS_MAX_SERVICE_NAME + 1] ;
	char exec[1024 + 1] ;
	char user[1024 + 1] ;
	char frontend[SS_MAX_PATH_LEN + 1] ;
	int id ;
	uint8_t state ;
	UT_hash_handle hh ;
} ;

#define SERVICE_ZERO { {0}, {0}, {0}, {0}, 0, 0, NULL }

extern void service_hash_free(struct service_s **hservice) ;
extern struct service_s *service_search_byname(struct service_s **hservice, const char *name) ;
extern struct service_s *service_search_byid(struct service_s **hservice, int id) ;
extern int service_get_list(stralloc *sa, launcher_t *launcher) ;
extern void service_add_hash(launcher_t *launcher, struct service_s *service) ;
extern void service_remove_hash(launcher_t *launcher, const char *name) ;
extern int service_environ_owner_path(char *store, launcher_t *launcher) ;
extern int service_environ_file_name(char *store, launcher_t *launcher) ;
extern int service_parse(struct service_s *service, const char *path) ;
extern int service_frontend_path(char *store, launcher_t *launcher, const char *service) ;
extern int service_resolve_path(char *store, launcher_t *launcher, const char *service) ;
extern int service_write_frontend(launcher_t *launcher, struct service_s *service) ;
extern int service_translate(launcher_t *launcher, const char *name) ;
extern int service_load(launcher_t *launcher) ;
extern void service_handle_state(stralloc *sa, launcher_t *launcher) ;
extern void service_sync_launcher_broker(launcher_t *launcher) ;
extern int service_reload(launcher_t *launcher) ;
extern int service_activate(launcher_t *launcher, int id) ;
extern int service_reactivate(struct service_s *service) ;
extern int service_deactivate(struct service_s *service) ;
extern void service_discard(launcher_t *launcher, struct service_s *service) ;
extern void service_discard_tree(void) ;







#endif

