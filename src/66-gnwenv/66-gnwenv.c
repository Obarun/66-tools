/*
 * 66-gnwenv.c
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

#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>

#include <oblibs/string.h>
#include <oblibs/log.h>
#include <oblibs/types.h>
#include <oblibs/opt.h>
#include <oblibs/environ.h>
#include <oblibs/exec.h>
#include <oblibs/io.h>
#include <oblibs/fd.h>

#define MAX_ENV 4095
static char const *pattern = 0 ;
static unsigned int EXACT = 0 ;

static opt_t const opts[] = {
    { .id = OPT_ID_HELP, .shortname = 'h', .longname = "help",  .arg = OPT_NONE,                        .help = "print this help" },
    { .id = 'x',         .shortname = 'x', .longname = "exact", .arg = OPT_NONE,                        .help = "match exactly with the process name" },
    { .id = 'm',         .shortname = 'm', .longname = "mode",  .arg = OPT_REQUIRED, .argname = "mode", .help = "create dir with given mode" },
} ;

static opt_cmd_t const cmd = {
    .name = "66-gnwenv",
    .operands = "process dir file",
    .opts = opts,
    .nopts = OPT_COUNT(opts),
} ;

static void string_env(char *tmp,char const *s,size_t len)
{
    size_t pos = 0 ;
    ssize_t r = 0 ;

    while ((pos < len) && (r != -1))
    {
        r = get_len_until(s+pos,'\n') ;
        memcpy(tmp+pos,s+pos,r) ;
        tmp[pos+r] = 0 ;
        pos += ++r ;/**+1 to skip the \n character*/
    }
}

static unsigned int get_nbline(char const *str, size_t len)
{
    unsigned int pos, loop ;
    ssize_t r = 0 ;
    pos = loop = 0 ;


    while ((pos < len) && (r != -1))
    {
        r = get_len_until(str+pos,'\n') ;
        pos = r+pos+1 ;//+1 to skip the \n character
        loop++ ;
    }

    return loop ;
}

int main (int argc, char const *const *argv)
{
    int r = 0 , pf, rm = 0, m = 0, fd[2], did = 0 ;
    ssize_t slen = 0 ;

    uint32_t mode = S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH ;

    char const *dir = 0 , *file = 0 ;
    char const *newargv[6+1] ;
    char const *newread[6+1] ;
    char md[U32_OFMT] ;
    char buf[MAX_ENV+1] ;
    char tmp[MAX_ENV+1] ;

    PROG = "66-gnwenv" ;
    {
        opt_scan_t st = OPT_SCAN_ZERO ;

        for (;;)
        {
            int o = opt_scan(argc, argv, opts, OPT_COUNT(opts), &st) ;
            if (o == OPT_END) break ;
            switch (o)
            {
                case OPT_ID_HELP : return opt_emit_help(cmd.name, &cmd) ;
                case 'x' :  EXACT = 1 ; break ;
                case 'm' :  if (!u32_scan_strict_base(st.arg, &mode, 8)) return opt_emit_usage(cmd.name, &cmd) ; did = 1 ; break ;
                default :   return opt_emit_error(cmd.name, &cmd, o, &st) ;
            }
        }
        argc -= st.ind ; argv += st.ind ;
    }
    if (argc < 3) return opt_emit_usage(cmd.name, &cmd) ;
    pattern = argv[0] ;
    dir = argv[1] ;
    if (dir[0] != '/') log_die(LOG_EXIT_USER,dir," must be an absolute path") ;
    file = argv[2] ;

    newread[rm++] = "66-getenv" ;
    if (EXACT)
        newread[rm++] = "-x" ;
    newread[rm++] = pattern ;
    newread[rm++] = dir ;
    newread[rm++] = file ;
    newread[rm++] = 0 ;

    if (pipe(fd) < 0) log_dieusys(LOG_EXIT_SYS,"pipe") ;
    pf = fork() ;
    if (pf < 0) log_dieusys(LOG_EXIT_SYS,"fork") ;
    if (!pf)
    {
        dup2(fd[1],1) ;
        exec_path(newread[0], newread, (char const *const *)environ) ;
    }
    else
    {
        close_fd(fd[1]) ;
        wait(NULL) ;
        slen = io_allread(fd[0],buf,MAX_ENV) ;
        if (!slen) return 0 ;
        buf[slen] = 0 ;
    }
    r = get_nbline(buf,slen) ;

    string_env(tmp,buf,slen) ;

    md[u32_ofmt(md,mode)] = 0 ;

    newargv[m++] = "66-writenv" ;
    if (did)
    {
        newargv[m++] = "-m" ;
        newargv[m++] = md ;
    }
    newargv[m++] = dir ;
    newargv[m++] = file ;
    newargv[m++] = 0 ;

    char const *v[r + 1] ;
    if (!environ_make(v, r ,tmp, slen)) log_dieusys(LOG_EXIT_SYS,"make environment") ;
    v[r] = 0 ;

    exec_path_die(newargv[0], newargv, v) ;
}
