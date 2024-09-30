/*
 * dbus.h
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

#ifndef DBS_DBUS_H_INCLUDE
#define DBS_DBUS_H_INCLUDE

#include <66-tools/config.h>

#ifdef SS_TOOLS_USE_BASU
#include <basu/sd-bus.h>
#include <basu/sd-bus-vtable.h>
#else
#ifdef SS_TOOLS_USE_ELOGIND
#include <elogind/sd-bus.h>
#include <elogind/sd-bus-vtable.h>
#else

#error No sd_bus backend configured

#endif
#endif

#define MACHINEID 32 // https://www.freedesktop.org/software/systemd/man/latest/machine-id.html

extern const sd_bus_vtable launcher_vtable[] ;
extern sd_bus *dbs_close_unref(sd_bus *bus) ;
extern int dbs_method_reload_config(sd_bus_message *message, void *userdata, sd_bus_error *error) ;
extern void dbs_get_socket_path(char *store) ;
extern int dbs_get_socket_unix_path(char *store) ;
extern int dbs_socket_bind(void) ;
extern int dbs_setenv_dbus_address(void) ;

#endif

