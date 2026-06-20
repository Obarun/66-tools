/*
 * launcher.h
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

#ifndef DBS_LAUNCHER_H_INCLUDE
#define DBS_LAUNCHER_H_INCLUDE

#include <stdint.h>
#include <sys/types.h>

#include <oblibs/sse.h>
#include <oblibs/hash.h>

#include "dbus.h"
#include "macro.h"

/** Avoid circular dependency with service.h header file*/
struct service_s ;

typedef struct launcher_s launcher_t, *launcher_t_ref ;
struct launcher_s
{
	int fd_dbus ; // fd of the dbus socket
	int fd_controller_in ; // fd to pass to the broker
	int fd_controller_out ; // fd to pass to the broker
	char machineid[MACHINEID + 2] ; // machine id string
	uid_t uid ; // uid of the owner of the process
	gid_t gid ; // gid of the owner of the process
	sd_bus *bus_controller ;
	sd_bus *bus_regular ;
	pid_t bpid ; // pid of the broker
	int sync[2] ; // synchronization between parent and child
	sse_epoll_t p ; // event loop
	sse_watcher_t wsignal ; // signal watcher (replaces selfpipe)
	sse_watcher_t wbus ; // controller bus io watcher
	int loopret ; // return code carried out of the event loop
	uint32_t nservice ; // counter for struct service_s -> id, never reset
	hash_t *hservice ;
} ;

extern launcher_t *launcher_free(launcher_t *launcher) ;

DBS_DEFINE_CLEANUP(launcher_t *, launcher_free) ;

extern int launcher_new(launcher_t_ref *launcher, hash_t *hservice, int socket) ;
extern int launcher_setup(launcher_t *launcher) ;
extern int launcher_run_broker(launcher_t *launcher) ;
extern int launcher_add_listener(launcher_t *launcher) ;
extern int launcher_on_message(sd_bus_message *m, void *userdata, sd_bus_error *error) ;
extern int launcher_on_reload_config(sd_bus_message *message, void *userdata, sd_bus_error *error) ;
extern void launcher_update_environment(launcher_t *launcher, sd_bus_message *m) ;
extern void launcher_get_machine_id(launcher_t *launcher) ;
extern int launcher_drop_permissions(launcher_t *launcher) ;
extern int launcher_run(launcher_t *launcher) ;
extern int launcher_fork(launcher_t *launcher) ;
extern int launcher_loop(launcher_t *launcher) ;
extern int launcher_connect(launcher_t *launcher) ;

#endif

