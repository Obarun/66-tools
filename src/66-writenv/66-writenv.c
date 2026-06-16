/*
 * 66-writenv.c
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
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>//fsync,close
#include <errno.h>

#include <oblibs/log.h>
#include <oblibs/string.h>
#include <oblibs/types.h>
#include <oblibs/opt.h>
#include <oblibs/stream.h>
#include <oblibs/io.h>

#define MAX_ENV 4095

static opt_t const opts[] = {
    { .id = OPT_ID_HELP, .shortname = 'h', .longname = "help", .arg = OPT_NONE,                       .help = "print this help" },
    { .id = 'm',         .shortname = 'm', .longname = "mode", .arg = OPT_REQUIRED, .argname = "mode", .help = "create dir with given mode" },
} ;

static opt_cmd_t const cmd = {
    .name = "66-writenv",
    .operands = "dir file",
    .opts = opts,
    .nopts = OPT_COUNT(opts),
} ;

int main (int argc, char const *const *argv, char const *const *envp)
{
    uint32_t mode = S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH ;
    size_t dirlen, filen ;
    ostream b ;
    int fd ;
    char const *dir = 0 , *file = 0 ;
    char buf[MAX_ENV+1] ;
    PROG = "66-writenv" ;
    {
        opt_scan_t st = OPT_SCAN_ZERO ;

        for (;;)
        {
            int o = opt_scan(argc, argv, opts, OPT_COUNT(opts), &st) ;
            if (o == OPT_END) break ;
            switch (o)
            {
                case OPT_ID_HELP :  return opt_emit_help(cmd.name, &cmd) ;
                case 'm' :  if (!u32_scan_strict_base(st.arg, &mode, 8)) return opt_emit_usage(cmd.name, &cmd) ; break ;
                default :   return opt_emit_error(cmd.name, &cmd, o, &st) ;
            }
        }
        argc -= st.ind ; argv += st.ind ;
    }
    if (argc < 2) return opt_emit_usage(cmd.name, &cmd) ;
    dir = argv[0] ;
    if (dir[0] != '/') log_die(LOG_EXIT_USER,dir," must be an absolute path") ;
    file = argv[1] ;

    if (mkdir(dir, mode) < 0)
    {
        struct stat st ;
        if (errno != EEXIST) log_dieusys(LOG_EXIT_SYS, "mkdir ", dir) ;
        if (stat(dir, &st) < 0)
            log_dieusys(LOG_EXIT_SYS, "stat ", dir) ;
        if (!S_ISDIR(st.st_mode))
        {
            errno = ENOTDIR ;
            log_dieusys(LOG_EXIT_SYS, "mkdir ", dir) ;
        }
    }
    dirlen = strlen(dir) ;
    filen = strlen(file) ;
    char fn[dirlen + 1 + filen + 1] ;
    auto_strings(fn, dir, "/", file) ;
    fd = io_open_mode(fn, O_WRONLY | O_NONBLOCK | O_TRUNC | O_CREAT, 0666) ;
    if (fd < 0) log_dieusys(LOG_EXIT_SYS,"open trunc: ",fn) ;
    ostream_init(&b,fd,buf,MAX_ENV) ;
    for (; *envp ; envp++)
    {
        if ((!ostream_put(&b, *envp,strlen(*envp))) ||
        (!ostream_put(&b, "\n",1))) { close(fd) ; log_dieusys(LOG_EXIT_SYS,"write buffer") ; }
    }
    if(!ostream_flush(&b)){ close(fd) ; log_dieusys(LOG_EXIT_SYS,"flush") ; }
    if (fsync(fd) < 0){ close(fd) ; log_dieusys(LOG_EXIT_SYS,"sync") ; }
    close(fd) ;
    return 0 ;
}
