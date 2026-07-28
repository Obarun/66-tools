/*
 * runtimedir.c
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
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <oblibs/log.h>
#include <oblibs/string.h>
#include <oblibs/types.h>
#include <oblibs/directory.h>

#include <66-tools/config.h>
#include "userd.h"
#include "runtimedir.h"

static void runtime_path(char *path, char const *base, uid_t uid)
{
    char uidstr[UID_FMT] ;
    uidstr[uid_format(uidstr, uid)] = 0 ;
    auto_strings(path, base, "/", uidstr) ;
}

static int runtimedir_up(uid_t uid, gid_t gid, char const *base)
{
    char path[strlen(base) + 1 + UID_FMT] ;
    size_t olen = strlen("mode=0700,uid=,gid=,size=") ;

    runtime_path(path, base, uid) ;

    if (mkdir(path, 0700) < 0 && errno != EEXIST) {
        log_warnusys("create runtime dir ", path) ;
        return 0 ;
    }

    char suid[UID_FMT] ;
    suid[uid_format(suid, uid)] = 0 ;
    char sgid[GID_FMT] ;
    sgid[gid_format(sgid, gid)] = 0 ;

    char opts[olen + UID_FMT + GID_FMT + sizeof(SS_TOOLS_USERD_RUNTIME_SIZE) + 1] ;
    auto_strings(opts, "mode=0700,uid=", suid, ",gid=", sgid, ",size=", SS_TOOLS_USERD_RUNTIME_SIZE) ;

    if (mount("tmpfs", path, "tmpfs", MS_NOSUID | MS_NODEV, opts) < 0)
        log_warnusys_return(LOG_EXIT_ZERO, "mount tmpfs on ", path) ;

    return 1 ;
}

static int runtimedir_down(uid_t uid, char const *base)
{
    char path[strlen(base) + 1 + UID_FMT] ;

    runtime_path(path, base, uid) ;

    if (umount2(path, 0) < 0) {
        if (errno != EBUSY) {
            log_warnusys("unmount runtime dir ", path) ;
            return 0 ;
        }
        /** Busy (an open fd or a sub-mount still holds it): detach lazily so the
         * dir leaves the namespace now and the kernel reaps it once idle. */
        log_warn("runtime dir ", path, " busy on unmount; detaching lazily") ;
        if (umount2(path, MNT_DETACH) < 0) {
            log_warnusys("lazy-detach runtime dir ", path) ;
            return 0 ;
        }
    }

    if (dir_destroy(path) < 0) {
        log_warnusys("remove runtime dir ", path) ;
        return 0 ;
    }

    return 1 ;
}

int runtimedir_register(user_t *u, char const *base)
{
    if (u->nsessions != 1)
        return 1 ;

    if (u->runtimedir_up)
        return 1 ;

    if (!runtimedir_up(u->uid, u->gid, base))
        log_warn_return(LOG_EXIT_ZERO, "runtime dir mount failed for user ", u->name) ;

    u->runtimedir_up = 1 ;

    return 1 ;
}

int runtimedir_release(user_t *u, char const *base)
{
    if (u->nsessions != 0)
        return 1 ;

    if (!u->runtimedir_up)
        return 1 ;

    if (!runtimedir_down(u->uid, base))
        log_warn_return(LOG_EXIT_ZERO, "runtime dir unmount failed for user ", u->name) ;

    u->runtimedir_up = 0 ;
    return 1 ;
}

int runtimedir_ismount(uid_t uid, char const *base)
{
    char path[strlen(base) + 1 + UID_FMT] ;
    runtime_path(path, base, uid) ;

    struct stat self ;
    if (lstat(path, &self) < 0) {
        if (errno == ENOENT)
            return 0 ; // no dir -> not mounted
        return -1 ;
    }

    struct stat parent ;
    if (lstat(base, &parent) < 0)
        return -1 ;

    return self.st_dev != parent.st_dev ? 1 : 0 ;
}
