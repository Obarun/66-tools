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
#include <fcntl.h>
#include <dirent.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <stdint.h>
#include <linux/mount.h>

#include <oblibs/log.h>
#include <oblibs/sse.h>
#include <oblibs/io.h>
#include <oblibs/files.h>
#include <oblibs/string.h>
#include <oblibs/types.h>

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

/* Catchable control signals an admin/supervisor (e.g. s6-svc) may send, relayed
 * to the daemon at BOTH layers: the invoker forwards them to pid 1, pid 1
 * forwards them to the tracked main. Both must cover the same set, otherwise a
 * signal kills the layer that does not handle it (default disposition).
 * SIGKILL/SIGSTOP are uncatchable (SIGKILL is covered by the namespace teardown);
 * SIGCHLD is handled internally; fault signals are self-generated, not control. */
static int const ns_fwd_signals[] = {
    SIGTERM, SIGINT, SIGQUIT, SIGHUP,
    SIGUSR1, SIGUSR2, SIGALRM, SIGWINCH,
} ;
#define NS_FWD_N (sizeof(ns_fwd_signals) / sizeof(*ns_fwd_signals))

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

    for (size_t i = 0 ; i < NS_FWD_N ; i++)
        if (!sse_attach_signal(&sw, ns_fwd_signals[i]))
            log_dieusys(LOG_EXIT_SYS, "attach forward signal") ;

    if (!sse_start_child(&loop, &cw, on_child, &m, child, 0, true))
        log_dieusys(LOG_EXIT_SYS, "sse_start_child") ;

    if (!sse_poll(&loop, SSE_TIMEOUT_INFINITE) && !m.done)
        log_warnusys("event loop") ;

    sse_free(&loop) ;

    return m.code ;
}

// PID1 mini-init: reaper + signal forwarder + main follower

/* Create a private procfs bound to our pid namespace and return a dirfd to its
 * root. fsmount yields a detached mount (no mountpoint, never visible in the
 * tree) usable as a dirfd for openat/fdopendir -- exactly what we need to scan
 * our children without touching the user's /proc. */
int ns_proc_open(void)
{
    int fsfd = (int)syscall(__NR_fsopen, "proc", FSOPEN_CLOEXEC) ;
    if (fsfd < 0)
        return -1 ;

    int mfd = -1 ;
    if (syscall(__NR_fsconfig, fsfd, FSCONFIG_CMD_CREATE, (void *)0, (void *)0, 0) == 0)
        mfd = (int)syscall(__NR_fsmount, fsfd, FSMOUNT_CLOEXEC, 0) ;

    close(fsfd) ;
    return mfd ;
}

/* 1 if @pid is a live (non-zombie) direct child of pid 1, read from @dirfd
 * (the private procfs). 0 otherwise (gone, zombie, or not our direct child). */
static int ns_proc_is_child(int dirfd, pid_t pid)
{
    char pidstr[PID_FMT] ;
    pidstr[pid_format(pidstr, pid)] = 0 ;

    char rel[PID_FMT + sizeof("/status")] ;
    auto_strings(rel, pidstr, "/status") ;

    int fd = openat(dirfd, rel, O_RDONLY | O_CLOEXEC) ;
    if (fd < 0)
        return 0 ;

    char buf[1024] ;
    ssize_t r = io_read(fd, buf, sizeof(buf) - 1) ;
    close(fd) ;
    if (r <= 0)
        return 0 ;
    buf[r] = 0 ;

    // a zombie is about to be reaped, not a survivor (str_contain -> offset past "State:")
    int st = str_contain(buf, "State:") ;
    if (st >= 0) {
        char const *p = buf + st ;
        while (*p == ' ' || *p == '\t') p++ ;
        if (*p == 'Z')
            return 0 ;
    }

    int pp = str_contain(buf, "PPid:") ;
    if (pp < 0)
        return 0 ;

    char const *p = buf + pp ;
    while (*p == ' ' || *p == '\t') p++ ;

    pid_t ppid ;
    if (!pid_parse(p, &ppid))
        return 0 ;

    return ppid == 1 ;
}

/* Count our live direct children, capped at 2 (we only need 0 / 1 / >=2).
 * On return *@first holds the first survivor found (when the count is >= 1). */
static int ns_proc_count(int dirfd, pid_t *first)
{
    int fd = openat(dirfd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC) ;
    if (fd < 0)
        return 0 ;

    DIR *d = fdopendir(fd) ;
    if (!d) {
        close(fd) ;
        return 0 ;
    }

    int n = 0 ;
    struct dirent *e ;
    while (n < 2 && (e = readdir(d))) {

        if (e->d_name[0] < '1' || e->d_name[0] > '9')
            continue ; // ".", "..", and pids never start with 0

        pid_t pid ;
        if (!pid_parse(e->d_name, &pid))
            continue ;

        if (ns_proc_is_child(dirfd, pid)) {
            if (!n)
                *first = pid ;
            n++ ;
        }
    }

    closedir(d) ;
    return n ;
}

/* Read @path (interpreted inside the namespace), parse a pid, and return it only
 * if it is a live direct child of pid 1. -1 otherwise. */
static pid_t ns_pidfile_pick(char const *path, int dirfd)
{
    char buf[64] ;
    ssize_t r = file_read(path, buf, sizeof(buf) - 1) ;
    if (r <= 0)
        return -1 ;
    buf[r] = 0 ;

    char const *p = buf ;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++ ;

    pid_t pid ;
    if (!pid_parse(p, &pid) || pid <= 0)
        return -1 ;

    if (!ns_proc_is_child(dirfd, pid))
        return -1 ;

    return pid ;
}

/* The currently tracked main. File-scope so the async signal handler can read it
 * (a pid 1 is a singleton per process). Plain pid_t reads/writes are atomic on
 * Linux; the handler only ever reads it to pick a kill target. */
static volatile pid_t pid1_main = 0 ;

struct pid1_s {
    int          code ;       // tracked main's exit code (returned to the invoker)
    int          proc_dirfd ; // private procfs to inspect our children
    char const  *pidfile ;    // authoritative in-ns pidfile, or NULL (heuristic)
} ;

/* Forward a signal received from an ancestor down to the tracked main. Installed
 * with sigaction (not signalfd) for two reasons: oblibs' sse multiplexes a single
 * global signalfd, so it cannot host a second signal watcher beside SIGCHLD; and a
 * real handler (disposition != SIG_DFL) is what makes the kernel deliver an
 * ancestor's signal to a pid-namespace init at all (SIGNAL_UNKILLABLE otherwise
 * drops SIG_DFL signals from an ancestor). kill() is async-signal-safe. */
static void pid1_forward(int sig)
{
    pid_t m = pid1_main ;
    if (m > 0)
        kill(m, sig) ;
}

/* Reap everything (we are the namespace reaper). When the tracked main dies,
 * decide the service's fate. With a pidfile the named pid is authoritative: the
 * service is over once it no longer names a live child -- we never chase an
 * unnamed survivor. Without one, the sole surviving child is taken to be the
 * double-fork continuation; anything else means the service is gone. Returning
 * from pid 1 makes the kernel SIGKILL the whole namespace. */
static void pid1_reap(struct pid1_s *m, sse_epoll_t *loop)
{
    for (;;) {

        int wst ;
        pid_t pid = waitpid(-1, &wst, WNOHANG) ;
        if (pid <= 0)
            break ;

        if (pid != pid1_main)
            continue ; // a worker/orphan was reaped; keep draining

        m->code = ns_compute_exit(wst) ; // faithful exit code of the main

        if (m->pidfile) {
            // authoritative: follow the live child named by the pidfile, else stop
            pid_t h = ns_pidfile_pick(m->pidfile, m->proc_dirfd) ;
            if (h > 0) {
                pid1_main = h ;
                continue ;
            }
            loop->running = false ;
            break ;
        }

        // heuristic: a single survivor is the double-fork continuation
        pid_t first = -1 ;
        if (ns_proc_count(m->proc_dirfd, &first) == 1) {
            pid1_main = first ;
            continue ;
        }

        loop->running = false ;
        break ;
    }
}

static void on_pid1_sigchld(sse_watcher_t *w, void *data, int event)
{
    (void)event ;
    pid1_reap(data, w->p) ;
}

int ns_pid1(pid_t mainpid, int proc_dirfd, char const *pidfile)
{
    log_flow() ;

    /* die if the invoker disappears */
    if (prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0) != 0)
        log_dieusys(LOG_EXIT_SYS, "prctl PR_SET_PDEATHSIG") ;

    pid1_main = mainpid ;

    /* Give the forwarded signals a real handler (see ns_fwd_signals): a sigaction
     * disposition is what makes the kernel deliver an ancestor's signal to a
     * pid-namespace init at all, and the handler relays it to the tracked main. */
    struct sigaction sa ;
    sigemptyset(&sa.sa_mask) ;
    sa.sa_flags = SA_RESTART ;
    sa.sa_handler = pid1_forward ;
    for (size_t i = 0 ; i < NS_FWD_N ; i++)
        if (sigaction(ns_fwd_signals[i], &sa, 0) == -1)
            log_dieusys(LOG_EXIT_SYS, "sigaction") ;

    sse_epoll_t loop = SSE_EPOLL_ZERO ;
    sse_watcher_t cw = SSE_WATCHER_ZERO ;
    struct pid1_s m = { .code = LOG_EXIT_SYS, .proc_dirfd = proc_dirfd, .pidfile = pidfile } ;

    if (!sse_new(&loop, 8))
        log_dieusys(LOG_EXIT_SYS, "sse_new") ;

    if (!sse_start_signal(&loop, &cw, on_pid1_sigchld, &m, 0))
        log_dieusys(LOG_EXIT_SYS, "sse_start_signal") ;
    if (!sse_attach_signal(&cw, SIGCHLD))
        log_dieusys(LOG_EXIT_SYS, "attach signal: SIGCHLD") ;

    // catch a main that already died in the fork/arm window
    pid1_reap(&m, &loop) ;

    if (loop.running)
        if (!sse_poll(&loop, SSE_TIMEOUT_INFINITE))
            log_warnusys("event loop") ;

    sse_free(&loop) ;

    return m.code ;
}
