/*
 * ns_internal.h
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
 * Cross-translation-unit internals of the ns library. Not part of the public
 * API (ns.h). Everything here takes the ns_t context explicitly -- no globals.
 */

#ifndef NS_INTERNAL_H
#define NS_INTERNAL_H

#include <sys/types.h>
#include <stdint.h>

#include "ns.h"

// ns_dir.c

/** @brief Compute the (constant) scratch paths base_dir/nstmp/nshidden into
 *  @ns->paths and point the three ns_t fields at them. Pure: no fs, no privilege;
 *  safe to call in the invoker before clone. */
extern void ns_dir_setup(ns_t *ns) ;

/** @brief Build the ephemeral scratch: over-mount a private (unbindable) tmpfs on
 *  the scratch point, create nstmp, and the hidden placeholders only if a hidden
 *  element is requested. Must run in the child, after "/" is made slave and
 *  before the rbind of "/". */
extern void ns_prepare_directory(ns_t *ns) ;

// ns_mount.c

/** @brief Snapshot /proc/self/mounts into @ns->mntinfo / @ns->mntsb. */
extern void ns_mntinfo_init(ns_t *ns) ;

/** @brief Mount type / options of the mountpoint @path in the snapshot, or NULL. */
extern char const *ns_mntinfo_type(ns_t *ns, char const *path) ;
extern char const *ns_mntinfo_opts(ns_t *ns, char const *path) ;

/** @brief 1 if @path is a mountpoint (directory whose st_dev differs from its
 *  parent), else 0. */
extern int ns_is_mnt(char const *path) ;

/** @brief Lazily detach @path and every filesystem mounted below it
 *  (umount2 MNT_DETACH). Best-effort; @path must be a mountpoint. */
extern void ns_umount_recursive(char const *path) ;

/** @brief chdir(@path) + MS_MOVE @path over "/" + chroot + chdir("/").
 *  @return 1 on success, 0 on failure (warns). */
extern int ns_move_root(char const *path) ;

/** @brief Recreate @path's node (file/dir/fifo/symlink/dev/sock) at @target,
 *  preserving mode/owner where possible. */
extern void ns_clone_node(char const *path, char const *target) ;

/** @brief mount(2) wrapper: on ENOENT with @create, clone the node and retry.
 *  @return 1 on success; dies on hard failure. */
extern int ns_do_mount(char const *path, char const *target, char const *type,
                       unsigned long flags, char const *opts, uint8_t create) ;

/** @brief Split a comma-separated mount-options string @optstr into kernel
 *  flags (folded into *@flags with |=) and the remaining data options (stored
 *  in @ns->sb, offset returned in *@kept, or -1 if none). */
extern void ns_split_opts(ns_t *ns, char const *optstr, unsigned long *flags, ssize_t *kept) ;

/** @brief ns_split_opts over the concatenation "@a,@b" (either may be empty). */
extern void ns_compute_opts(ns_t *ns, char const *a, char const *b, unsigned long *flags, ssize_t *kept) ;

/** @brief Remount every sub-mount found under entry @e->path as MS_REMOUNT|BIND|REC. */
extern void ns_mount_recursive(ns_t *ns, char const *root, ns_entry_t *e) ;

/** @brief If @e carries MS_RDONLY, remount @e and its kept sub-mounts read-only. */
extern void ns_remount_ro(ns_t *ns, ns_entry_t *e) ;

/** @brief Offset (in @ns->sb) of the hidden placeholder node matching @path's
 *  file type (file/directory/fifo/sock/chr/blk). */
extern ssize_t ns_hidden_path(ns_t *ns, char const *path) ;

/** @brief realpath() the source path of every bind entry. */
extern void ns_resolve_symlinks(ns_t *ns) ;

/** @brief Sort entries by path, drop exact duplicates, prune children of a
 *  hidden parent. */
extern void ns_clean_entries(ns_t *ns) ;

/** @brief The in-child mount orchestration: slave-rec "/", bind "/" to nstmp,
 *  apply every entry, remount-ro pass, move root, final propagation. */
extern void ns_setup_ns(ns_t *ns) ;

// ns_elements.c

/** @brief Apply one entry @e under @root (handles every NS_TYPE_*, with the
 *  rootless fallbacks). */
extern void ns_apply_entry(ns_t *ns, ns_entry_t *e, char const *root) ;

// ns_supervise.c

/** @brief Raw clone(2): syscall(__NR_clone, flags, 0). */
extern int ns_raw_clone(unsigned long flags) ;

/** @brief Convert a wait(2) status into an exit code (128+sig if signalled). */
extern int ns_compute_exit(int wstat) ;

/** @brief Invoker supervisor: sse loop watching @child (pidfd) and forwarding
 *  SIGTERM/SIGINT/SIGQUIT/SIGHUP. Returns @child's exit code. */
extern int ns_supervise(pid_t child) ;

/** @brief PID1 of a new pid namespace: reap until ECHILD, return @grandchild's
 *  exit code. */
extern int ns_pid1(pid_t grandchild) ;

// ns_userns.c

/** @brief From the invoker, write @child's setgroups/gid_map/uid_map in the
 *  strict order required for an unprivileged single-id mapping. */
extern void ns_userns_write_maps(ns_t *ns, pid_t child) ;

// ns_session.c

/** @brief Apply new-session (setsid + optional controlling tty) per @ns. */
extern void ns_session_setup(ns_t *ns) ;

// ns_net.c

/** @brief Bring the loopback interface up (127.0.0.1/8 + IFF_UP) in the current
 *  network namespace, via rtnetlink. Call only after CLONE_NEWNET. */
extern void ns_loopback_up(void) ;

#endif
