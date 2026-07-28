/*
 * session.c
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

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include <oblibs/string.h>
#include <oblibs/types.h>
#include <oblibs/environ.h>
#include <oblibs/strbuf.h>
#include <oblibs/sbl.h>

#include "constants.h"
#include "userd.h"
#include "session.h"
#include "common.h"

static char const *const session_type_table[] = {
    "unspecified", "tty", "x11", "wayland", 0
} ;

static char const *const session_class_table[] = {
    "user", "greeter", "lock-screen", "background", 0
} ;

static char const *const session_state_table[] = {
    "opening", "online", "active", "closing", 0
} ;

static char const *const user_state_table[] = {
    "offline", "opening", "online", "closing", 0
} ;

static char const *table_str(char const *const *table, unsigned int i)
{
    return table[i] ;
}

static int table_from(char const *const *table, char const *s)
{
    for (unsigned int i = 0 ; table[i] ; i++)
        if (!strcmp(table[i], s))
            return (int)i ;
    return -1 ;
}

char const *session_type_str(session_type_t t)
{
    return table_str(session_type_table, t) ;
}

char const *session_class_str(session_class_t c)
{
    return table_str(session_class_table, c) ;
}

char const *session_state_str(session_state_t st)
{
    return table_str(session_state_table, st) ;
}

char const *user_state_str(user_state_e st)
{
    return table_str(user_state_table, st) ;
}


session_type_t session_type_from(char const *s)
{
    int r = table_from(session_type_table, s) ;
    return r < 0 ? SESSION_TYPE_UNSPECIFIED : (session_type_t)r ;
}

session_class_t session_class_from(char const *s)
{
    int r = table_from(session_class_table, s) ;
    return r < 0 ? SESSION_CLASS_USER : (session_class_t)r ;
}

session_state_t session_state_from(char const *s)
{
    int r = table_from(session_state_table, s) ;
    return r < 0 ? SESSION_STATE_OPENING : (session_state_t)r ;
}

user_state_e user_state_from(char const *s)
{
    int r = table_from(user_state_table, s) ;
    return r < 0 ? USER_STATE_OFFLINE : (user_state_e)r ;
}

int session_set(char *field, size_t cap, char const *value)
{
    if (!value || !value[0]) {
        field[0] = 0 ;
        return 1 ;
    }

    size_t len = strlen(value) ;
    if (len >= cap)
        return (errno = EOVERFLOW, 0) ;

    memcpy(field, value, len + 1) ;

    return 1 ;
}

void session_normalize(session_t *s, int class_provided)
{
    // Normalize TTY (logind's PAM_TTY rearrangements).
    if (s->tty[0]) {

        if (strchr(s->tty, ':')) {
            /** A tty with a colon is an X11 display parked in PAM_TTY for utmp.
             * Move it to DISPLAY (without clobbering an explicit one), drop tty. */
            if (!s->display[0])
                session_set(s->display, sizeof(s->display), s->tty) ;

            s->tty[0] = 0 ;

        } else if (!strcmp(s->tty, "cron")) {

            s->type = SESSION_TYPE_UNSPECIFIED ;
            s->class = SESSION_CLASS_BACKGROUND ;
            s->tty[0] = 0 ;

        } else if (!strcmp(s->tty, "ssh")) {

            s->type = SESSION_TYPE_TTY ;
            s->class = SESSION_CLASS_USER ;
            s->tty[0] = 0 ;

        } else if (!strncmp(s->tty, "/dev/", 5)) {

            memmove(s->tty, s->tty + 5, strlen(s->tty + 5) + 1) ; // overlapping: not memcpy
        }
    }

    /** An explicit XDG_VTNR is already in s->vtnr; else derive it from a
     * "ttyN" virtual console (and only when the whole suffix is numeric). */
    if (!s->vtnr && !strncmp(s->tty, "tty", 3) && s->tty[3]) {
        uint32_t n ;
        size_t got = u32_scan(s->tty + 3, &n) ;
        if (got && !s->tty[3 + got])
            s->vtnr = n ;
    }

    /** keep an explicit seat; else put a local session (a VC with a vtnr,
     * or a local X display) on seat0, and leave a remote one seatless. */
    if (!s->seat[0] && !s->remote && (s->vtnr || s->display[0]))
        session_set(s->seat, sizeof(s->seat), "seat0") ;

    // Only seat0 owns a VTNR (any other seat, including none, clears it).
    if (strcmp(s->seat, "seat0"))
        s->vtnr = 0 ;

    /** A display means x11, else a tty means tty. Runs on an
     * UNSPECIFIED type only, so an explicit x11/tty/wayland is preserved. */
    if (s->type == SESSION_TYPE_UNSPECIFIED && s->display[0]) {

        s->type = SESSION_TYPE_X11 ;

    } else if (s->type == SESSION_TYPE_UNSPECIFIED && s->tty[0]) {

        s->type = SESSION_TYPE_TTY ;
    }

    /** an unspecified type is a background job, anything else a user session.
     * An explicit class — including greeter/lock-screen, which derivation
     * never invents — is left untouched. */
    if (!class_provided)
        s->class = s->type == SESSION_TYPE_UNSPECIFIED ? SESSION_CLASS_BACKGROUND : SESSION_CLASS_USER ;
}

session_t *session_new(char const *id)
{
    if (!id || !id[0] || strlen(id) >= SESSION_ID_MAX)
        return (errno = EINVAL, (session_t *)0) ;

    session_t *s = calloc(1, sizeof(*s)) ;
    if (!s)
        return (errno = ENOMEM, (session_t *)0) ;

    auto_strings(s->id, id) ;
    s->leaderfd = -1 ;
    s->state = SESSION_STATE_OPENING ;

    return s ;
}

void session_free(session_t *s)
{
    free(s) ;
}

int session_serialize(session_t const *s, strbuf *sb)
{
    sb->len = 0 ;
    return sf_u64(sb, "UID", s->uid) &&
           sf_u64(sb, "LEADER", (uint64_t)s->leader) &&
           sf_u64(sb, "LEADER_STARTTIME", (uint64_t)s->leader_starttime) &&
           sf_str(sb, "SEAT", s->seat) &&
           sf_u64(sb, "VTNR", s->vtnr) &&
           sf_str(sb, "TTY", s->tty) &&
           sf_str(sb, "DISPLAY", s->display) &&
           sf_str(sb, "TYPE", session_type_str(s->type)) &&
           sf_str(sb, "CLASS", session_class_str(s->class)) &&
           sf_u64(sb, "REMOTE", s->remote) &&
           sf_str(sb, "SERVICE", s->service) &&
           sf_str(sb, "REMOTE_USER", s->remote_user) &&
           sf_str(sb, "REMOTE_HOST", s->remote_host) &&
           sf_str(sb, "STATE", session_state_str(s->state)) &&
           sf_u64(sb, "TIMESTAMP", s->timestamp) ;
}

int session_from_env(session_t *s, strbuf *env)
{
    size_t pos = 0 ;

    FOREACH_SBL(env, pos) {

        char *kv = env->s + pos ;
        char *eq = strchr(kv, '=') ;
        if (!eq)
            continue ;

        *eq = 0 ;

        char const *key = kv ;
        char const *val = eq + 1 ;

        if (!strcmp(key, "UID")) {

            uid_parse(val, &s->uid) ;

        } else if (!strcmp(key, "LEADER")) {

            pid_parse(val, &s->leader) ;

        } else if (!strcmp(key, "LEADER_STARTTIME")) {

            uint64_t n ;
            if (u64_scan(val, &n))
                s->leader_starttime = (unsigned long)n ;

        } else if (!strcmp(key, "VTNR")) {

            u32_scan(val, &s->vtnr) ;

        } else if (!strcmp(key, "REMOTE")) {

            uint32_t u ;
            if (u32_scan(val, &u))
                s->remote = u ? 1 : 0 ;

        } else if (!strcmp(key, "TYPE")) {

            s->type = session_type_from(val) ;

        } else if (!strcmp(key, "CLASS")) {

            s->class = session_class_from(val) ;

        } else if (!strcmp(key, "STATE")) {

            s->state = session_state_from(val) ;

        } else if (!strcmp(key, "TIMESTAMP")) {

            uint64_t n ;
            if (u64_scan(val, &n))
                s->timestamp = n ;

        } else if (!strcmp(key, "SEAT")) {

            if (!session_set(s->seat, sizeof(s->seat), val))
                return 0 ;

        } else if (!strcmp(key, "TTY")) {

            if (!session_set(s->tty, sizeof(s->tty), val))
                return 0 ;

        } else if (!strcmp(key, "DISPLAY")) {

            if (!session_set(s->display, sizeof(s->display), val))
                return 0 ;

        } else if (!strcmp(key, "SERVICE")) {

            if (!session_set(s->service, sizeof(s->service), val))
                return 0 ;

        } else if (!strcmp(key, "REMOTE_USER")) {

            if (!session_set(s->remote_user, sizeof(s->remote_user), val))
                return 0 ;

        } else if (!strcmp(key, "REMOTE_HOST")) {

            if (!session_set(s->remote_host, sizeof(s->remote_host), val))
                return 0 ;
        }
    }

    return 1 ;
}

int session_deserialize(session_t *s, char const *block)
{
    _cleanup_strbuf_ strbuf tmp = STRBUF_ZERO ;

    if (!environ_merge_string(&tmp, block))
        return 0 ;

    return session_from_env(s, &tmp) ;
}
