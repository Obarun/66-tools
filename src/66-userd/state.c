/*
 * state.c
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

#include "oblibs/sbl.h"
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <oblibs/string.h>
#include <oblibs/directory.h>
#include <oblibs/environ.h>
#include <oblibs/files.h>
#include <oblibs/types.h>
#include <oblibs/strbuf.h>

#include "constants.h"
#include "userd.h"
#include "common.h"
#include "session.h"
#include "state.h"

static int state_read(strbuf *env, char const *statedir, char const *sub, char const *name)
{
    _alloc_strbuf_(path, strlen(statedir) + strlen(sub) + 1 + strlen(name) + 1) ;

    auto_strbuf(&path, statedir, sub, "/", name) ;

    _cleanup_strbuf_ strbuf tmp = STRBUF_ZERO ;

    if (!environ_merge_file(&tmp, path.s))
        return 0 ;

    return strbuf_copyb(env, tmp.s, tmp.len) ;

}

static int state_write(char const *statedir, char const *sub, char const *name, strbuf *sb)
{
    _alloc_strbuf_(dir, strlen(statedir) + strlen(sub) + 1 + strlen(name) + 1) ;

    auto_strbuf(&dir, statedir, sub, "/", name) ;

    return file_write(dir.s, sb->s, sb->len) ;
}

static int state_remove(char const *statedir, char const *sub, char const *name)
{
    _alloc_strbuf_(path, strlen(statedir) + strlen(sub) + 1 + strlen(name) + 1) ;

    auto_strbuf(&path, statedir, sub, "/", name) ;

    if (unlink(path.s) < 0 && errno != ENOENT)
        return 0 ;

    return 1 ;
}

int state_init(char const *statedir)
{
    _alloc_strbuf_(sdir, strlen(statedir) + sizeof(USERD_SESSIONS_SUB) + 1) ;

    auto_strbuf(&sdir, statedir, USERD_SESSIONS_SUB) ;

    if (!dir_create_parent(sdir.s, 0755))
        return 0 ;

    sdir.len = 0 ;
    auto_strbuf(&sdir, statedir, USERD_USERS_SUB) ;

    if (!dir_create_parent(sdir.s, 0755))
        return 0 ;

    return 1 ;
}

int state_save_session(char const *statedir, session_t const *s)
{
    _alloc_strbuf_(sb, STRBUF_THRESHOLD + 1) ;

    if (!session_serialize(s, &sb))
        return 0 ;

    return state_write(statedir, USERD_SESSIONS_SUB, s->id, &sb) ;
}

int state_load_session(char const *statedir, char const *id, session_t *s)
{
    _alloc_strbuf_(sb, STRBUF_THRESHOLD + 1) ;

    if (!state_read(&sb, statedir, USERD_SESSIONS_SUB, id))
        return 0 ;

    return session_from_env(s, &sb) ;
}

int state_remove_session(char const *statedir, char const *id)
{
    return state_remove(statedir, USERD_SESSIONS_SUB, id) ;
}

int state_save_user(char const *statedir, user_t const *u, char const *sessions)
{
    char uidbuf[UID_FMT] ;
    _alloc_strbuf_(sb, STRBUF_THRESHOLD + 1) ;

    uidbuf[uid_format(uidbuf, u->uid)] = 0 ;

    int ok = sf_str(&sb, "NAME", u->name) &&
             sf_u64(&sb, "GID", u->gid) &&
             sf_str(&sb, "STATE", user_state_str(u->state)) &&
             sf_u64(&sb, "NSESSIONS", u->nsessions) &&
             sf_str(&sb, "SESSIONS", sessions) ;
    /** Persist the supervised guardian's identity so a restart can re-adopt it.
     * Only meaningful while one exists. */
    if (ok && u->guardian_pid > 0) {
        ok = sf_u64(&sb, "GUARDIAN_PID", (uint64_t)u->guardian_pid) &&
             sf_u64(&sb, "GUARDIAN_STARTTIME", (uint64_t)u->guardian_starttime) ;
    }

    if (ok)
        ok = sf_u64(&sb, "TIMESTAMP", u->timestamp) ;

    if (!ok)
        return 0 ;

    return state_write(statedir, USERD_USERS_SUB, uidbuf, &sb) ;
}

int state_load_user(char const *statedir, uid_t uid, user_t *u, char *sessions, size_t sessionsmax)
{
    char uidbuf[UID_FMT] ;
    _alloc_strbuf_(env, STRBUF_THRESHOLD + 1) ;

    uidbuf[uid_format(uidbuf, uid)] = 0 ;

    if (!state_read(&env, statedir, USERD_USERS_SUB, uidbuf))
        return 0 ;

    u->uid = uid ;
    if (sessions)
        sessions[0] = 0 ; // no SESSIONS key -> no live sessions listed

    size_t pos = 0 ;

    FOREACH_SBL(&env, pos) {

        char *kv = env.s + pos ;
        char *eq = strchr(kv, '=') ;
        if (!eq)
            continue ;
        *eq = 0 ;

        char const *key = kv ;
        char const *val = eq + 1 ;

        if (!strcmp(key, "NAME")) {

            if (!session_set(u->name, sizeof(u->name), val))
                return 0 ;
        }
        else if (!strcmp(key, "GID")) {

            gid_parse(val, &u->gid) ;

        } else if (!strcmp(key, "STATE")) {

            u->state = user_state_from(val) ;

        } else if (!strcmp(key, "NSESSIONS")) {

            u32_scan(val, &u->nsessions) ;

        } else if (!strcmp(key, "TIMESTAMP")) {

            uint64_t ts ;
            if (u64_scan(val, &ts))
                u->timestamp = ts ;

        } else if (!strcmp(key, "SESSIONS")) {
            if (sessions) {
                size_t vl = strlen(val) ;
                if (vl >= sessionsmax)
                    return (errno = EOVERFLOW, 0) ;
                memcpy(sessions, val, vl + 1) ;
            }
        }
    }

    return 1 ;
}

int state_load_user_guardian(char const *statedir, uid_t uid, pid_t *pid, unsigned long *starttime)
{
    char uidbuf[UID_FMT] ;
    _alloc_strbuf_(env, STRBUF_THRESHOLD + 1) ;

    uidbuf[uid_format(uidbuf, uid)] = 0 ;

    if (!state_read(&env, statedir, USERD_USERS_SUB, uidbuf))
        return 0 ;

    // Keys absent (no guardian was running) -> pid 0, not an error.
    *pid = 0 ;
    *starttime = 0 ;

    size_t pos = 0 ;

    FOREACH_SBL(&env, pos) {

        char *kv = env.s + pos ;
        char *eq = strchr(kv, '=') ;
        if (!eq)
            continue ;
        *eq = 0 ;

        if (!strcmp(kv, "GUARDIAN_PID")) {

            pid_parse(eq + 1, pid) ;

        } else if (!strcmp(kv, "GUARDIAN_STARTTIME")) {

            uint64_t n ;
            if (u64_scan(eq + 1, &n))
                *starttime = (unsigned long)n ;
        }
    }

    return 1 ;
}

int state_remove_user(char const *statedir, uid_t uid)
{
    char uidbuf[UID_FMT] ;
    uidbuf[uid_format(uidbuf, uid)] = 0 ;

    return state_remove(statedir, USERD_USERS_SUB, uidbuf) ;
}
