/*
 * ns_userns.c
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
 *
 * Rootless support: from the invoker, write the child's uid/gid maps for a
 * single-id mapping (invoking euid/egid -> inner id). setgroups must be denied
 * before gid_map for an unprivileged writer (user_namespaces(7)).
 */

#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>

#include <oblibs/log.h>
#include <oblibs/string.h>
#include <oblibs/types.h>

#include "ns.h"
#include "ns_internal.h"

static void write_proc(pid_t child, char const *file, char const *content)
{
    char path[sizeof("/proc//") + PID_FMT + 16] ;
    char pid[PID_FMT] ;
    pid[pid_format(pid, child)] = 0 ;
    auto_strings(path, "/proc/", pid, "/", file) ;

    int fd = open(path, O_WRONLY | O_CLOEXEC) ;
    if (fd == -1)
        log_dieusys(LOG_EXIT_SYS, "open: ", path) ;

    size_t len = 0 ;
    while (content[len]) len++ ;

    if (write(fd, content, len) != (ssize_t)len)
        log_dieusys(LOG_EXIT_SYS, "write: ", path) ;

    close(fd) ;
}

void ns_userns_write_maps(ns_t *ns, pid_t child)
{
    log_flow() ;

    char euid[UID_FMT], egid[GID_FMT], inu[UID_FMT], ing[GID_FMT] ;
    euid[uid_format(euid, ns->euid)] = 0 ;
    egid[gid_format(egid, ns->egid)] = 0 ;
    inu[uid_format(inu, ns->map_inner_uid)] = 0 ;
    ing[gid_format(ing, ns->map_inner_gid)] = 0 ;

    /* strict order: deny setgroups, then gid_map, then uid_map */
    write_proc(child, "setgroups", "deny") ;

    {
        char line[GID_FMT + 1 + GID_FMT + 3] ;
        auto_strings(line, ing, " ", egid, " 1") ;
        write_proc(child, "gid_map", line) ;
    }
    {
        char line[UID_FMT + 1 + UID_FMT + 3] ;
        auto_strings(line, inu, " ", euid, " 1") ;
        write_proc(child, "uid_map", line) ;
    }
}
