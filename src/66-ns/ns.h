/*
 * ns.h
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
 * ns -- Linux namespace/sandbox library. Declaratively builds a mount
 * namespace (and optionally pid/net/ipc/uts/cgroup/user namespaces) from a
 * list of typed "elements", then execs a program inside it. The library core
 * is ns_run(ns_t *); 66-ns is a thin wrapper around it.
 */

#ifndef NS_H
#define NS_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <oblibs/strbuf.h>
#include <oblibs/genbuf.h>

/**
 * Element types
 *
 * How a single filesystem element is materialised inside the namespace.
 * NS_TYPE_BIND is the default (a plain MS_BIND of the host path).
 */
enum ns_type_e {
    NS_TYPE_BIND = 0,   // MS_BIND the host path (default)
    NS_TYPE_TMPFS,      // fresh tmpfs on the target
    NS_TYPE_HIDDEN,     // make the target inaccessible (ro bind of empty node)
    NS_TYPE_RECURSIVE,  // MS_BIND|MS_REC of the host path
    NS_TYPE_CLONE,      // copy the node (file/dir/fifo/sym/dev) into the ns
    NS_TYPE_PROC,       // fresh procfs (only valid for /proc)
    NS_TYPE_DEV,        // fresh dev (only valid for /dev)
    NS_TYPE_SYS,        // fresh sysfs (only valid for /sys)
    NS_TYPE_ENDOFKEY
} ;

extern char const *const ns_type_str[NS_TYPE_ENDOFKEY] ;

/**
 * ns_entry_t -- one declarative mount element
 *
 * String fields are byte offsets into the owning ns_t::sb arena, never
 * pointers (the arena may realloc). -1 means "unset".
 */
typedef struct ns_entry_s ns_entry_t ;
struct ns_entry_s {
    ssize_t       path ;    // host source path (absolute). Mandatory.
    ssize_t       target ;  // destination inside the ns; -1 => same as path
    ssize_t       opts ;    // user mount options string; -1 => none
    int           type ;    // enum ns_type_e
    unsigned long flags ;   // MS_* flags, computed from type + opts
    uint8_t       create ;  // create target if missing (default 1)
    uint8_t       ignore ;  // skip element if source missing (default 0)
    uint8_t       done ;    // runtime: element applied
    uint8_t       skip ;    // runtime: dropped by dedup/hidden pruning
} ;

#define NS_ENTRY_ZERO \
    { .path = -1, .target = -1, .opts = -1, .type = NS_TYPE_BIND, \
      .flags = 0, .create = 1, .ignore = 0, .done = 0, .skip = 0 }

/**
 * mntinfo_t -- a cached /proc/self/mounts line
 **/
typedef struct mntinfo_s mntinfo_t ;
struct mntinfo_s {
    ssize_t dir ;   // mount point (offset in ns_t::mntsa)
    ssize_t type ;  // filesystem type
    ssize_t opts ;  // mount options
} ;

#define MNTINFO_ZERO { .dir = -1, .type = -1, .opts = -1 }

/**
 * ns_t -- the whole namespace context (replaces every former global)
 **/
typedef struct ns_s ns_t ;
struct ns_s {

    // CONFIG: filled by the parser, read-only after that

    unsigned long  clone_flags ;       // SIGCHLD|CLONE_NEWNS|...
    unsigned long  root_propagation ;  // final propagation of "/" (MS_SHARED)
    uint8_t        rootless ;          // CLONE_NEWUSER requested
    uint8_t        want_pid1 ;         // CLONE_NEWPID requested
    uint8_t        no_new_privs ;      // PR_SET_NO_NEW_PRIVS
    uint8_t        new_session ;       // setsid() before exec
    uint8_t        take_ctty ;         // TIOCSCTTY after setsid

    uid_t          map_inner_uid ;     // rootless: inner uid (default 0)
    gid_t          map_inner_gid ;     // rootless: inner gid (default 0)

    char const    *base_dir ;          // run dir root: /run/66/ns/<uid>
    char const    *nstmp ;             // base_dir + "/nstmp"
    char const    *nshidden ;          // base_dir + "/hidden"
    strbuf         paths ;             // stable storage for the 3 paths above
    char const    *rule_dir ;          // default dir for named .rule files
    ssize_t        hostname ;          // offset in sb, -1 if unset
    int            notif_fd ;          // readiness fd (-d), -1 if none

    genbuf         entries ;           // ns_entry_t[]
    strbuf         sb ;                // string arena for the fields above

    // RUNTIME: filled during ns_run

    uid_t          euid ;              // geteuid() captured before clone
    gid_t          egid ;              // getegid() captured before clone
    pid_t          child_pid ;         // top clone child
    pid_t          gchild_pid ;        // pid1's exec child (NEWPID)
    int            ef_ready_p2c ;      // invoker -> child gate
    int            ef_exit_c2p ;       // child/pid1 -> invoker exit status
    int            ef_userns_c2p ;     // child -> invoker "write my maps"

    genbuf         mntinfo ;           // mntinfo_t[] snapshot of mounts
    strbuf         mntsb ;             // arena for the mounts snapshot

    char const *const *prog ;          // argv to exec inside the ns
    char const *const *envp ;          // environment to pass to prog
} ;

#define NS_T_ZERO \
    { .clone_flags = 0, .root_propagation = 0, .rootless = 0, .want_pid1 = 0, \
      .no_new_privs = 0, .new_session = 0, .take_ctty = 0, \
      .map_inner_uid = 0, .map_inner_gid = 0, \
      .base_dir = 0, .nstmp = 0, .nshidden = 0, .paths = STRBUF_ZERO, .rule_dir = 0, \
      .hostname = -1, .notif_fd = -1, \
      .entries = GENBUF_ZERO, .sb = STRBUF_ZERO, \
      .euid = 0, .egid = 0, .child_pid = 0, .gchild_pid = 0, \
      .ef_ready_p2c = -1, .ef_exit_c2p = -1, .ef_userns_c2p = -1, \
      .mntinfo = GENBUF_ZERO, .mntsb = STRBUF_ZERO, \
      .prog = 0, .envp = 0 }

#define _cleanup_ns_ __attribute__((cleanup(ns_free)))

/** @brief Release every resource held by @ns (arenas, genbufs, fds). */
extern void ns_free(ns_t *ns) ;

/** @brief Append @s to the arena @ns->sb and return its byte offset, or die on
 *  allocation failure. The returned offset stays valid across further appends. */
extern size_t ns_arena_add(ns_t *ns, char const *s) ;

/** @brief Resolve an element type name ("tmpfs", "proc", ...) to its enum value,
 *  or -1 if unknown. (ns_run.c) */
extern int ns_type_by_name(char const *name) ;

/** @brief Parse one "-o" namespace-options value (e.g. "unshare=pid,nonewprivileges")
 *  into @ns. Dies (LOG_EXIT_USER) on invalid input. */
extern void ns_parse_options(ns_t *ns, char const *str) ;

/** @brief Validate one raw "-e" element value (absolute path) and append it to
 *  the @dir list (an sbl of pending element strings). Dies on invalid input. */
extern void ns_collect_element(strbuf *dir, char const *element) ;

/** @brief Load the .rule file @name (relative/absolute path, or a bare name
 *  looked up under @rule_dir), parse its INI sections honouring [include], and
 *  append the resulting element strings to @dir (skipping a path already present
 *  -- so an earlier -e wins). Dies on error. */
extern void ns_collect_rule(strbuf *dir, char const *name, char const *rule_dir) ;

/** @brief Parse one element string ("/etc:type=tmpfs:options=ro") into an
 *  ns_entry_t and append it to @ns->entries. Pure: touches no filesystem. */
extern void ns_parse_element(ns_t *ns, char const *element) ;

/** @brief Build @ns->entries from every element string collected in @dir. */
extern void ns_build_entries(ns_t *ns, strbuf *dir) ;

/** @brief Build the namespace described by @ns and exec @ns->prog inside it.
 *  Returns the exit status of @ns->prog (or never returns when @ns is configured
 *  to exec directly without supervision). */
extern int ns_run(ns_t *ns) ;

/** @brief opt_cmd_fn-shaped entry point: parse @argv into an ns_t and ns_run().
 *  This is the 66-ns main() body and the future 66 sub-command handler.
 *  @data is unused for now (reserved for ssexec_t *). */
extern int ns_main(int argc, char const *const *argv, void *data) ;

#endif
