/*
 * pam_userd.c
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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <security/pam_modules.h>
#include <security/pam_ext.h>

#include <oblibs/account.h>
#include <oblibs/environ.h>
#include <oblibs/strbuf.h>
#include <oblibs/string.h>
#include <oblibs/types.h>

#include <66-tools/config.h>
#include "constants.h"
#include "common.h"
#include "proto.h"
#include "userd.h"

#define USERD_PAM_DATA "pam_userd_session"

static char const *pam_item(pam_handle_t *pamh, int type)
{
    void const *v = 0 ;
    if (pam_get_item(pamh, type, &v) != PAM_SUCCESS)
        return 0 ;
    return v ;
}

static void reexport_putenv(pam_handle_t *pamh, char const *key, char const *val)
{
    char kv[18 + SESSION_FIELD_MAX + 1] ;
    size_t klen = strlen(key), vlen = strlen(val) ;

    if (klen + 1 + vlen + 1 > sizeof(kv)) {
        pam_syslog(pamh, LOG_WARNING, "re-export of %s skipped: value too long", key) ;
        return ;
    }

    memcpy(kv, key, klen) ;
    kv[klen] = '=' ;
    memcpy(kv + klen + 1, val, vlen + 1) ;

    if (pam_putenv(pamh, kv) != PAM_SUCCESS)
        pam_syslog(pamh, LOG_WARNING, "pam_putenv(%s) failed", key) ;
}

static void reexport_session_context(pam_handle_t *pamh, char const *sock, char const *id)
{
    _alloc_strbuf_(payload, 3 + SESSION_ID_MAX + 1) ;

    if (!sf_str(&payload, "ID", id))
        return ;                        // id already validated to fit; unreachable

    uint16_t rop, rlen ;
    char info[PROTO_PAYLOAD_MAX + 1] ;

    if (!userd_call(sock, PROTO_GET_SESSION, payload.s, (uint16_t)payload.len, &rop, info, PROTO_PAYLOAD_MAX, &rlen)) {
        pam_syslog(pamh, LOG_WARNING, "GET_SESSION unreachable; XDG context not re-exported") ;
        return ;
    }

    if (rop != PROTO_SESSION_INFO) {
        pam_syslog(pamh, LOG_WARNING, "66-userd refused GET_SESSION; XDG context not re-exported") ;
        return ;
    }

    info[rlen] = 0 ; // environ_search_value parses a C string

    _alloc_strbuf_(v, SESSION_FIELD_MAX + 1) ;

    if (environ_search_value(&v, info, "TYPE"))
        reexport_putenv(pamh, "XDG_SESSION_TYPE", v.s) ;

    if (environ_search_value(&v, info, "CLASS"))
        reexport_putenv(pamh, "XDG_SESSION_CLASS", v.s) ;

    if (environ_search_value(&v, info, "SEAT") && v.s[0])
        reexport_putenv(pamh, "XDG_SEAT", v.s) ;

    uint32_t vtnr ;
    if (environ_search_value(&v, info, "VTNR") && u32_scan(v.s, &vtnr) && vtnr > 0)
        reexport_putenv(pamh, "XDG_VTNR", v.s) ;
}

static void id_cleanup(pam_handle_t *pamh, void *data, int error_status)
{
    (void)pamh ;
    (void)error_status ;
    free(data) ;
}

PAM_EXTERN int pam_sm_open_session(pam_handle_t *pamh, int flags, int argc, char const **argv)
{
    (void)flags ;
    (void)argc ;
    (void)argv ;

    char const *sock = USERD_SOCKET_PATH ;

    char const *user = 0 ;
    if (pam_get_user(pamh, &user, 0) != PAM_SUCCESS || !user || !*user) {
        pam_syslog(pamh, LOG_ERR, "cannot determine PAM_USER; skipping session tracking") ;
        return PAM_SUCCESS ; // best-effort: never block login
    }

    uid_t u = -1 ;

    if (get_uidbyname(user, &u) < 0) {
        pam_syslog(pamh, LOG_ERR, "getpwnam(%s) failed; skipping session tracking", user) ;
        return PAM_SUCCESS ;
    }

    char const *rhost = pam_item(pamh, PAM_RHOST) ;

    char uid[UID_FMT] ;
    uid[uid_format(uid, u)] = 0 ;
    char leader[PID_FMT] ;
    leader[pid_format(leader, getpid())] = 0 ; // the PAM-calling process (decision: LEADER)

    _cleanup_strbuf_ strbuf payload = STRBUF_ZERO ;
    int ok = sf_str(&payload, "UID", uid) &&
            sf_str(&payload, "LEADER", leader) &&
            sf_str(&payload, "VTNR", pam_getenv(pamh, "XDG_VTNR")) &&
            sf_str(&payload, "REMOTE", (rhost && *rhost) ? "1" : 0) &&
            sf_str(&payload, "TYPE", pam_getenv(pamh, "XDG_SESSION_TYPE")) &&
            sf_str(&payload, "CLASS", pam_getenv(pamh, "XDG_SESSION_CLASS")) &&
            sf_str(&payload, "TTY", pam_item(pamh, PAM_TTY)) &&
            sf_str(&payload, "DISPLAY", pam_item(pamh, PAM_XDISPLAY)) &&
            sf_str(&payload, "SEAT", pam_getenv(pamh, "XDG_SEAT")) &&
            sf_str(&payload, "SERVICE", pam_item(pamh, PAM_SERVICE)) &&
            sf_str(&payload, "REMOTE_USER", pam_item(pamh, PAM_RUSER)) &&
            sf_str(&payload, "REMOTE_HOST", rhost) ;

    if (!ok || payload.len > PROTO_PAYLOAD_MAX) {
        pam_syslog(pamh, LOG_ERR, "session context exceeds %d bytes; not registering", PROTO_PAYLOAD_MAX) ;
        return PAM_SUCCESS ;
    }

    uint16_t rop, rlen ;
    char reply[PROTO_PAYLOAD_MAX + 1] ;
    if (!userd_call(sock, PROTO_REGISTER_SESSION, payload.s, (uint16_t)payload.len,
                    &rop, reply, PROTO_PAYLOAD_MAX, &rlen)) {
        pam_syslog(pamh, LOG_WARNING, "66-userd unreachable; session not tracked (login continues)") ;
        return PAM_SUCCESS ;
    }

    reply[rlen] = 0 ;

    if (rop != PROTO_SESSION_REGISTERED) {
        pam_syslog(pamh, LOG_WARNING, "66-userd refused REGISTER: %s", rlen ? reply : "(no detail)") ;
        return PAM_SUCCESS ;
    }

    /** Trust boundary: the id must fit a session-id buffer (id + NUL). A reply
     * longer than SESSION_ID_MAX-1 is rejected, never truncated — mirroring the
     * daemon's own "rejected, never truncated" rule on REGISTER input, and
     * keeping the envid memcpy below in bounds. */
    if (rlen >= SESSION_ID_MAX) {
        pam_syslog(pamh, LOG_WARNING, "invalid session id received from 66-userd") ;
        return PAM_SUCCESS ;
    }

    char *id = strdup(reply) ; // reply holds the assigned session id
    if (!id) {
        pam_syslog(pamh, LOG_ERR, "out of memory storing session id") ;
        return PAM_SUCCESS ;
    }

    if (pam_set_data(pamh, USERD_PAM_DATA, id, id_cleanup) != PAM_SUCCESS) {
        pam_syslog(pamh, LOG_ERR, "pam_set_data failed; session id %s will leak at logout", id) ;
        free(id) ;
        return PAM_SUCCESS ;
    }

    char envid[15 + SESSION_ID_MAX + 1] ;
    auto_strings(envid, "XDG_SESSION_ID=", reply) ;

    if (pam_putenv(pamh, envid) != PAM_SUCCESS)
        pam_syslog(pamh, LOG_WARNING, "pam_putenv(XDG_SESSION_ID) failed") ;

    // Re-export the daemon-normalized XDG context (TYPE/CLASS/SEAT/VTNR).
    reexport_session_context(pamh, sock, reply) ;

    /** Export XDG_RUNTIME_DIR=<base>/<uid> and DBUS_SESSION_BUS_ADDRESS, but only
     * after verifying the daemon really brought the runtime dir up and that it is
     * owned by this user. No fallback: if the dir is absent or not owned by uid,
     * set nothing and warn — a wrong or guessed value is worse than none. */
    char rdenv[sizeof("XDG_RUNTIME_DIR=" SS_TOOLS_USERD_RUNTIME_BASE "/") + UID_FMT] ;
    auto_strings(rdenv, "XDG_RUNTIME_DIR=" SS_TOOLS_USERD_RUNTIME_BASE "/", uid) ;

    char const *rdpath = rdenv + 16 ;
    struct stat st ;
    if (stat(rdpath, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == u) {

        if (pam_putenv(pamh, rdenv) != PAM_SUCCESS)
            pam_syslog(pamh, LOG_WARNING, "pam_putenv(XDG_RUNTIME_DIR) failed") ;

        char busenv[sizeof("DBUS_SESSION_BUS_ADDRESS=" SS_TOOLS_USERD_DBUS_ADDR_PREFIX SS_TOOLS_USERD_RUNTIME_BASE "/bus") + UID_FMT] ;
        auto_strings(busenv, "DBUS_SESSION_BUS_ADDRESS=" SS_TOOLS_USERD_DBUS_ADDR_PREFIX, rdpath, "/bus") ;

        if (pam_putenv(pamh, busenv) != PAM_SUCCESS)
            pam_syslog(pamh, LOG_WARNING, "pam_putenv(DBUS_SESSION_BUS_ADDRESS) failed") ;

    } else {
        pam_syslog(pamh, LOG_WARNING, "runtime dir %s absent or not owned by uid %s; XDG_RUNTIME_DIR and DBUS_SESSION_BUS_ADDRESS not set", rdpath, uid) ;
    }

    pam_syslog(pamh, LOG_INFO, "registered session %s for uid %s", reply, uid) ;

    return PAM_SUCCESS ;
}

PAM_EXTERN int pam_sm_close_session(pam_handle_t *pamh, int flags, int argc, char const **argv)
{
    (void)flags ;
    (void)argc ;
    (void)argv ;

    char const *sock = USERD_SOCKET_PATH ;

    void const *data = 0 ;
    if (pam_get_data(pamh, USERD_PAM_DATA, &data) != PAM_SUCCESS || !data)
        return PAM_SUCCESS ;

    char const *id = data ;
    _alloc_strbuf_(payload, 3 + SESSION_ID_MAX + 1) ;

    if (!sf_str(&payload, "ID", id)) {
        pam_syslog(pamh, LOG_ERR, "session id %s too long to release", id) ;
        return PAM_SUCCESS ;
    }

    uint16_t rop, rlen ;
    char reply[PROTO_PAYLOAD_MAX + 1] ;

    if (!userd_call(sock, PROTO_RELEASE_SESSION, payload.s, (uint16_t)payload.len,
                    &rop, reply, PROTO_PAYLOAD_MAX, &rlen)) {
        pam_syslog(pamh, LOG_WARNING, "66-userd unreachable; could not release session %s", id) ;
        return PAM_SUCCESS ;
    }

    if (rop != PROTO_OK) {
        reply[rlen] = 0 ;
        pam_syslog(pamh, LOG_WARNING, "66-userd refused RELEASE of %s: %s", id, rlen ? reply : "(no detail)") ;
        return PAM_SUCCESS ;
    }

    pam_syslog(pamh, LOG_INFO, "released session %s", id) ;

    return PAM_SUCCESS ;
}
