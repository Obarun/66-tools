/*
 * leader.c
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
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/pidfd.h>

#include <oblibs/io.h>
#include <oblibs/fd.h>
#include <oblibs/types.h>
#include <oblibs/string.h>

#include "leader.h"

int leader_starttime(pid_t pid, unsigned long *out)
{
    char fmt[PID_FMT] ;
    fmt[pid_format(fmt, pid)] = 0 ;

    char path[6 + PID_FMT + 5 + 1] ; /* "/proc/" + pid + "/stat" */
    auto_strings(path, "/proc/", fmt, "/stat") ;

    int fd = io_open(path, O_RDONLY | O_CLOEXEC) ;
    if (fd < 0)
        return errno == ENOENT ? 0 : -1 ;

    /** The line through field 22 (start time) is well under 512 bytes (comm is
     * capped at 16 chars and no later field is parenthesized), so a single fill
     * carries everything the parse below needs; trailing fields are irrelevant. */
    char buf[512] ;
    size_t got = io_allread(fd, buf, sizeof(buf) - 1) ;
    buf[got] = 0 ;
    close_fd(fd) ;

    char const *p = strrchr(buf, ')') ;
    if (!p)
        return (errno = EIO, -1) ;
    p++ ;

    /** Fields after comm are space-separated, 1-indexed: 1=state .. 20=starttime.
     * Skip the first 19, then scan the 20th. */
    unsigned int field = 0 ;
    for (; field < 19 ; field++) {

        while (*p == ' ') p++ ;
        if (!*p) return (errno = EIO, -1) ;
        while (*p && *p != ' ') p++ ;
    }

    while (*p == ' ') p++ ;

    uint64_t st ;
    if (!u64_scan(p, &st))
        return (errno = EIO, -1) ;

    *out = st ;

    return 1 ;
}

int leader_check(pid_t pid, unsigned long persisted, int *pidfd_out)
{
    *pidfd_out = -1 ;

    int fd = pidfd_open(pid, 0) ;
    if (fd < 0)
        return errno == ESRCH ? 0 : -1 ;

    unsigned long cur ;
    int r = leader_starttime(pid, &cur) ;
    if (r <= 0) {
        close_fd(fd) ;
        return r ;
    }

    if (cur != persisted) {
        close_fd(fd) ;
        return 0 ;
    }

    *pidfd_out = fd ;

    return 1 ;
}
