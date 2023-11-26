/*
 * execl-toc.c
 *
 * Copyright (c) 2018-2021 Eric Vidal <eric@obarun.org>
 *
 * All rights reserved.
 *
 * This file is part of Obarun. It is subject to the license terms in
 * the LICENSE file found in the top-level directory of this
 * distribution.
 * This file may not be copied, modified, propagated, or distributed
 * except according to the terms contained in the LICENSE file./
 *
 * This program is inspired by https://github.com/skarnet/s6/blob/master/src/conn-tools/s6-ipcserver-socketbinder.c
 * and https://github.com/skarnet/s6-portable-utils/blob/master/src/skaembutils/s6-test.c.
 *
 */

#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>//getenv
#include <stdio.h>//getenv
#include <sys/mount.h>

#include <oblibs/log.h>
#include <oblibs/directory.h>
#include <oblibs/types.h>
#include <oblibs/sastr.h>
#include <oblibs/string.h>

#include <skalibs/buffer.h>
#include <skalibs/types.h>
#include <skalibs/sgetopt.h>
#include <skalibs/djbunix.h>
#include <skalibs/cspawn.h>
#include <skalibs/socket.h>
#include <skalibs/exec.h>

#define USAGE "execl-toc [ -h ] [ -v verbosity ] [ -n ] [ -t ] [ -D ] [ -X ] [ -d|p|S|m|L|e|b|c|k|n|g|r|s|t|u|w|x|f|z|O|U|N|V|E element ] [ -o opts ] [ -t type ] [ -d device ] [ -g gid ] [ -u uid ] [ -m mode ] [ -M mode ] [ -s|D|B|b ] [ -b backlog ] prog..."

static inline void info_help (void)
{
  static char const *help =
"execl-toc <main_options> <test_options> element <create_options> prog\n"
"\n"
"main_options :\n"
"   -h: print this help\n"
"   -v: increase/decrease verbosity\n"
"   -n: negate the test\n"
"   -t: exit 0 if the test fails\n"
"   -D: only test and disable creation\n"
"   -X: do not execute prog\n"
"\n"
"test_options :\n"
"\n"
"   test and create\n\n"
"   -d: true if file is a directory\n"
"   -p: true if file is a pipe\n"
"   -S: true if file is a socket\n"
"   -m: true if file is a mountpoint\n"
"   -L: true if file is a symlink\n"
"\n"
"   test only\n\n"
"   -e: true if file exists\n"
"   -b: true if file is a block\n"
"   -c: true if file is a character\n"
"   -k: true if sticky bit is set for file\n"
"   -n: true if the length of file is non-zero\n"
"   -g: true if file is set-group-id\n"
"   -r: true if file is readable\n"
"   -s: true if the size of file is greater than zero\n"
"   -t: true if file is open and refers to a terminal\n"
"   -u: true if set-user-id bit is set for file\n"
"   -w: true if file is writable\n"
"   -x: true if file is executable\n"
"   -f: true if file is regular file\n"
"   -z: true if the length of file is zero\n"
"   -O: true if file is owned by the effective user id\n"
"   -U: true if file is owned by the effective group id\n"
"   -N: true if file has been modified since it was last read\n"
"   -V: true if file exists on the environment\n"
"   -E: true if file is an empty directory\n"
"\n"
"create_options :\n"
"   -o: mount options(same as mount -o)\n"
"   -t: type mount options(same as mount -t), target for symlink\n"
"   -d: device mount options (same as mount -t type device /directory)\n"
"   -u: changes element's owner to (numeric/name) uid after the creation\n"
"   -g: changes element's owner to (numeric/name) gid after the creation\n"
"   -m: creates element's permissions to (numeric) mode.\n"
"   -M: creates parent directories of the *element* with permissions to (numeric) mode.\n"
"   -s: type of element will be SOCK_DGRAM instead of SOCK_STREAM\n"
"   -D: disallow instant rebinding at element to the same path\n"
"   -B: the element will be blocking.\n"
"   -b: set a maximum of backlog connections on the element.\n"
;

 if (buffer_putsflush(buffer_1, help) < 0)
    log_dieusys(LOG_EXIT_SYS, "write to stdout") ;
}

enum opts_e
{
  // can create
  T_DIR , // -d expect mode chown
  T_FIFO , // -p expect mode chown
  T_SOCKET , // -S expect mode chow style
  T_MNT ,// -m mountpoint expect mount_opts device type
  T_SYMLINK , // -L
   // just test
  T_EXIST , // -e create empty file
  T_BLOCK , // -b
  T_CHAR , // -c
  T_STICKY , // -k
  T_NONZERO , // -n
  T_SGID , // -g
  T_READABLE , // -r
  T_NONZEROFILE , // -s
  T_TERM , // -t fd
  T_SUID , // -u
  T_WRITABLE , // -w
  T_EXECUTABLE , // -x
  T_REGULAR , // -f
  T_ZERO , // -z
  T_EUID , // -O
  T_EGID , // -U
  T_MODIFIED , // -N
  T_ENV , // -V environment value
  T_EMPTY // -E empty directories
} ;

typedef struct opts_common_s opts_common_t, *opts_common_t_ref ;
struct opts_common_s
{
    char const *func_name ; // name of the function
    char const *test_on ; // argument to test
    uint8_t not_create ; // 0 create, 1 simply test
    uint8_t noprog ; // 0 execute prog, 1 do not execute prog
    int argc ;
    char **argv ;
    char const *const *envp ;
    enum opts_e test ;
    char const *minus_o ; // -o -> opt for mount
    char const *minus_t ; // -t -> type for mount, target for symlink
    char const *minus_d ; // -d -> device for mount
    gid_t minus_g ; // -g
    uid_t minus_u ; // -u
    mode_t minus_m ; // -m ou -a for socket
    mode_t minus_M ; // -M directory of the element permissions
    // socket option
    uint8_t minus_s ; // -s -> 0 inactive meaning SOCK_STREAM(default), 1 active SOCK_DGRAM
    int minus_D ; //-D -> 0 inactive(default), 1 active
    unsigned int minus_B ; //-B -> O_NONBLOCK(default), 0 blocking
    int minus_b ; // -b -> -1 inactive(default), other value active
} ;

#define OPTS_COMMON_ZERO \
{ \
    0 , \
    0 , \
    0 , \
    0 , \
    0 , \
    0 , \
    0 , \
    0 , \
    0 , \
    0 , \
    0 , \
    -1 , \
    -1 , \
    0 , \
    0 , \
    0 , \
    0 , \
    O_NONBLOCK , \
    -1  \
}

typedef int execl_func_t (opts_common_t *arguments, char **nargv) ;
typedef execl_func_t *execl_func_t_ref ;

execl_func_t execl_directory ;
execl_func_t execl_fifo ;
execl_func_t execl_socket ;
execl_func_t execl_mountpoint ;
execl_func_t execl_symlink ;
execl_func_t execl_common ;

void parse(opts_common_t *arguments,char **nargv)
{
    int n = 0, i = 0 ;
    int argc = arguments->argc ;
    char **argv = arguments->argv ;

    {
        subgetopt l = SUBGETOPT_ZERO ;

        for (;;)
        {
            int opt = subgetopt_r(argc,(char const *const *)argv, "o:t:d:u:g:m:M:sDBb:", &l) ;
            if (opt == -1) break ;
            switch (opt)
            {
                case 'o' :  arguments->minus_o = l.arg ; break ;
                case 't' :  arguments->minus_t = l.arg ; break ;
                case 'd' :  arguments->minus_d = l.arg ; break ;
                case 'u' :  if (!uid0_scan(l.arg, &arguments->minus_u))
                                if (get_uidbyname(l.arg,&arguments->minus_u) == -1)
                                    log_dieusys(LOG_EXIT_SYS,"get uid of: ",l.arg) ;
                            break ;
                case 'g' :
                            {
                                if (!gid0_scan(l.arg, &arguments->minus_g))
                                {
                                    if (get_gidbyname(l.arg,&arguments->minus_g) == 1)
                                    {
                                        stralloc ngid = STRALLOC_ZERO ;
                                        if (get_groupbygid(arguments->minus_g,&ngid) == -1)
                                            log_dieusys(LOG_EXIT_SYS,"get gid of: ",l.arg) ;
                                        if (ngid.len)
                                        {
                                            if (!stralloc_0(&ngid)) log_die_nomem("stralloc") ;
                                            if (get_gidbygroup(ngid.s,&arguments->minus_g) == -1)
                                                log_dieusys(LOG_EXIT_SYS,"get gid of: ",l.arg) ;
                                        }else if (get_gidbygroup(l.arg,&arguments->minus_g) == -1)
                                            log_dieusys(LOG_EXIT_SYS,"get gid of: ",l.arg) ;
                                        stralloc_free(&ngid) ;
                                    }
                                    else if (get_gidbygroup(l.arg,&arguments->minus_g) == -1)
                                        log_dieusys(LOG_EXIT_SYS,"get gid of: ",l.arg) ;
                                }
                                break ;
                            }
                case 'm' :  if (!uint0_oscan(l.arg, &arguments->minus_m)) log_usage(USAGE) ; break ;
                case 'M' :  if (!uint0_oscan(l.arg, &arguments->minus_M)) log_usage(USAGE) ; break ;
                // socket special case
                case 's' :  arguments->minus_s = 1 ; break ;
                case 'D' :  arguments->minus_D = 1 ; break ;
                case 'B' :  arguments->minus_B = 0 ; break ;
                case 'b' :  if (!uint0_scan(l.arg,(unsigned int *)&arguments->minus_b)) log_usage(USAGE) ; break ;
                default :   log_usage(USAGE) ;
            }
        }
        argc -= l.ind ; argv += l.ind ;
    }

    for (; i < argc ; i++)
        nargv[n++] = (char *)argv[i] ;

    nargv[n] = 0 ;
    arguments->argc = n ;
    arguments->argv = nargv ;
}

static int auto_dir(opts_common_t *arguments)
{
    int r ;
    char const *dest = arguments->test_on ;
    size_t len = strlen(dest) ;
    char dir[len + 1] ;
    if (!ob_dirname(dir,dest)) log_dieu(LOG_EXIT_SYS,"get dirname of: ",dest) ;

    mode_t mode = !arguments->minus_M ? 0755 : arguments->minus_M ;

    r = scan_mode(dir,S_IFDIR) ;
    if (r == -1) return 0 ;
    if (!r) {
        if (arguments->minus_M) umask(0) ;
        if (!dir_create_parent(dir,mode))
            log_dieusys(LOG_EXIT_SYS,"create dir: ",dir) ;

        /** dir_create_parent do not warn if the parent directory
         * can not be created, so check it */
        r = scan_mode(dir,S_IFDIR) ;
        if (!r) return 0 ;

        /** only the last element should match the uid and gid
         * given at commandline */
        //if (chown(dir, arguments->minus_u,arguments->minus_g) == -1)
        //  log_dieusys(LOG_EXIT_SYS,"chown: ",dir) ;
    }
    return 1 ;
}

/* @Return -1 doesn't exist
 * @Return 0 if mounted
 * @Return 1 if not mounted */
static int is_mnt(char const *str)
{
    struct stat st;
    size_t slen = strlen(str) ;
    int is_not_mnt = 0 ;
    if (lstat(str,&st) < 0) return -1 ;
    if (S_ISDIR(st.st_mode))
    {
        dev_t st_dev = st.st_dev ; ino_t st_ino = st.st_ino ;
        char p[slen+4] ;
        memcpy(p,str,slen) ;
        memcpy(p + slen,"/..",3) ;
        p[slen+3] = 0 ;
        if (!stat(p,&st))
            is_not_mnt = (st_dev == st.st_dev) && (st_ino != st.st_ino) ;
    }else return 0 ;
    return is_not_mnt ? 1 : 0 ;
}

int execl_directory(opts_common_t *arguments,char **nargv)
{
    int r ;
    parse(arguments,nargv) ;

    if (!arguments->argc && !arguments->noprog) log_die(LOG_EXIT_USER,"missing argument prog") ;

    mode_t mode = !arguments->minus_m ? 0755 : arguments->minus_m ;

    r = scan_mode(arguments->test_on,S_IFDIR) ;
    if (r == -1) log_warn_return(LOG_EXIT_ZERO,arguments->test_on," exist but it is not a directory") ;
    if (!r && !arguments->not_create) {
        if (arguments->minus_m) umask(0) ;
        if (!dir_create_parent(arguments->test_on,mode))
            log_dieusys(LOG_EXIT_SYS,"create dir: ",arguments->test_on) ;

        if (chown(arguments->test_on, arguments->minus_u,arguments->minus_g) == -1)
            log_dieusys(LOG_EXIT_SYS,"chown: ",arguments->test_on) ;
        return 1 ;
    }
    if (!r) log_warn_return(LOG_EXIT_ZERO,arguments->test_on," is not a directory") ;

    return 1 ;
}

int execl_fifo(opts_common_t *arguments,char **nargv)
{
    int r ;

    parse(arguments,nargv) ;

    if (!arguments->argc && !arguments->noprog) log_die(LOG_EXIT_USER,"missing argument prog") ;

    mode_t mode = !arguments->minus_m ? S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH : arguments->minus_m ;

    r = scan_mode(arguments->test_on,S_IFIFO) ;
    if (r == -1) log_warn_return(LOG_EXIT_ZERO,arguments->test_on," exist but it is not a fifo") ;
    if (!r && !arguments->not_create) {
        if (!auto_dir(arguments)) {
            errno = EEXIST ;
            log_diesys(LOG_EXIT_SYS,"conflicting format in parent directories of: ",arguments->test_on) ;
        }

        umask(S_IXUSR|S_IXGRP|S_IXOTH) ;

        if (mkfifo(arguments->test_on,mode) < 0)
            log_dieusys(LOG_EXIT_SYS,"create fifo: ",arguments->test_on) ;

        if (chown(arguments->test_on, arguments->minus_u,arguments->minus_g) == -1)
            log_dieusys(LOG_EXIT_SYS,"chown: ",arguments->test_on) ;
        return 1 ;
    }
    if (!r) log_warn_return(LOG_EXIT_ZERO,arguments->test_on," is not a fifo") ;

    return 1 ;
}

int execl_mountpoint(opts_common_t *arguments,char **nargv)
{
    pid_t pid ;
    int r, wstat ;
    char const *newargv[8] ;
    unsigned int m = 0 ;

    parse(arguments,nargv) ;

    if (!arguments->argc && !arguments->noprog) log_die(LOG_EXIT_USER,"missing argument prog") ;

    if ((!arguments->minus_t || !arguments->minus_d) && !arguments->not_create) log_usage(USAGE) ;

    mode_t mode = !arguments->minus_m ? 0755 : arguments->minus_m ;

    r = is_mnt(arguments->test_on) ;
    if (r == -1 || r == 1)
    {
        if (!arguments->not_create) {
            if (arguments->minus_m) umask(0) ;
            if (!dir_create_parent(arguments->test_on,mode))
                log_dieusys(LOG_EXIT_SYS,"create dir: ",arguments->test_on) ;
            goto mount ;
        }
        log_warn_return(LOG_EXIT_ZERO,arguments->test_on," is not a mountpoint") ;
    }
    if (!r) {
        // mounted, nothing to do
        return 1 ;
    }
    if (r == 1 && arguments->not_create) log_warn_return(LOG_EXIT_ZERO,arguments->test_on," is not a mountpoint") ;

    mount:/*
    log_info("d->",arguments->minus_d) ;
    log_info("test->on",arguments->test_on) ;
    log_info("t->",arguments->minus_t) ;
    log_info("o->",arguments->minus_o) ;
    if (mount(0,arguments->test_on,arguments->minus_t,0,arguments->minus_o) < 0)
        log_dieusys(LOG_EXIT_SYS,"mount: ",arguments->test_on) ;
    */
    newargv[m++] = "mount" ;
    if (arguments->minus_o) {
        newargv[m++] = "-o" ;
        newargv[m++] = arguments->minus_o ;
    }
    newargv[m++] = "-t" ;
    newargv[m++] = arguments->minus_t ;
    newargv[m++] = arguments->minus_d ;
    newargv[m++] = arguments->test_on ;
    newargv[m++] = 0 ;


    pid = child_spawn0(newargv[0],newargv,arguments->envp) ;

    if (waitpid_nointr(pid,&wstat, 0) < 0)
            log_dieusys(LOG_EXIT_SYS,"wait for mount") ;

    if (wstat) log_dieu(LOG_EXIT_SYS,"mount: ",arguments->test_on) ;

    return 1 ;
}

int execl_socket(opts_common_t *arguments,char **nargv)
{
    int r ;
    struct stat st ;
    parse(arguments,nargv) ;

    if (!arguments->argc && !arguments->noprog) log_die(LOG_EXIT_USER,"missing argument prog") ;

    close(0) ;
    int flagdgram = arguments->minus_s ? 1 : 0 ;
    int flag = arguments->minus_B ;
    int flagreuse = arguments->minus_D ? 0 : 1 ;
    unsigned int backlog = arguments->minus_b > 0 ? arguments->minus_b : SOMAXCONN ;
    mode_t mode = !arguments->minus_m ? 0777 : arguments->minus_m ;

    r = stat(arguments->test_on, &st) ;
    if (!r && !S_ISSOCK(st.st_mode)) log_warn_return(LOG_EXIT_ZERO,arguments->test_on," exist but it is not a socket") ;
    if (r == -1 && !arguments->not_create)
    {
        if (!auto_dir(arguments)) {
            errno = EEXIST ;
            log_diesys(LOG_EXIT_SYS,"conflicting format in parent directories of: ",arguments->test_on) ;
        }

        if (flagdgram ? ipc_datagram_internal(flag) : ipc_stream_internal(flag))
            log_dieusys(LOG_EXIT_SYS, "create socket") ;

        {
            mode_t m = umask(~mode & 0777) ;
            if ((flagreuse ? ipc_bind_reuse(0, arguments->test_on) : ipc_bind(0, arguments->test_on)) < 0)
                log_dieusys(LOG_EXIT_SYS,"bind to: ", arguments->test_on) ;
            umask(m) ;
        }
        if (backlog && ipc_listen(0, backlog) < 0)
            log_dieusys(LOG_EXIT_SYS, "listen to: ", arguments->test_on) ;

        return 1 ;
    }
    if (r == -1) log_warn_return(LOG_EXIT_ZERO,arguments->test_on," is not a socket") ;

    return 1 ;
}

int execl_symlink(opts_common_t *arguments,char **nargv)
{
    int r ;
    parse(arguments,nargv) ;

    if (!arguments->argc && !arguments->noprog) log_die(LOG_EXIT_USER,"missing argument prog") ;

    if (!arguments->minus_t) log_usage(USAGE) ;
    struct stat st ;
    r = lstat(arguments->test_on, &st) ;
    if (r == -1 && !arguments->not_create) {
        if (!auto_dir(arguments)) {
            errno = EEXIST ;
            log_diesys(LOG_EXIT_SYS,"conflicting format in parent directories of: ",arguments->test_on) ;
        }
        if (symlink(arguments->minus_t,arguments->test_on) < 0)
            log_dieusys(LOG_EXIT_SYS,"create symlink: ",arguments->test_on) ;

        return 1 ;
    }

    if (!S_ISLNK(st.st_mode)) log_warn_return(LOG_EXIT_ZERO,arguments->test_on," is not a symlink") ;

    return 1 ;
}

int execl_common(opts_common_t *arguments, char **nargv)
{
    int r, n = 0, i = 0, e = 1 ;
    struct stat st ;
    char const *test = arguments->test_on ;
    int argc = arguments->argc ;
    char **argv = arguments->argv ;

    switch (arguments->test)
    {
        case T_EXIST :

                            if (stat(test, &st) == -1)
                                { log_warn(test," doesn't exist") ; e = 0 ; }
                            break ;

        case T_BLOCK :
                            r = scan_mode(test,S_IFBLK) ;
                            if (r == -1){ log_warn(test," exist but it is not a block") ; e = 0 ; }
                            if (!r) { log_warn(test," is not a block") ; e = 0 ; }
                            break ;

        case T_CHAR :
                            r = scan_mode(test,S_IFCHR) ;
                            if (r == -1){ log_warn(test," exist but it is not a character") ; e = 0 ; }
                            if (!r) { log_warn(test," is not a character") ; e = 0 ; }
                            break ;

        case T_REGULAR :
                            r = scan_mode(test,S_IFREG) ;
                            if (r == -1) { log_warn(test," exist but it is not a regular file") ; e = 0 ; }
                            if (!r) { log_warn(test," is not a regular file") ; e = 0 ; }
                            break ;

        case T_SGID :
                            if (stat(test, &st) == -1) {
                                log_warnusys("stat: ",test) ;
                                e = 0 ;
                                break ;
                            }
                            if (!(st.st_mode & S_ISGID))
                                { log_warn(test," is not set-group-id") ; e = 0 ; }
                            break ;

        case T_SUID :
                            if (stat(test, &st) == -1) {
                                log_warnusys("stat: ",test) ;
                                e = 0 ;
                                break ;
                            }
                            if (!(st.st_mode & S_ISUID))
                                { log_warn("set-user-id bit is not set for: ",test) ; e = 0 ; }
                            break ;

        case T_STICKY :
                            if (stat(test, &st) == -1) {
                                log_warnusys("stat: ",test) ;
                                e = 0 ;
                                break ;
                            }
                            if(!(st.st_mode & S_ISVTX))
                                { log_warn("not sticky bit set for: ",test) ; e = 0 ; }
                            break ;

        case T_NONZEROFILE :
                            if (stat(test, &st) == -1) {
                                log_warnusys("stat: ",test) ;
                                e = 0 ;
                                break ;
                            }
                            if (!st.st_size)
                                { log_warn("file size of: ",test," is zero") ; e = 0 ; }
                            break ;

        case T_MODIFIED :
                            if (stat(test, &st) == -1) {
                                log_warnusys("stat: ",test) ;
                                e = 0 ;
                                break ;
                            }
                            if (st.st_mtime < st.st_atime)
                                { log_warn(test," was not modified") ; e = 0 ; }
                            break ;

        case T_EUID :
                            if (stat(test, &st) == -1) {
                                log_warnusys("stat: ",test) ;
                                e = 0 ;
                                break ;
                            }
                            if (st.st_uid != geteuid())
                                { log_warn(test," is not owned by the effective user id") ; e = 0 ; }
                            break ;

        case T_EGID :
                            if (stat(test, &st) == -1) {
                                log_warnusys("stat: ",test) ;
                                e = 0 ;
                                break ;
                            }
                            if (st.st_gid != getegid())
                                { log_warn(test," is not owned by the effective group id") ; e = 0 ; }
                            break ;

        case T_READABLE :
                            if (stat(test, &st) == -1) {
                                log_warnusys("stat: ",test) ;
                                e = 0 ;
                                break ;
                            }
                            if (st.st_uid == geteuid()) {
                                if (!(st.st_mode & S_IRUSR))
                                    { log_warn(test," is not readable") ; e = 0 ; }
                            }
                            else if (st.st_gid == getegid()) {
                                if (!(st.st_mode & S_IRGRP))
                                    { log_warn(test," is not writable") ; e = 0 ; }
                            }
                            else if (!(st.st_mode & S_IROTH)) {
                                { log_warn(test," is not writable") ; e = 0 ; }
                            }
                            break ;

        case T_WRITABLE :
                            if (stat(test, &st) == -1) {
                                log_warnusys("stat: ",test) ;
                                e = 0 ;
                                break ;
                            }
                            if (st.st_uid == geteuid()) {
                                if (!(st.st_mode & S_IWUSR))
                                    { log_warn(test," is not writable") ; e = 0 ; }
                            }
                            else if (st.st_gid == getegid()) {
                                if (!(st.st_mode & S_IWGRP))
                                    { log_warn(test," is not writable") ; e = 0 ; }
                            }
                            else if (!(st.st_mode & S_IWOTH)) {
                                { log_warn(test," is not writable") ; e = 0 ; }
                            }
                            break ;

        case T_EXECUTABLE :
                            if (stat(test, &st) == -1) {
                                log_warnusys("stat: ",test) ;
                                e = 0 ;
                                break ;
                            }
                            if (st.st_uid == geteuid()) {
                                if (!(st.st_mode & S_IXUSR))
                                    { log_warn(test," is not executable") ; e = 0 ; }
                            }
                            else if (st.st_gid == getegid()) {
                                if (!(st.st_mode & S_IXGRP))
                                    { log_warn(test," is not executable") ; e = 0 ; }
                            }
                            else if (!(st.st_mode & S_IXOTH)) {
                                { log_warn(test," is not executable") ; e = 0 ; }
                            }
                            break ;

        case T_TERM :
                        {
                            unsigned int fd ;
                            if (!uint0_scan(test, &fd)) {
                                log_warnusys("scan: ",test) ;
                                e = 0 ;
                                break ;
                            }
                            if (!isatty(fd)) { log_warn(test," do not refer to a terminal") ; e = 0 ; }
                            break ;
                        }

        case T_NONZERO :
                            if (!test[0]) { log_warn(test," is empty") ; e = 0 ; }
                            break ;

        case T_ZERO :
                            if (arguments->test_on[0]) { log_warn(test," is not empty") ; e = 0 ; }
                            break ;

        case T_ENV :
                            if (!getenv(arguments->test_on)) { log_warn("variable: ", test," is not set") ; e = 0 ; }
                            break ;

        case T_EMPTY :
                        {
                            stralloc satree = STRALLOC_ZERO ;
                            char const *exclude[1] = { 0 } ;
                            if (!sastr_dir_get(&satree,test,exclude,S_IFBLK|S_IFCHR|S_IFIFO|S_IFREG|S_IFDIR|S_IFLNK)) {
                                log_warnusys("get contain of directory: ",test) ;
                                e = 0 ;
                                break ;
                            }
                            if (satree.len) { log_warn("directory: ",test," is not empty") ; e = 0 ; }
                            break ;
                        }

        default:            log_die(LOG_EXIT_SYS, "operation not implemented") ;
    }

    argc -= 1 ; argv += 1 ;
    for (; i < argc ; i++)
        nargv[n++] = (char *)argv[i] ;

    nargv[n] = 0 ;
    arguments->argc = n ;
    arguments->argv = nargv ;

    if (!arguments->argc && !arguments->noprog) log_die(LOG_EXIT_USER,"missing argument prog") ;

    return e ;
}

int main(int argc,char const *const *argv, char const *const *envp)
{
    int r, f = 0, n = 0 ;
    uint8_t negat = 0, treat_zero = 0 ;
    char *nargv[argc + 2] ;

    execl_func_t_ref func = 0 ;
    opts_common_t arguments = OPTS_COMMON_ZERO ;

    // do not output nothing by default except system error
    VERBOSITY = 0 ;

    PROG = "execl-toc" ;
    {
        subgetopt l = SUBGETOPT_ZERO ;

        for (;;)
        {
          int opt = subgetopt_r(argc, argv, "hv:ntDX", &l) ;
          if (opt == -1) break ;
          switch (opt)
          {
            case 'h' :  info_help(); return 0 ;
            case 'v' :  if (!uint0_scan(l.arg, &VERBOSITY))
                            log_usage(USAGE) ;
                        break ;
            case 'n' :  negat = 1 ; break ;
            case 't' :  treat_zero = 1 ; break ;
            case 'D' :  arguments.not_create = 1 ; break ;
            case 'X' :  arguments.noprog = 1 ; break ;
            default :   break ;
          }
        }
        argc -= l.ind ; argv += l.ind ;
    }
    argc++ ; argv-- ;
    nargv[n++] = "fake_opts" ;
    for (int i = 0 ; i < argc ; i++)
        nargv[n++] = (char *)argv[i] ;

    nargv[n] = 0 ;

    {
        subgetopt l = SUBGETOPT_ZERO ;

        for (;;)
        {
            int opt = subgetopt_r(3,(char const *const *)nargv, "d:p:S:m:L:b:c:k:n:g:r:s:t:u:w:x:e:f:z:O:U:N:V:E:", &l) ;
            if (opt == -1) break ;
            switch (opt)
            {
                case 'd' :  func = &execl_directory ;
                            arguments.test_on = l.arg ;
                            arguments.func_name = "execl_directory" ;
                            break ;
                case 'p' :  func = &execl_fifo ;
                            arguments.test_on = l.arg ;
                            arguments.func_name = "execl_fifo" ;
                            break ;
                case 'S' :  func = &execl_socket ;
                            arguments.test_on = l.arg ;
                            arguments.func_name = "execl_socket" ;
                            break ;
                case 'm' :  func = &execl_mountpoint ;
                            arguments.test_on = l.arg ;
                            arguments.func_name = "execl_mountpoint" ;
                            break ;
                case 'L' :  func = &execl_symlink ;
                            arguments.test_on = l.arg ;
                            arguments.func_name = "execl_symlink" ;
                            break ;
                // only test
                case 'b' :  arguments.test = T_BLOCK ;
                            func = &execl_common ;
                            arguments.test_on = l.arg ;
                            arguments.func_name = "execl_common" ;
                            break ;
                case 'c' :  arguments.test = T_CHAR ;
                            func = &execl_common ;
                            arguments.test_on = l.arg ;
                            arguments.func_name = "execl_common" ;
                            break ;
                case 'k' :  arguments.test = T_STICKY ;
                            func = &execl_common ;
                            arguments.test_on = l.arg ;
                            arguments.func_name = "execl_common" ;
                            break ;
                case 'n' :  arguments.test = T_NONZERO ;
                            func = &execl_common ;
                            arguments.test_on = l.arg ;
                            arguments.func_name = "execl_common" ;
                            break ;
                case 'g' :  arguments.test = T_SGID ;
                            func = &execl_common ;
                            arguments.test_on = l.arg ;
                            arguments.func_name = "execl_common" ;
                            break ;
                case 'r' :  arguments.test = T_READABLE ;
                            func = &execl_common ;
                            arguments.test_on = l.arg ;
                            arguments.func_name = "execl_common" ;
                            break ;
                case 's' :  arguments.test = T_NONZEROFILE ;
                            func = &execl_common ;
                            arguments.test_on = l.arg ;
                            arguments.func_name = "execl_common" ;
                            break ;
                case 't' :  arguments.test = T_TERM ;
                            func = &execl_common ;
                            arguments.test_on = l.arg ;
                            arguments.func_name = "execl_common" ;
                            break ;
                case 'u' :  arguments.test = T_SUID ;
                            func = &execl_common ;
                            arguments.test_on = l.arg ;
                            arguments.func_name = "execl_common" ;
                            break ;
                case 'w' :  arguments.test = T_WRITABLE ;
                            func = &execl_common ;
                            arguments.test_on = l.arg ;
                            arguments.func_name = "execl_common" ;
                            break ;
                case 'x' :  arguments.test = T_EXECUTABLE ;
                            func = &execl_common ;
                            arguments.test_on = l.arg ;
                            arguments.func_name = "execl_common" ;
                            break ;
                case 'e' :  arguments.test = T_EXIST ;
                            func = &execl_common ;
                            arguments.test_on = l.arg ;
                            arguments.func_name = "execl_common" ;
                            break ;
                case 'f' :  arguments.test = T_REGULAR ;
                            func = &execl_common ;
                            arguments.test_on = l.arg ;
                            arguments.func_name = "execl_common" ;
                            break ;
                case 'z' :  arguments.test = T_ZERO ;
                            func = &execl_common ;
                            arguments.test_on = l.arg ;
                            arguments.func_name = "execl_common" ;
                            break ;
                case 'O' :  arguments.test = T_EUID ;
                            func = &execl_common ;
                            arguments.test_on = l.arg ;
                            arguments.func_name = "execl_common" ;
                            break ;
                case 'U' :  arguments.test = T_EGID ;
                            func = &execl_common ;
                            arguments.test_on = l.arg ;
                            arguments.func_name = "execl_common" ;
                            break ;
                case 'N' :  arguments.test = T_MODIFIED ;
                            func = &execl_common ;
                            arguments.test_on = l.arg ;
                            arguments.func_name = "execl_common" ;
                            break ;
                case 'V' :  arguments.test = T_ENV ;
                            func = &execl_common ;
                            arguments.test_on = l.arg ;
                            arguments.func_name = "execl_common" ;
                            break ;
                case 'E' :  func = &execl_common ;
                            arguments.test = T_EMPTY ;
                            arguments.test_on = l.arg ;
                            arguments.func_name = "execl_common" ;
                            break ;
                default :   for (int i = 0 ; i < n ; i++)
                            {
                                if (!argv[l.ind]) log_usage(USAGE) ;
                                if (!strcmp(nargv[i],argv[l.ind]))
                                    f = 1 ;
                            }
                            if (!f) nargv[n++] = (char *)argv[l.ind] ;
                            f = 0 ;
                            break ;
            }
        }
        argc -= l.ind ; argv += l.ind ;
    }
    if (argc <= 0 && !arguments.noprog) log_usage(USAGE) ;
    n = 0 ;
    argc++ ; argv-- ;
    nargv[n++] = (char *)arguments.func_name ;
    for (int i = 0 ; i < argc ; i++)
        nargv[n++] = (char *)argv[i] ;

    nargv[n] = 0 ;

    arguments.argc = n ;
    arguments.argv = nargv ;
    arguments.envp = envp ;
    // argument doesn't exit
    if (!arguments.func_name) log_die(LOG_EXIT_SYS, "operation not implemented") ;

    r = (*func)(&arguments,nargv) ;

    if (negat == r) return treat_zero ? LOG_EXIT_ZERO : LOG_EXIT_SYS ;
    if (arguments.noprog) return LOG_EXIT_ZERO ;

    xexec_ae(arguments.argv[0],(char const *const *)arguments.argv,arguments.envp) ;
}
