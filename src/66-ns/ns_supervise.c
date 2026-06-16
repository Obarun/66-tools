/*
 * ns_supervise.c
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
 * Process plumbing: raw clone, the invoker-side supervisor (sse event loop on
 * a pidfd + signalfd), and the PID1 reaper for a new pid namespace.
 */

#include <errno.h>
#include <signal.h>
#include <sched.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <stdint.h>

#include <oblibs/log.h>
#include <oblibs/sse.h>

#include "ns.h"
#include "ns_internal.h"

int ns_raw_clone(unsigned long flags)
{
#if defined(__s390__) || defined(__CRIS__)
    return (int)syscall(__NR_clone, 0UL, flags) ;
#else
    return (int)syscall(__NR_clone, flags, 0UL) ;
#endif
}

int ns_compute_exit(int wstat)
{
    if (WIFEXITED(wstat))
        return WEXITSTATUS(wstat) ;
    if (WIFSIGNALED(wstat))
        return 128 + WTERMSIG(wstat) ;
    return LOG_EXIT_SYS ;
}

// invoker supervisor

struct monitor_s {
    int   code ;   // exit code to return
    int   done ;   // child has exited and been collected
    pid_t child ;  // the process to forward signals to
} ;

static void on_child(sse_watcher_t *w, void *data, int event)
{
    (void)event ;
    struct monitor_s *m = data ;
    sse_child_t *c = w->sdata ;

    if (c && (WIFEXITED(c->status) || WIFSIGNALED(c->status))) {
        m->code = ns_compute_exit(c->status) ;
        m->done = 1 ;
        w->p->running = false ;
    }
}

static void on_signal(sse_watcher_t *w, void *data, int event)
{
    (void)event ;
    struct monitor_s *m = data ;
    sse_signal_t *s = w->sdata ;

    if (!s)
        return ;

    int sig = (int)s->si.ssi_signo ;
    if (sig == SIGCHLD)
        return ; // the child watcher handles exit/reaping

    // forward the signal into the sandbox
    kill(m->child, sig) ;
}

int ns_supervise(pid_t child)
{
    log_flow() ;

    sse_epoll_t loop = SSE_EPOLL_ZERO ;
    sse_watcher_t cw = SSE_WATCHER_ZERO, sw = SSE_WATCHER_ZERO ;
    struct monitor_s m = { .code = LOG_EXIT_SYS, .done = 0, .child = child } ;

    if (!sse_new(&loop, 8))
        log_dieusys(LOG_EXIT_SYS, "sse_new") ;

    if (!sse_start_signal(&loop, &sw, on_signal, &m, 0))
        log_dieusys(LOG_EXIT_SYS, "sse_start_signal") ;

    if (!sse_attach_signal(&sw, SIGTERM))
        log_dieusys(LOG_EXIT_SYS, "attach signal: SIGTERM") ;

    if (!sse_attach_signal(&sw, SIGINT))
        log_dieusys(LOG_EXIT_SYS, "attach signal: SIGINT") ;

    if (!sse_attach_signal(&sw, SIGQUIT))
        log_dieusys(LOG_EXIT_SYS, "attach signal: SIGQUIT") ;

    if (!sse_attach_signal(&sw, SIGHUP))
        log_dieusys(LOG_EXIT_SYS, "attach signal: SIGHUP") ;

    if (!sse_start_child(&loop, &cw, on_child, &m, child, 0, true))
        log_dieusys(LOG_EXIT_SYS, "sse_start_child") ;

    if (!sse_poll(&loop, SSE_TIMEOUT_INFINITE) && !m.done)
        log_warnusys("event loop") ;

    sse_free(&loop) ;

    return m.code ;
}

// PID1 reaper

int ns_pid1(pid_t grandchild)
{
    /* die if the invoker disappears */
    if (prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0) != 0)
        log_dieusys(LOG_EXIT_SYS, "prctl PR_SET_PDEATHSIG") ;

    int code = LOG_EXIT_SYS ;

    for (;;) {

        int wstat ;
        pid_t c = wait(&wstat) ;

        if (c == grandchild)
            code = ns_compute_exit(wstat) ;

        if (c == -1) {

            if (errno == EINTR)
                continue ;

            if (errno != ECHILD)
                log_dieusys(LOG_EXIT_SYS, "wait") ;

            break ;
        }
    }
    return code ;
}
