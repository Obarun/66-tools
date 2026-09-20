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

#include <limits.h>
#include <stdint.h>
#include <stddef.h>

#include "launcher.h"

#include <oblibs/hash.h>
#include <oblibs/strbuf.h>

#include <66/constants.h>
#include <66/config.h>

#define DBS_SERVICE_SUFFIX ".dbus"
#define DBS_SERVICE_SUFFIX_LEN (sizeof DBS_SERVICE_SUFFIX - 1)
#define DBS_SERVICE_FILE_SUFFIX ".service"
#define DBS_SERVICE_NAME_MAX (SS_MAX_SERVICE_NAME < NAME_MAX - DBS_SERVICE_SUFFIX_LEN ? SS_MAX_SERVICE_NAME : NAME_MAX - DBS_SERVICE_SUFFIX_LEN)
#define DBS_SERVICE_TREE "dbus"
#define DBS_SERVICE_FRONTEND_CURRENT 1
#define DBS_SERVICE_FRONTEND_WRITTEN 2
#define DBS_SERVICE_DIR_READ 1
#define DBS_SERVICE_DIR_ABSENT 2
#define DBS_SERVICE_SECTION "[D-BUS Service]"
#define DBS_SERVICE_SECTION_LEN (sizeof DBS_SERVICE_SECTION - 1)
#define DBS_SERVICE_GROUP_START "\n["
#define DBS_SERVICE_ID_NONE 0
#define DBS_SERVICE_ID_FIRST 1

#define DBS_SERVICE_OK (1 << 1)
#define DBS_SERVICE_INSERT (1 << 2)
#define DBS_SERVICE_PARSE (1 << 3)
#define DBS_SERVICE_DELETE (1 << 4)
#define DBS_SERVICE_SENT (1 << 5)

struct service_s {
	char name[SS_MAX_SERVICE_NAME + 1] ;
	char exec[1024 + 1] ;
	char user[1024 + 1] ;
	char file[NAME_MAX + 1] ;
	char frontend[SS_MAX_PATH_LEN + 1] ;
	int id ;
	uint8_t state ;
	hash_node_t node ;
} ;

#define SERVICE_ZERO { {0}, {0}, {0}, {0}, {0}, 0, 0, HASH_NODE_ZERO }

extern void service_hash_free(hash_t *hservice) ;
extern struct service_s *service_search_byname(hash_t *hservice, const char *name) ;
extern int service_dir_list(strbuf *names, const char *dir, const char *suffix) ;
extern int service_get_list(strbuf *sa, launcher_t *launcher) ;
extern int service_add_hash(launcher_t *launcher, struct service_s *service) ;
extern int service_translate(launcher_t *launcher, strbuf *claimed, strbuf *unread, const char *name) ;
extern int service_load(launcher_t *launcher) ;
extern void service_handle_state(strbuf *sa, launcher_t *launcher) ;
extern void service_sync_launcher_broker(launcher_t *launcher) ;
extern int service_reload(launcher_t *launcher) ;
extern int service_activate(launcher_t *launcher, int id) ;
extern void service_discard_tree(void) ;







#endif

