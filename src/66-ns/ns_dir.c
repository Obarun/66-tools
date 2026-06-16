/*
 * ns_dir.c
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
 * Ephemeral scratch for the staging dir and the hidden placeholder nodes. Like
 * bwrap, a private tmpfs is over-mounted on a guaranteed mountpoint inside the
 * child's mount namespace; it dies with the namespace, so there is no on-disk
 * run dir and no prerequisite. The placeholders are materialised only when a
 * hidden element is actually requested.
 */

#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/mount.h>

#include <oblibs/log.h>
#include <oblibs/strbuf.h>
#include <oblibs/genbuf.h>
#include <oblibs/string.h>
#include <oblibs/directory.h>
#include <oblibs/files.h>

#include "ns.h"
#include "ns_internal.h"

void ns_dir_setup(ns_t *ns)
{
    log_flow() ;

    /** A unique, private scratch mountpoint under /tmp (always writable, no
     * prerequisite). The tmpfs is over-mounted on this DEDICATED dir, not on
     * /tmp itself, so real bind sources under /tmp are not shadowed. Created in
     * the invoker; the path is inherited by the child via clone, and the (empty)
     * host dir is rmdir'd by the invoker once the child is gone. */
    char tmpl[] = "/tmp/66ns-XXXXXX" ;
    if (!mkdtemp(tmpl))
        log_dieusys(LOG_EXIT_SYS, "create scratch dir under /tmp") ;

    size_t base_off = ns->paths.len ;
    if (!strbuf_cats(&ns->paths, tmpl) || !strbuf_terminate(&ns->paths))
        log_die_nomem("paths") ;

    size_t nstmp_off = ns->paths.len ;
    if (!strbuf_cats(&ns->paths, tmpl) || !strbuf_cats(&ns->paths, "/nstmp") ||
        !strbuf_terminate(&ns->paths))
        log_die_nomem("paths") ;

    size_t hidden_off = ns->paths.len ;
    if (!strbuf_cats(&ns->paths, tmpl) || !strbuf_cats(&ns->paths, "/hidden") ||
        !strbuf_terminate(&ns->paths))
        log_die_nomem("paths") ;

    ns->base_dir = ns->paths.s + base_off ;
    ns->nstmp    = ns->paths.s + nstmp_off ;
    ns->nshidden = ns->paths.s + hidden_off ;
}

// 1 if at least one element is a hidden mask (placeholders are needed only then)
static int ns_has_hidden(ns_t *ns)
{
    size_t n = genbuf_len(ns_entry_t, &ns->entries) ;
    ns_entry_t *a = genbuf_s(ns_entry_t, &ns->entries) ;
    for (size_t i = 0 ; i < n ; i++)
        if (a[i].type == NS_TYPE_HIDDEN)
            return 1 ;
    return 0 ;
}

void ns_prepare_directory(ns_t *ns)
{
    log_flow() ;

    mode_t mode = S_IRWXU ; // 0700

    /** Over-mount a private tmpfs on the scratch point and make it unbindable so
     * the subsequent rbind of "/" omits it (leaving the real underlying dir). */
    if (mount("tmpfs", ns->base_dir, "tmpfs", MS_NOSUID | MS_NODEV, "mode=0700") == -1)
        log_dieusys(LOG_EXIT_SYS, "mount scratch tmpfs on: ", ns->base_dir) ;

    if (mount(NULL, ns->base_dir, NULL, MS_UNBINDABLE, NULL) == -1)
        log_dieusys(LOG_EXIT_SYS, "make scratch unbindable: ", ns->base_dir) ;

    if (mkdir(ns->nstmp, mode) == -1)
        log_dieusys(LOG_EXIT_SYS, "create: ", ns->nstmp) ;

    if (!ns_has_hidden(ns))
        return ;

    /** Placeholders live on their OWN tmpfs, NOT on the unbindable scratch: an
     * unbindable mount cannot be a bind source, and a hidden mask works by
     * bind-mounting a placeholder over its target. This inner tmpfs is bindable
     * (the default) and, being under the unbindable scratch, is still skipped by
     * the rbind of "/". */
    if (!dir_create_parent(ns->nshidden, mode))
        log_dieusys(LOG_EXIT_SYS, "create: ", ns->nshidden) ;

    if (mount("tmpfs", ns->nshidden, "tmpfs", MS_NOSUID | MS_NODEV, "mode=0700") == -1)
        log_dieusys(LOG_EXIT_SYS, "mount placeholder tmpfs on: ", ns->nshidden) ;

    /** Two inaccessible (mode 0000) placeholders are enough to hide anything: a
     * bind mount only requires source and target to agree on dir vs non-dir, so
     * "directory" hides directories and "file" hides every other node type.
     * Both are creatable unprivileged (works rootless). */
    mode_t u = umask(0000) ;

    {
        size_t hlen = strlen(ns->nshidden) ;
        char p[hlen + sizeof("/directory")] ;
        auto_strings(p, ns->nshidden, "/directory") ;

        if (mkdir(p, 0000) == -1)
            log_dieusys(LOG_EXIT_SYS, "create hidden node: ", p) ;
    }

    if (!file_create_empty(ns->nshidden, "file", 0000))
        log_dieusys(LOG_EXIT_SYS, "create hidden node: ", ns->nshidden) ;

    umask(u) ;
}
