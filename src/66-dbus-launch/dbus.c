/*
 * dbus.c
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
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "dbus.h"
#include "launcher.h"
#include "service.h"

#include <oblibs/log.h>
#include <oblibs/io.h>
#include <oblibs/string.h>
#include <oblibs/stack.h>
#include <oblibs/socket.h>

#include <skalibs/types.h>

#include <66/config.h>

#include <66-tools/config.h>

const sd_bus_vtable launcher_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("ReloadConfig", NULL, NULL, launcher_on_reload_config, 0),
	SD_BUS_VTABLE_END
} ;

sd_bus *dbs_close_unref(sd_bus *bus)
{
	log_flow() ;
	/** It is not sufficient to simply call sd_bus_unref(), as messages
	* in the bus' queues may pin the bus itself. Also,
	* sd_bus_flush_close_unref() is not always appropriate as it would
	* block in poll waiting for messages to be flushed to the socket.
	*
	* In some cases all we really want to do is close the socket and
	* release all the memory, ignoring whether or not it has been
	* flushed to the kernel (typically in error paths). */
	if (!bus)
		return NULL ;

	sd_bus_flush(bus) ;
	//sd_bus_close(bus) ;

	return sd_bus_unref(bus) ;
}

void dbs_get_socket_path(char *path)
{
	log_flow() ;

	uid_t uid = getuid() ;

	if (!uid) {
		auto_strings(path, "/run/dbus/", SS_TOOLS_DBS_SYSTEM_NAME) ;
	} else {
		char ustr[UID_FMT] ;
		ustr[uid_fmt(ustr, uid)] = 0 ;
		auto_strings(path, "/run/user/", ustr, "/", SS_TOOLS_DBS_SESSION_NAME) ;
	}
}

int dbs_get_socket_unix_path(char *path)
{
	_alloc_stk_(s, SS_MAX_PATH) ;
	dbs_get_socket_path(s.s) ;
	s.len = strlen(s.s) ;
	if (!stack_insert(&s, 0, "unix:path="))
		log_warnusys_return(DBS_EXIT_FATAL, "stack insert") ;

	auto_strings(path, s.s) ;

	return 1 ;
}

int dbs_socket_bind(void)
{
	log_flow() ;

	_alloc_stk_(path, SS_MAX_PATH) ;
	dbs_get_socket_path(path.s) ;

	unlink(path.s) ;

	close(0) ;
	int fd = socket_open(SOCK_NONBLOCK|SOCK_CLOEXEC) ;
	if (fd < 0)
		log_dieusys(LOG_EXIT_SYS, "create socket") ;

	mode_t m = umask(0000) ;
	if (socket_bind(fd, path.s) < 0) {
		close(fd) ;
		log_dieusys(LOG_EXIT_SYS, "bind socket: ", path.s) ;
	}
	umask(m) ;

	if (socket_listen(fd, SOCK_BACKLOG) < 0) {
		close(fd) ;
		log_dieusys(LOG_EXIT_SYS, "listen socket: ", path.s) ;
	}

	return fd ;
}

int dbs_setenv_dbus_address(void)
{
	/** bus_set_address_user and bus_set_address_system rely
	 * on this environment variable*/

	uid_t uid = getuid() ;
	char *path = 0 ;
	_alloc_stk_(stk, SS_MAX_PATH) ;

	if (!uid) {

		path = getenv("DBUS_SYSTEM_BUS_ADDRESS") ;

		if (!path) {

			if (dbs_get_socket_unix_path(stk.s) < 0)
				log_warnusys_return(DBS_EXIT_FATAL, "get dbus socket path") ;

			if (setenv("DBUS_SYSTEM_BUS_ADDRESS", stk.s, 1) < 0)
				log_warnusys_return(DBS_EXIT_FATAL, "set DBUS_SYSTEM_BUS_ADDRESS=", stk.s, " environment variable") ;

			return 1 ;
		}

		if (setenv("DBUS_SYSTEM_BUS_ADDRESS", path, 1) < 0)
			log_warnusys_return(DBS_EXIT_FATAL, "set DBUS_SYSTEM_BUS_ADDRESS=", path, " environment variable") ;

	} else {

		path = getenv("DBUS_SESSION_BUS_ADDRESS") ;

		if (!path) {

			if (dbs_get_socket_unix_path(stk.s) < 0)
			log_warnusys_return(DBS_EXIT_FATAL, "get dbus socket path") ;

			if (setenv("DBUS_SESSION_BUS_ADDRESS", stk.s, 1) < 0)
				log_warnusys_return(DBS_EXIT_FATAL, "set DBUS_SESSION_BUS_ADDRESS=", stk.s, " environment variable") ;

			return 1 ;
		}

		if (setenv("DBUS_SESSION_BUS_ADDRESS", path, 1) < 0)
			log_warnusys_return(DBS_EXIT_FATAL, "set DBUS_SESSION_BUS_ADDRESS=", path, " environment variable") ;
	}

	return 1 ;
}