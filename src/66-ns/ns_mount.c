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
#include <sys/syscall.h>
#include <linux/openat2.h>
#include <limits.h>
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
#include <oblibs/io.h>

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

#define MOUNTINFO_CHUNK 4096
#define MOUNTINFO_FIELDS 6

typedef struct livemnt_s livemnt_t ;
struct livemnt_s {
    uint64_t id ;
    size_t   dir ;
    size_t   opts ;
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

static size_t mntsa_add(strbuf *sb, char const *s)
{
    size_t off = sb->len ;
    if (!strbuf_catb(sb, s, strlen(s) + 1))
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
        cp.dir  = (ssize_t)mntsa_add(&ns->mntsb, m->mnt_dir) ;
        cp.type = (ssize_t)mntsa_add(&ns->mntsb, m->mnt_type) ;
        cp.opts = (ssize_t)mntsa_add(&ns->mntsb, m->mnt_opts) ;
        if (!genbuf_append(mntinfo_t, &ns->mntinfo, &cp))
            log_die_nomem("mntinfo") ;
    }

    endmntent(f) ;
}

static size_t mntsa_add_unescaped(strbuf *sb, char const *s)
{
    size_t off = sb->len ;
    size_t len = strlen(s) ;

    for (size_t i = 0 ; i < len ; i++) {

        char c = s[i] ;

        if (c == '\\' && len - i > 3) {

            char oct[4] = { s[i + 1], s[i + 2], s[i + 3], 0 } ;
            uint8_t u ;

            if (u8_scan_strict_base(oct, &u, 8)) {
                c = (char)u ;
                i += 3 ;
            }
        }

        if (!strbuf_catb(sb, &c, 1))
            log_die_nomem("mntsb") ;
    }

    if (!strbuf_catb(sb, "", 1))
        log_die_nomem("mntsb") ;

    return off ;
}

static void livemnt_read(genbuf *g, strbuf *sb)
{
    _cleanup_strbuf_ strbuf raw = STRBUF_ZERO ;
    char buf[MOUNTINFO_CHUNK] ;
    ssize_t r ;

    int fd = io_open("/proc/self/mountinfo", O_RDONLY | O_CLOEXEC) ;
    if (fd < 0)
        log_dieusys(LOG_EXIT_SYS, "open: /proc/self/mountinfo") ;

    while ((r = io_read(fd, buf, sizeof(buf))) > 0)
        if (!strbuf_catb(&raw, buf, (size_t)r))
            log_die_nomem("mountinfo") ;

    close(fd) ;

    if (r < 0)
        log_dieusys(LOG_EXIT_SYS, "read: /proc/self/mountinfo") ;

    if (!strbuf_terminate(&raw))
        log_die_nomem("mountinfo") ;

    if (!sbl_split_string_in_nline(&raw))
        log_dieu(LOG_EXIT_SYS, "parse: /proc/self/mountinfo") ;

    _cleanup_strbuf_ strbuf fields = STRBUF_ZERO ;
    size_t pos = 0 ;
    {
        FOREACH_SBL(&raw, pos) {

            if (!sbl_clean_string(&fields, raw.s + pos))
                log_dieu(LOG_EXIT_SYS, "parse: /proc/self/mountinfo") ;

            char const *tok[MOUNTINFO_FIELDS] ;
            size_t nt = 0, fpos = 0 ;
            {
                FOREACH_SBL(&fields, fpos) {
                    if (nt < MOUNTINFO_FIELDS)
                        tok[nt++] = fields.s + fpos ;
                }
            }

            uint64_t id ;
            if (nt < MOUNTINFO_FIELDS || !u64_scan(tok[0], &id))
                log_die(LOG_EXIT_SYS, "malformed line in: /proc/self/mountinfo") ;

            livemnt_t m = { .id = id, .dir = mntsa_add_unescaped(sb, tok[4]), .opts = mntsa_add(sb, tok[5]) } ;
            if (!genbuf_append(livemnt_t, g, &m))
                log_die_nomem("mountinfo") ;
        }
    }
}

static int livemnt_visible(livemnt_t const *m, char const *dir)
{
    struct statx stx ;

    if (statx(AT_FDCWD, dir, AT_SYMLINK_NOFOLLOW, STATX_MNT_ID, &stx) < 0)
        return 0 ;

    if (!(stx.stx_mask & STATX_MNT_ID))
        log_die(LOG_EXIT_SYS, "statx does not report mount ids for: ", dir) ;

    return stx.stx_mnt_id == m->id ;
}

int ns_mntinfo_live_opts(char const *path, strbuf *opts)
{
    size_t plen = strlen(path) ;
    char p[plen + 1] ;
    auto_strings(p, path) ;

    dir_unslash(p) ;

    _cleanup_strbuf_ strbuf live = STRBUF_ZERO ;
    genbuf mnt = GENBUF_ZERO ;
    livemnt_read(&mnt, &live) ;

    size_t n = genbuf_len(livemnt_t, &mnt) ;
    livemnt_t *a = genbuf_s(livemnt_t, &mnt) ;
    char const *found = 0 ;

    for (size_t i = 0 ; i < n ; i++)
        if (!strcmp(live.s + a[i].dir, p))
            found = live.s + a[i].opts ;

    if (found && (!strbuf_cats(opts, found) || !strbuf_terminate(opts)))
        log_die_nomem("opts") ;

    genbuf_free(&mnt) ;

    return !!found ;
}

// primitives

int ns_is_mnt(char const *str)
{
    struct statx stx ;

    if (statx(AT_FDCWD, str, AT_SYMLINK_NOFOLLOW, STATX_TYPE, &stx) < 0)
        return 0 ;

    if (!S_ISDIR(stx.stx_mode))
        return 0 ;

    if (!(stx.stx_attributes_mask & STATX_ATTR_MOUNT_ROOT))
        log_die(LOG_EXIT_SYS, "statx does not report mount roots for: ", str) ;

    return !!(stx.stx_attributes & STATX_ATTR_MOUNT_ROOT) ;
}


void ns_mountpoint_create(char const *path, char const *target)
{
    log_flow() ;

    struct stat st ;
    if (stat(path, &st) == -1)
        log_dieusys(LOG_EXIT_SYS, "find: ", path) ;

    if (S_ISDIR(st.st_mode)) {

        log_trace("create directory: ", target) ;
        if (!dir_create_parent(target, st.st_mode & 07777))
            log_dieusys(LOG_EXIT_SYS, "create directory: ", target) ;

        return ;
    }

    size_t tlen = strlen(target) ;
    char dir[tlen + 1], name[tlen + 1] ;
    if (!ob_dirname(dir, target) || !ob_basename(name, target))
        log_dieusys(LOG_EXIT_SYS, "split path: ", target) ;

    if (!dir_create_parent(dir, 0755))
        log_dieusys(LOG_EXIT_SYS, "create directory: ", dir) ;

    log_trace("create file: ", target) ;
    if (!file_create_empty(dir, name, 0644))
        log_dieusys(LOG_EXIT_SYS, "create file: ", target) ;
}

void ns_clone_node(char const *path, char const *target)
{
    log_flow() ;

    struct stat st ;
    if (lstat(path, &st) == -1)
        log_dieusys(LOG_EXIT_SYS, "find: ", path) ;

    if (!S_ISDIR(st.st_mode)) {

        char parent[strlen(target) + 1] ;
        if (!ob_dirname(parent, target))
            log_dieusys(LOG_EXIT_SYS, "get dirname of: ", target) ;

        if (!dir_create_parent(parent, 0755))
            log_dieusys(LOG_EXIT_SYS, "create directory: ", parent) ;
    }

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
            ns_mountpoint_create(path, target) ;

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

    char const *path0 = ns->sb.s + e->path ;
    char path[strlen(path0) + 1] ;
    auto_strings(path, path0) ;

    char const *useropts0 = e->opts >= 0 ? ns->sb.s + e->opts : "" ;
    char useropts[strlen(useropts0) + 1] ;
    auto_strings(useropts, useropts0) ;

    char const *target = ns->sb.s + e->target ;
    char tgt[strlen(target) + 1] ;
    auto_strings(tgt, strcmp(target, "/") ? target : "") ;
    size_t plen = strcmp(path, "/") ? strlen(path) : 0 ;
    size_t tlen = strlen(tgt) ;

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

            flags |= e->flags | MS_REMOUNT | MS_BIND | MS_REC ;
            flags &= ~(unsigned long)MS_RDONLY ;

            char const *sub = dir + plen ;
            char target[prefix_len + tlen + strlen(sub) + 1] ;
            auto_strings(target, root, tgt, sub) ;

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

    char const *target0 = ns->sb.s + e->target ;
    size_t tlen = strlen(target0) ;
    char target[tlen + 1] ;
    auto_strings(target, target0) ;

    char ntarget[nlen + tlen + 1] ;
    auto_strings(ntarget, ns->nstmp, target) ;
    dir_unslash(ntarget) ;

    /* current options of the namespace mounts: flags locked in a user
     * namespace (nosuid, nodev, atime) must be kept on remount */
    _cleanup_strbuf_ strbuf live = STRBUF_ZERO ;
    genbuf mnt = GENBUF_ZERO ;
    livemnt_read(&mnt, &live) ;

    char const *keepopts = "" ;
    size_t n = genbuf_len(livemnt_t, &mnt) ;
    livemnt_t *a = genbuf_s(livemnt_t, &mnt) ;
    size_t en = genbuf_len(ns_entry_t, &ns->entries) ;
    ns_entry_t *ea = genbuf_s(ns_entry_t, &ns->entries) ;

    for (size_t i = n ; i-- > 0 ; ) {

        char const *dir = live.s + a[i].dir ;

        if (!livemnt_visible(&a[i], dir))
            continue ;

        if (!strcmp(dir, ntarget)) {
            keepopts = live.s + a[i].opts ;
            continue ;
        }

        if (dir_is_child(ntarget, dir) <= 0)
            continue ;

        // hidden/tmpfs hide everything below -- nothing to remount
        if (e->type == NS_TYPE_HIDDEN || e->type == NS_TYPE_TMPFS)
            continue ;

        /* skip a sub-mount that is itself a (kept) entry or under one */
        char const *rel = dir + nlen ;
        uint8_t skip = 0 ;
        for (size_t j = 0 ; j < en ; j++) {

            char const *toskip = ns->sb.s + ea[j].target ;
            if (!strcmp(toskip, target) || dir_is_child(target, toskip) <= 0)
                continue ;

            if (!strcmp(toskip, rel) || (dir_is_child(toskip, rel) > 0)) {
                skip = 1 ;
                break ;
            }
        }

        if (skip)
            continue ;

        unsigned long flags = 0 ;
        ssize_t kept = -1 ;

        ns_compute_opts(ns, live.s + a[i].opts, e->opts >= 0 ? ns->sb.s + e->opts : "", &flags, &kept) ;

        flags |= myflag | e->flags ;

        log_trace("remount ro: ", dir) ;
        ns_do_mount(dir, dir, NULL, flags, kept >= 0 ? ns->sb.s + kept : NULL, 0) ;
    }

    {
        unsigned long flags = 0 ;
        ssize_t kept = -1 ;

        ns_compute_opts(ns, keepopts, e->opts >= 0 ? ns->sb.s + e->opts : "", &flags, &kept) ;

        flags |= myflag | e->flags ;

        log_trace("remount ro: ", ntarget) ;
        ns_do_mount(ntarget, ntarget, NULL, flags, kept >= 0 ? ns->sb.s + kept : NULL, 0) ;
    }

    genbuf_free(&mnt) ;
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

void ns_drop_missing(ns_t *ns)
{
    log_flow() ;

    size_t n = genbuf_len(ns_entry_t, &ns->entries) ;
    for (size_t i = 0 ; i < n ; i++) {

        ns_entry_t *m = &genbuf_s(ns_entry_t, &ns->entries)[i] ;
        if (!m->ignore || m->type == NS_TYPE_HIDDEN)
            continue ;

        char const *path = ns->sb.s + m->path ;
        struct stat st ;

        if (lstat(path, &st) == -1) {

            if (errno != ENOENT)
                log_dieusys(LOG_EXIT_SYS, "find: ", path) ;

            log_trace("ignore missing element: ", path) ;
            m->skip = 1 ;

        } else m->ignore = 0 ;
    }
}

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

static void target_resolve(ns_t *ns, ns_entry_t *m, int rootfd)
{
    char const *target = ns->sb.s + m->target ;
    size_t tlen = strlen(target) ;
    char head[tlen + 1] ;
    auto_strings(head, target) ;

    struct open_how how = {
        .flags = O_PATH | O_CLOEXEC | (m->type == NS_TYPE_HIDDEN || m->type == NS_TYPE_CLONE ? O_NOFOLLOW : 0),
        .resolve = RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS
    } ;

    size_t cut = tlen ;
    int fd ;

    while ((fd = (int)syscall(SYS_openat2, rootfd, head, &how, sizeof(how))) < 0) {

        if (errno != ENOENT)
            log_dieusys(LOG_EXIT_SYS, "resolve target: ", target) ;

        cut = (size_t)get_rlen_until(head, '/', strlen(head)) ;
        head[cut ? cut : 1] = 0 ;
        how.flags &= ~(uint64_t)O_NOFOLLOW ;
    }

    if (m->type == NS_TYPE_HIDDEN && cut == tlen)
        m->ignore = 0 ;

    if (cut < tlen) {

        char const *name = target + cut + 1 ;
        size_t nlen = str_search(name, '/') ;
        char first[nlen + 1] ;
        memcpy(first, name, nlen) ;
        first[nlen] = 0 ;

        struct stat st ;
        if (!fstatat(fd, first, &st, AT_SYMLINK_NOFOLLOW) && S_ISLNK(st.st_mode))
            log_die(LOG_EXIT_USER, "target goes through a dangling symlink: ", target) ;
    }

    char fdstr[U32_FMT] ;
    fdstr[u32_fmt(fdstr, (uint32_t)fd)] = 0 ;

    char proc[sizeof("/proc/self/fd/") + U32_FMT] ;
    auto_strings(proc, "/proc/self/fd/", fdstr) ;

    char real[PATH_MAX] ;
    ssize_t r = readlink(proc, real, sizeof(real) - 1) ;
    close(fd) ;

    if (r < 0)
        log_dieusys(LOG_EXIT_SYS, "readlink: ", proc) ;

    real[r] = 0 ;

    size_t rlen = strlen(ns->nstmp) ;
    if (str_start_with(real, ns->nstmp) || (real[rlen] && real[rlen] != '/'))
        log_die(LOG_EXIT_SYS, "target resolves outside the namespace root: ", target) ;

    char const *rel = real + rlen ;
    char const *tail = target + cut ;
    char resolved[strlen(rel) + strlen(tail) + 2] ;
    auto_strings(resolved, rel, tail) ;

    if (!resolved[0])
        auto_strings(resolved, "/") ;

    if (strcmp(resolved, target))
        m->target = (ssize_t)ns_arena_add(ns, resolved) ;
}

void ns_resolve_targets(ns_t *ns)
{
    log_flow() ;

    int rootfd = io_open(ns->nstmp, O_PATH | O_DIRECTORY | O_CLOEXEC) ;
    if (rootfd < 0)
        log_dieusys(LOG_EXIT_SYS, "open: ", ns->nstmp) ;

    size_t n = genbuf_len(ns_entry_t, &ns->entries) ;
    for (size_t i = 0 ; i < n ; i++) {

        ns_entry_t *m = &genbuf_s(ns_entry_t, &ns->entries)[i] ;
        if (m->skip)
            continue ;

        target_resolve(ns, m, rootfd) ;
    }

    close(rootfd) ;
}

// stable insertion sort by target string -- entry counts are tiny
static void entries_sort(ns_t *ns)
{
    size_t n = genbuf_len(ns_entry_t, &ns->entries) ;
    ns_entry_t *a = genbuf_s(ns_entry_t, &ns->entries) ;

    for (size_t i = 1 ; i < n ; i++) {

        ns_entry_t t = a[i] ;
        size_t j = i ;
        for (; j > 0 && strcmp(ns->sb.s + a[j - 1].target, ns->sb.s + t.target) > 0 ; j--)
            a[j] = a[j - 1] ;

        a[j] = t ;
    }
}

void ns_clean_entries(ns_t *ns)
{
    log_flow() ;

    entries_sort(ns) ;

    size_t n = genbuf_len(ns_entry_t, &ns->entries) ;
    ns_entry_t *a = genbuf_s(ns_entry_t, &ns->entries) ;

    /* drop exact-duplicate targets (keep the later one, mark the earlier) */
    for (size_t i = 0 ; i + 1 < n ; i++) {

        if (a[i].skip || a[i].ignore)
            continue ;

        if (!strcmp(ns->sb.s + a[i].target, ns->sb.s + a[i + 1].target)) {
            a[i].skip = 1 ;
            a[i].ignore = 1 ;
        }
    }

    /* prune children of a hidden parent */
    size_t gparent = 0 ;
    for (size_t i = 0 ; i < n ; i++) {

        if (a[i].ignore)
            continue ;

        char const *pp = ns->sb.s + a[i].target ;
        if (!i && pp[0] == '/' && !pp[1]) {
            gparent = 0 ;
            continue ;
        }

        uint8_t hidden = a[gparent].type == NS_TYPE_HIDDEN ;
        char const *parent = ns->sb.s + a[gparent].target ;

        if (i + 1 >= n)
            break ;

        char const *child = ns->sb.s + a[i + 1].target ;

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

    ns_drop_missing(ns) ;
    ns_resolve_symlinks(ns) ;

    /* disconnect "/" from the host: nothing we do propagates back */
    if (mount(NULL, "/", NULL, MS_SLAVE | MS_REC, NULL) == -1)
        log_dieusys(LOG_EXIT_SYS, "remount / as slave") ;

    /* now in the private ns: build the ephemeral scratch (tmpfs) holding nstmp
     * and the hidden placeholders, before binding "/" onto nstmp */
    ns_prepare_directory(ns) ;

    ns_entry_t *root = 0 ;
    size_t n = genbuf_len(ns_entry_t, &ns->entries) ;
    for (size_t i = 0 ; i < n ; i++) {

        ns_entry_t *m = &genbuf_s(ns_entry_t, &ns->entries)[i] ;
        if (m->skip || strcmp(ns->sb.s + m->target, "/"))
            continue ;

        if (m->type != NS_TYPE_BIND && m->type != NS_TYPE_RECURSIVE)
            log_die(LOG_EXIT_USER, "type ", ns_type_str[m->type], " cannot be used for the root: ", ns->sb.s + m->path) ;

        root = m ;
    }

    /* bind "/" into the staging directory */
    if (mount("/", ns->nstmp, NULL, MS_BIND | MS_REC, NULL) == -1)
        log_dieusys(LOG_EXIT_SYS, "bind / to: ", ns->nstmp) ;

    /* make the staging dir unbindable so a recursive bind of /run won't see it */
    if (mount(NULL, ns->nstmp, NULL, MS_UNBINDABLE, NULL) == -1)
        log_dieusys(LOG_EXIT_SYS, "make unbindable: ", ns->nstmp) ;

    if (root) {
        ns_apply_entry(ns, root, ns->nstmp) ;
        root->done = 1 ;
    }

    ns_resolve_targets(ns) ;
    ns_clean_entries(ns) ;

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
