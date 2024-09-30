/*
 * execl-subuidgid.c
 *
 * Copyright (c) 2018-2024 Eric Vidal <eric@obarun.org>
 *
 * All rights reserved.
 *
 * This file is part of Obarun. It is subject to the license terms in
 * the LICENSE file found in the top-level directory of this
 * distribution.
 * This file may not be copied, modified, propagated, or distributed
 * except according to the terms contained in the LICENSE file./
 */
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <stdio.h>
#include <errno.h>
#include <pwd.h>

#include <oblibs/log.h>
#include <oblibs/environ.h>
#include <oblibs/sastr.h>
#include <oblibs/string.h>

#include <skalibs/types.h>
#include <skalibs/buffer.h>
#include <skalibs/stralloc.h>
#include <skalibs/env.h>
#include <skalibs/sgetopt.h>
#include <skalibs/djbunix.h>
#include <skalibs/exec.h>

#include <execline/execline.h>

#define USAGE "execl-subuidgid [ -h ] [ -o owner ] prog..."

static inline void info_help (void)
{
  static char const *help =
"execl-subuidgid <options> prog\n"
"\n"
"options :\n"
"   -h: print this help\n"
"   -o: owner to use\n"
;

 if (buffer_putsflush(buffer_1, help) < 0)
    log_dieusys(LOG_EXIT_SYS, "write to stdout") ;
}

/** Implement again this function coming from
 * 66. This is avoid the dependency from it*/
static int youruid(uid_t *passto,char const *owner)
{
    int e ;
    e = errno ;
    errno = 0 ;
    struct passwd *st ;
    if (!(st = getpwnam(owner)))
    {
        if (!errno) errno = ESRCH ;
        return 0 ;
    }
    *passto = st->pw_uid ;
    errno = e ;
    return 1 ;
}

static int yourgid(gid_t *passto,uid_t owner)
{
    int e ;
    e = errno ;
    errno = 0 ;
    struct passwd *st ;
    if (!(st = getpwuid(owner)))
    {
        if (!errno) errno = ESRCH ;
        return 0 ;
    }
    *passto = st->pw_gid ;
    errno = e ;
    return 1 ;
}

int main (int argc, char const **argv, char const *const *envp)
{
    uid_t uid ;
    gid_t gid ;
    int r ;
    char const *owner = 0 ;
    _alloc_sa_(sa) ;
    _alloc_sa_(dst) ;
    exlsn_t info = EXLSN_ZERO;
    char cuid[UID_FMT], cgid[GID_FMT] ;

    PROG = "execl-subuidgid" ;

    {
        subgetopt l = SUBGETOPT_ZERO ;

        for (;;)
        {
          int opt = subgetopt_r(argc, argv, "ho:", &l) ;
          if (opt == -1) break ;
          switch (opt)
          {
            case 'h' :  info_help(); return 0 ;
            case 'o' :  owner = l.arg ; break ;
            default :   log_usage(USAGE) ;
          }
        }
        argc -= l.ind ; argv += l.ind ;
    }
    if (owner)
    {
        if (!youruid(&uid,owner)) log_dieusys(LOG_EXIT_SYS,"get uid of: ",owner) ;
    }
    else uid = getuid() ;

    if (!yourgid(&gid,uid)) log_dieusys(LOG_EXIT_SYS,"get gid") ;
    cuid[uid_fmt(cuid,uid)] = 0 ;
    cgid[gid_fmt(cgid,gid)] = 0 ;

    _alloc_stk_(ukey, 4 + strlen(cuid) + 1) ;
    _alloc_stk_(gkey, 4 + strlen(cgid) + 1) ;
    auto_strings(ukey.s, "UID=", cuid) ;
    auto_strings(gkey.s, "GID=", cgid) ;

    if (!sastr_add_string(&sa, ukey.s) ||
        !sastr_add_string(&sa, gkey.s))
            log_die_nomem("stralloc") ;

    if (!environ_substitute(&sa, &info))
        log_dieusys(LOG_EXIT_SYS, "substitue environment variables") ;

    sa.len = 0 ;

    if (!environ_import_arguments(&sa, argv, argc))
        log_dieusys(LOG_EXIT_SYS, "import arguments to environment") ;

    r = el_substitute (&dst, sa.s, sa.len, info.vars.s, info.values.s,
        genalloc_s (elsubst_t const, &info.data),genalloc_len (elsubst_t const, &info.data)) ;
    if (r < 0) log_dieusys(LOG_EXIT_SYS,"el_substitute") ;
    else if (!r) _exit(0) ;

    stralloc_free(&sa) ;

    {
        char const *v[r + 1];
        if (!env_make (v, r, dst.s, dst.len)) log_dieusys(LOG_EXIT_SYS, "env_make") ;
        v[r] = 0 ;
        mexec_fm (v, envp, env_len (envp), info.modifs.s, info.modifs.len) ;
    }

    return 0 ;
}
