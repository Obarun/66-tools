/*
 * execl-subuidgid.c
 *
 * Copyright (c) 2018 Eric Vidal <eric@obarun.org>
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
#include <errno.h>
#include <pwd.h>

#include <oblibs/log.h>
#include <oblibs/environ.h>
#include <oblibs/sbl.h>
#include <oblibs/string.h>
#include <oblibs/strbuf.h>
#include <oblibs/subst.h>
#include <oblibs/opt.h>
#include <oblibs/types.h>
#include <oblibs/exec.h>

static opt_t const opts[] = {
    { .id = OPT_ID_HELP, .shortname = 'h', .help = "print this help" },
    { .id = 'o',         .shortname = 'o', .arg = OPT_REQUIRED, .argname = "owner", .help = "owner to use" },
} ;

static opt_cmd_t const cmd = {
    .name = "execl-subuidgid",
    .operands = "prog...",
    .opts = opts,
    .nopts = OPT_COUNT(opts),
} ;

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
    _cleanup_strbuf_ strbuf sa = STRBUF_ZERO ;
    _cleanup_strbuf_ strbuf dst = STRBUF_ZERO ;
    subst_t info = SUBST_ZERO ;
    char cuid[UID_FMT], cgid[GID_FMT] ;

    PROG = "execl-subuidgid" ;

    {
        opt_scan_t st = OPT_SCAN_ZERO ;

        for (;;)
        {
          int o = opt_scan(argc, argv, opts, OPT_COUNT(opts), &st) ;
          if (o == OPT_END) break ;
          switch (o)
          {
            case OPT_ID_HELP :  return opt_emit_help(cmd.name, &cmd) ;
            case 'o' :  owner = st.arg ; break ;
            default :   return opt_emit_error(cmd.name, &cmd, o, &st) ;
          }
        }
        argc -= st.ind ; argv += st.ind ;
    }
    if (owner)
    {
        if (!youruid(&uid,owner)) log_dieusys(LOG_EXIT_SYS,"get uid of: ",owner) ;
    }
    else uid = getuid() ;

    if (!yourgid(&gid,uid)) log_dieusys(LOG_EXIT_SYS,"get gid") ;
    cuid[uid_format(cuid,uid)] = 0 ;
    cgid[gid_format(cgid,gid)] = 0 ;

    _alloc_strbuf_(ukey, 4 + strlen(cuid) + 1) ;
    _alloc_strbuf_(gkey, 4 + strlen(cgid) + 1) ;
    auto_strings(ukey.s, "UID=", cuid) ;
    auto_strings(gkey.s, "GID=", cgid) ;

    if (!sbl_add(&sa, ukey.s) ||
        !sbl_add(&sa, gkey.s))
            log_die_nomem("strbuf") ;

    if (!environ_substitute(&sa, &info))
        log_dieusys(LOG_EXIT_SYS, "substitue environment variables") ;

    sa.len = 0 ;

    if (!environ_import_arguments(&sa, argv, argc))
        log_dieusys(LOG_EXIT_SYS, "import arguments to environment") ;

    r = subst(&dst, sa.s, sa.len, &info) ;
    if (r < 0) log_dieusys(LOG_EXIT_SYS,"subst") ;
    else if (!r) _exit(0) ;

    strbuf_free(&sa) ;

    {
        char const *v[r + 1];
        if (!environ_make (v, r, dst.s, dst.len)) log_dieusys(LOG_EXIT_SYS, "environ_make") ;
        v[r] = 0 ;
        exec_path_merge (v[0], v, envp, info.modifs.s, info.modifs.len) ;
    }

    return 0 ;
}
