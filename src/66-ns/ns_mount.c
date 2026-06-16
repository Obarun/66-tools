/*
 * ns_mount.c
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
 *
 * The mount engine: /proc/self/mounts snapshot, mount/umount helpers, the
 * read-only and recursive remount passes, root pivot and the in-child setup
 * orchestration. All state lives in ns_t.
 */

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <mntent.h>

#include <oblibs/log.h>
#include <oblibs/opt.h>
#include <oblibs/lexer.h>
#include <oblibs/strbuf.h>
#include <oblibs/sbl.h>
#include <oblibs/genbuf.h>
#include <oblibs/string.h>
#include <oblibs/types.h>
#include <oblibs/directory.h>
#include <oblibs/files.h>

#include "ns.h"
#include "ns_internal.h"

// mount-options string <-> kernel flags
enum { MNT_ADD, MNT_IGNORE } ;

typedef struct mnt_opt_s mnt_opt_t ;
struct mnt_opt_s {
    char const   *name ;
    unsigned long flag ;
    int           action ;
} ;

static mnt_opt_t const mnt_opt_table[] = {
    { "rw",            MS_RDONLY,      MNT_IGNORE },
    { "ro",            MS_RDONLY,      MNT_ADD    },
    { "bind",          MS_BIND,        MNT_ADD    },
    { "rbind",         MS_BIND|MS_REC, MNT_ADD    },
    { "atime",         MS_NOATIME,     MNT_IGNORE },
    { "noatime",       MS_NOATIME,     MNT_ADD    },
    { "dev",           MS_NODEV,       MNT_IGNORE },
    { "nodev",         MS_NODEV,       MNT_ADD    },
    { "diratime",      MS_NODIRATIME,  MNT_IGNORE },
    { "nodiratime",    MS_NODIRATIME,  MNT_ADD    },
    { "exec",          MS_NOEXEC,      MNT_IGNORE },
    { "noexec",        MS_NOEXEC,      MNT_ADD    },
    { "mand",          MS_MANDLOCK,    MNT_ADD    },
    { "nomand",        MS_MANDLOCK,    MNT_IGNORE },
    { "relatime",      MS_RELATIME,    MNT_ADD    },
    { "norelatime",    MS_RELATIME,    MNT_IGNORE },
    { "strictatime",   MS_STRICTATIME, MNT_ADD    },
    { "nostrictatime", MS_STRICTATIME, MNT_IGNORE },
    { "suid",          MS_NOSUID,      MNT_IGNORE },
    { "nosuid",        MS_NOSUID,      MNT_ADD    },
    { "iversion",      MS_I_VERSION,   MNT_ADD    },
    { "noiversion",    MS_I_VERSION,   MNT_IGNORE },
    { "sync",          MS_SYNCHRONOUS, MNT_ADD    },
    { "async",         MS_SYNCHRONOUS, MNT_IGNORE },
    { "move",          MS_MOVE,        MNT_IGNORE },
    { "remount",       MS_REMOUNT,     MNT_IGNORE },
    { "dirsync",       MS_DIRSYNC,     MNT_ADD    },
    { "shared",        MS_SHARED,      MNT_ADD    },
    { "slave",         MS_SLAVE,       MNT_ADD    },
    { "private",       MS_PRIVATE,     MNT_ADD    },
    { "unbindable",    MS_UNBINDABLE,  MNT_ADD    },
} ;

static int mnt_flag_consume(unsigned long *flags, char const *tok)
{
    for (size_t i = 0 ; i < OPT_COUNT(mnt_opt_table) ; i++) {

        if (!strcmp(mnt_opt_table[i].name, tok)) {

            if (mnt_opt_table[i].action == MNT_ADD) {
                *flags |= mnt_opt_table[i].flag ;
                return 1 ;
            }

            *flags &= ~mnt_opt_table[i].flag ;
            return 0 ;
        }
    }
    return 0 ;
}

void ns_split_opts(ns_t *ns, char const *optstr, unsigned long *flags, ssize_t *kept)
{
    *kept = -1 ;

    if (!optstr || !*optstr)
        return ;

    size_t slen = strlen(optstr) ;
    _alloc_strbuf_(toks, slen + 1) ;

    if (!lexer_trim_with_delim(&toks, optstr, ','))
        log_dieu(LOG_EXIT_USER, "parse options: ", optstr) ;

    _alloc_sbl_(keep, slen + 1) ;

    size_t pos = 0 ;
    {
        FOREACH_SBL(&toks, pos) {

            char const *o = toks.s + pos ;
            if (strchr(o, '=')) {

                if (!sbl_add(&keep, o))
                    log_die_nomem("options") ;

            } else if (!mnt_flag_consume(flags, o)) {

                if (!sbl_add(&keep, o))
                    log_die_nomem("options") ;
            }
        }
    }

    if (keep.len) {

        if (!sbl_rebuild_with_delim(&keep, ','))
            log_dieu(LOG_EXIT_USER, "rebuild options: ", optstr) ;

        *kept = (ssize_t)ns_arena_add(ns, keep.s) ;
    }
}

void ns_compute_opts(ns_t *ns, char const *a, char const *b, unsigned long *flags, ssize_t *kept)
{
    if (!a) a = "" ;
    if (!b) b = "" ;
    *flags = 0 ;

    _alloc_strbuf_(o, strlen(a) + strlen(b) + 2) ;

    if (!strbuf_cats(&o, a))
        log_die_nomem("options") ;

    if (*a && *b) {
        if (!strbuf_cats(&o, ","))
            log_die_nomem("options") ; ;
    }

    if (!strbuf_cats(&o, b) || !strbuf_terminate(&o))
        log_die_nomem("options") ;

    ns_split_opts(ns, o.s, flags, kept) ;
}

// /proc/self/mounts snapshot

static size_t mntsa_add(ns_t *ns, char const *s)
{
    size_t off = ns->mntsb.len ;
    if (!strbuf_catb(&ns->mntsb, s, strlen(s) + 1))
        log_die_nomem("mntsb") ;

    return off ;
}

void ns_mntinfo_init(ns_t *ns)
{
    log_flow() ;

    FILE *f = setmntent("/proc/self/mounts", "r") ;
    if (!f)
        log_dieusys(LOG_EXIT_SYS, "open: /proc/self/mounts") ;

    struct mntent *m ;
    while ((m = getmntent(f))) {
        mntinfo_t cp = MNTINFO_ZERO ;
        cp.dir  = (ssize_t)mntsa_add(ns, m->mnt_dir) ;
        cp.type = (ssize_t)mntsa_add(ns, m->mnt_type) ;
        cp.opts = (ssize_t)mntsa_add(ns, m->mnt_opts) ;
        if (!genbuf_append(mntinfo_t, &ns->mntinfo, &cp))
            log_die_nomem("mntinfo") ;
    }

    endmntent(f) ;
}

static char const *mntinfo_find(ns_t *ns, char const *path, int want_opts)
{
    size_t plen = strlen(path) ;
    char p[plen + 1] ;
    auto_strings(p, path) ;

    dir_unslash(p) ;

    size_t n = genbuf_len(mntinfo_t, &ns->mntinfo) ;
    mntinfo_t *a = genbuf_s(mntinfo_t, &ns->mntinfo) ;

    for (size_t i = 0 ; i < n ; i++) {

        if (!strcmp(ns->mntsb.s + a[i].dir, p))
            return ns->mntsb.s + (want_opts ? a[i].opts : a[i].type) ;
    }

    return 0 ;
}

char const *ns_mntinfo_type(ns_t *ns, char const *path)
{
    return mntinfo_find(ns, path, 0) ;
}
char const *ns_mntinfo_opts(ns_t *ns, char const *path)
{
    return mntinfo_find(ns, path, 1) ;
}

// primitives

int ns_is_mnt(char const *str)
{
    struct stat st ;
    size_t slen = strlen(str) ;

    if (lstat(str, &st) < 0)
        return 0 ;

    if (!S_ISDIR(st.st_mode))
        return 0 ;

    dev_t st_dev = st.st_dev ;
    ino_t st_ino = st.st_ino ;
    char p[slen + 4] ;
    auto_strings(p, str, "/..") ;

    if (!stat(p, &st)) {
        if ((st_dev == st.st_dev) && (st_ino != st.st_ino))
            return 0 ; /* same device, different inode -> parent reachable -> not a mountpoint */
    }
    return 1 ;
}

void ns_clone_node(char const *path, char const *target)
{
    log_flow() ;

    struct stat st ;
    if (lstat(path, &st) == -1)
        log_dieusys(LOG_EXIT_SYS, "find: ", path) ;

    if (S_ISREG(st.st_mode)) {

        log_trace("create file: ", target) ;
        if (!file_copy(path, target, st.st_mode))
            log_dieusys(LOG_EXIT_SYS, "create file: ", target) ;

    } else if (S_ISDIR(st.st_mode)) {

        log_trace("create directory: ", target) ;
        if (!dir_create_parent(target, st.st_mode & 07777))
            log_dieusys(LOG_EXIT_SYS, "create directory: ", target) ;

    } else if (S_ISFIFO(st.st_mode)) {

        log_trace("create fifo: ", target) ;
        if (mkfifo(target, st.st_mode) < 0)
            log_dieusys(LOG_EXIT_SYS, "create fifo: ", target) ;

    } else if (S_ISLNK(st.st_mode)) {

        char dest[4096] ;
        ssize_t d = readlink(path, dest, sizeof(dest) - 1) ;
        if (d == -1)
            log_dieusys(LOG_EXIT_SYS, "readlink: ", path) ;
        dest[d] = 0 ;

        log_trace("create symlink: ", target, " -> ", dest) ;
        if (symlink(dest, target) < 0)
            log_dieusys(LOG_EXIT_SYS, "create symlink: ", target) ;

    } else if (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode) || S_ISSOCK(st.st_mode)) {

        log_trace("create node: ", target) ;
        if (mknod(target, st.st_mode, st.st_rdev) < 0)
            log_dieusys(LOG_EXIT_SYS, "create node: ", target) ;
    }

    /* best-effort: rootless cannot chown to unmapped ids nor chmod foreign nodes */
    if (lchown(target, st.st_uid, st.st_gid) == -1)
        log_trace("lchown (ignored): ", target) ;

    if (!S_ISLNK(st.st_mode))
        if (chmod(target, st.st_mode) == -1)
            log_trace("chmod (ignored): ", target) ;
}

int ns_do_mount(char const *path, char const *target, char const *type, unsigned long flags, char const *opts, uint8_t create)
{
    log_flow() ;

    if (mount(path, target, type, flags, opts) == -1) {

        if (errno == ENOENT && create) {

            log_trace("mount failed (ENOENT) -- create target: ", target) ;
            ns_clone_node(path, target) ;

            if (mount(path, target, type, flags, opts) == -1)
                log_dieusys(LOG_EXIT_SYS, "mount: ", path, " to: ", target) ;

        } else log_dieusys(LOG_EXIT_SYS, "mount: ", path, " to: ", target) ;
    }

    return 1 ;
}

int ns_move_root(char const *path)
{
    log_flow() ;

    if (chdir(path) == -1)
        log_warnusys_return(0, "chdir: ", path) ;

    if (mount(path, "/", NULL, MS_MOVE, NULL) == -1)
        log_warnusys_return(0, "move ", path, " to: /") ;

    if (chroot(".") == -1)
        log_warnusys_return(0, "chroot") ;

    if (chdir("/") == -1)
        log_warnusys_return(0, "chdir: /") ;

    return 1 ;
}

void ns_umount_recursive(char const *path)
{
    log_flow() ;

    /* "/" is already MS_SLAVE|MS_REC (see ns_setup_ns): nothing here propagates
     * to the host. MNT_DETACH on a mountpoint lazily disconnects it AND every
     * filesystem mounted below it in one shot -- it never blocks (a dead
     * FUSE/NFS daemon cannot hang us), so a per-submount sweep is unnecessary.
     * All callers gate this on ns_is_mnt(target), so path is always a mount;
     * the guard below stays as defence in depth. Best-effort: a busy or already
     * gone mount must not abort a half-built namespace. */
    if (ns_is_mnt(path)) {

        log_trace("unmount: ", path) ;
        if (umount2(path, UMOUNT_NOFOLLOW | MNT_DETACH) == -1)
            log_warnusys("umount: ", path) ;
    }
}

void ns_mount_recursive(ns_t *ns, char const *root, ns_entry_t *e)
{
    log_flow() ;

    size_t prefix_len = strlen(root) ;
    char const *path = ns->sb.s + e->path ;
    char const *useropts = e->opts >= 0 ? ns->sb.s + e->opts : "" ;

    size_t n = genbuf_len(mntinfo_t, &ns->mntinfo) ;
    mntinfo_t *a = genbuf_s(mntinfo_t, &ns->mntinfo) ;

    for (size_t i = 0 ; i < n ; i++) {

        char const *dir = ns->mntsb.s + a[i].dir ;
        if (!strcmp(dir, path) || !strcmp(dir, "/"))
            continue ;

        if (dir_is_child(path, dir) > 0) {

            unsigned long flags = 0 ;
            ssize_t kept = -1 ;
            char const *mopts = ns->mntsb.s + a[i].opts ;

            ns_compute_opts(ns, mopts, useropts, &flags, &kept) ;

            flags |= MS_REMOUNT | MS_BIND | MS_REC ;
            flags &= ~(unsigned long)MS_RDONLY ;

            size_t len = strlen(dir) ;
            char target[prefix_len + len + 1] ;
            auto_strings(target, root, dir) ;

            log_trace("remount submount: ", target) ;
            ns_do_mount(NULL, target, NULL, flags, kept >= 0 ? ns->sb.s + kept : NULL, e->create) ;
        }
    }
}

void ns_remount_ro(ns_t *ns, ns_entry_t *e)
{
    log_flow() ;

    if (e->ignore || !FLAGS_ISSET(e->flags, MS_RDONLY))
        return ;

    unsigned long const myflag = MS_BIND | MS_REMOUNT | MS_RDONLY ;
    size_t nlen = strlen(ns->nstmp) ;
    char const *target = ns->sb.s + e->target ;
    size_t tlen = strlen(target) ;
    char ntarget[nlen + tlen + 1] ;
    auto_strings(ntarget, ns->nstmp, target) ;

    /* options carried by the entry's own mountpoint, if any */
    char keepopts[4096] ;
    keepopts[0] = 0 ;

    size_t n = genbuf_len(mntinfo_t, &ns->mntinfo) ;
    mntinfo_t *a = genbuf_s(mntinfo_t, &ns->mntinfo) ;
    size_t en = genbuf_len(ns_entry_t, &ns->entries) ;
    ns_entry_t *ea = genbuf_s(ns_entry_t, &ns->entries) ;

    for (size_t i = n ; i-- > 0 ; ) {

        char const *dir = ns->mntsb.s + a[i].dir ;

        if (!strcmp(dir, target)) {
            auto_strings(keepopts, ns->mntsb.s + a[i].opts) ;
            continue ;
        }

        if (!strcmp(dir, "/"))
            continue ;

        /* skip a sub-mount that is itself a (kept) entry or under one */
        uint8_t skip = 0 ;
        for (size_t j = 0 ; j < en ; j++) {

            char const *toskip = ns->sb.s + ea[j].target ;
            if (!strcmp(toskip, "/"))
                continue ;

            if (!strcmp(toskip, dir) || (dir_is_child(toskip, dir) > 0)) {
                skip = 1 ;
                break ;
            }
        }

        if (dir_is_child(target, dir) > 0 && !skip) {

            // hidden/tmpfs hide everything below -- nothing to remount
            if (e->type == NS_TYPE_HIDDEN || e->type == NS_TYPE_TMPFS)
                continue ;

            unsigned long flags = 0 ;
            ssize_t kept = -1 ;

            ns_compute_opts(ns, ns->mntsb.s + a[i].opts, e->opts >= 0 ? ns->sb.s + e->opts : "", &flags, &kept) ;

            flags |= myflag | e->flags ;

            size_t mlen = strlen(dir) ;
            char dest[nlen + mlen + 1] ;
            auto_strings(dest, ns->nstmp, dir) ;

            log_trace("remount ro: ", dest) ;
            ns_do_mount(dest, dest, NULL, flags, kept >= 0 ? ns->sb.s + kept : NULL, 0) ;
        }
    }

    {
        unsigned long flags = 0 ;
        ssize_t kept = -1 ;

        ns_compute_opts(ns, keepopts, e->opts >= 0 ? ns->sb.s + e->opts : "", &flags, &kept) ;

        flags |= myflag | e->flags ;

        log_trace("remount ro: ", ntarget) ;
        ns_do_mount(ntarget, ntarget, NULL, flags, kept >= 0 ? ns->sb.s + kept : NULL, 0) ;
    }
}

ssize_t ns_hidden_path(ns_t *ns, char const *path)
{
    log_flow() ;

    struct stat st ;
    if (lstat(path, &st) == -1)
        log_dieusys(LOG_EXIT_SYS, "find: ", path) ;

    // one placeholder per dir/non-dir class is enough for a bind overmount
    char const *node ;
    if (S_ISLNK(st.st_mode))
        log_die(LOG_EXIT_USER, "type hidden cannot be used for a symlink: ", path) ;

    node = S_ISDIR(st.st_mode) ? "/directory" : "/file" ;

    size_t hlen = strlen(ns->nshidden) ;
    char hidden[hlen + strlen(node) + 1] ;
    auto_strings(hidden, ns->nshidden, node) ;

    return (ssize_t)ns_arena_add(ns, hidden) ;
}

// entry list grooming

void ns_resolve_symlinks(ns_t *ns)
{
    log_flow() ;

    size_t n = genbuf_len(ns_entry_t, &ns->entries) ;
    for (size_t i = 0 ; i < n ; i++) {

        ns_entry_t *m = &genbuf_s(ns_entry_t, &ns->entries)[i] ;
        /* hidden masks the requested path itself: never dereference its source,
         * otherwise a symlink target would be silently masked (leaving the link
         * visible) and the S_ISLNK guard in ns_hidden_path would be unreachable. */
        if (m->ignore || m->type == NS_TYPE_HIDDEN)
            continue ;

        if (FLAGS_ISSET(m->flags, MS_BIND) || FLAGS_ISSET(m->flags, MS_REC)) {

            char *sym = realpath(ns->sb.s + m->path, NULL) ;

            if (sym) {
                m->path = (ssize_t)ns_arena_add(ns, sym) ;
                free(sym) ;
            }
        }
    }
}

// selection sort by path string -- entry counts are tiny
static void entries_sort(ns_t *ns)
{
    size_t n = genbuf_len(ns_entry_t, &ns->entries) ;
    ns_entry_t *a = genbuf_s(ns_entry_t, &ns->entries) ;

    for (size_t i = 0 ; i + 1 < n ; i++) {

        size_t min = i ;
        for (size_t j = i + 1 ; j < n ; j++) {
            if (strcmp(ns->sb.s + a[j].path, ns->sb.s + a[min].path) < 0)
                min = j ;
        }

        if (min != i) {
            ns_entry_t t = a[i] ;
            a[i] = a[min] ;
            a[min] = t ;
        }
    }
}

void ns_clean_entries(ns_t *ns)
{
    log_flow() ;

    entries_sort(ns) ;

    size_t n = genbuf_len(ns_entry_t, &ns->entries) ;
    ns_entry_t *a = genbuf_s(ns_entry_t, &ns->entries) ;

    /* drop exact-duplicate paths (keep the later one, mark the earlier) */
    for (size_t i = 0 ; i + 1 < n ; i++) {

        if (a[i].skip || a[i].ignore)
            continue ;

        if (!strcmp(ns->sb.s + a[i].path, ns->sb.s + a[i + 1].path)) {
            a[i].skip = 1 ;
            a[i].ignore = 1 ;
        }
    }

    /* prune children of a hidden parent */
    size_t gparent = 0 ;
    for (size_t i = 0 ; i < n ; i++) {

        if (a[i].ignore)
            continue ;

        char const *pp = ns->sb.s + a[i].path ;
        if (!i && pp[0] == '/' && !pp[1]) {
            gparent = 0 ;
            continue ;
        }

        uint8_t hidden = a[gparent].type == NS_TYPE_HIDDEN ;
        char const *parent = ns->sb.s + a[gparent].path ;

        if (i + 1 >= n)
            break ;

        char const *child = ns->sb.s + a[i + 1].path ;

        if (dir_is_child(parent, child) && hidden) {
            a[i + 1].skip = 1 ;
            continue ;
        }

        gparent = i + 1 ;
    }
}

// in-child orchestration

void ns_setup_ns(ns_t *ns)
{
    log_flow() ;

    unsigned long prop = ns->root_propagation ? ns->root_propagation : MS_SHARED ;

    ns_resolve_symlinks(ns) ;

    /* disconnect "/" from the host: nothing we do propagates back */
    if (mount(NULL, "/", NULL, MS_SLAVE | MS_REC, NULL) == -1)
        log_dieusys(LOG_EXIT_SYS, "remount / as slave") ;

    /* now in the private ns: build the ephemeral scratch (tmpfs) holding nstmp
     * and the hidden placeholders, before binding "/" onto nstmp */
    ns_prepare_directory(ns) ;

    /* bind "/" into the staging directory */
    if (mount("/", ns->nstmp, NULL, MS_BIND | MS_REC, NULL) == -1)
        log_dieusys(LOG_EXIT_SYS, "bind / to: ", ns->nstmp) ;

    /* make the staging dir unbindable so a recursive bind of /run won't see it */
    if (mount(NULL, ns->nstmp, NULL, MS_UNBINDABLE, NULL) == -1)
        log_dieusys(LOG_EXIT_SYS, "make unbindable: ", ns->nstmp) ;

    ns_clean_entries(ns) ;

    size_t n = genbuf_len(ns_entry_t, &ns->entries) ;

    for (size_t i = 0 ; i < n ; i++) {

        ns_entry_t *m = &genbuf_s(ns_entry_t, &ns->entries)[i] ;

        if (m->done || m->skip)
            continue ;

        ns_apply_entry(ns, m, ns->nstmp) ;

        m->done = 1 ;
    }

    /* read-only pass (after all mounts exist so sub-mounts can be set up rw first) */
    for (size_t i = 0 ; i < n ; i++) {

        ns_entry_t *m = &genbuf_s(ns_entry_t, &ns->entries)[i] ;
        if (m->skip)
            continue ;

        ns_remount_ro(ns, m) ;
    }

    if (!ns_move_root(ns->nstmp))
        log_dieu(LOG_EXIT_SYS, "pivot into new root") ;

    if (mount(NULL, "/", NULL, prop | MS_REC, NULL) == -1)
        log_dieusys(LOG_EXIT_SYS, "remount / with final propagation") ;
}
