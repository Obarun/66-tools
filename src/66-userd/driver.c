/*
 * driver.c
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

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/pidfd.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <oblibs/directory.h>
#include <oblibs/fd.h>
#include <oblibs/io.h>
#include <oblibs/log.h>
#include <oblibs/opt.h>
#include <oblibs/process.h>
#include <oblibs/string.h>
#include <oblibs/types.h>
#include <oblibs/strbuf.h>

#include <66/constants.h>
#include <66/ssexec.h>
#include <66/utils.h>
#include <66/svc.h>

#include <66-tools/config.h>
#include "constants.h"
#include "userd.h"
#include "driver.h"
#include "userenv.h"

/** 66 exposes the scandir/tree signal handlers only as linkable symbols, not in a
 * public header. Mirror ssexec_scandir_wrapper.c / ssexec_tree_wrapper.c: set the
 * signal name (and options through the on_* applier), then call the ssexec_*
 * handler, which reads that state — it does not parse argv. */
extern opt_on_option_fn on_scandir_signal ;
extern opt_cmd_fn do_scandir_start ;
extern opt_cmd_fn do_scandir_quit ;
extern opt_cmd_fn do_tree_start ;
extern opt_cmd_fn do_tree_stop ;

#define GUARDIAN_QUIT_TIMEOUT_MS 5000

int driver_register(user_t *u, int readyfd)
{
    if (u->nsessions != 1)
        return 1 ;

    if (u->scandir_up)
        return 1 ;

    pid_t pid = driver_guardian_spawn(u->uid, readyfd) ;
    if (pid <= 0)
        log_warn_return(LOG_EXIT_ZERO, "66 first-start failed for user ", u->name) ;

    u->guardian_pid = pid ;
    u->scandir_up = 1 ;
    u->state = USER_STATE_OPENING ;

    return 1 ;
}

int driver_release(user_t *u)
{
    if (u->nsessions != 0)
        return 1 ;

    if (!u->scandir_up)
        return 1 ;

    if (!driver_guardian_stop(u->guardian_pid))
        log_warn_return(LOG_EXIT_ZERO, "66 last-stop failed for user ", u->name) ;

    u->state = USER_STATE_CLOSING ;
    return 1 ;
}

static void driver_drop_priv(uid_t uid)
{
    struct passwd *pw = getpwuid(uid) ;
    if (!pw) {

        if (!errno)
            errno = ESRCH ;

        log_dieusys(LOG_EXIT_SYS, "look up uid for privilege drop") ;
    }

    if (initgroups(pw->pw_name, pw->pw_gid) < 0)
        log_dieusys(LOG_EXIT_SYS, "initgroups") ;

    if (setgid(pw->pw_gid) < 0)
        log_dieusys(LOG_EXIT_SYS, "setgid") ;

    if (setuid(uid) < 0)
        log_dieusys(LOG_EXIT_SYS, "setuid") ;
}

int driver_scandir_ok(uid_t uid)
{
    _cleanup_strbuf_ strbuf scandir = STRBUF_ZERO ;

    int r = set_livescan(&scandir, uid) ;
    if (r <= 0)
        return -1 ;

    return svc_scandir_ok(scandir.s) ;
}

static int fd_accmode(char const *piddir, char const *fdname)
{
    size_t plen = strlen(piddir), nlen = strlen(fdname), flen = 6 ;
    char path[ plen + 8 + nlen + 1] ;
    auto_strings(path, piddir, "/fdinfo/", fdname) ;

    int fd = io_open(path, O_RDONLY | O_CLOEXEC) ;
    if (fd < 0)
        return -1 ;

    char buf[256] ;
    size_t s = io_allread(fd, buf, sizeof(buf) - 1) ;
    buf[s] = 0 ;
    close_fd(fd) ;

    char const *p = strstr(buf, "flags:") ;
    if (!p)
        return -1 ;

    p += flen ;
    while (*p == ' ' || *p == '\t') p++ ;

    uint32_t flags ;
    if (!u32_oscan(p, &flags))
        return -1 ;

    return (int)(flags & O_ACCMODE) ;
}

static int proc_holds_reader(char const *piddir, struct stat const *want)
{
    size_t plen = strlen(piddir) ;
    char fddir[plen + 4] ;
    auto_strings(fddir, piddir, "/fd") ;

    DIR *dir = opendir(fddir) ;
    if (!dir)
        return 0 ;

    int hit = 0, err = 0 ;
    struct dirent *d ;
    errno = 0 ;

    while (!hit && (d = readdir(dir))) {

        if (d->d_name[0] == '.')
            continue ;

        size_t flen = strlen(fddir), nlen = strlen(d->d_name) ;
        char path[flen + 1 + nlen + 1] ;
        auto_strings(path, fddir, "/", d->d_name) ;

        struct stat st ;
        if (stat(path, &st) < 0) {
            errno = 0 ;
            continue ; // raced with a close
        }

        if (st.st_dev != want->st_dev || st.st_ino != want->st_ino)
            continue ;

        if (fd_accmode(piddir, d->d_name) != O_WRONLY)
            hit = 1 ;

        errno = 0 ;
    }

    if (!hit && errno)
        err = errno ;

    dir_close(dir) ;

    if (err) {
        errno = err ;
        hit = 0 ;
    }

    return hit ;
}

int driver_scandir_pidfd(uid_t uid, pid_t *pid_out, int *pidfd_out)
{
    *pid_out = 0 ;
    *pidfd_out = -1 ;

    _cleanup_strbuf_ strbuf scandir = STRBUF_ZERO ;

    if (set_livescan(&scandir, uid) <= 0)
        return (errno = ENOMEM, -1) ;

    char fifo[scandir.len + SS_SVSCAN_LEN + USERD_CONTROL_LEN + 1] ;
    auto_strings(fifo, scandir.s, SS_SVSCAN, USERD_CONTROL) ;

    struct stat want ;
    if (stat(fifo, &want) < 0)
        return errno == ENOENT ? 0 : -1 ;

    DIR *proc = opendir("/proc") ;
    if (!proc)
        return -1 ;

    int found = 0, err = 0 ;
    struct dirent *d ;
    errno = 0 ;

    while (!found && (d = readdir(proc))) {

        uint32_t pid ;
        if (!u32_scan_strict(d->d_name, &pid) || !pid) {
            errno = 0 ;
            continue ;
        }

        char piddir[6 + PID_FMT + 1] ;
        auto_strings(piddir, "/proc/", d->d_name) ;

        struct stat pst ;
        if (stat(piddir, &pst) < 0 || pst.st_uid != uid) {
            errno = 0 ;
            continue ;
        }

        if (!proc_holds_reader(piddir, &want)) {
            errno = 0 ;
            continue ; // not it, or could not tell: either way not a candidate
        }

        int fd = pidfd_open((pid_t)pid, 0) ;
        if (fd < 0) {
            errno = 0 ;
            continue ; // died between the scan and here
        }

        // be paranoid about race condition
        if (!proc_holds_reader(piddir, &want) || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
            close_fd(fd) ;
            errno = 0 ;
            continue ;
        }

        *pid_out = (pid_t)pid ;
        *pidfd_out = fd ;
        found = 1 ;
    }

    if (!found && errno)
        err = errno ;

    dir_close(proc) ;

    if (err)
        return (errno = err, -1) ;

    return found ;
}

static void build_ssexec(ssexec_t *info, uid_t uid)
{
    info->owner = uid ;
    info->ownerlen = uid_format(info->ownerstr, uid) ;
    info->ownerstr[info->ownerlen] = 0 ;
    info->skip_opt_tree = 1 ;

    if (!set_ownersysdir(&info->base, uid))
        log_dieusys(LOG_EXIT_SYS, "set owner system directory") ;

    set_info(info) ;
}

static void prepare_exec(uid_t uid, ssexec_t *info)
{
    driver_drop_priv(uid) ;
    build_ssexec(info, uid) ;
}

static void driver_scandir_up(uid_t uid, int notif_wfd)
{
    if (fcntl(notif_wfd, F_SETFD, 0) < 0)
        log_dieusys(LOG_EXIT_SYS, "clear close-on-exec on notification fd") ;

    ssexec_t info = SSEXEC_ZERO ;

    prepare_exec(uid, &info) ;

    char snotif[U32_FMT] ;
    snotif[u32_fmt(snotif, (unsigned int)notif_wfd)] = 0 ;

    on_scandir_signal('d', snotif, &info) ;
    do_scandir_start(0, 0, &info) ;

    flog_die(LOG_EXIT_SYS, "scandir start returned for user %u without exec'ing -- a scandir may have appeared since the probe", uid) ;
}

static void driver_trees_up(uid_t uid)
{
    ssexec_t info = SSEXEC_ZERO ;
    prepare_exec(uid, &info) ;
    // no operand: bring up every tree of the owner.
    _exit(do_tree_start(0, 0, &info)) ;
}

static void driver_trees_down(uid_t uid)
{
    ssexec_t info = SSEXEC_ZERO ;
    prepare_exec(uid, &info) ;
    _exit(do_tree_stop(0, 0, &info)) ;
}

static void driver_scandir_down(uid_t uid)
{
    ssexec_t info = SSEXEC_ZERO ;
    prepare_exec(uid, &info) ;
    _exit(do_scandir_quit(0, 0, &info)) ;
}

static void guardian_sigunblock(int sig, sigset_t *ssig)
{
    sigset_t s ;
    sigset_t *rs = &s ;
    if (ssig)
        rs = ssig ;
    sigemptyset(rs) ;
    sigaddset(rs, sig) ;
    sigprocmask(SIG_UNBLOCK, rs, 0) ;
}

static void guardian_sigblock(int sig, sigset_t *ssig)
{
    sigset_t s ;
    sigset_t *rs = &s ;
    if (ssig)
        rs = ssig ;
    sigemptyset(rs) ;
    sigaddset(rs, sig) ;
    sigprocmask(SIG_BLOCK, rs, 0) ;
}

static void guardian_run_child(void (*action)(uid_t uid), uid_t uid, char const *what)
{
    pid_t pid = fork() ;
    if (pid < 0) {
        log_warnusys("fork guardian for ", what) ;
        return ;
    }

    if (!pid) {
        guardian_sigunblock(SIGTERM, 0) ;
        action(uid) ; // never returns
        flog_die(LOG_EXIT_SYS, "action %s returned for user %u", what, uid) ;
    }

    int wstat ;
    process_wait(pid, &wstat) ;

    if (!WIFEXITED(wstat) || WEXITSTATUS(wstat))
        flog_warn("guardian %s failed for user %u", what, uid) ;
}

static void guardian_main(uid_t uid, int readyfd)
{
    // Detach from the daemon's controlling context (no controlling tty).
    if (setsid() < 0)
        log_dieusys(LOG_EXIT_SYS, "setsid") ;

    /** Block SIGTERM and read it via a signalfd instead of a handler, so a stop
     * request is delivered as a pollable fd event — never lost to a race. */
    sigset_t mask ;
    guardian_sigblock(SIGTERM, &mask) ;

    int sigfd = signalfd(-1, &mask, SFD_CLOEXEC) ;
    if (sigfd < 0)
        log_dieusys(LOG_EXIT_SYS, "signalfd") ;

    char uidstr[UID_FMT] ;
    uidstr[uid_format(uidstr, uid)] = 0 ;
    char runtime_dir[sizeof(SS_TOOLS_USERD_RUNTIME_BASE) + 1 + UID_FMT] ;
    auto_strings(runtime_dir, SS_TOOLS_USERD_RUNTIME_BASE, "/", uidstr) ;

    if (!userenv_build(uid, runtime_dir))
        log_dieusys(LOG_EXIT_SYS, "build user environment for uid: ", uidstr) ;

    pid_t scandir = 0 ;
    int svfd = -1, adopted = 0 ;

    int up = driver_scandir_ok(uid) ;
    if (up < 0)
        log_dieusys(LOG_EXIT_SYS, "probe the scandir of uid: ", uidstr) ;

    if (up) {

        int r = driver_scandir_pidfd(uid, &scandir, &svfd) ;
        if (r < 0)
            log_dieusys(LOG_EXIT_SYS, "look up the running scandir of uid: ", uidstr) ;

        if (!r)
            flog_die(LOG_EXIT_SYS, "scandir of user %u is up but no process of that user reads its control fifo -- refusing to drive a scandir that cannot be supervised", uid) ;

        adopted = 1 ;
        flog_trace("adopted running scandir (pid %u) of user %u -- this guardian did not start it", (unsigned int)scandir, uid) ;
    }

    if (!adopted) {

        int p[2] ;
        if (pipe2(p, O_CLOEXEC) < 0)
            log_dieusys(LOG_EXIT_SYS, "pipe2") ;

        scandir = fork() ;
        if (scandir < 0)
            log_dieusys(LOG_EXIT_SYS, "fork scandir") ;

        if (!scandir) {
            close(p[0]) ;
            guardian_sigunblock(SIGTERM, 0) ;
            driver_scandir_up(uid, p[1]) ; // becomes 66-scandir; never returns
            flog_die(LOG_EXIT_SYS, "scandir child returned for user %u", uid) ;
        }
        close(p[1]) ;

        svfd = pidfd_open(scandir, 0) ;
        if (svfd < 0 || fcntl(svfd, F_SETFD, FD_CLOEXEC) < 0) {
            log_warnusys("pidfd_open on scandir") ;
            if (svfd >= 0)
                close(svfd) ;
            close(p[0]) ;
            close(sigfd) ;
            kill(scandir, SIGKILL) ;
            process_wait(scandir, 0) ;
            _exit(LOG_EXIT_SYS) ;
        }

        /** bring-up: gate on the readiness byte, but also watch the scandir dying
         * before it notifies (EOF / pidfd) and a stop arriving mid-bring-up. */
        struct pollfd pfd[3] = {
            { p[0], POLLIN, 0 }, // readiness pipe
            { svfd, POLLIN, 0 }, // scandir death
            { sigfd, POLLIN, 0 }, // stop request
        } ;

        int ready = 0, stop = 0 ;

        while (!ready && !stop) {

            if (poll(pfd, 3, -1) < 0) {
                if (errno == EINTR)
                    continue ;
                break ;
            }

            if (pfd[0].revents & POLLIN) {

                char buf[16] ;
                ssize_t r = io_read(p[0], buf, sizeof buf) ;
                if (r > 0 && memchr(buf, '\n', (size_t)r)) {
                    ready = 1 ;
                } else if (r <= 0)
                    break ; // EOF/error: scandir failed to notify

            } else if (pfd[0].revents & POLLHUP)
                break ; // writer gone: scandir failed

            if (!ready && (pfd[1].revents & (POLLIN | POLLHUP)))
                break ; // scandir died before readiness

            if (pfd[2].revents & POLLIN)
                stop = 1 ;
        }
        close(p[0]) ;

        if (!ready) {
            /** Failed to come up, or stop requested before readiness: make sure the
             * scandir is gone, then exit. The daemon's watcher fires either way. */
            if (stop) log_info("stop for user: ", uidstr, " before readiness") ;
            else log_warn("scandir for user: ", uidstr, " failed to signal readiness") ;
            kill(scandir, SIGKILL) ; // SIGKILL: the blocking reap below must not hang on a wedged scandir
            process_wait(scandir, 0) ;
            close(svfd) ;
            close(sigfd) ;
            _exit(stop ? 0 : LOG_EXIT_SYS) ;
        }
    }

    // supervisor up: start the user's enabled trees, then supervise.
    log_info("scandir up for user: ", uidstr, "; starting trees") ;
    guardian_run_child(driver_trees_up, uid, "tree start") ;

    /** Trees are up (the start above is synchronous), so tell the daemon the user is
     * fully online, not merely "scandir reached readiness". The eventfd has no peer
     * and no EOF, so this write can never raise SIGPIPE — no signal disposition is
     * touched. A write failure is not fatal: the user just stays OPENING until a
     * reconcile re-probes the scandir. */
    if (readyfd >= 0) {
        uint64_t one = 1 ;
        if (write(readyfd, &one, sizeof one) != sizeof one)
            log_warnusys("notify readiness to the daemon") ;
        close(readyfd) ;
    }

    struct pollfd sup[2] = {
        { svfd, POLLIN, 0 }, // scandir death
        { sigfd, POLLIN, 0 }, // stop request
    } ;

    for (;;) {

        if (poll(sup, 2, -1) < 0) {
            if (errno == EINTR)
                continue ;
            break ;
        }
        if (sup[1].revents & POLLIN) {
            /** Stop requested: tear down trees BEFORE the scandir (they need it
             * up), then quit the scandir and reap it. */
            log_info("stopping user: ", uidstr) ;
            guardian_run_child(driver_trees_down, uid, "tree stop") ;
            guardian_run_child(driver_scandir_down, uid, "scandir quit") ;

            /** Bound the wait on the pidfd: "scandir quit" should make 66-scandir exit,
             * but a wedged directory must not freeze the guardian (SIGTERM is consumed,
             * the poll loop is gone). On timeout or poll error, SIGKILL guarantees the
             * reap below returns. */
            struct pollfd q = { svfd, POLLIN, 0 } ;
            if (poll(&q, 1, GUARDIAN_QUIT_TIMEOUT_MS) <= 0) {
                log_warn("scandir for user: ", uidstr, " did not exit on quit; killing") ;
                kill(scandir, SIGKILL) ;
            }

            if (!adopted)
                process_wait(scandir, 0) ;
            close(svfd) ;
            close(sigfd) ;
            _exit(0) ;
        }

        if (sup[0].revents & (POLLIN | POLLHUP)) {
            // scandir died on its own; the guardian follows it out.
            log_info("scandir for user: ", uidstr, " exited; guardian following") ;
            if (!adopted)
                process_wait(scandir, 0) ;
            close(svfd) ;
            close(sigfd) ;
            _exit(0) ;
        }
    }

    log_warn("supervise loop exited unexpectedly for user: ", uidstr) ;
    close(svfd) ;
    close(sigfd) ;
    _exit(LOG_EXIT_SYS) ;
}

pid_t driver_guardian_spawn(uid_t uid, int readyfd)
{
    pid_t pid = fork() ;
    if (pid < 0)
        log_warnusys_return(LOG_EXIT_LESSONE, "fork guardian") ;

    if (!pid) {
        guardian_main(uid, readyfd) ;
        flog_die(LOG_EXIT_SYS, "guardian returned for user %u", uid) ;
    }

    return pid ;
}

int driver_guardian_stop(pid_t guardian_pid)
{
    if (guardian_pid <= 0)
        return 0 ;

    if (kill(guardian_pid, SIGTERM) < 0)
        log_warnusys_return(LOG_EXIT_ZERO, "signal guardian") ;

    return 1 ;
}

