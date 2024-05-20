/*
 * 66-ns.c
 *
 * Copyright (c) 2018-2024 Eric Vidal <eric@obarun.org>
 *
 * All rights reserved.
 *
 * This file is part of Obarun. It is subject to the license terms in
 * the LICENSE file found in the top-level directory of this
 * distribution.
 * This file may not be copied, modified, propagated, or distributed
 * except according to the terms contained in the LICENSE file.
 *
 */

#include <stdint.h>
#include <string.h>
#include <unistd.h>//readlink
#include <fcntl.h>
#include <stdlib.h> //free
#include <errno.h>

/** mount */
#include <sys/mount.h>
#include <sys/types.h>
#include <mntent.h>

/** process */
#include <sys/prctl.h>
#include <sys/syscall.h> //__NR_CLONE

/** signal */
#include <signal.h> //SIGXXX
#include <sys/eventfd.h>
#include <sys/wait.h>//WEXITSTATUS,...

/** namespace */
#include <sched.h> //CLONE_NEWNS,unshare
#include <sys/socket.h>

#include <oblibs/sastr.h>
#include <oblibs/log.h>
#include <oblibs/string.h>
#include <oblibs/files.h>
#include <oblibs/directory.h>
#include <oblibs/types.h>
#include <oblibs/stack.h>
#include <oblibs/lexer.h>

#include <skalibs/sgetopt.h>
#include <skalibs/types.h>
#include <skalibs/stralloc.h>
#include <skalibs/stralloc.h>
#include <skalibs/genalloc.h>
#include <skalibs/buffer.h>
#include <skalibs/iopause.h>
#include <skalibs/selfpipe.h>
#include <skalibs/tai.h>
#include <skalibs/djbunix.h> //wait_nohang,...
#include <skalibs/exec.h>

#include <66-tools/config.h>

#include <stdio.h>
#define NS_MAXOPTS 6
#define ns_checkopts(n) if (n >= NS_MAXOPTS) log_die(LOG_EXIT_USER, "too many namespace options")
#define NS_COLON_DELIM ':'
#define RULE_MAXOPTS 9
#define rule_checkopts(n) if (n >= RULE_MAXOPTS) log_die(LOG_EXIT_USER, "too many rule options")
#define NS_COMMA_DELIM ','

/** __compar_fn_t is an internal function of glibc
 * so define it for musl
 * */
#ifndef __COMPAR_FN_T
#define __COMPAR_FN_T
typedef int (*__compar_fn_t)(const void *, const void *);
#endif

#define qsort_indirect(p, n, func) ({ \
    int (*_func_)(const __typeof__(p[0])*, const __typeof__(p[0])*) = func ; \
    qsort((p), (n), sizeof((p)[0]), (__compar_fn_t) _func_) ; \
})

#define FOREACH_GA(ga,type,pos) \
    genalloc_ref _garef_ = ga ; \
    for (; pos < genalloc_len(type,_garef_) ; pos ++)

/** return 1 if set, else 0*/
#define MNT_FLAGS_IS_SET(has, want) \
        ((~(has) & (want)) == 0)

static stralloc SADATA = STRALLOC_ZERO ;
static unsigned long NS_MNT_FLAGS = MS_SHARED ;
static int NS_CLONE_FLAGS = SIGCHLD | CLONE_NEWNS ;
static uint8_t NS_NONEWPRIVELEGES = 0 ;
static size_t HOSTNAME = 0 ;
static uint8_t PID1 = 0 ; //CLONE_NEWPID is set

stralloc _MNTFILE_SA = STRALLOC_ZERO ;
genalloc _MNTFILE_GA = GENALLOC_ZERO ;

static char *NSHIDDEN = "/run/66/ns/hidden" ;
static char *NSPREFIX = "/run/66/ns" ;
static char *NSTMP = "/run/66/ns/nstmp" ;

#define USAGE "66-ns [ -h ] [ -z ] [ -v verbosity ] [ -d notif ] [ -o ns_options,... ] [ -e element:type:options:... ] [ -r rule ] prog"

static inline void info_help (void)
{
    static char const *help =
        "66-ns <options> prog\n"
        "\n"
        "options:\n"
        "   -h: print this help\n"
        "   -z: use color\n"
        "   -v: increase/decrease verbosity\n"
        "   -o: comma separated list of namespace options\n"
        "   -d: notify readiness on file descriptor notif\n"
        "   -e: element to handle. Can be passed multiple time.\n"
        "   -r: list of rule to apply. Can be passed multiple time.\n"
        "\n"
        ;

    if (buffer_putsflush(buffer_1, help) < 0)
        log_dieusys(LOG_EXIT_SYS, "write to stdout") ;
}

typedef struct ns_opts_map_s ns_opts_map_t ;
struct ns_opts_map_s
{
    char const *str ;
    int const id ;
} ;

typedef enum enum_ns_opts_e enum_ns_opts_t, *enum_ns_opts_t_ref ;
enum enum_ns_opts_e
{
    OPTS_FLAGS = 0,
    OPTS_PRIVELEGES,
    OPTS_UNSHARE,
    OPTS_HOSTNAME,
    OPTS_ENDOFKEY
} ;

ns_opts_map_t const ns_opts_table[] =
{
    { .str = "flag", .id = OPTS_FLAGS },
    { .str = "nonewprivileges", .id = OPTS_PRIVELEGES },
    { .str = "unshare", .id = OPTS_UNSHARE },
    { .str = "hostname", .id = OPTS_HOSTNAME },
    { .str = 0 }
} ;

typedef enum enum_type_flags_e enum_type_flags_t, *enum_type_flags_t_ref ;
enum enum_type_flags_e
{
    TYPE_TMPFS ,
    TYPE_HIDDEN ,
    TYPE_RECURSIVE ,
    TYPE_CLONE ,
    TYPE_PROC ,
    TYPE_DEV ,
    TYPE_SYS ,
    TYPE_ENDOFKEY
} ;

char const *enum_type_flags_str[] = {
    "tmpfs" ,
    "hidden" ,
    "recursive" ,
    "clone" ,
    "proc" ,
    "dev" ,
    "sys" ,
    0
} ;

typedef struct ns_entry_s ns_entry_t, *ns_entry_t_ref ;
struct ns_entry_s
{
    /** directory to handle */
    ssize_t path ;

    /** destination of the mount if any.
     * if not set destination correspond to the host. */
    ssize_t target ;

    /** type to apply to the mount.
     * Can be recursive,tmpfs,hidden */
    ssize_t type ;

    /** options to pass to mount. Kernel flag are separated
     * and added to flags, the rest is passed as-is. */
    ssize_t opts ;

    /** skip the entry if the directory doesn't exist. */
    uint8_t ignore ;

    /** create the destination if it's doesn't exist yet. */
    uint8_t create ;

    /** the following is for inner use */

    /** mount flags to apply */
    unsigned long flags ;

    /** already applied */
    uint8_t done ;

    /** skip this entry, see ns_clean_ns_entry */
    uint8_t skip ;

} ;

#define NS_ENTRY_ZERO { \
    .path = -1, \
    .target = -1, \
    .type = -1, \
    .opts = -1, \
    .ignore = 0, \
    .create = 1, \
    .flags = 0, \
    .done = 0, \
    .skip = 0, \
}

typedef enum enum_rule_e enum_rule_t, *enum_rule_t_ref ;
enum enum_rule_e
{
    RULE_TARGET = 0,
    RULE_TYPE,
    RULE_OPTS,
    RULE_IGNORE,
    RULE_CREATE,
    RULE_ENDOFKEY
} ;

typedef struct ns_rule_map_s ns_rule_map_t ;
struct ns_rule_map_s
{
    char const *str ;
    ns_entry_t entry ;
    int const id ;
} ;

ns_rule_map_t const ns_rule_table[] =
{
    { .str = "target", .entry = NS_ENTRY_ZERO, .id = RULE_TARGET },
    { .str = "type", .entry = NS_ENTRY_ZERO, .id = RULE_TYPE },
    { .str = "options", .entry = NS_ENTRY_ZERO, .id = RULE_OPTS },
    { .str = "ignore", .entry = NS_ENTRY_ZERO, .id = RULE_IGNORE },
    { .str = "create", .entry = NS_ENTRY_ZERO, .id = RULE_CREATE },
    { .str = 0 }
} ;

typedef enum enum_mount_e enum_mount_t, *enum_mount_t_ref ;
enum enum_mount_e
{
    MNT_RW = 0, // rw
    MNT_RDONLY, //ro

    MNT_BIND, // bind  - filesystem and mode is ignored
    MNT_RBIND, // rbind - filesystem and mode is ignored

    MNT_ATIME, //atime
    MNT_NOATIME, //noatime

    MNT_DEV, //dev
    MNT_NODEV, //nodev

    MNT_DIRATIME, // diratime
    MNT_NODIRATIME, //nodiratime

    MNT_EXEC, //exec
    MNT_NOEXEC, //noexec

    MNT_MANDLOCK, //mand
    MNT_NOMANDLOCK, //nomand

    MNT_RELATIME, //relatime
    MNT_NORELATIME, //norelatime

    MNT_STRICTATIME, //strictatime
    MNT_NOSTRICTATIME, //nostrictatime

    MNT_SUID, //suid
    MNT_NOSUID, //nosuid

    MNT_I_VERSION, //iversion
    MNT_NOI_VERSION, //noiversion

    MNT_SYNCHRONOUS, //sync
    MNT_NOSYNCHRONOUS, //async

    MNT_MOVE, // move
    MNT_REMOUNT, //remount
    MNT_DIRSYNC, //dirsync

    MNT_SHARED,//shared
    MNT_SLAVE, //slave
    MNT_PRIVATE, //private
    MNT_UNBINDABLE, //unbindable

    // inner value
    MNT_REC,

    MNT_ADD,
    MNT_IGNORE,
    MNT_ENDOFKEY
} ;

char const *enum_mount_str[] = {

    "rw",
    "ro",

    "bind",
    "rbind",

    "atime",
    "noatime",

    "dev",
    "nodev",

    "diratime",
    "nodiratime",

    "exec",
    "noexec",

    "mand",
    "nomand",

    "relatime",
    "norelatime",

    "strictatime",
    "nostrictatime",

    "suid",
    "nosuid",

    "iversion",
    "noiversion",

    "sync",
    "async",

    "move"
    "remount",
    "dirsync",

    "shared",
    "slave",
    "private",
    "unbindable",

    "rec",
    "add",
    "drop",

    0

} ;

typedef struct mount_opts_map_s mount_opts_map_t, *mount_opts_map_t_ref ;
struct mount_opts_map_s
{
    char const *opts ;
    unsigned long flags ;
    enum_mount_t action ;
} ;

mount_opts_map_t const mount_opts_table [] =
{
    { "rw",             MS_RDONLY, MNT_IGNORE },
    { "ro",             MS_RDONLY, MNT_ADD },

    { "bind",           MS_BIND, MNT_ADD },
    { "rbind",          MS_BIND|MS_REC, MNT_ADD },

    { "atime",          MS_NOATIME, MNT_IGNORE },
    { "noatime",        MS_NOATIME, MNT_ADD },

    { "dev",            MS_NODEV, MNT_IGNORE },
    { "nodev",          MS_NODEV, MNT_ADD },

    { "diratime",       MS_NODIRATIME, MNT_IGNORE },
    { "nodiratime",     MS_NODIRATIME, MNT_ADD },

    { "exec",           MS_NOEXEC, MNT_IGNORE },
    { "noexec",         MS_NOEXEC, MNT_ADD },

    { "mand",           MS_MANDLOCK, MNT_ADD },
    { "nomand",         MS_MANDLOCK, MNT_IGNORE },

    { "relatime",       MS_RELATIME, MNT_ADD },
    { "norelatime",     MS_RELATIME, MNT_IGNORE },

    { "strictatime",    MS_STRICTATIME, MNT_ADD },
    { "nostrictatime",  MS_STRICTATIME, MNT_IGNORE },

    { "suid",           MS_NOSUID, MNT_IGNORE },
    { "nosuid",         MS_NOSUID, MNT_ADD },

    { "iversion",       MS_I_VERSION, MNT_ADD },
    { "noiversion",     MS_I_VERSION, MNT_IGNORE },

    { "sync",           MS_SYNCHRONOUS, MNT_ADD },
    { "async",          MS_SYNCHRONOUS, MNT_IGNORE },

    /** We remove move and remount flags.
     * We control this flags*/
    { "move",           MS_MOVE, MNT_IGNORE },
    { "remount",        MS_REMOUNT, MNT_IGNORE },
    { "dirsync",        MS_DIRSYNC, MNT_ADD },

    /** Cannot be possible for user to pass it.
     * We implement it for the future. */
    { "shared",         MS_SHARED, MNT_ADD },
    { "slave",          MS_SLAVE, MNT_ADD },
    { "private",        MS_PRIVATE,MNT_ADD },
    { "unbindable",     MS_UNBINDABLE, MNT_ADD },
    { 0, 0, 0}

} ;

typedef struct mntfile_s mntfile_t,*mntfile_t_ref ;
struct mntfile_s
{

    ssize_t mnt_fsname ;
    ssize_t mnt_dir ;
    ssize_t mnt_type ;
    ssize_t mnt_opts ;
    ssize_t mnt_freq ;
    ssize_t mnt_passno ;
} ;

#define MNTENT_GA_ZERO \
{ \
    -1 , \
    -1 , \
    -1 , \
    -1 , \
    -1 , \
    -1 \
}

typedef enum enum_mntfile_e enum_mntfile_t,*enum_mntfile_t_ref ;
enum enum_mntfile_e
{
    MNTFILE_FSNAME = 0 ,
    MNTFILE_DIR ,
    MNTFILE_TYPE ,
    MNTFILE_OPTS ,
    MNTFILE_FREQ ,
    MNTFILE_PASSNO ,
    MNTFILE_ENDOFKEY

} ;

/**
 *
 * Function declaration
 *
 * */

void ns_split_from_section(stralloc *sadir, stralloc *secname, char const *str, char const *filename) ;

void ns_parse_rule(stralloc *sadir,stralloc *sarule, char const *filename) ;

/**
 *
 * Helper function
 *
 * */

void notify(int *fd)
{
    if ((*fd) >= 0) {

        fd_write((*fd),"\n", 1) ;
        fd_close((*fd)) ;
        (*fd) = -1 ;
    }
}

static ssize_t get_key(char *table,char const *str)
{
    ssize_t pos = -1 ;

    pos = get_len_until(str,'=') ;

    if (pos == -1)
        return -1 ;

    auto_strings(table,str) ;

    table[pos] = 0 ;

    pos++ ; // remove '='

    return pos ;
}

ssize_t get_enum_type_flags_by_key(char const *str)
{
    uint8_t pos = 0;

    for (; pos < N_ELEMENTS(enum_type_flags_str) && enum_type_flags_str[pos] ; pos++) {

        if (!strcmp(enum_type_flags_str[pos],str))
           return pos ;
    }

    return -1 ;
}

ssize_t ns_add_to_sadata(char const *str)
{
    ssize_t pos = SADATA.len ;

    if (!sastr_add_string(&SADATA,str))
        log_die_nomem("stralloc") ;

    return pos ;
}

ssize_t ns_get_previous_element(stralloc *sa,size_t where)
{
    size_t n = sastr_nelement(sa) ;
    n-- ;
    if ((n - where) < n)
        return -1 ;

    return sastr_find_element_byid(sa,n - where) ;
}

void ns_clone_node(char const *path, char const *target)
{
    log_flow() ;

    struct stat st ;

    /** We want to know if path is a symlink.
     * So, we use lstat() instead of stat().
     * In case of symlink, we use realpath
     * to find the final target. */
    if (lstat(path,&st) == -1)
        log_dieusys(LOG_EXIT_SYS,"find: ",path) ;


    if (S_ISREG(st.st_mode)) {

        log_trace("create file: ",target) ;

        if (filecopy_unsafe(path,target,st.st_mode) < 0)
            log_dieusys(LOG_EXIT_SYS,"create file: ",target) ;
    }
    else if (S_ISDIR(st.st_mode)) {

        log_trace("create directory: ",target) ;

        if (!dir_create_parent(target,st.st_mode))
            log_dieusys(LOG_EXIT_SYS,"create directory: ",target) ;

    }
    else if (S_ISFIFO(st.st_mode)){

        log_trace("create fifo: ",target) ;

        if (mkfifo(target, st.st_mode) < 0)
            log_dieusys(LOG_EXIT_SYS,"create fifo: ",target) ;

    }
    else if (S_ISLNK(st.st_mode)) {

        /* should be enough */
        char dest[4096] ;
        ssize_t d = readlink(path,dest,4096) ;
        if (d == -1)
            log_dieusys(LOG_EXIT_SYS,"readlink: ",path) ;

        dest[d] = 0 ;

        log_trace("create symlink: ",target," pointing to: ",dest) ;

        if (symlink(dest,target) < 0)
            log_dieusys(LOG_EXIT_SYS,"create symlink: ",target) ;
    }
    else if (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode) || S_ISSOCK(st.st_mode)) {

        log_trace("create node: ",target) ;

        if (mknod(target, st.st_mode, st.st_rdev) < 0)
            log_dieusys(LOG_EXIT_SYS,"create node: ",target) ;
    }

    lchown(target, st.st_uid, st.st_gid) ;
    if (!S_ISLNK(st.st_mode))
        if (chmod(target, st.st_mode) == -1)
            log_dieusys(LOG_EXIT_SYS,"chmod: ",target) ;

}

/** @Return MNT_ADD for kernel flags
 * @Return MNT_IGNORE for user flags */
static int mount_sanitize_flags(unsigned long *flags, char const *str)
{
    log_flow() ;

    size_t n = sizeof (mount_opts_table) / sizeof (*mount_opts_table) ;

    size_t i = 0 ;

    for (; i < n && mount_opts_table[i].opts; i++)
    {
        if (!strcmp(mount_opts_table[i].opts,str))
        {
            switch (mount_opts_table[i].action)
            {
                case MNT_ADD:

                    *flags |= mount_opts_table[i].flags ;
                    return MNT_ADD ;

                case MNT_IGNORE:

                    *flags &= ~mount_opts_table[i].flags ;
                    return MNT_IGNORE ;

                default:
                    return MNT_IGNORE ;
            }
        }
    }

    return MNT_IGNORE ;
}

static void mount_split_opts_flags(ns_entry_t *entry,char const *str)
{
    log_flow() ;

    size_t pos = 0, len = 0, next = 0, slen = strlen(str), klen = 0 ;

    ssize_t dummy = -1 ;

    int r = MNT_IGNORE ;

    _alloc_stk_(stk, slen + 1) ;

    char keep[slen + 1] ;
    for (;pos < slen + 1; pos++)
        keep[pos] = 0 ;

    if (!lexer_trim_with_delim(&stk,str,NS_COMMA_DELIM))
        log_dieu(LOG_EXIT_SYS,"parse options line: ",str) ;

    pos = 0 ;

    FOREACH_STK(&stk,pos) {

        char *o = stk.s + pos ;

        next = pos + (strlen(stk.s + pos) + 1) < stk.len ? 1 : 0 ;

        len = strlen(o) ;

        char k[len + 1] ;

        dummy = get_key(k,o) ;

        // kernel flag do not contain '=' character
        if (dummy == -1) {

            r = mount_sanitize_flags(&entry->flags,o) ;

        } else {

            r = MNT_IGNORE ;
        }

        klen = strlen(keep) ;

        if (r == MNT_IGNORE) {

            auto_string_from(keep,klen,o) ;

            klen = strlen(keep) ;

            if (next)
                auto_string_from(keep,klen,",") ;
        }
    }

    klen = strlen(keep) ;

    if (keep[klen-1] == ',')
        keep[klen-1] = 0 ;

    if (*keep)
        entry->opts = ns_add_to_sadata(keep) ;
    else entry->opts = -1 ;
}

ns_entry_t ns_compute_opts(char const *mntopts,char const *useropts)
{
    log_flow() ;

    ns_entry_t new = NS_ENTRY_ZERO ;

    size_t mntlen = strlen(mntopts), userlen = strlen(useropts) ;
    char o[mntlen + 1 + userlen + 1] ;

    auto_strings(o,mntopts,!*mntopts ? "" :",",useropts) ;

    if (strlen(o))
        mount_split_opts_flags(&new,o) ;

    return new ;
}

/**
 *
 * Mount function
 *
 * */

void mntfile_free()
{
    stralloc_free(&_MNTFILE_SA) ;
    genalloc_free(mntfile_t,&_MNTFILE_GA) ;
}

ssize_t mntfile_add_string(char const *str)
{
    ssize_t len = _MNTFILE_SA.len ;

    size_t slen = strlen(str) ;
    if (!stralloc_catb(&_MNTFILE_SA,str,slen + 1))
        log_die_nomem("stralloc") ;

    return len ;
}

void mntfile_init()
{
    log_flow() ;

    FILE *file_mounts = setmntent ("/proc/self/mounts", "r") ;

    if (!file_mounts)
        log_dieusys(LOG_EXIT_SYS,"open: /proc/self/mounts") ;

    for (;;) {

        struct mntent *m ;
        m = getmntent(file_mounts) ;

        if (!m)
            break ;

        mntfile_t cp = MNTENT_GA_ZERO ;

        cp.mnt_fsname = mntfile_add_string(m->mnt_fsname) ;
        cp.mnt_dir = mntfile_add_string(m->mnt_dir) ;
        cp.mnt_type = mntfile_add_string(m->mnt_type) ;
        cp.mnt_opts = mntfile_add_string(m->mnt_opts) ;
        cp.mnt_freq = m->mnt_freq ;
        cp.mnt_passno = m->mnt_passno ;

        if (!genalloc_append(mntfile_t,&_MNTFILE_GA,&cp))
            log_die_nomem("genalloc") ;
    }

    endmntent(file_mounts) ;
}

ssize_t mntfile_get(char const *path, uint8_t entry)
{
    log_flow() ;

    char *str = _MNTFILE_SA.s ;
    size_t pos = 0, plen = strlen(path) ;
    char p[plen + 1] ;

    auto_strings(p,path) ;

    dir_unslash(p) ;

    for (; pos < genalloc_len(mntfile_t,&_MNTFILE_GA) ; pos++) {

        mntfile_t_ref m ;

        m = &genalloc_s(mntfile_t,&_MNTFILE_GA)[pos] ;

        if (!strcmp(str + m->mnt_dir,p)) {

            switch(entry) {

                case MNTFILE_FSNAME :

                    return m->mnt_fsname ;

                case MNTFILE_DIR :

                    return m->mnt_dir ;

                case MNTFILE_TYPE :

                    return m->mnt_type ;

                case MNTFILE_OPTS :

                    return m->mnt_opts ;

                case MNTFILE_FREQ :

                    return m->mnt_freq ;

                case MNTFILE_PASSNO :

                    return m->mnt_passno ;

                default :
                /** should never happens */
                   return -1 ;
            }
        }

    }

    return -1 ;
}

static int is_mnt(char const *str)
{
    log_flow() ;

    struct stat st;

    size_t slen = strlen(str) ;

    int is_not_mnt = 0 ;

    if (lstat(str,&st) < 0)
        return 0 ;

    if (S_ISDIR(st.st_mode)) {

        dev_t st_dev = st.st_dev ; ino_t st_ino = st.st_ino ;

        char p[slen+4] ;

        auto_strings(p,str,"/..") ;

        if (!stat(p,&st))
            is_not_mnt = (st_dev == st.st_dev) && (st_ino != st.st_ino) ;
    }
    else
        return 0 ;

    return is_not_mnt ? 0 : 1 ;
}

static void umount_recursive(char const *path)
{
    log_flow() ;

    size_t pos = 0, nstmp_len = strlen(NSTMP), mnt_len = 0 ;
    char *str = _MNTFILE_SA.s ;

    /** umount first the sub-mounts*/
    genalloc_reverse(mntfile_t,&_MNTFILE_GA) ;

    FOREACH_GA(&_MNTFILE_GA,mntfile_t,pos) {

        mntfile_t_ref m ;
        m = &genalloc_s(mntfile_t,&_MNTFILE_GA)[pos] ;
        mnt_len = strlen(str + m->mnt_dir) ;
        char toumount[nstmp_len + 1 + mnt_len + 1] ;

        auto_strings(toumount,NSTMP,str + m->mnt_dir) ;

        if (!strcmp(toumount,path))
            continue ;

        if (dir_is_child(path,toumount) > 0) {

            int itype = mntfile_get(str + m->mnt_dir,MNTFILE_TYPE) ;

            if (itype == -1)
                log_dieu(LOG_EXIT_SYS,"find type of path: ", toumount) ;

            char *type = _MNTFILE_SA.s + itype ;

            /**
             *
             * This is really
             *
             * UGLY
             *
             * IMPROVE IT
             *
             *
             * */
            if (!strncmp(type, "fuse", 4))
            {

                log_trace("unmount fuse mountpoint: ",path) ;
                if (umount2(toumount,UMOUNT_NOFOLLOW | MNT_FORCE) == -1)
                    log_warnusys("umount: ", toumount) ;

            }
            else if (is_mnt(toumount)) {

                log_trace("unmount: ",toumount) ;
                if (umount2(toumount,UMOUNT_NOFOLLOW) == -1)
                    log_dieusys(LOG_EXIT_SYS,"umount: ", toumount) ;
            }
        }
    }

    if (is_mnt(path)) {

        log_trace("unmount: ",path) ;
        /** unmount the path itself */
        if (umount2(path,UMOUNT_NOFOLLOW) == -1)
            log_dieusys(LOG_EXIT_SYS,"umount: ", path) ;

    }
    /** reorganize the genalloc */
    genalloc_reverse(mntfile_t,&_MNTFILE_GA) ;
}

int mount_move_root (char const *path)
{
    log_flow() ;

    if (chdir(path) == -1)
        log_warnusys_return(LOG_EXIT_ZERO,"chdir") ;

    if (mount(path, "/", NULL, MS_MOVE, NULL) == -1)
        log_warnusys_return(LOG_EXIT_ZERO,"move: ",path," to: /") ;

    if (chroot(".") == -1)
        log_warnusys_return(LOG_EXIT_ZERO,"chroot") ;

    if (chdir("/") == -1)
        log_warnusys_return(LOG_EXIT_ZERO,"chdir") ;

    return 1 ;
}

int ns_mount(char const *path,char const *target,char const *type,unsigned long flags, char const *opts,uint8_t create)
{
    log_flow() ;

    int fd ;
    size_t len = strlen("/proc/self/fd/") ;
    char fmt[UINT_FMT] ;

    fd = open(target,O_PATH | O_CLOEXEC | O_NOFOLLOW) ;

    if (fd == -1)
        log_dieusys(LOG_EXIT_SYS,"open: ",target) ;

    size_t fmtlen = uint_fmt(fmt,fd) ;
    fmt[fmtlen] = 0 ;

    char ftarget[len + fmtlen + 1] ;
    auto_strings(ftarget,"/proc/self/fd/",fmt) ;

    if (mount(path,target,type,flags,opts) == -1) {

       if (errno == ENOENT && create) {

            log_trace("last mount failed -- try to create: ",target) ;

            ns_clone_node(path,target) ;

            if (mount(path,target,type,flags,opts) == -1)
                log_dieusys(LOG_EXIT_SYS,"mount: ",path," to: ",target) ;
        }
        else log_dieusys(LOG_EXIT_SYS,"mount: ",path," to: ",target) ;
    }

    fd_close(fd) ;
    return 1 ;
}

void mnt_remount_ro(ns_entry_t *entry,genalloc *list)
{
    log_flow() ;

    if (entry->ignore)
        return ;

    size_t pos = 0, gpos = 0, len = strlen(SADATA.s + entry->target), nlen = strlen(NSTMP) ;

    char *str = _MNTFILE_SA.s, *target = 0, *dir = 0 ;
    char mopts[4096] ;
    char ntarget[nlen + len + 1] ;
    unsigned long myflag = MS_BIND | MS_REMOUNT | MS_RDONLY ;

    auto_strings(ntarget,NSTMP,SADATA.s + entry->target) ;

    memset(mopts,0,4096) ;

    if (MNT_FLAGS_IS_SET(entry->flags,MS_RDONLY)) {

        genalloc_reverse(mntfile_t,&_MNTFILE_GA) ;

        FOREACH_GA(&_MNTFILE_GA,mntfile_t,pos) {

            uint8_t skip = 0 ;
            mntfile_t_ref m ;
            m = &genalloc_s(mntfile_t,&_MNTFILE_GA)[pos] ;

            dir = str + m->mnt_dir ;

            target = SADATA.s + entry->target ;

            if (!strcmp(dir,target)) {
                /** we keep options to apply.
                 * this is avoid to make an another loop
                 * if we need to mount dir */
                auto_strings(mopts, str + m->mnt_opts) ;
                continue ;
            }
            if (!strcmp(dir,"/"))
                continue ;

            /** We skip every entry and sub-mount of the entry
             * that we handle.
             * For example we want /etc -> ro but
             * /etc/resolv.conf -> rw. */
            gpos = 0 ;
            FOREACH_GA(list,ns_entry_t,gpos) {

                ns_entry_t_ref lm = &genalloc_s(ns_entry_t,list)[gpos] ;

                char *toskip = SADATA.s + lm->target ;

                if (!strcmp(toskip,"/"))
                    continue ;

                if (!strcmp(toskip,dir) || (dir_is_child(toskip,dir) > 0)) {
                    skip = 1 ;
                    break ;
                }

            }

            /** skip automatically every sub-mount for a
             * target with type == TYPE_HIDDEN. we don't want to
             * see any sub-mounts beside it anyway */
            if (dir_is_child(target,dir) > 0 && !skip) {

                /** hidden and tmpfs directory umount every sub-mounts */
                if (entry->type == TYPE_HIDDEN || entry->type == TYPE_TMPFS)
                    continue ;

                size_t mlen = strlen(dir) ;
                ns_entry_t nopts = ns_compute_opts(str + m->mnt_opts,SADATA.s + entry->opts) ;

                char dest[nlen + mlen + 1] ;

                auto_strings(dest,NSTMP,dir) ;

                nopts.flags |= myflag | entry->flags ;

                log_trace("remount ro: ",dest) ;
                if (!ns_mount(dest,dest,NULL,nopts.flags,SADATA.s + nopts.opts,0))
                    log_dieusys(LOG_EXIT_SYS,"remount ro: ", dest) ;
            }
        }
        {

            ns_entry_t nopts = ns_compute_opts(mopts,SADATA.s + entry->opts) ;

            nopts.flags |= myflag | entry->flags ;

            log_trace("remount ro: ",ntarget) ;
            /** Remount the path itself */
            if (!ns_mount(ntarget,ntarget,NULL,nopts.flags,SADATA.s + nopts.opts,0))
                log_dieusys(LOG_EXIT_SYS,"remount ro: ", ntarget) ;
        }
    }
}

void ns_mount_recursive(char const *root, ns_entry_t *entry)
{
    log_flow() ;

    size_t pos = 0, prefix_len = strlen(root) ;
    char *str = _MNTFILE_SA.s, *sentry = SADATA.s ;
    char *path = sentry + entry->path ;

    FOREACH_GA(&_MNTFILE_GA,mntfile_t,pos) {

        mntfile_t_ref m ;

        m = &genalloc_s(mntfile_t,&_MNTFILE_GA)[pos] ;

        char *dir = str + m->mnt_dir ;

        if (!strcmp(dir,path))
            continue ;

        if (!strcmp(dir,"/"))
            continue ;

        if (dir_is_child(path,dir) > 0) {

            size_t len = strlen(dir) ;

            char target[prefix_len + len + 1] ;
            auto_strings(target,root,dir) ;

            ns_entry_t nopts = ns_compute_opts(str + m->mnt_opts,SADATA.s + entry->opts) ;

            nopts.flags |= MS_REMOUNT | MS_BIND | MS_REC ;
            /** We need to apply it after. For example
             * if we have /dev with ro and /dev/pts was
             * asked, we cannot mount the submount on a ro filesystem*/
            nopts.flags &= ~MS_RDONLY ;

            log_trace("remount submount: ",target) ;
            if (!ns_mount(NULL,target,NULL,nopts.flags,SADATA.s + nopts.opts,entry->create))
                log_dieusys(LOG_EXIT_SYS,"remount submount: ",target) ;
        }
    }
}

/**
 *
 * Process function
 *
 * */

int raw_clone (unsigned long flags)
{
    #if defined(__s390__) || defined(__CRIS__)
        /* On s390 and cris the order of the first and second arguments
         * of the raw clone() system call is reversed.*/
        return (int) syscall (__NR_clone, 0, flags) ;
    #else
        return (int) syscall (__NR_clone, flags, 0) ;
    #endif
}

void parent_die(void)
{

    if (MNT_FLAGS_IS_SET(NS_CLONE_FLAGS,CLONE_NEWPID))
        if (prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0) != 0)
            log_dieusys(LOG_EXIT_SYS,"prctl deathsig") ;

}

int compute_exit(int wstat)
{
    if (WIFEXITED(wstat))
        return WEXITSTATUS(wstat) ;

    if (WIFSIGNALED(wstat))
        return 128 + WTERMSIG(wstat) ;

    return 111 ;
}

static int init (int parent_fd, pid_t child_pid)
{
    int iwstat = 1;

    parent_die() ;

    for(;;) {

        pid_t child ;
        int wstat ;

        child = wait(&wstat) ;

        if (child == child_pid) {

            iwstat = compute_exit(wstat) ;

            uint64_t e ;
            int w __attribute__((__unused__)) ;

            e = iwstat ;
            /* main_fd can be closed if parent die */
            if (parent_fd) {

                if (fcntl(parent_fd, F_GETFD) != -1)
                    w = write (parent_fd, &e, 8) ;
            }
        }

        /** no more children */
        if (child == -1 && errno != EINTR) {

            if (errno != ECHILD)
                log_dieusys(LOG_EXIT_SYS,"wait init") ;

            break ;
        }
    }
    close(parent_fd) ;
    return iwstat ;
}

static int handle_signal(pid_t child_pid)
{
    int wstat ;
    pid_t pid ;

    for (;;)
    {
        switch (selfpipe_read()) {

            case -1 :

                log_dieusys(LOG_EXIT_ZERO,"selfpipe_read") ;

            case 0 :

                return 0 ;

            case SIGTERM:
            case SIGINT:
            case SIGQUIT:
            case SIGCHLD:

                while ((pid = wait_nohang(&wstat)) > 0)
                    if (pid == child_pid)
                        return compute_exit(wstat) ;
                break ;
        }
    }
    log_die(LOG_EXIT_SYS,"please make a bug report") ;

    return 0 ;
}

static int monitor_child(int parent_fd,int wait_parent_fd,pid_t child_pid)
{

    int r ;
    ssize_t fdread ;
    uint64_t fdret = 1 ;
    iopause_fd x[2] = { { .events = IOPAUSE_READ }, { .events = IOPAUSE_READ } } ;
    tain deadline ;
    deadline = tain_infinite_relative ;
    tain_now_set_stopwatch_g() ;
    tain_add_g(&deadline,&deadline) ;

    x[1].fd = parent_fd ;
    x[0].fd = selfpipe_init() ;

    if (x[0].fd < 0)
        log_dieusys(LOG_EXIT_SYS, "selfpipe_trap") ;

    if (!selfpipe_trap(SIGINT))
        log_dieusys(LOG_EXIT_SYS, "selfpipe_trap") ;

    if (!selfpipe_trap(SIGTERM))
        log_dieusys(LOG_EXIT_SYS, "selfpipe_trap") ;

    if (!selfpipe_trap(SIGQUIT))
        log_dieusys(LOG_EXIT_SYS, "selfpipe_trap") ;

    if (!selfpipe_trap(SIGCHLD))
        log_dieusys(LOG_EXIT_SYS, "selfpipe_trap") ;

    fdread = write(wait_parent_fd, &fdret, 8) ;
    close(wait_parent_fd) ;

    for(;;) {

        r = iopause_g(x,2,&deadline) ;
        if (r == -1)
            log_dieusys(LOG_EXIT_SYS,"iopause") ;
        if (!r)
            log_dieusys(LOG_EXIT_SYS,"timeout") ;

        /** grandchild do not contain more children */
        if (x[1].revents & IOPAUSE_READ) {
            errno = 0 ;
            fdread = selfpipe_read() ;
            if (fdread == -1 && errno != EINTR && errno != EAGAIN)
                log_dieusys(LOG_EXIT_SYS,"read parent_fd") ;

            if (fdread == 8) {
                selfpipe_finish() ;
                return (int) fdret - 1 ;
            }
        }
        if (x[0].revents & IOPAUSE_READ) {
            int r = handle_signal(child_pid) ;
            selfpipe_finish() ;
            return r ;
        }
    }
    log_die(LOG_EXIT_SYS,"please make a bug report") ;

    return 0 ;
}

/**
 *
 * Namespace preparation and setup
 *
 * */

int ns_path_compare(const ns_entry_t *a,const ns_entry_t *b)
{
    return strcmp(SADATA.s + a->path, SADATA.s + b->path) ;
}

void ns_drop_element(genalloc *ga)
{
    log_flow() ;

    size_t pos = 0 ;
    size_t galen = genalloc_len(ns_entry_t,ga) ;

    FOREACH_GA(ga,ns_entry_t,pos) {

        if (genalloc_s(ns_entry_t,ga)[pos].skip || genalloc_s(ns_entry_t,ga)[pos].ignore)
            continue ;

        char *one = SADATA.s + genalloc_s(ns_entry_t,ga)[pos].path ;

        size_t ltwo = pos + 1 ;
        if (ltwo >= galen)
            break ;

        char *two = SADATA.s + genalloc_s(ns_entry_t,ga)[pos+1].path ;

        if (!strcmp(one,two)) {
            genalloc_s(ns_entry_t,ga)[pos].skip = 1 ;
            genalloc_s(ns_entry_t,ga)[pos].ignore = 1 ;
        }
    }
}

void ns_drop_hidden(genalloc *ga)
{
    log_flow() ;

    size_t pos = 0 ;
    size_t galen = genalloc_len(ns_entry_t,ga) ;
    uint8_t gparent = 0, hidden = 0 ;

    FOREACH_GA(ga,ns_entry_t,pos) {

        if (genalloc_s(ns_entry_t,ga)[pos].ignore)
            continue ;

        /** do not drop '/' */
        if (!pos && (SADATA.s[genalloc_s(ns_entry_t,ga)[pos].path] == '/' &&
        !SADATA.s[genalloc_s(ns_entry_t,ga)[pos].path + 1])) {

            gparent++ ;
            continue ;
        }

        hidden = genalloc_s(ns_entry_t,ga)[gparent].type == TYPE_HIDDEN ? 1 : 0 ;

        char *parent = SADATA.s + genalloc_s(ns_entry_t,ga)[gparent].path ;

        size_t lchild = pos + 1 ;
        if (lchild >= galen)
            break ;

        char *child = SADATA.s + genalloc_s(ns_entry_t,ga)[pos+1].path ;

        if (dir_is_child(parent,child) && hidden) {

            genalloc_s(ns_entry_t,ga)[pos+1].skip = 1 ;
            continue ;
        }

        gparent = pos + 1 ;

    }

}

void ns_clean_ns_entry(genalloc *gaentry)
{
    log_flow() ;

    qsort_indirect(genalloc_s(ns_entry_t,gaentry),genalloc_len(ns_entry_t,gaentry),ns_path_compare) ;

    ns_drop_element(gaentry) ;

    ns_drop_hidden(gaentry) ;

}

ssize_t ns_hidden_path(char const *path)
{
    log_flow() ;

    size_t path_len = strlen(path), hidden_len = strlen(NSHIDDEN) + 10 ;
    char hidden[path_len + hidden_len + 1] ;
    struct stat st ;

    if (lstat(path,&st) == -1)
        log_dieusys(LOG_EXIT_SYS,"find: ",path) ;

    if (S_ISREG(st.st_mode)) {

        auto_strings(hidden,NSHIDDEN,"/file") ;
        return ns_add_to_sadata(hidden) ;
    }
    else if (S_ISDIR(st.st_mode)) {

        auto_strings(hidden,NSHIDDEN,"/directory") ;
        return ns_add_to_sadata(hidden) ;
    }
    else if (S_ISFIFO(st.st_mode)){

        auto_strings(hidden,NSHIDDEN,"/fifo") ;
        return ns_add_to_sadata(hidden) ;
    }
    else if (S_ISLNK(st.st_mode)) {

        log_die(LOG_EXIT_SYS,"hidden type cannot be used for symlinks") ;
    }
    else if (S_ISCHR(st.st_mode)) {

        auto_strings(hidden,NSHIDDEN,"/chr") ;
        return ns_add_to_sadata(hidden) ;
    }
    else if (S_ISBLK(st.st_mode)) {

        auto_strings(hidden,NSHIDDEN,"/blk") ;
        return ns_add_to_sadata(hidden) ;
    }
    else if (S_ISSOCK(st.st_mode)) {

        auto_strings(hidden,NSHIDDEN,"/sock") ;
        return ns_add_to_sadata(hidden) ;
    }

    log_die(LOG_EXIT_SYS,"unrecognized node for: ",path) ;

    return -1 ;
}

void ns_resolve_symlinks(genalloc *gaentry)
{
    log_flow() ;

    size_t pos = 0 ;

    FOREACH_GA(gaentry,ns_entry_t,pos) {

        ns_entry_t_ref m = &genalloc_s(ns_entry_t,gaentry)[pos] ;

        if (m->ignore)
            continue ;

        char const *old = 0 ;
        char *sym = 0 ;

        /** MS_REC imply MS_BIND */
        if (MNT_FLAGS_IS_SET(m->flags,MS_BIND) || MNT_FLAGS_IS_SET(m->flags,MS_REC)) {

            old = SADATA.s + m->path ;
            sym = realpath(old,NULL) ;
            if (sym)
                m->path = ns_add_to_sadata(sym) ;
        }

        free(sym) ;
    }

}

void ns_apply_entry(ns_entry_t *entry,char const *root)
{
    log_flow() ;

    if (entry->ignore)
        return ;

    size_t prefix_len = strlen(root), len = 0 ;
    ssize_t mnt_type = -1 ;
    uint8_t recursive = 0 ;
    char *type = 0, *str = SADATA.s, *popts = 0 ;

    unsigned long flags ;
    len = strlen(str + entry->target) ;

    struct stat st ;

    char target[prefix_len + len + 1] ;
    auto_strings(target,root,str + entry->target) ;

    flags = entry->flags ;

    /** We need to apply it after. For example
     * if we have /dev with ro and /dev/pts was
     * asked, we cannot mount the submount on a ro filesystem*/
    flags &= ~MS_RDONLY ;

    switch (entry->type) {

        case TYPE_TMPFS:

            if (is_mnt(target))
                umount_recursive(target) ;

            log_trace("mount: tmpfs"," to: ",target) ;
            if (!ns_mount("tmpfs",target,"tmpfs",flags,str + entry->opts,entry->create))
                log_dieusys(LOG_EXIT_SYS,"mount: tmpfs"," to: ",target) ;

            return ;

        case TYPE_HIDDEN:

            if (is_mnt(target))
                umount_recursive(target) ;

            entry->path = ns_hidden_path(str + entry->path) ;

            /** ns_hidden_path change the pointer,
             * so redefine it */
            str = SADATA.s ;

            log_trace("mount: ",str + entry->path," to: ",target) ;
            if (!ns_mount(str + entry->path,target,NULL,flags,NULL,entry->create))
                log_dieusys(LOG_EXIT_SYS,"mount: ",str + entry->path," to: ",target) ;

            return ;

        case TYPE_RECURSIVE:

            if (is_mnt(target))
                umount_recursive(target) ;

            break ;

        case TYPE_CLONE:

            if (is_mnt(target))
                log_die(LOG_EXIT_USER,"type clone cannot be used for a mountpoint") ;

            ns_clone_node(str + entry->path,target) ;

            return ;

        case TYPE_PROC:

            if (is_mnt(target))
                umount_recursive(target) ;

            log_trace("mount: proc to: ",target) ;
            if (!ns_mount("proc",target,"proc",flags,NULL,entry->create))
                log_dieusys(LOG_EXIT_SYS,"mount: ",str + entry->path," to: ",target) ;

            return ;

        case TYPE_DEV:

            if (is_mnt(target))
                umount_recursive(target) ;

            log_trace("mount: dev to: ",target) ;
            if (!ns_mount("dev",target,"devtmpfs",flags,NULL,entry->create))
                log_dieusys(LOG_EXIT_SYS,"mount: ",str + entry->path," to: ",target) ;

            return ;

        case TYPE_SYS:

            if (is_mnt(target))
                umount_recursive(target) ;

            log_trace("mount: sys to: ",target) ;
            if (!ns_mount("sys",target,"sysfs",flags,NULL,entry->create))
                log_dieusys(LOG_EXIT_SYS,"mount: ",str + entry->path," to: ",target) ;

            return ;

        default:

            break ;
    }

    recursive = MNT_FLAGS_IS_SET(flags,MS_REC) ;

    if (is_mnt(target)) {

        mnt_type = mntfile_get(str + entry->path,MNTFILE_TYPE) ;
        if (mnt_type == -1)
            log_dieu(LOG_EXIT_USER,"get the type from mounts file of: ",str + entry->path) ;

        type = _MNTFILE_SA.s + mnt_type ;

        ns_entry_t nentry = ns_compute_opts(_MNTFILE_SA.s + mntfile_get(str + entry->path,MNTFILE_OPTS), str + entry->opts) ;

        flags |= nentry.flags ;

        popts = SADATA.s + nentry.opts ;

        /** ns_compute_opts modify the str pointer */
        str = SADATA.s ;

    }
    else {

        if (lstat(target,&st) == -1)
            ns_clone_node(str + entry->path,target) ;

        popts = SADATA.s + entry->opts ;
    }

    /** if target is already a mountpoint
     * remount it instead create a new one */
    if (is_mnt(target))
        flags |= MS_REMOUNT ;


    log_trace("mount: ",str + entry->path," to: ",target) ;
    if (!ns_mount(str + entry->path,target,type,flags,popts,entry->create))
        log_dieusys(LOG_EXIT_SYS,"mount: ",str + entry->path," to: ",target) ;

    /** We need to remount everything under the directory. */
    if (recursive)
        ns_mount_recursive(root,entry) ;
}

void ns_setup_ns(genalloc *gaentry)
{
    log_flow() ;

    ns_resolve_symlinks(gaentry) ;

    /** make a private copy of '/' */
    if (unshare(CLONE_NEWNS) == -1) {
        log_warnusys("unshare") ;
            goto err ;
    }

     /* disconnect '/' from the host. Every new mountpoint will be
      * not visible from the host. However, the namespace view the
      * mountpoint of the host. */
    if (!ns_mount(NULL, "/", NULL, MS_SLAVE|MS_REC, NULL, 0)) {
        log_warnusys("remount / as SLAVE") ;
        goto err ;
   }

    /* we are disconnected, we can mount the '/' to the temporary
     * directory. */
    if (!ns_mount("/", NSTMP, NULL, MS_BIND|MS_REC, NULL, 0)) {
        log_warnusys("mount / at: ",NSTMP) ;
        goto err ;
    }
    /** Make NSTMP unbindable, this is avoid to have
     * the NSTMP mounted when /run was asked with MS_REC */
    if (!ns_mount(NULL, NSTMP, NULL, MS_UNBINDABLE, NULL, 0)) {
        log_warnusys("mount /run/66/ns/nstmp unbindable") ;
        goto err ;
    }

    ns_clean_ns_entry(gaentry) ;

    if (genalloc_len(ns_entry_t,gaentry)) {

        size_t pos = 0 ;

        FOREACH_GA(gaentry,ns_entry_t,pos) {

            ns_entry_t m = genalloc_s(ns_entry_t,gaentry)[pos] ;

            if (m.done)
                continue ;

            ns_apply_entry(&m,NSTMP) ;

            m.done = 1 ;
        }

        {
            /** Apply MS_RDONLY flag */
            pos = 0 ;
            FOREACH_GA(gaentry,ns_entry_t,pos) {

                ns_entry_t m = genalloc_s(ns_entry_t,gaentry)[pos] ;

                mnt_remount_ro(&m,gaentry) ;

            }
        }
    }

    /** NSTMP become '/' */
    if (!mount_move_root(NSTMP))
        goto err ;

    /** Remount '/' */
    if (!ns_mount(NULL, "/", NULL, NS_MNT_FLAGS | MS_REC, NULL, 0)) {
        log_warnusys("remount root") ;
        goto err ;
    }

    rm_rf(NSTMP) ;
    return ;

    err:
        rm_rf(NSTMP) ;
        log_dieu(LOG_EXIT_SYS,"setup namespace") ;
}

void ns_prepare_hidden_directory(void)
{
    log_flow() ;

    size_t i = 0 ;
    mode_t u ;
    static const struct {
        const char *name ;
        mode_t mode ;
    }
    table[] = {
        { "/run/66/ns/hidden",              S_IFDIR  | 0000 },
        { "/run/66/ns/hidden/file",         S_IFREG  | 0000 },
        { "/run/66/ns/hidden/directory",    S_IFDIR  | 0000 },
        { "/run/66/ns/hidden/fifo",         S_IFIFO  | 0000 },
        { "/run/66/ns/hidden/sock",         S_IFSOCK | 0000 },
        { "/run/66/ns/hidden/chr",          S_IFCHR  | 0000 },
        { "/run/66/ns/hidden/blk",          S_IFBLK  | 0000 },
    } ;


    u = umask(0000) ;

    if (rm_rf(NSHIDDEN) == -1)
        log_dieusys(LOG_EXIT_SYS,"remove directory: ",NSHIDDEN) ;

    for (; i < nb_el(table) ; i++) {

        char path[strlen(table[i].name) + 1] ;

        auto_strings(path,table[i].name) ;

        if (S_ISDIR(table[i].mode)) {
            if (!dir_create_parent(path,table[i].mode & 07777))
                log_dieusys(LOG_EXIT_SYS,"create hidden directory: ", path) ;
        }
        else {
            if (mknod(path, table[i].mode, (dev_t)0) == -1)
                log_dieusys(LOG_EXIT_SYS,"create hidden node: ", path) ;
        }
    }
    umask(u) ;
}

void ns_prepare_directory(void)
{
    log_flow() ;

    mode_t mode = S_IRWXU ;// 0700

    if (rm_rf(NSPREFIX) == -1)
        log_dieusys(LOG_EXIT_SYS,"remove directory: ",NSPREFIX) ;

    if ((!dir_create_parent(NSPREFIX,mode)) || (chmod(NSPREFIX,mode) == -1))
        log_dieusys(LOG_EXIT_SYS,"create directory: ",NSPREFIX) ;

    if (rm_rf(NSTMP) == -1)
        log_dieusys(LOG_EXIT_SYS,"remove directory: ",NSTMP) ;

    if (!dir_create_parent(NSTMP,mode))
        log_dieusys(LOG_EXIT_SYS,"create directory: ",NSTMP) ;

    ns_prepare_hidden_directory() ;
}

/**
 *
 * Parse function
 *
 * */

void ns_compute_entry(ns_entry_t *entry)
{
    log_flow() ;

    /** check if the entry exist.
     * if not and entry->ignore is set to no
     * die else warn the user */
     struct stat st ;

     if (lstat(SADATA.s + entry->path,&st) == -1) {
        if (!entry->ignore) {
            log_dieusys(LOG_EXIT_SYS,"find: ",SADATA.s + entry->path) ;
        } else {
            log_warnsys("ignoring entry: ",SADATA.s + entry->path) ;
            entry->done = entry->skip = 1 ;
        }
    } else {
        entry->ignore = 0 ;
    }

    // no target was set, use the same as path
    if (entry->target == -1) {
        entry->target = entry->path ;
    }

    switch(entry->type) {

        case TYPE_TMPFS:

            entry->flags &= ~MS_BIND ;
            entry->flags &= ~MS_REC ;

            break ;

        case TYPE_HIDDEN:

            entry->flags = MS_BIND | MS_RDONLY | MS_NOSUID | MS_NODEV  ;

            break ;

        case TYPE_RECURSIVE:

            entry->flags |= MS_BIND | MS_REC ;

            break ;

        case TYPE_CLONE:

            entry->flags = 0 ;
            entry->opts = -1 ;

            break ;

        case TYPE_PROC:

            if (strcmp(SADATA.s + entry->path,"/proc"))
                log_die(LOG_EXIT_USER,"type proc was asked -- found element: ",SADATA.s + entry->path) ;

            entry->flags = MS_NOSUID | MS_NODEV | MS_NOEXEC ;
            entry->skip = entry->done = 0 ;

            break ;

        case TYPE_DEV:

            if (strcmp(SADATA.s + entry->path,"/dev"))
                log_die(LOG_EXIT_USER,"type dev was asked -- found element: ",SADATA.s + entry->path) ;

            entry->flags = MS_NOSUID| MS_STRICTATIME | MS_NOEXEC | MS_REC ;
            entry->opts = ns_add_to_sadata("mode=755") ;
            entry->skip = entry->done = 0 ;

            break ;

        case TYPE_SYS:

            if (strcmp(SADATA.s + entry->path,"/sys"))
                log_die(LOG_EXIT_USER,"type sys was asked -- found element: ",SADATA.s + entry->path) ;

            entry->flags = MS_NOSUID | MS_STRICTATIME | MS_NOEXEC ;
            entry->skip = entry->done = 0 ;

            break ;


        default:

            entry->flags |= MS_BIND ;

            break ;

    }
}

static void ns_parse_line(genalloc *gaentry, char const *str)
{
    log_flow() ;

    size_t pos = 0, len = 0 ;
    ssize_t r = 0 ;

    char *current = 0, *key = 0, *val = 0 ;

    _alloc_stk_(stk, strlen(str) + 1) ;

    ns_rule_map_t const *t ;

    ns_entry_t entry = NS_ENTRY_ZERO ;

    if (!lexer_trim_with_delim(&stk,str,NS_COLON_DELIM))
        log_dieu(LOG_EXIT_SYS,"parse directory options") ;

    unsigned int nopts = 0 , old = 0 ;

    /** +1, the first string is the name of
     * the element to handle */
    rule_checkopts(stk.count + 1) ;

    FOREACH_STK(&stk,pos) {

        current = stk.s + pos ;

        if (!pos) {

            if (current[0] != '/')
                 log_die(LOG_EXIT_USER,"Path must be absolute: ",current) ;

            entry.path = ns_add_to_sadata(current) ;
            continue ;
        }

        t = ns_rule_table ;
        old = nopts ;

        for (; t->str; t++)
        {
            len = strlen(current) ;

            char tmp[len + 1] ;

            r = get_key(tmp,current) ;

            if (r == -1)
                log_die(LOG_EXIT_USER,"invalid format of key: ",current," for entry: ",SADATA.s + entry.path) ;

            key = tmp ;
            val = current + r ;

            if (!strcmp(key,t->str))
            {
                if ((get_len_until(val,',') >= 0) && (t->id != RULE_OPTS))
                    log_die(LOG_EXIT_USER,"only one value is allowed with options ",key," for entry: ",SADATA.s + entry.path) ;

                switch(t->id)
                {
                    case RULE_TARGET:

                        if (val[0] != '/')
                            log_die(LOG_EXIT_USER,"Path must be absolute: ",val," for entry: ",SADATA.s + entry.path) ;

                        entry.target = ns_add_to_sadata(val) ;

                        break ;

                    case RULE_TYPE:

                        r = get_enum_type_flags_by_key(val) ;

                        if (r == -1)
                            log_die(LOG_EXIT_SYS,"invalid value for key: ",key," for entry: ",SADATA.s + entry.path) ;

                        entry.type = r ;

                        break ;

                     case RULE_OPTS:

                        entry.opts = ns_add_to_sadata(val) ;

                        break ;

                    case RULE_IGNORE:

                        if (val[0] == 'y')
                            entry.ignore = 1 ;

                       break ;

                    case RULE_CREATE:

                        if (val[0] == 'n')
                            entry.create = 0 ;

                        break ;

                    default:
                        // nothing to do here
                        break ;

                }
                nopts++ ;
                break ;
            }
        }
        if (old == nopts)
            log_die(LOG_EXIT_USER,"invalid option: ",current," for entry: ",SADATA.s + entry.path) ;
    }

    if (entry.opts >= 0)
        mount_split_opts_flags(&entry,SADATA.s + entry.opts) ;

    ns_compute_entry(&entry) ;

    if (!genalloc_append(ns_entry_t,gaentry,&entry))
        log_die_nomem("genalloc") ;
}

int ns_get_section(stralloc *secname, char const *str,size_t *pos)
{
    log_flow() ;

    size_t len = strlen(str) ;
    _alloc_stk_(stk, len + 1) ;
    lexer_config cfg = LEXER_CONFIG_ZERO ;
    cfg.open = "[" ; cfg.olen = 1 ;
    cfg.close = "]" ; cfg.clen = 1 ;
    cfg.skip = " \t\r" ; cfg.skiplen = 3 ;
    cfg.forceclose = 1 ; cfg.firstoccurence = 1 ;
    cfg.kopen = 0 ; cfg.kclose = 0 ;

    while ((*pos) < len) {

        stack_reset(&stk) ;
        cfg.found = 0 ;

        if (!lexer_trim_with_g(&stk, str + (*pos), &cfg))
            return 0 ;

        (*pos) += cfg.pos ;

        if (cfg.found) break ;
        if (!cfg.count) {
            /** no more section*/
            *pos = len ;
        }
    }

    if (cfg.found) {
        if (!stralloc_catb(secname,stk.s,strlen(stk.s) + 1))
            return -1 ;
    }

    return cfg.found ;
}

void ns_rebuild_rule_for_entry(stralloc *sadir, stralloc *sa,char const *secname)
{
    log_flow() ;

    size_t pos = 0, next = 0 ;

    stralloc tmp = STRALLOC_ZERO ;

    if (!sastr_split_string_in_nline(sa))
        log_dieu(LOG_EXIT_SYS,"split string") ;

    // copy first the directory path
    if (!auto_stra(&tmp,secname,":"))
            log_die_nomem("stralloc") ;

    FOREACH_SASTR(sa,pos) {

        next = pos + (strlen(sa->s + pos) + 1) < sa->len ? 1 : 0 ;

        if (!stralloc_catb(&tmp,sa->s + pos,strlen(sa->s + pos)))
            log_die_nomem("stralloc") ;

        if (next)
            if (!stralloc_catb(&tmp,":",1))
                log_die_nomem("stralloc") ;
    }

    if (!stralloc_0(&tmp))
        log_die_nomem("stralloc") ;

    if (!sastr_rebuild_in_oneline(&tmp))
        log_dieu(LOG_EXIT_SYS,"rebuild line") ;

    if (!stralloc_catb(sadir,tmp.s,strlen(tmp.s) + 1))
        log_die_nomem("stralloc") ;

    stralloc_free(&tmp) ;
}

void ns_split_from_section(stralloc *sadir, stralloc *secname, char const *str, char const *filename)
{
    log_flow() ;

    stralloc sarule = STRALLOC_ZERO ;
    stralloc tmp = STRALLOC_ZERO ;

    // find the name of the current section
    ssize_t previous_sec = ns_get_previous_element(secname,0) ;

    size_t pos = 0 ;

    if (previous_sec == -1)
        log_dieu(LOG_EXIT_SYS,"get previous section") ;

    // we are on include section
    if (secname->s[previous_sec] == 'i') {

        if (!stralloc_cats(&tmp,str))
            log_die_nomem("stralloc") ;

        if (!sastr_split_string_in_nline(&tmp))
            log_dieu(LOG_EXIT_SYS,"split string") ;

        pos = 0 ;
        FOREACH_SASTR(&tmp,pos) {

            char *filename = tmp.s + pos ;

            sarule.len = 0 ;

            /** try first with relative/absolute path.*/
            if (!file_readputsa_g(&sarule,filename))
                /** try with SS_TOOLS_NSRULE prefix*/
                if (!file_readputsa(&sarule,SS_TOOLS_NSRULE,filename))
                    log_dieusys(LOG_EXIT_SYS,"open rule file: ", filename) ;

            // parse each new rule
            ns_parse_rule(sadir,&sarule,filename) ;
        }
    }
    else {

        // so, directory
        if (secname->s[previous_sec] != '/')
            log_die(LOG_EXIT_USER,"Path must be absolute: ",secname->s + previous_sec," on rule file",filename) ;

        /** -e options overwrite a same element
         * found on a rule file*/
        if (sastr_cmp(sadir,secname->s) >= 0)
            return ;

        if (!stralloc_cats(&tmp,str) ||
            !stralloc_0(&tmp))
                log_die_nomem("stralloc") ;

        if (!tmp.len)
            log_die(LOG_EXIT_USER,"section: ",secname->s," is empty") ;

        // prepare to be able to use ns_parse_line() function
        ns_rebuild_rule_for_entry(sadir,&tmp,secname->s + previous_sec) ;
    }

    stralloc_free(&tmp) ;
    stralloc_free(&sarule) ;
}

void ns_parse_rule(stralloc *sadir,stralloc *sarule, char const *filename)
{
    log_flow() ;

    stralloc secname = STRALLOC_ZERO ;

    size_t pos = 0, start = 0, len = 0 ;

    int found = 0 ;

    while (pos < sarule->len) {

        found = 0 ;

        /** keep the starting point of the begin
         * of the section search process */
        start = pos ;

        // get the section name
        found = ns_get_section(&secname,sarule->s,&pos) ;

        if (found == -1)
            log_die_nomem("stralloc") ;

        // we are not able to find any section and we are at the start of the file
        if (!found && !start)
            log_die(LOG_EXIT_USER,"invalid rule file: ", filename) ;

        // not more section, end of file. We copy the rest of the file
        if (!found) {

            len = sarule->len - start ;

            char tmp[len + 1] ;

            auto_strings(tmp,sarule->s + start + 1) ;

            ns_split_from_section(sadir,&secname,tmp,filename) ;

            break ;
        }
        else {

            /** again, keep the starting point of the begin
             * of the section search process */
            start = pos ;

            found = 0 ;

            /* search for the next section
             * we don't want to keep the section name to
             * avoid a double entry at the next pass of the loop */
            len = secname.len ;
            found = ns_get_section(&secname,sarule->s,&pos) ;
            secname.len = len ;

            if (found == -1)
                log_die_nomem("stralloc") ;

            int r = get_rlen_until(sarule->s,'\n',pos-1) ;

            // found a new one
            if (found) {

                len = r - start ;

                char tmp[len + 1] ;
                memcpy(tmp, sarule->s + start + 1, len) ;
                tmp[len] = 0 ;

                ns_split_from_section(sadir, &secname, tmp, filename) ;
            }
            else {

                len = sarule->len - start  ;

                char tmp[len + 1] ;

                auto_strings(tmp,sarule->s + start + 1) ;

                ns_split_from_section(sadir,&secname,tmp,filename) ;

                // end of file
                break ;
            }
            /** restart the next research from the end of the previous
             * copy A.K.A start of the next section just found previously */
            pos = start ;
        }
    }

    stralloc_free(&secname) ;
}

static void ns_parse_options(char const *str)
{
    log_flow() ;

    size_t pos = 0, len = 0 ;
    ssize_t r ;
    char *current = 0, *key = 0, *val = 0 ;
    ns_opts_map_t const *t ;

    _alloc_stk_(stk, strlen(str) + 1) ;

    if (!lexer_trim_with_delim(&stk,str,NS_COMMA_DELIM))
        log_dieu(LOG_EXIT_SYS,"clean options") ;

    unsigned int nopts = 0 , old ;

    ns_checkopts(stk.count) ;

    FOREACH_STK(&stk,pos) {

        current = stk.s + pos ;
        t = ns_opts_table ;
        old = nopts ;

        for (; t->str; t++) {

            len = strlen(current) ;
            char tmp[len + 1] ;

            r = get_key(tmp,current) ;
            if (r != -1) {

                key = tmp ;
                val = current + r ;
            }
            else
                key = val = current ;

            if (!strcmp(key,t->str)) {

                switch(t->id) {

                    case OPTS_FLAGS:

                        if (!strcmp(val,"private")) {

                            NS_MNT_FLAGS = MS_PRIVATE ;

                        }
                        else if (!strcmp(val,"slave")) {

                            NS_MNT_FLAGS = MS_SLAVE ;

                        }
                        else if (!strcmp(val,"unbindable")) {

                            NS_MNT_FLAGS = MS_UNBINDABLE ;

                        }
                        else if (!strcmp(val,"shared")) {

                            /** default */
                            break ;
                        }
                        else
                            log_die(LOG_EXIT_SYS,"invalid flag option: ",val) ;

                        break ;

                    case OPTS_PRIVELEGES:

                        if (!strcmp(val,"nonewprivileges"))

                            NS_NONEWPRIVELEGES = 1 ;

                        else
                            log_die(LOG_EXIT_SYS,"invalid option: ",current) ;

                        break ;

                    case OPTS_UNSHARE:

                        {
                            size_t upos = 0 ;
                            struct stat st ;
                            _alloc_stk_(unsa, strlen(val) + 1) ;
                            char *uncurrent = 0 ;

                            if (!lexer_trim_with_delim(&unsa,val,NS_COLON_DELIM))
                                log_dieu(LOG_EXIT_SYS,"clean unshare options") ;

                            FOREACH_STK(&unsa,upos) {

                                uncurrent = unsa.s + upos ;

                                if (!strcmp(uncurrent,"all")) {

                                    NS_CLONE_FLAGS |= CLONE_NEWNET | CLONE_NEWIPC | CLONE_NEWUTS | CLONE_NEWCGROUP ;

                                }
                                else if (!strcmp(uncurrent,"pid")) {

                                    NS_CLONE_FLAGS |= CLONE_NEWPID ;

                                    PID1 = 1 ;
                                }
                                else if (!strcmp(uncurrent,"net")) {

                                    NS_CLONE_FLAGS |= CLONE_NEWNET ;
                                }
                                else if (!strcmp(uncurrent,"ipc")) {

                                    NS_CLONE_FLAGS |= CLONE_NEWIPC ;

                                }
                                else if (!strcmp(uncurrent,"uts")) {

                                    NS_CLONE_FLAGS |= CLONE_NEWUTS ;

                                }
                                else if (!strcmp(uncurrent,"cgroup")) {

                                    if (stat("/proc/self/ns/cgroup", &st)) {

                                        if (errno == ENOENT)
                                            log_die(LOG_EXIT_SYS,"kernel does not support cgroup namespace") ;

                                        else
                                            log_dieusys(LOG_EXIT_SYS,"stat: /proc/self/ns/cgroup") ;
                                    }
                                    NS_CLONE_FLAGS |= CLONE_NEWCGROUP ;
                                }
                                else
                                    log_die(LOG_EXIT_SYS,"invalid unshare option: ",uncurrent) ;
                            }

                        }

                        break ;

                    case OPTS_HOSTNAME:

                        HOSTNAME = (size_t)ns_add_to_sadata(val) ;

                        break ;

                    default:

                        break ;
                }
                nopts++ ;
            }
        }
        if (old == nopts)
            log_die(LOG_EXIT_SYS,"invalid option: ",current) ;
    }
    if (HOSTNAME)
        NS_CLONE_FLAGS |= CLONE_NEWUTS ;
}

int main(int argc, char const *const *argv, char const *const *envp)
{

    /* Nothing to do if we are not root */
    if (getuid()) {
        errno = EPERM ;
        log_diesys(LOG_EXIT_USER,"you must be root to run this program") ;
    }

    /* Type: ns_entry_t
     * list of all mount entry set by commandline or rule files */
    stralloc gaentry = GENALLOC_ZERO ;
    stralloc sadir = STRALLOC_ZERO ; // element list to handle
    stralloc sarule = STRALLOC_ZERO ; // rule list

    size_t pos = 0 ;
    ssize_t fdread __attribute__((__unused__)) ;
    uint64_t fdret ;
    pid_t pid ;
    mode_t omask ;
    int parent_fd = -1, wait_parent_fd = -1, notif = -1 ;

    log_color = &log_color_disable ;

    PROG = "66-ns" ;
    {

        subgetopt l = SUBGETOPT_ZERO ;

        for (;;)
        {
            int opt = subgetopt_r(argc, argv, "hzv:d:e:r:o:", &l) ;

            if (opt == -1) break ;

            switch (opt)
            {
                case 'h':

                    info_help() ; return 0 ;

                case 'z':

                    log_color = !isatty(1) ? &log_color_disable : &log_color_enable ;

                    break ;

                case 'v':

                    if (!uint0_scan(l.arg, &VERBOSITY))
                            log_usage(USAGE) ;
                    break ;

                case 'd':

                    {
                        unsigned int u ;

                        if (!uint0_scan(l.arg, &u))
                            log_usage(USAGE) ;

                        notif = !isatty(1) ? u : -1 ;

                        break ;
                    }

                case 'e':

                    if (l.arg[0] != '/')
                        log_die(LOG_EXIT_USER,"Path must be absolute: ",l.arg) ;

                    if (!sastr_add_string(&sadir,l.arg))
                        log_die_nomem("stralloc") ;

                    break ;

                case 'r':

                    if (!sastr_add_string(&sarule,l.arg))
                        log_dieusys(LOG_EXIT_SYS,"store rules file to handle") ;

                    break ;

                case 'o':

                    ns_parse_options(l.arg) ;

                    break ;

                default:

                    log_usage(USAGE) ;
            }
        }
        argc -= l.ind ; argv += l.ind ;
    }

    if (!argc) log_usage(USAGE) ;

    if (notif > 0)
    {
        if (notif < 3)
            log_die(LOG_EXIT_USER,"notification fd must be 3 or more") ;

        if (fcntl(notif, F_GETFD) < 0)
            log_diesys(LOG_EXIT_USER,"invalid notification fd") ;
    }

    if (sarule.len) {

        stralloc rule = STRALLOC_ZERO ;

        pos = 0 ;

        FOREACH_SASTR(&sarule,pos) {

            char *filename = sarule.s + pos ;

            rule.len = 0 ;

            /** try first with relative/absolute path.*/
            if (!file_readputsa_g(&rule,filename))
                /** try with SS_TOOLS_NSRULE prefix*/
                if (!file_readputsa(&rule,SS_TOOLS_NSRULE,filename))
                    log_dieusys(LOG_EXIT_SYS,"open rule file: ", filename) ;

            ns_parse_rule(&sadir,&rule,filename) ;
        }

        stralloc_free(&rule) ;
    }

    pos = 0 ;

    FOREACH_SASTR(&sadir,pos) {

        char *str = sadir.s + pos ;

        // Pass -d and -r to struct ns_entry_t
        ns_parse_line(&gaentry,str) ;
    }

    /** Freed what we don't need anymore */
    stralloc_free(&sadir) ;
    stralloc_free(&sarule) ;

    if (NS_NONEWPRIVELEGES)
        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0)
            log_dieusys(LOG_EXIT_SYS,"set NO_NEW_PRIVILEGES") ;

    ns_prepare_directory() ;

    /** save the /proc/self/mounts file */
    mntfile_init() ;

    parent_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK) ;
    if (parent_fd == -1)
        log_dieusys(LOG_EXIT_SYS,"parent_fd") ;

    wait_parent_fd = eventfd(0, EFD_CLOEXEC) ;
    if (wait_parent_fd == -1)
        log_dieusys(LOG_EXIT_SYS,"wait_parent_fd") ;

    /* we clone */
    pid = raw_clone(NS_CLONE_FLAGS) ;
    if (pid == -1)
        log_dieusys(LOG_EXIT_SYS,"clone") ;


    if (pid > 0) {

        parent_die() ;

        genalloc_free(ns_entry_t,&gaentry) ;
        stralloc_free(&SADATA) ;
        mntfile_free() ;

        return monitor_child(parent_fd, wait_parent_fd, pid) ;
    }

    /** wait for the parent to be ready */
    fdread = read(wait_parent_fd, &fdret, 8) ;
    close(wait_parent_fd) ;


    /*
     *
     *
     * NEED TO BE IMPLEMENTED
     *
     *
     *
     * if (MNT_FLAGS_IS_SET(NS_CLONE_FLAGS,CLONE_NEWNET))
     *  loopback_setup() ;
     *
     * */

    omask = umask(0) ;

    ns_setup_ns(&gaentry) ;

    if (HOSTNAME)
        if (sethostname(SADATA.s + HOSTNAME, strlen(SADATA.s + HOSTNAME)) == -1)
            log_dieusys(LOG_EXIT_SYS,"set hostname: ", SADATA.s + HOSTNAME) ;

    umask(omask) ;

    /** Freed what we don't need anymore */
    genalloc_free(ns_entry_t,&gaentry) ;
    stralloc_free(&SADATA) ;
    mntfile_free() ;

    if (PID1) {

        /* Make pid 1 inside pid namespace to avoid zombies */
        pid = fork() ;

        if (pid == -1)
            log_dieusys(LOG_EXIT_SYS,"fork pid 1") ;

        if (pid)
            return init(parent_fd, pid) ;
    }

    parent_die() ;

    notify(&notif) ;

    exec_ae(argv[0],argv,envp) ;

    log_dieusys(LOG_EXIT_SYS,"exec: ",argv[0]) ;
}
