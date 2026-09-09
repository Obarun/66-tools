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

#include <oblibs/strbuf.h>

#include <66-tools/config.h>

#include "odbus.h"

#define MACHINEID 32 // https://www.freedesktop.org/software/systemd/man/latest/machine-id.html

#define DBS_DBUS_CALL_TIMEOUT_MS 30000

extern const odbus_method launcher_methods[] ;
extern void dbs_get_socket_path(char *store) ;
extern int dbs_get_socket_unix_path(strbuf *store) ;
extern int dbs_socket_bind(void) ;
extern int dbs_setenv_dbus_address(void) ;

#endif

