/*
 * ns_run.c
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
 * Core orchestration: prepare the layout, clone the requested namespaces,
 * hand-shake with the child (writing its uid/gid maps when rootless), build
 * the mount namespace in the child, optionally become pid 1, then drop
 * privileges and exec the program.
 */

#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <sys/mount.h>
#include <signal.h>
#include <stdint.h>

#include <oblibs/log.h>
#include <oblibs/io.h>
#include <oblibs/strbuf.h>
#include <oblibs/genbuf.h>
#include <oblibs/types.h>
#include <oblibs/exec.h>

#include "ns.h"
#include "ns_internal.h"

char const *const ns_type_str[NS_TYPE_ENDOFKEY] = {
    [NS_TYPE_BIND]      = "bind",
    [NS_TYPE_TMPFS]     = "tmpfs",
    [NS_TYPE_HIDDEN]    = "hidden",
    [NS_TYPE_RECURSIVE] = "recursive",
    [NS_TYPE_CLONE]     = "clone",
    [NS_TYPE_PROC]      = "proc",
    [NS_TYPE_DEV]       = "dev",
    [NS_TYPE_SYS]       = "sys"
} ;

int ns_type_by_name(char const *name)
{
    for (int i = 0 ; i < NS_TYPE_ENDOFKEY ; i++)
        if (ns_type_str[i] && !strcmp(ns_type_str[i], name))
            return i ;

    return -1 ;
}

size_t ns_arena_add(ns_t *ns, char const *s)
{
    size_t off = ns->sb.len ;
    if (!strbuf_catb(&ns->sb, s, strlen(s) + 1))
        log_die_nomem("arena") ;

    return off ;
}

void ns_free(ns_t *ns)
{
    genbuf_free(&ns->entries) ;
    strbuf_free(&ns->sb) ;
    genbuf_free(&ns->mntinfo) ;
    strbuf_free(&ns->mntsb) ;
    strbuf_free(&ns->paths) ;
}

static void notify_ready(int *fd)
{
    if (*fd >= 0) {
        if (io_write(*fd, (char *)"\n", 1) < 0)
            log_warnusys("notify readiness") ;
        close(*fd) ;
        *fd = -1 ;
    }
}

/* The child path: wait the gate, build the namespace, optionally become pid 1,
 * drop privileges, then exec. Never returns on success. */
static int ns_child(ns_t *ns)
{
    // gate: block until the invoker has written our maps (rootless) / is ready
    uint64_t v ;
    if (io_read(ns->ef_ready_p2c, (char *)&v, 8) != 8)
        log_dieusys(LOG_EXIT_SYS, "read readiness gate") ;
    close(ns->ef_ready_p2c) ;
    ns->ef_ready_p2c = -1 ;

    mode_t omask = umask(0) ;
    ns_setup_ns(ns) ;

    if (ns->hostname >= 0)
        if (sethostname(ns->sb.s + ns->hostname, strlen(ns->sb.s + ns->hostname)) == -1)
            log_dieusys(LOG_EXIT_SYS, "set hostname: ", ns->sb.s + ns->hostname) ;

    // a fresh netns has 'lo' down with no address; bring it up so 127.0.0.1 works
    if (FLAGS_ISSET(ns->clone_flags, CLONE_NEWNET))
        ns_loopback_up() ;

    umask(omask) ;

    if (ns->want_pid1) {

        pid_t g = fork() ;

        if (g == -1)
            log_dieusys(LOG_EXIT_SYS, "fork pid 1") ;

        if (g)
            return ns_pid1(g) ;

        // grandchild falls through to exec
    }

    // late, in order: no-new-privs, then new session
    if (ns->no_new_privs)
        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0)
            log_dieusys(LOG_EXIT_SYS, "set no_new_privs") ;

    ns_session_setup(ns) ;

    // die with the supervising parent
    if (prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0) != 0)
        log_dieusys(LOG_EXIT_SYS, "prctl PR_SET_PDEATHSIG") ;

    notify_ready(&ns->notif_fd) ;

    exec_path_die(ns->prog[0], ns->prog, ns->envp) ;

    return LOG_EXIT_SYS ; // unreachable
}

int ns_run(ns_t *ns)
{
    log_flow() ;

    // CLONE_NEWNS is always implied; SIGCHLD so the invoker can wait()
    ns->clone_flags |= (unsigned long)SIGCHLD | CLONE_NEWNS ;

    if (!ns->root_propagation)
        ns->root_propagation = MS_SHARED ;

    if (ns->rootless) {

        ns->euid = geteuid() ;
        ns->egid = getegid() ;

    } else if (geteuid()) {

        errno = EPERM ;
        log_diesys(LOG_EXIT_USER, "you must be root, or request a user namespace with -o unshare=user") ;
    }

    if (ns->notif_fd >= 0 && fcntl(ns->notif_fd, F_GETFD) < 0)
        log_diesys(LOG_EXIT_USER, "invalid notification fd") ;

    ns_dir_setup(ns) ;
    ns_mntinfo_init(ns) ;

    ns->ef_ready_p2c = eventfd(0, EFD_CLOEXEC) ;
    if (ns->ef_ready_p2c == -1)
        log_dieusys(LOG_EXIT_SYS, "eventfd") ;

    pid_t pid = ns_raw_clone(ns->clone_flags) ;
    if (pid == -1)
        log_dieusys(LOG_EXIT_SYS, "clone") ;

    if (pid > 0) {
        // invoker
        ns->child_pid = pid ;

        if (ns->rootless)
            ns_userns_write_maps(ns, pid) ;

        uint64_t one = 1 ;
        if (io_write(ns->ef_ready_p2c, (char *)&one, 8) != 8)
            log_dieusys(LOG_EXIT_SYS, "release readiness gate") ;
        close(ns->ef_ready_p2c) ;
        ns->ef_ready_p2c = -1 ;

        int rc = ns_supervise(pid) ;
        // the scratch tmpfs lived in the child's ns; the host mountpoint is now
        // an empty dir -- best-effort removal
        if (ns->base_dir)
            rmdir(ns->base_dir) ;

        return rc ;
    }

    return ns_child(ns) ;
}
