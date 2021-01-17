/*
 * 66-olexec.c
 *
 * Copyright (c) 2018-2020 Eric Vidal <eric@obarun.org>
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
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <termios.h>
#include <errno.h>

#include <oblibs/string.h>
#include <oblibs/files.h>
#include <oblibs/log.h>

#include <skalibs/sgetopt.h>
#include <skalibs/buffer.h>
#include <skalibs/djbunix.h>
#include <skalibs/types.h>
#include <skalibs/bytestr.h>

#define TTY_LEN 256
#define PREFIX "/sys/class/tty/"
#define PREFIX_LEN sizeof(PREFIX) - 1
#define NAME "/active"
#define NAME_LEN sizeof(NAME) - 1

static char current_tty[TTY_LEN] ;

#define USAGE "66-olexec [ -h ] [ -d tty ] program"

static inline void info_help (void)
{
  static char const *help =
"66-olexec <options> program\n"
"\n"
"options :\n"
"   -h: print this help\n"
"   -d: absolute path of tty to use\n"
;
    if (buffer_putsflush(buffer_1, help) < 0)
        log_dieusys(LOG_EXIT_SYS, "write to stdout") ;
}

/** this function is largely inspired by jjk-jacky at
 * https://github.com/jjk-jacky/anopa/blob/master/src/utils/aa-tty.c */

void get_current_tty(void)
{
    int r ;
    size_t skip, max ;
    memcpy(current_tty, PREFIX, PREFIX_LEN) ;
    memcpy(current_tty + PREFIX_LEN, "console", 7) ;
    memcpy(current_tty + PREFIX_LEN + 7, NAME, NAME_LEN) ;
    current_tty[PREFIX_LEN + 7 + NAME_LEN] = 0 ;

    max = file_get_size(current_tty) ;
    char name[max] ;

    r = openreadnclose (current_tty, name, max) ;
    if (r <= 0)
        log_dieusys(LOG_EXIT_SYS, "read: ", current_tty) ;

    skip = byte_rchr (name, r, ' ') + 1 ;

    if (skip > (size_t) r) skip = 0 ;

    for (;;)
    {
        const char *s = name + skip ;
        size_t l = r - skip ;

        memcpy(current_tty + PREFIX_LEN,s,l) ;
        memcpy(current_tty + PREFIX_LEN + l - 1, NAME, NAME_LEN) ;
        current_tty[PREFIX_LEN + l - 1 + NAME_LEN] = 0 ;

        r = openreadnclose (current_tty, name, max) ;

        if (r <= 0)
        {
            if (errno == ENOENT)
            {
                memcpy(current_tty,"/dev/",5) ;
                memcpy(current_tty + 5,s,l-1) ;
                current_tty[5+l-1] = 0 ;
                return ;
            }
            else
                log_dieusys(LOG_EXIT_SYS, "read: ", current_tty);
        }
        skip = 0;
    }
}

int main(int argc, char const *const *argv,char const *const *envp)
{
    char const *dev = 0 ;
    int r, fd, wstat = 0 , i ;
    pid_t pid ;
    char efmt[UINT_FMT] ;

    PROG = "66-olexec" ;
    {
        subgetopt_t l = SUBGETOPT_ZERO ;
        for (;;)
        {
            int opt = subgetopt_r(argc, argv, "hd:", &l) ;
            if (opt == -1) break ;
            switch (opt)
            {
                case 'h': info_help() ; return 0 ;
                case 'd': dev = l.arg ; break ;
                default : log_usage(USAGE) ;
            }
        }
        argc -= l.ind ; argv += l.ind ;
    }

    if (!argc) log_usage(USAGE) ;

    if (getuid() != 0) log_die(LOG_EXIT_SYS,"only superuser can run this program") ;

    if (!dev)
    {
        get_current_tty() ;
        dev = current_tty ;
    }

    close(0) ;
    close(1) ;
    fd = open(dev, O_RDWR,0666) ;
    if (fd < 0) log_dieusys(LOG_EXIT_SYS,"open: ",dev) ;
    dup(fd) ;
    close(2) ;
    dup(fd) ;

    /** we lock the fd anyway, maybe is useless to use it */
    if (ioctl(fd,TIOCEXCL) == -1) log_dieusys(LOG_EXIT_SYS,"get exclusivity of: ",dev) ;
    if (!isatty(fd)) log_die(LOG_EXIT_SYS,"not a tty device") ;
    r = flock(fd, LOCK_EX | LOCK_NB);
    if ((r == -1) && (errno == EWOULDBLOCK))
        log_dieu(LOG_EXIT_SYS,"lock: ",dev," -- it locked by another process");

    char const *cmd[argc+1] ;

    for (i = 0 ; i < argc; i++)
        cmd[i] = argv[i] ;

    cmd[i] = 0 ;

    pid = child_spawn0(cmd[0],cmd,envp) ;
    if (waitpid_nointr(pid,&wstat, 0) < 0)
        log_dieusys(LOG_EXIT_SYS,"wait for: ", cmd[0]) ;

    if (wstat)
    {
        efmt[uint_fmt(efmt, WIFSIGNALED(wstat) ? WTERMSIG(wstat) : WEXITSTATUS(wstat))] = 0 ;
        log_die(LOG_EXIT_SYS,cmd[0],WIFSIGNALED(wstat) ? " failed " : " crashed ", "with exitcode: ",efmt) ;
    }

    flock(fd,LOCK_UN) ;
    close(fd) ;

    return 0 ;
}
