/*
 * userenv.c
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
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <oblibs/string.h>
#include <oblibs/types.h>

#include <66-tools/config.h>
#include "userenv.h"

int userenv_build(uid_t uid, char const *runtime_dir)
{
    errno = 0 ;
    struct passwd *pw = getpwuid(uid) ;
    if (!pw)
        return (errno = errno ? errno : ESRCH, 0) ;

    char uidstr[UID_FMT] ;
    uidstr[uid_format(uidstr, uid)] = 0 ;
    char gidstr[GID_FMT] ;
    gidstr[gid_format(gidstr, pw->pw_gid)] = 0 ;

    if (setenv("HOME", pw->pw_dir, 1) < 0 ||
        setenv("USER", pw->pw_name, 1) < 0 ||
        setenv("LOGNAME", pw->pw_name, 1) < 0 ||
        setenv("SHELL", pw->pw_shell, 1) < 0 ||
        setenv("UID", uidstr, 1) < 0 ||
        setenv("GID", gidstr, 1) < 0 ||
        setenv("XDG_RUNTIME_DIR", runtime_dir, 1) < 0)
        return 0 ;

    if (!getenv("PATH"))
        if (setenv("PATH", SS_TOOLS_USERD_DEFAULT_PATH, 1) < 0)
            return 0 ;

    char dbus[sizeof(SS_TOOLS_USERD_DBUS_ADDR_PREFIX) + strlen(runtime_dir) + 4] ;
    auto_strings(dbus, SS_TOOLS_USERD_DBUS_ADDR_PREFIX, runtime_dir, "/bus") ;

    if (setenv("DBUS_SESSION_BUS_ADDRESS", dbus, 1) < 0)
        return 0 ;

    return 1 ;
}
