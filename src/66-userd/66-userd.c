/*
 * 66-userd.c
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
#include <pwd.h>
#include <signal.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/pidfd.h>
#include <sys/stat.h>

#include <oblibs/io.h>
#include <oblibs/fd.h>
#include <oblibs/clock.h>
#include <oblibs/hash.h>
#include <oblibs/log.h>
#include <oblibs/opt.h>
#include <oblibs/process.h>
#include <oblibs/sse.h>
#include <oblibs/socket.h>
#include <oblibs/string.h>
#include <oblibs/environ.h>
#include <oblibs/types.h>
#include <oblibs/strbuf.h>

#include <66/utils.h>
#include <66/shutdown.h>

#include <66-tools/config.h>
#include "constants.h"
#include "userd.h"
#include "session.h"
#include "proto.h"
#include "state.h"
#include "driver.h"
#include "power.h"
#include "runtimedir.h"
#include "leader.h"

#define USERD_NOTIFY_FD 3
#define USERD_MAX_EVENTS 16
#define USERD_READY_PRIO 5
#define USERD_GUARDIAN_PRIO 6
#define USERD_LEADER_PRIO 7

typedef struct userd_s userd_t ;
struct userd_s {
    sse_epoll_t epoll ;
    sse_watcher_t wserver ;
    sse_watcher_t wsignal ;
    int sfd ;
    int fdlock ;
    char const *sockpath ;
    char const *statedir ;
    char const *rundir ; // runtime dir base (SS_TOOLS_USERD_RUNTIME_BASE, build-time, shared with PAM)
    uint64_t next_id ; // monotonic session-id counter
    hash_t sessions ; // keyed by id
    hash_t users ; // keyed by uid
} ;

static userd_t userd = { 0 } ;

static void send_reply(int fd, uint16_t opcode, char const *payload, uint16_t size)
{
    proto_header_t h = { opcode, size } ;
    if (io_allwrite(fd, (char *)&h, sizeof(h)) != sizeof(h))
        return ;

    if (size)
        io_allwrite(fd, (char *)payload, size) ;
}

static void send_error(int fd, char const *msg)
{
    send_reply(fd, PROTO_ERROR, msg, (uint16_t)strlen(msg)) ;
}

static uint64_t get_timenow(void)
{
    struct timespec ts ;
    clock_now(&ts) ;
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000 ;
}

static user_t *user_get(uid_t uid)
{
    return hash_find(&userd.users, &uid, sizeof uid) ;
}

static void user_save_state(user_t const *u)
{
    char stk[u->nsessions * (SESSION_ID_MAX + 1) + 1] ;
    session_t *s, *tmp ;
    size_t pos = 0 ;

    HASH_FOREACH(&userd.sessions, s, tmp) {

        if (s->uid == u->uid) {
            if (pos)
                stk[pos++] = ' ' ;
            size_t len = strlen(s->id) ;
            memcpy(stk + pos, s->id, len) ;
            pos += len ;
        }
    }

    stk[pos] = 0 ;

    if (!state_save_user(userd.statedir, u, strlen(stk) ? stk : ""))
        log_warnusys("save user state") ;
}

static user_t *user_ensure(uid_t uid)
{
    user_t *u = user_get(uid) ;
    if (u)
        return u ;

    u = user_new(uid) ;
    if (!u)
        return 0 ;

    struct passwd *pw = getpwuid(uid) ;
    if (pw) {
        u->gid = pw->pw_gid ;
        session_set(u->name, sizeof(u->name), pw->pw_name) ;
    }

    u->state = USER_STATE_ONLINE ;
    u->timestamp = get_timenow() ;
    if (!hash_add(&userd.users, &u->uid, sizeof u->uid, u)) {
        log_warnusys("add user to hash") ;
        user_free(u) ;
        return 0 ;
    }

    return u ;
}

static void session_unarm(session_t *s)
{
    if (s->leaderfd < 0)
        return ;

    sse_free_io(&s->leaderw) ;

    s->leaderfd = -1 ;
}

static void reconcile_user(void *arg) ;

static void defer_session_free(session_t *s)
{
    if (!sse_defer(&userd.epoll, (void (*)(void *))session_free, s))
        log_warn("defer session free failed (out of memory); leaking session") ;
}

static void defer_reconcile(uid_t uid)
{
    if (!sse_defer(&userd.epoll, reconcile_user, (void *)(uintptr_t)uid))
        log_warn("defer user reconcile failed (out of memory)") ;
}

static void release_session(session_t *s)
{
    uid_t uid = s->uid ;

    session_unarm(s) ;

    hash_del(&userd.sessions, s) ;

    if (!state_remove_session(userd.statedir, s->id))
        log_warnusys("remove session state: ", s->id) ;

    defer_session_free(s) ;

    user_t *u = user_get(uid) ;
    if (!u)
        return ;

    if (u->nsessions)
        u->nsessions-- ;

    if (u->nsessions) {

        user_save_state(u) ;

    } else if (u->scandir_up && u->guardian_pid > 0) {

        driver_release(u) ;
        user_save_state(u) ;

    } else {

        defer_reconcile(uid) ;
    }
}

static void gc_cb(sse_watcher_t *w, void *data, int revents)
{
    (void)w ;
    (void)revents ;

    session_t *s = data ;

    log_info("dead leader, releasing session ", s->id) ;

    release_session(s) ;
}

static void guardian_gc_cb(sse_watcher_t *w, void *data, int revents) ;

static void ready_unarm(user_t *u)
{
    if (u->readyw.fd < 0)
        return ;

    sse_free_eventfd(&u->readyw) ;

    u->readyw.fd = -1 ;
}

static void ready_cb(sse_watcher_t *w, void *data, int revents)
{
    (void)revents ;

    user_t *u = data ;

    sse_read_eventfd(w) ;

    u->state = USER_STATE_ONLINE ;
    u->timestamp = get_timenow() ;

    log_info("user online: ", u->name) ;

    // one-shot: release the channel so the user is promoted once per guardian.
    ready_unarm(u) ;
    user_save_state(u) ;
}

static int ready_arm(user_t *u)
{
    ready_unarm(u) ;

    if (!sse_start_eventfd(&userd.epoll, &u->readyw, ready_cb, u, USERD_READY_PRIO)) {
        log_warnusys("arm readiness watcher for user ", u->name) ;
        u->readyw.fd = -1 ;
        return 0 ;
    }

    return 1 ;
}

static void guardian_unarm(user_t *u)
{
    if (u->guardianfd < 0)
        return ;

    sse_free_io(&u->guardianw) ;

    u->guardianfd = -1 ;
}

static int guardian_arm(user_t *u)
{
    if (leader_starttime(u->guardian_pid, &u->guardian_starttime) <= 0) {
        log_warnusys("read guardian start time for user ", u->name) ;
        u->guardian_starttime = 0 ;
    }

    u->guardianfd = pidfd_open(u->guardian_pid, 0) ;
    if (u->guardianfd < 0)
        log_warnusys_return(LOG_EXIT_ZERO, "open guardian pidfd for user ", u->name) ;

    if (!sse_start_io(&userd.epoll, &u->guardianw, guardian_gc_cb, u, u->guardianfd, SSE_READ, USERD_GUARDIAN_PRIO)) {
        log_warnusys("arm guardian watcher for user ", u->name) ;
        close(u->guardianfd) ;
        u->guardianfd = -1 ;
        return 0 ;
    }

    return 1 ;
}

static int guardian_spawn_arm(user_t *u)
{
    ready_arm(u) ;
    pid_t pid = driver_guardian_spawn(u->uid, u->readyw.fd) ;
    if (pid <= 0) {
        ready_unarm(u) ;
        u->guardian_pid = 0 ;
        u->scandir_up = 0 ;
        return 0 ;
    }

    u->guardian_pid = pid ;

    if (!guardian_arm(u)) {
        driver_guardian_stop(pid) ;
        ready_unarm(u) ;
        u->guardian_pid = 0 ;
        u->scandir_up = 0 ;
        return 0 ;
    }

    u->scandir_up = 1 ;
    u->state = USER_STATE_OPENING ;

    return 1 ;
}

static void reconcile_user(void *arg)
{
    uid_t uid = (uid_t)(uintptr_t)arg ;
    user_t *u = user_get(uid) ;

    if (!u || u->guardian_pid > 0)
        return ;

    if (u->nsessions) {
        /** This CLOSING test is why guardian_gc_cb must NOT reset u->state: it is the
         * only thing telling a re-login during tear-down from an unexpected death. */
        if (u->state == USER_STATE_CLOSING) {
            log_info("re-login during tear-down, restarting for user ", u->name) ;
            if (!guardian_spawn_arm(u)) {
                log_warn("restart failed for user ", u->name) ;
                u->state = USER_STATE_OFFLINE ;
            }
        } else {
            log_warn("died unexpectedly with live sessions for user ", u->name) ;
            u->state = USER_STATE_OFFLINE ;     // no scandir is being driven anymore
        }

        user_save_state(u) ;

        return ;
    }

    runtimedir_release(u, userd.rundir) ;
    hash_del(&userd.users, u) ;

    if (!state_remove_user(userd.statedir, u->uid))
        log_warnusys("remove user state") ;

    user_free(u) ;
}

static void guardian_gc_cb(sse_watcher_t *w, void *data, int revents)
{
    (void)w ;
    (void)revents ;

    user_t *u = data ;
    pid_t pid = u->guardian_pid ;
    int wstat = 0 ;

    if (pid > 0)
        process_wait(pid, &wstat) ;

    if (pid > 0) {

        if (WIFSIGNALED(wstat)) {

            flog_warn("guardian of user %s (pid %u) was killed by signal %d", u->name, (unsigned int)pid, WTERMSIG(wstat)) ;

        } else if (WIFEXITED(wstat) && WEXITSTATUS(wstat)) {

            flog_warn("guardian of user %s (pid %u) exited with code %d", u->name, (unsigned int)pid, WEXITSTATUS(wstat)) ;

        } else flog_trace("guardian of user %s (pid %u) exited", u->name, (unsigned int)pid) ;
    }

    guardian_unarm(u) ;
    ready_unarm(u) ; // died before signalling readiness: release the channel
    u->scandir_up = 0 ;
    u->guardian_pid = 0 ;

    defer_reconcile(u->uid) ;
}

static void do_register(int fd, char const *payload)
{
    char id[U64_FMT] ;
    id[u64_fmt(id, userd.next_id)] = 0 ;

    session_t *s = session_new(id) ;
    if (!s) {
        send_error(fd, "out of memory") ;
        return ;
    }

    if (!session_deserialize(s, payload)) {
        session_free(s) ;
        send_error(fd, "malformed payload") ;
        return ;
    }

    _alloc_strbuf_(v, PROTO_PAYLOAD_MAX) ;
    int class_provided = environ_search_value(&v, payload, "CLASS") ;
    session_normalize(s, class_provided) ;

    if (s->leader <= 0) {
        session_free(s) ;
        send_error(fd, "missing or invalid LEADER") ;
        return ;
    }

    s->state = SESSION_STATE_ONLINE ;
    s->timestamp = get_timenow() ;

    s->leaderfd = pidfd_open(s->leader, 0) ;
    if (s->leaderfd < 0) {
        int e = errno ;
        session_free(s) ;
        send_error(fd, e == ESRCH ? "leader already gone" : "open leader pidfd failed") ;
        return ;
    }

    int st = leader_starttime(s->leader, &s->leader_starttime) ;
    if (st <= 0) {
        close(s->leaderfd) ;
        session_free(s) ;
        send_error(fd, !st ? "leader already gone" : "read leader starttime failed") ;
        return ;
    }

    if (!sse_start_io(&userd.epoll, &s->leaderw, gc_cb, s, s->leaderfd, SSE_READ, USERD_LEADER_PRIO)) {
        close(s->leaderfd) ;
        session_free(s) ;
        send_error(fd, "arm leader watcher failed") ;
        return ;
    }

    user_t *u = user_ensure(s->uid) ;
    if (!u) {
        session_unarm(s) ;
        session_free(s) ;
        send_error(fd, "out of memory") ;
        return ;
    }

    if (!hash_add(&userd.sessions, s->id, strlen(s->id), s)) {
        session_unarm(s) ;
        session_free(s) ;
        send_error(fd, "out of memory") ;
        return ;
    }

    u->nsessions++ ;
    userd.next_id++ ;

    runtimedir_register(u, userd.rundir) ;

    /** A fresh guardian is the one with a pid but no supervision yet; any other
     * outcome (later session, scandir already up, start failure) means nothing will
     * ever signal the readiness channel, so it is released right away. */
    ready_arm(u) ;
    if (driver_register(u, u->readyw.fd)
        && u->guardian_pid > 0 && u->guardianfd < 0) {
        if (!guardian_arm(u)) {
            driver_guardian_stop(u->guardian_pid) ;
            ready_unarm(u) ;
            u->guardian_pid = 0 ;
            u->scandir_up = 0 ;
        }
    } else
        ready_unarm(u) ;

    if (!state_save_session(userd.statedir, s))
        log_warnusys("persist session state: ", id) ;

    user_save_state(u) ;

    log_info("registered session ", id) ;

    send_reply(fd, PROTO_SESSION_REGISTERED, id, (uint16_t)strlen(id)) ;
}

static void do_release(int fd, char const *payload)
{
    _alloc_strbuf_(v, PROTO_PAYLOAD_MAX) ;
    if (!environ_search_value(&v, payload, "ID")) {
        send_error(fd, "missing ID") ;
        return ;
    }

    session_t *s = hash_find(&userd.sessions, v.s, strlen(v.s)) ;
    if (!s) {
        send_error(fd, "unknown session") ;
        return ;
    }

    release_session(s) ;

    log_info("released session ", v.s) ;

    send_reply(fd, PROTO_OK, 0, 0) ;
}

static void do_list(int fd)
{
    char buf[PROTO_PAYLOAD_MAX] ;
    size_t len = 0 ;
    session_t *s, *tmp ;

    HASH_FOREACH(&userd.sessions, s, tmp) {

        size_t idlen = strlen(s->id) ;
        if (len + idlen + 1 > sizeof(buf)) {
            log_warn("session list exceeds one frame; not all ids returned") ;
            break ;
        }

        memcpy(buf + len, s->id, idlen) ;
        len += idlen ;
        buf[len++] = '\n' ;
    }

    send_reply(fd, PROTO_SESSION_LIST, buf, (uint16_t)len) ;
}

static void do_get(int fd, char const *payload)
{
    _alloc_strbuf_(v, PROTO_PAYLOAD_MAX) ;
    if (!environ_search_value(&v, payload, "ID")) {
        send_error(fd, "missing ID") ;
        return ;
    }

    session_t *s = hash_find(&userd.sessions, v.s, strlen(v.s)) ;
    if (!s) {
        send_error(fd, "unknown session") ;
        return ;
    }

    _alloc_strbuf_(out, STRBUF_THRESHOLD) ;
    if (!session_serialize(s, &out)) {
        send_error(fd, "serialize failed") ;
        return ;
    }

    if (out.len > PROTO_PAYLOAD_MAX) {
        send_error(fd, "session too large") ;
        return ;
    }

    send_reply(fd, PROTO_SESSION_INFO, out.s, (uint16_t)out.len) ;
}

static int caller_has_active_local_session(uid_t uid)
{
    session_t *s, *tmp ;
    HASH_FOREACH(&userd.sessions, s, tmp) {
        if (s->uid == uid && !s->remote
            && (s->state == SESSION_STATE_ONLINE || s->state == SESSION_STATE_ACTIVE))
                return 1 ;
    }
    return 0 ;
}

static int other_active_users(uid_t uid)
{
    user_t *u, *tmp ;
    HASH_FOREACH(&userd.users, u, tmp) {
        if (u->uid != uid && u->nsessions)
            return 1 ;
    }
    return 0 ;
}

static void do_power(int fd, char const *payload, uid_t caller)
{
    _alloc_strbuf_(v, PROTO_PAYLOAD_MAX) ;
    if (!environ_search_value(&v, payload, "ACTION")) {
        send_error(fd, "missing ACTION") ;
        return ;
    }

    int action = power_action_from(v.s) ;
    if (action < 0) {
        send_error(fd, "unknown power action") ;
        return ;
    }

    _alloc_strbuf_(fv, 16) ;

    int force = environ_search_value(&fv, payload, "FORCE") && !strcmp(fv.s, "1") ;

    /** Gather the policy facts from the registry and shutdown.allow, then decide.
     * `allowed` is the whitelist gate: an absent file imposes no restriction. */
    int is_root = caller == 0 ;
    int has_local = 0, has_other = 0, allowed = 1 ;

    if (!is_root) {
        has_local = caller_has_active_local_session(caller) ;
        has_other = other_active_users(caller) ;
        struct passwd *pw = getpwuid(caller) ;
        int present = access(SHUTDOWN_FILE, F_OK) == 0 ;
        allowed = !present || (pw && shutdown_search(pw->pw_name) == 1) ;
    }

    power_policy_t verdict = power_policy(is_root, has_local, allowed, has_other, force) ;
    if (verdict != POWER_ALLOW) {
        log_warn("denied power request (", v.s, "): ", power_policy_str(verdict)) ;
        send_error(fd, power_policy_str(verdict)) ;
        return ;
    }

    if (!power_trigger((power_action_t)action)) {
        send_error(fd, "power action failed") ;
        return ;
    }

    send_reply(fd, PROTO_OK, 0, 0) ;
}

static void handle_client(int fd)
{
    struct ucred cred ;
    if (socketunix_getucred(fd, &cred) < 0) {
        log_warnusys("get peer credentials") ;
        return ;
    }

    proto_header_t h ;
    if (io_allread(fd, (char *)&h, sizeof(h)) != sizeof(h))
        return ;

    if (h.size > PROTO_PAYLOAD_MAX) {
        send_error(fd, "payload too large") ;
        return ;
    }

    char payload[PROTO_PAYLOAD_MAX + 1] ;
    if (h.size && io_allread(fd, payload, h.size) != h.size)
        return ;

    payload[h.size] = 0 ;

    /** The socket is mode 0666 so a non-root peer (a display manager) can reach
     * PROTO_POWER; every other opcode stays root-only. POWER itself is gated by the
     * power policy inside do_power. */
    if (h.opcode != PROTO_POWER && cred.uid != 0) {
        log_warn("rejected privileged request from non-root peer") ;
        send_error(fd, "permission denied") ;
        return ;
    }

    switch (h.opcode) {
        case PROTO_REGISTER_SESSION : do_register(fd, payload) ; break ;
        case PROTO_RELEASE_SESSION  : do_release(fd, payload) ; break ;
        case PROTO_LIST_SESSIONS    : do_list(fd) ; break ;
        case PROTO_GET_SESSION      : do_get(fd, payload) ; break ;
        case PROTO_POWER            : do_power(fd, payload, cred.uid) ; break ;
        default                     : send_error(fd, "unknown opcode") ; break ;
    }
}

static void last_stop_dead_user(uid_t uid)
{
    struct passwd *pw = getpwuid(uid) ;
    char const *who = pw ? pw->pw_name : "?" ;

    pid_t gpid = 0 ;
    unsigned long gst = 0 ;
    int gfd = -1 ;

    if (state_load_user_guardian(userd.statedir, uid, &gpid, &gst) && gpid > 0 && leader_check(gpid, gst, &gfd) == 1) {

        close(gfd) ;
        log_info("stopping orphaned guardian for dead user ", who) ;
        driver_guardian_stop(gpid) ;

    } else if (driver_scandir_ok(uid) == 1) {
        log_warn("abandoning unsupervised orphan scandir for dead user ", who) ;
    }

    user_t u ;
    memset(&u, 0, sizeof(u)) ;
    u.uid = uid ;

    if (pw) {
        u.gid = pw->pw_gid ;
        session_set(u.name, sizeof(u.name), pw->pw_name) ;
    }

    u.nsessions = 0 ;
    u.runtimedir_up = (runtimedir_ismount(uid, userd.rundir) == 1) ;

    runtimedir_release(&u, userd.rundir) ;
}

static void reconcile_users(void)
{
    user_t *u, *utmp ;

    HASH_FOREACH(&userd.users, u, utmp) {
        user_save_state(u) ;
    }

    DIR *d = opendir(USERD_USERS_DIR) ;
    if (!d)
        return ;

    struct dirent *e ;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue ;
        uid_t uid ;
        if (!uid_parse(e->d_name, &uid))
            continue ;
        if (!user_get(uid)) {
            last_stop_dead_user(uid) ;
            state_remove_user(userd.statedir, uid) ;
        }
    }
    closedir(d) ;
}

static void userd_reconcile(void)
{
    DIR *d = opendir(USERD_SESSIONS_DIR) ;
    if (!d)
        return ;

    uint64_t maxid = 0 ;
    struct dirent *e ;

    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue ;

        char const *id = e->d_name ;
        uint64_t idnum ;
        if (u64_scan(id, &idnum) && idnum > maxid)
            maxid = idnum ;

        session_t *s = session_new(id) ;
        if (!s)
            continue ;

        if (!state_load_session(userd.statedir, id, s)) {
            log_warnusys("load session state: ", id) ;
            session_free(s) ;
            continue ;
        }

        int pidfd = -1 ;
        if (s->leader <= 0 || leader_check(s->leader, s->leader_starttime, &pidfd) != 1) {
            log_info("dropping dead-leader session ", id) ;
            state_remove_session(userd.statedir, id) ;
            session_free(s) ;
            continue ;
        }

        user_t *u = user_ensure(s->uid) ;
        if (!u) {
            close(pidfd) ;
            session_free(s) ;
            continue ;
        }

        s->leaderfd = pidfd ;
        if (!sse_start_io(&userd.epoll, &s->leaderw, gc_cb, s, s->leaderfd, SSE_READ, USERD_LEADER_PRIO)) {
            log_warnusys("arm leader watcher for session ", id) ;
            close(s->leaderfd) ;
            s->leaderfd = -1 ;
            state_remove_session(userd.statedir, id) ;
            session_free(s) ;
            continue ;
        }

        if (!hash_add(&userd.sessions, s->id, strlen(s->id), s)) {
            log_warnusys("add session to hash ", id) ;
            session_unarm(s) ;
            state_remove_session(userd.statedir, id) ;
            session_free(s) ;
            continue ;
        }
        u->nsessions++ ;
    }

    closedir(d) ;

    userd.next_id = maxid + 1 ;

    user_t *u, *utmp ;
    HASH_FOREACH(&userd.users, u, utmp) {

        pid_t gpid = 0 ;
        unsigned long gst = 0 ;
        int gfd = -1 ;

        if (state_load_user_guardian(userd.statedir, u->uid, &gpid, &gst) && gpid > 0 && leader_check(gpid, gst, &gfd) == 1) {

            u->guardian_pid = gpid ;
            u->guardian_starttime = gst ;
            u->guardianfd = gfd ;

            if (sse_start_io(&userd.epoll, &u->guardianw, guardian_gc_cb, u,u->guardianfd, SSE_READ, USERD_GUARDIAN_PRIO)) {

                u->scandir_up = 1 ;

                /** A re-adopted guardian has no readiness channel — the eventfd died
                 * with the previous daemon — so readiness is probed, not awaited. */
                u->state = driver_scandir_ok(u->uid) == 1 ? USER_STATE_ONLINE : USER_STATE_OPENING ;

            } else {
                log_warnusys("arm guardian watcher for user ", u->name) ;
                close(u->guardianfd) ;
                u->guardianfd = -1 ;
                u->guardian_pid = 0 ;
                u->scandir_up = 0 ;
            }

        } else {

            u->scandir_up = 0 ;
            if (gpid > 0)
                log_info("guardian gone for user ", u->name) ;
        }

        u->runtimedir_up = (runtimedir_ismount(u->uid, userd.rundir) == 1) ;
    }

    reconcile_users() ;

    flog_info("%lu session(s) recovered", hash_count(&userd.sessions)) ;
}

static void server_cb(sse_watcher_t *w, void *data, int revents)
{
    (void)w ;
    (void)data ;

    if (revents & (SSE_ERROR | SSE_HUP)) {
        log_warnusys("server socket error") ;
        userd.epoll.running = false ;
        return ;
    }

    int fd = socketunix_accept(userd.sfd, O_CLOEXEC) ;
    if (fd < 0) {
        log_warnusys("accept connection") ;
        return ;
    }

    handle_client(fd) ;
    close(fd) ;
}

static void signal_cb(sse_watcher_t *w, void *data, int revents)
{
    (void)w ;
    (void)data ;
    (void)revents ;
    userd.epoll.running = false ;
}

static int userd_init(void)
{
    if (!sse_new(&userd.epoll, USERD_MAX_EVENTS))
        log_warnusys_return(LOG_EXIT_ZERO, "initialize event loop") ;

    if (!state_init(userd.statedir))
        log_warnusys_return(LOG_EXIT_ZERO, "create state directory: ", userd.statedir) ;

    if (!hash_init(&userd.sessions, 0, offsetof(session_t, node)))
        log_warnusys_return(LOG_EXIT_ZERO, "initialize session table") ;

    if (!hash_init(&userd.users, 0, offsetof(user_t, node)))
        log_warnusys_return(LOG_EXIT_ZERO, "initialize user table") ;

    userd_reconcile() ;

    userd.sfd = socketunix_create(O_CLOEXEC | O_NONBLOCK) ;
    if (userd.sfd < 0)
        log_warnusys_return(LOG_EXIT_ZERO, "create socket") ;

    if (socketunix_bind_reuse(userd.sfd, userd.sockpath, &userd.fdlock) < 0)
        log_warnusys_return(LOG_EXIT_ZERO, "bind socket: ", userd.sockpath) ;

    /** 0666 so a non-root peer (a display manager) can connect for PROTO_POWER;
     * every other opcode is still rejected from a non-root peer (SO_PEERCRED). */
    if (chmod(userd.sockpath, 0666) < 0)
        log_warnusys_return(LOG_EXIT_ZERO, "chmod socket: ", userd.sockpath) ;

    if (socketunix_listen(userd.sfd, SOMAXCONN) < 0)
        log_warnusys_return(LOG_EXIT_ZERO, "listen on socket") ;

    if (!sse_start_io(&userd.epoll, &userd.wserver, server_cb, 0, userd.sfd, SSE_READ, 10))
        log_warnusys_return(LOG_EXIT_ZERO, "start server watcher") ;

    if (!sse_start_signal(&userd.epoll, &userd.wsignal, signal_cb, 0, 5))
        log_warnusys_return(LOG_EXIT_ZERO, "start signal watcher") ;

    if (!sse_attach_signal(&userd.wsignal, SIGTERM) || !sse_attach_signal(&userd.wsignal, SIGINT))
        log_warnusys_return(LOG_EXIT_ZERO, "attach signals") ;

    return 1 ;
}

static void userd_cleanup(void)
{
    session_t *s, *stmp ;
    HASH_FOREACH(&userd.sessions, s, stmp) {
        session_unarm(s) ;
        session_free(s) ;
    }

    hash_free(&userd.sessions) ;

    user_t *u, *utmp ;
    HASH_FOREACH(&userd.users, u, utmp) {
        guardian_unarm(u) ;
        user_free(u) ;
    }

    hash_free(&userd.users) ;

    (void)unlink(userd.sockpath) ;

    char lockname[strlen(userd.sockpath) + 6] ;
    auto_strings(lockname, userd.sockpath, ".lock") ;
    (void)unlink(lockname) ;
}

int main(int argc, char const *const *argv)
{
    userd.sockpath = USERD_SOCKET_PATH ;
    userd.statedir = USERD_STATEDIR ;
    userd.rundir = SS_TOOLS_USERD_RUNTIME_BASE ; // system policy shared with the PAM module; build-time only
    userd.next_id = 1 ;
    userd.sfd = -1 ;

    int notif = 0 ;

    PROG = "66-userd" ;

    static opt_t const opts[] = {
        { .id = OPT_ID_HELP, .shortname = 'h', .longname = "help", .arg = OPT_NONE, .help = "print this help" },
        { .id = 'v', .shortname = 'v', .longname = "verbosity", .arg = OPT_REQUIRED, .argname = "number", .help = "increase/decrease verbosity" },
        { .id = 'd', .shortname = 'd', .longname = "notify", .arg = OPT_REQUIRED, .argname = "number", .help = "readiness notification file descriptor (default 3)" },
    } ;

    static opt_cmd_t const cmd = {
        .name = "66-userd",
        .opts = opts,
        .nopts = OPT_COUNT(opts),
    } ;

    opt_scan_t st = OPT_SCAN_ZERO ;
    for (;;) {
        int o = opt_scan(argc, argv, opts, OPT_COUNT(opts), &st) ;
        if (o == OPT_END)
            break ;
        switch (o) {
            case OPT_ID_HELP : return opt_emit_help(cmd.name, &cmd) ;
            case 'v' : if (!u32_scan_strict(st.arg, &VERBOSITY)) log_die(LOG_EXIT_USER, "invalid verbosity: ", st.arg) ; break ;
            case 'd' : notif = notifier_isvalid(st.arg) ; break ;
            default : return opt_emit_error(cmd.name, &cmd, o, &st) ;
        }
    }

    if (!userd_init())
        log_dieu(LOG_EXIT_SYS, "initialize userd") ;

    log_info("listening on ", userd.sockpath) ;

    if (notif) {
        char nl = '\n' ;
        if (io_write(notif, &nl, 1) == 1)
            close_fd(notif) ;
    }
    int r = sse_poll(&userd.epoll, SSE_TIMEOUT_INFINITE) ;

    userd_cleanup() ;

    sse_free(&userd.epoll) ;

    return r ? 0 : LOG_EXIT_SYS ;
}
