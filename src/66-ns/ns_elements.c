/*
 * ns_elements.c
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
 * Per-type element appliers. Each NS_TYPE_* knows how to materialise itself
 * under the staging root, including the rootless fallbacks for proc/dev/sys
 * (which cannot mount their real filesystem inside a user namespace).
 */

#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sched.h>
#include <sys/stat.h>
#include <sys/mount.h>

#include <oblibs/log.h>
#include <oblibs/opt.h>
#include <oblibs/io.h>
#include <oblibs/string.h>
#include <oblibs/types.h>

#include "ns.h"
#include "ns_internal.h"

/* In a user namespace devtmpfs cannot be mounted and device nodes cannot be
 * created; expose a minimal /dev as a tmpfs with the core nodes bind-mounted
 * from the host. */
static char const *const dev_nodes[] = {
    "/null", "/zero", "/full", "/random", "/urandom", "/tty"
} ;

static void dev_rootless(char const *target)
{
    log_trace("mount: rootless /dev tmpfs to: ", target) ;
    ns_do_mount("tmpfs", target, "tmpfs", MS_NOSUID | MS_STRICTATIME, "mode=755", 1) ;

    size_t tlen = strlen(target) ;

    for (size_t i = 0 ; i < OPT_COUNT(dev_nodes) ; i++) {

        char dst[tlen + strlen(dev_nodes[i]) + 1] ;
        auto_strings(dst, target, dev_nodes[i]) ;

        char hostsrc[strlen("/dev") + strlen(dev_nodes[i]) + 1] ;
        auto_strings(hostsrc, "/dev", dev_nodes[i]) ;

        /* a bind target only needs to exist as a plain file (mknod of a device
         * node is forbidden in a user namespace) */
        int fd = io_open_mode(dst, O_WRONLY | O_CREAT | O_NOFOLLOW, 0600) ;
        if (fd >= 0) {

            close(fd) ;

        } else if (errno != EEXIST) log_warnusys("create device target: ", dst) ;

        if (mount(hostsrc, dst, NULL, MS_BIND, NULL) == -1)
            log_warnusys("bind device: ", hostsrc) ;
    }
}

void ns_apply_entry(ns_t *ns, ns_entry_t *e, char const *root)
{
    log_flow() ;

    if (e->ignore || e->skip)
        return ;

    size_t prefix_len = strlen(root) ;

    // stable local copies: arena adds below may realloc ns->sb
    char const *path0 = ns->sb.s + e->path ;
    char path[strlen(path0) + 1] ;
    auto_strings(path, path0) ;

    char const *tgt0 = ns->sb.s + e->target ;
    size_t tlen = strlen(tgt0) ;
    char target[prefix_len + tlen + 1] ;
    auto_strings(target, root, tgt0) ;

    char const *useropts = e->opts >= 0 ? ns->sb.s + e->opts : "" ;
    char optbuf[strlen(useropts) + 1] ;
    auto_strings(optbuf, useropts) ;

    // RDONLY is applied in a later pass (sub-mounts must be writable first)
    unsigned long flags = e->flags & ~(unsigned long)MS_RDONLY ;

    switch (e->type) {

        case NS_TYPE_TMPFS :
            /* rootless: rbind'd sub-mounts are locked (MNT_LOCKED) and cannot be
             * unmounted; we overmount instead. */
            if (!ns->rootless && ns_is_mnt(target))
                ns_umount_recursive(target) ;

            log_trace("mount: tmpfs to: ", target) ;
            ns_do_mount("tmpfs", target, "tmpfs", flags, e->opts >= 0 ? optbuf : NULL, e->create) ;

            return ;

        case NS_TYPE_HIDDEN : {

            /* rootless: rbind'd sub-mounts are locked (MNT_LOCKED) and cannot be
             * unmounted; we overmount instead. */
            if (!ns->rootless && ns_is_mnt(target))
                ns_umount_recursive(target) ;

            ssize_t h = ns_hidden_path(ns, path) ;
            char const *hidden = ns->sb.s + h ;

            log_trace("mount: ", hidden, " to: ", target) ;
            ns_do_mount(hidden, target, NULL, flags, NULL, e->create) ;

            return ;
        }

        case NS_TYPE_CLONE :

            if (ns_is_mnt(target))
                log_die(LOG_EXIT_USER, "type clone cannot be used for a mountpoint: ", path) ;

            ns_clone_node(path, target) ;

            return ;

        case NS_TYPE_PROC :

            /* rootless: rbind'd sub-mounts are locked (MNT_LOCKED) and cannot be
             * unmounted; we overmount instead. */
            if (!ns->rootless && ns_is_mnt(target))
                ns_umount_recursive(target) ;

            if (ns->rootless && !FLAGS_ISSET(ns->clone_flags, CLONE_NEWPID)) {

                log_trace("rootless without pid ns: rbind host /proc to: ", target) ;
                ns_do_mount("/proc", target, NULL, MS_BIND | MS_REC, NULL, e->create) ;

            } else {

                log_trace("mount: proc to: ", target) ;
                ns_do_mount("proc", target, "proc", flags, NULL, e->create) ;
            }

            return ;

        case NS_TYPE_DEV :

            /* rootless: rbind'd sub-mounts are locked (MNT_LOCKED) and cannot be
             * unmounted; we overmount instead. */
            if (!ns->rootless && ns_is_mnt(target))
                ns_umount_recursive(target) ;

            if (ns->rootless) {

                dev_rootless(target) ;

            }else {

                log_trace("mount: dev to: ", target) ;
                ns_do_mount("dev", target, "devtmpfs", flags, "mode=755", e->create) ;
            }

            return ;

        case NS_TYPE_SYS :

            /* rootless: rbind'd sub-mounts are locked (MNT_LOCKED) and cannot be
             * unmounted; we overmount instead. */
            if (!ns->rootless && ns_is_mnt(target))
                ns_umount_recursive(target) ;

            if (ns->rootless && !FLAGS_ISSET(ns->clone_flags, CLONE_NEWNET)) {

                log_trace("rootless without net ns: rbind host /sys to: ", target) ;
                ns_do_mount("/sys", target, NULL, MS_BIND | MS_REC, NULL, e->create) ;

            } else {

                log_trace("mount: sys to: ", target) ;
                ns_do_mount("sys", target, "sysfs", flags, NULL, e->create) ;
            }
            return ;

        case NS_TYPE_RECURSIVE :

            /* rootless: rbind'd sub-mounts are locked (MNT_LOCKED) and cannot be
             * unmounted; we overmount instead. */
            if (!ns->rootless && ns_is_mnt(target))
                ns_umount_recursive(target) ;

            break ; // then run the shared bind logic after the switch

        case NS_TYPE_BIND :
        default :
            break ;
    }

    // default / recursive: a (recursive) bind mount of a host path
    int recursive = FLAGS_ISSET(flags, MS_REC) ;
    char const *type = NULL ;
    char const *popts = e->opts >= 0 ? optbuf : NULL ;

    if (ns_is_mnt(target)) {
        char const *mtype = ns_mntinfo_type(ns, path) ;
        char const *mopts = ns_mntinfo_opts(ns, path) ;

        if (!mtype)
            log_die(LOG_EXIT_USER, "no mounts entry for: ", path) ;

        char typebuf[strlen(mtype) + 1] ;
        auto_strings(typebuf, mtype) ;

        unsigned long mflags = 0 ;
        ssize_t kept = -1 ;

        ns_compute_opts(ns, mopts ? mopts : "", optbuf, &mflags, &kept) ;

        flags |= mflags | MS_REMOUNT ;

        log_trace("remount: ", path, " to: ", target) ;
        ns_do_mount(path, target, typebuf, flags, kept >= 0 ? ns->sb.s + kept : NULL, e->create) ;

    } else {

        struct stat st ;
        if (lstat(target, &st) == -1)
            ns_clone_node(path, target) ;

        log_trace("mount: ", path, " to: ", target) ;
        ns_do_mount(path, target, type, flags, popts, e->create) ;
    }

    if (recursive)
        ns_mount_recursive(ns, root, e) ;
}
