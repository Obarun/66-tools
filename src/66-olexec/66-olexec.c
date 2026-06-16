/*
 * 66-olexec.c
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
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <termios.h>
#include <errno.h>

#include <oblibs/string.h>
#include <oblibs/files.h>
#include <oblibs/log.h>
#include <oblibs/opt.h>
#include <oblibs/io.h>
#include <oblibs/spawn.h>
#include <oblibs/process.h>

#define TTY_LEN 256
#define PREFIX "/sys/class/tty/"
#define PREFIX_LEN sizeof(PREFIX) - 1
#define NAME "/active"
#define NAME_LEN sizeof(NAME) - 1

static char current_tty[TTY_LEN] ;

static opt_t const opts[] = {
    { .id = OPT_ID_HELP, .shortname = 'h', .longname = "help", .arg = OPT_NONE,                      .help = "print this help" },
    { .id = 'd',         .shortname = 'd', .longname = "tty",  .arg = OPT_REQUIRED, .argname = "tty", .help = "absolute path of tty to use" },
} ;

static opt_cmd_t const cmd = {
    .name = "66-olexec",
    .operands = "program",
    .opts = opts,
    .nopts = OPT_COUNT(opts),
} ;

/** this function is largely inspired by jjk-jacky at
 * https://github.com/jjk-jacky/anopa/blob/master/src/utils/aa-tty.c */

void get_current_tty(void)
{
    int r ;
    size_t skip, max ;
    auto_strings(current_tty, PREFIX, "console", NAME) ;

    max = file_get_size(current_tty) ;
    char name[max] ;

    r = file_read (current_tty, name, max) ;
    if (r <= 0)
        log_dieusys(LOG_EXIT_SYS, "read: ", current_tty) ;

    /** position right after the last space, or 0 if none */
    skip = 0 ;
    for (size_t k = r ; k ; k--)
        if (name[k-1] == ' ') { skip = k ; break ; }

    for (;;)
    {
        const char *s = name + skip ;
        size_t l = r - skip ;

        memcpy(current_tty + PREFIX_LEN,s,l) ;
        memcpy(current_tty + PREFIX_LEN + l - 1, NAME, NAME_LEN) ;
        current_tty[PREFIX_LEN + l - 1 + NAME_LEN] = 0 ;

        r = file_read (current_tty, name, max) ;

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

    PROG = "66-olexec" ;
    {
        opt_scan_t st = OPT_SCAN_ZERO ;

        for (;;)
        {
            int o = opt_scan(argc, argv, opts, OPT_COUNT(opts), &st) ;
            if (o == OPT_END) break ;
            switch (o)
            {
                case OPT_ID_HELP : return opt_emit_help(cmd.name, &cmd) ;
                case 'd' : dev = st.arg ; break ;
                default :  return opt_emit_error(cmd.name, &cmd, o, &st) ;
            }
        }
        argc -= st.ind ; argv += st.ind ;
    }

    if (!argc) return opt_emit_usage(cmd.name, &cmd) ;

    if (getuid() != 0) log_die(LOG_EXIT_SYS,"only superuser can run this program") ;

    if (!dev)
    {
        get_current_tty() ;
        dev = current_tty ;
    }

    close(0) ;
    close(1) ;
    fd = io_open(dev, O_RDWR) ;
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

    char const *nargv[argc+1] ;

    for (i = 0 ; i < argc; i++)
        nargv[i] = argv[i] ;

    nargv[i] = 0 ;

    pid = spawn_path(nargv[0],nargv,envp) ;
    if (process_wait(pid,&wstat) < 0)
        log_dieusys(LOG_EXIT_SYS,"wait for: ", nargv[0]) ;

    if (wstat)
        flog_die(LOG_EXIT_SYS, "%s %s with exitcode: %d", nargv[0], WIFSIGNALED(wstat) ? " failed " : " crashed ", WIFSIGNALED(wstat) ? WTERMSIG(wstat) : WEXITSTATUS(wstat)) ;

    flock(fd,LOCK_UN) ;
    close(fd) ;

    return 0 ;
}
