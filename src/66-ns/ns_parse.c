/*
 * ns_parse.c
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
 * Parsing of -o (namespace options), -e (element) and -r (rule files). Built
 * on the oblibs opt_fields/lexer layer.
 */

#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>

#include <oblibs/log.h>
#include <oblibs/opt.h>
#include <oblibs/lexer.h>
#include <oblibs/sbl.h>
#include <oblibs/strbuf.h>
#include <oblibs/string.h>
#include <oblibs/types.h>

#include <66/config.h>

#include "ns.h"
#include "ns_internal.h"

// Apply the type-specific flag policy and default the target. Pure: no fs.

static void entry_finalize(ns_t *ns, ns_entry_t *e)
{
    if (e->target < 0)
        e->target = e->path ;

    char const *path = ns->sb.s + e->path ;

    switch (e->type) {

        case NS_TYPE_TMPFS :
            e->flags &= ~(unsigned long)MS_BIND ;
            e->flags &= ~(unsigned long)MS_REC ;
            break ;

        case NS_TYPE_HIDDEN :
            e->flags = MS_BIND | MS_RDONLY | MS_NOSUID | MS_NODEV ;
            break ;

        case NS_TYPE_RECURSIVE :
            e->flags |= MS_BIND | MS_REC ;
            break ;

        case NS_TYPE_CLONE :
            e->flags = 0 ;
            e->opts = -1 ;
            break ;

        case NS_TYPE_PROC :

            if (strcmp(path, "/proc"))
                log_die(LOG_EXIT_USER, "type proc is only valid for /proc, got: ", path) ;

            e->flags = MS_NOSUID | MS_NODEV | MS_NOEXEC ;
            break ;

        case NS_TYPE_DEV :

            if (strcmp(path, "/dev"))
                log_die(LOG_EXIT_USER, "type dev is only valid for /dev, got: ", path) ;

            e->flags = MS_NOSUID | MS_STRICTATIME | MS_NOEXEC | MS_REC ;
            e->opts = ns_arena_add(ns, "mode=755") ;
            break ;

        case NS_TYPE_SYS :

            if (strcmp(path, "/sys"))
                log_die(LOG_EXIT_USER, "type sys is only valid for /sys, got: ", path) ;

            e->flags = MS_NOSUID | MS_STRICTATIME | MS_NOEXEC ;
            break ;

        case NS_TYPE_BIND :
        default :
            e->flags |= MS_BIND ;
            break ;
    }
}

// -e element parsing

enum { F_TARGET, F_TYPE, F_OPTS, F_IGNORE, F_CREATE } ;

static opt_field_t const e_fields[] = {
    { .id = F_TARGET, .key = "target",  .hasval = 1 },
    { .id = F_TYPE,   .key = "type",    .hasval = 1 },
    { .id = F_OPTS,   .key = "options", .hasval = 1 },
    { .id = F_IGNORE, .key = "ignore",  .hasval = 1 },
    { .id = F_CREATE, .key = "create",  .hasval = 1 },
} ;

struct edata { ns_t *ns ; ns_entry_t *e ; } ;

static int e_field_cb(int id, char const *key, char const *val, void *data)
{
    struct edata *d = data ;
    ns_t *ns = d->ns ;
    ns_entry_t *e = d->e ;

    (void)key ;

    switch (id) {

        case F_TARGET :

            if (val[0] != '/')
                log_die(LOG_EXIT_USER, "target path must be absolute: ", val) ;

            e->target = ns_arena_add(ns, val) ;
            break ;

        case F_TYPE : {

            int t = ns_type_by_name(val) ;
            if (t < 0)
                log_die(LOG_EXIT_USER, "invalid type: ", val) ;

            e->type = t ;
            break ;
        }

        case F_OPTS :

            e->opts = ns_arena_add(ns, val) ;
            break ;

        case F_IGNORE :

            if (val[0] == 'y')
                e->ignore = 1 ;
            break ;

        case F_CREATE :

            if (val[0] == 'n')
                e->create = 0 ;
            break ;
    }

    return 1 ;
}

void ns_parse_element(ns_t *ns, char const *str)
{
    log_flow() ;

    ns_entry_t e = NS_ENTRY_ZERO ;

    if (str[0] != '/')
        log_die(LOG_EXIT_USER, "path must be absolute: ", str) ;

    ssize_t c = get_len_until(str, ':') ;
    size_t plen = (c < 0) ? strlen(str) : (size_t)c ;

    {
        char path[plen + 1] ;
        memcpy(path, str, plen) ;
        path[plen] = 0 ;

        e.path = ns_arena_add(ns, path) ;
    }

    if (c >= 0 && str[c + 1]) {
        struct edata d = { ns, &e } ;
        if (!opt_fields(str + c + 1, ':', '=', e_fields, OPT_COUNT(e_fields), e_field_cb, &d))
            log_die(LOG_EXIT_USER, "invalid element: ", str) ;
    }

    if (e.opts >= 0)
        ns_split_opts(ns, ns->sb.s + e.opts, &e.flags, &e.opts) ;

    entry_finalize(ns, &e) ;

    if (!genbuf_append(ns_entry_t, &ns->entries, &e))
        log_die_nomem("entries") ;
}

void ns_build_entries(ns_t *ns, strbuf *dir)
{
    size_t pos = 0 ;

    FOREACH_SBL(dir, pos)
        ns_parse_element(ns, dir->s + pos) ;

}

// -o namespace options

enum { O_FLAG, O_NONEWPRIV, O_UNSHARE, O_HOSTNAME, O_UID, O_GID, O_NEWSESSION } ;

static void parse_unshare(ns_t *ns, char const *val)
{
    _alloc_strbuf_(toks, strlen(val) + 1) ;

    if (!lexer_trim_with_delim(&toks, val, ':'))
        log_dieu(LOG_EXIT_USER, "parse unshare: ", val) ;

    size_t pos = 0 ;
    {
        FOREACH_SBL(&toks, pos) {

            char const *u = toks.s + pos ;
            if (!strcmp(u, "all")) {

                ns->clone_flags |= CLONE_NEWNET | CLONE_NEWIPC | CLONE_NEWUTS | CLONE_NEWCGROUP ;

            } else if (!strcmp(u, "pid")) {

                ns->clone_flags |= CLONE_NEWPID ;
                ns->want_pid1 = 1 ;

            } else if (!strcmp(u, "net")) {

                ns->clone_flags |= CLONE_NEWNET ;

            } else if (!strcmp(u, "ipc")) {

                ns->clone_flags |= CLONE_NEWIPC ;

            } else if (!strcmp(u, "uts")) {

                ns->clone_flags |= CLONE_NEWUTS ;

            } else if (!strcmp(u, "cgroup")) {

                struct stat st ;
                if (stat("/proc/self/ns/cgroup", &st)) {
                    if (errno == ENOENT)
                        log_die(LOG_EXIT_USER, "kernel does not support cgroup namespace") ;

                    log_dieusys(LOG_EXIT_SYS, "stat: /proc/self/ns/cgroup") ;
                }

                ns->clone_flags |= CLONE_NEWCGROUP ;

            } else if (!strcmp(u, "user")) {

                ns->clone_flags |= CLONE_NEWUSER ;
                ns->rootless = 1 ;

            } else log_die(LOG_EXIT_USER, "invalid unshare option: ", u) ;
        }
    }
}

static int o_field_cb(int id, char const *key, char const *val, void *data)
{
    ns_t *ns = data ;
    (void)key ;

    switch (id) {

        case O_FLAG :

            if (!strcmp(val, "private")) {

                ns->root_propagation = MS_PRIVATE ;

            } else if (!strcmp(val, "slave")) {

                ns->root_propagation = MS_SLAVE ;

            } else if (!strcmp(val, "unbindable")) {

                ns->root_propagation = MS_UNBINDABLE ;

            } else if (!strcmp(val, "shared")) {

                ns->root_propagation = MS_SHARED ;

            } else log_die(LOG_EXIT_USER, "invalid flag option: ", val) ;

            break ;

        case O_NONEWPRIV :

            ns->no_new_privs = 1 ;
            break ;

        case O_UNSHARE :

            parse_unshare(ns, val) ;
            break ;

        case O_HOSTNAME :

            ns->hostname = (ssize_t)ns_arena_add(ns, val) ;
            ns->clone_flags |= CLONE_NEWUTS ;
            break ;

        case O_UID :

            if (!uid_parse_strict(val, &ns->map_inner_uid))
                log_die(LOG_EXIT_USER, "invalid uid: ", val) ;
            break ;

        case O_GID :

            if (!gid_parse_strict(val, &ns->map_inner_gid))
                log_die(LOG_EXIT_USER, "invalid gid: ", val) ;
            break ;

        case O_NEWSESSION :
            ns->new_session = 1 ;
            break ;
    }

    return 1 ;
}

void ns_parse_options(ns_t *ns, char const *str)
{
    log_flow() ;

    static opt_field_t const o_fields[] = {
        { .id = O_FLAG,       .key = "flag",            .hasval = 1 },
        { .id = O_NONEWPRIV,  .key = "nonewprivileges", .hasval = 0 },
        { .id = O_UNSHARE,    .key = "unshare",         .hasval = 1 },
        { .id = O_HOSTNAME,   .key = "hostname",        .hasval = 1 },
        { .id = O_UID,        .key = "uid",             .hasval = 1 },
        { .id = O_GID,        .key = "gid",             .hasval = 1 },
        { .id = O_NEWSESSION, .key = "newsession",      .hasval = 0 },
    } ;

    if (!opt_fields(str, ',', '=', o_fields, OPT_COUNT(o_fields), o_field_cb, ns))
        log_die(LOG_EXIT_USER, "invalid namespace options: ", str) ;
}

// -e collection and -r rule files

void ns_collect_element(strbuf *dir, char const *element)
{
    if (element[0] != '/')
        log_die(LOG_EXIT_USER, "path must be absolute: ", element) ;

    if (!sbl_add(dir, element))
        log_die_nomem("dir") ;
}

/** Is the element path @path already present in the @dir list? Compares the
 * path component (everything before the first ':'). */
static int dir_has_path(strbuf *dir, char const *path)
{
    size_t plen = strlen(path) ;
    size_t pos = 0 ;

    FOREACH_SBL(dir, pos) {

        char const *e = dir->s + pos ;
        ssize_t c = get_len_until(e, ':') ;
        size_t elen = (c < 0) ? strlen(e) : (size_t)c ;

        if (elen == plen && !memcmp(e, path, plen))
            return 1 ;
    }

    return 0 ;
}

/** .rule file parsing.
 * Sections are located with the oblibs lexer
 * ('[' open, ']' close, spaces skipped); the body between two section headers
 * is reassembled into a ':'-joined "-e" element string. Same two-step bridge as
 * the original (section -> "-e" string -> ns_parse_element). */

/** In-place trim of leading/trailing blanks (space, tab, CR) of one body line;
 * returns the first non-blank char. Section headers are handled by the lexer;
 * this only normalises the key=val body lines (no oblibs primitive for it). */
static char *rule_trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r')
        s++ ;

    size_t n = strlen(s) ;

    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r'))
        s[--n] = 0 ;

    return s ;
}

/* Extract the next "[name]" of @str starting at *pos. The name is appended to
 * @secname (a cumulative sbl: the current section is its last element) and *pos
 * is advanced past the header. Returns 1 found, 0 none, -1 on alloc failure. */
static int ns_get_section(strbuf *secname, char const *str, size_t *pos)
{
    size_t len = strlen(str) ;
    _alloc_strbuf_(sb, len + 1) ;

    lexer_config cfg = LEXER_CONFIG_ZERO ;
    cfg.open = "[" ; cfg.olen = 1 ;
    cfg.close = "]" ; cfg.clen = 1 ;
    cfg.skip = " \t\r" ; cfg.skiplen = 3 ;
    cfg.forceclose = 1 ; cfg.firstoccurence = 1 ;
    cfg.kopen = 0 ; cfg.kclose = 0 ;

    while (*pos < len) {

        sb.len = 0 ;
        cfg.found = 0 ;

        if (!lexer_trim_with_g(&sb, str + *pos, &cfg))
            return 0 ;

        *pos += cfg.pos ;

        if (cfg.found)
            break ;

        if (!cfg.count)
            *pos = len ;
    }

    if (cfg.found) {
        if (!strbuf_terminate(&sb) || !sbl_add(secname, sb.s))
            return -1 ;
    }

    return (int)cfg.found ;
}

// Current section name = the last element appended to @secname.
static char const *ns_section_name(strbuf *secname)
{
    size_t n = sbl_count(secname) ;
    if (!n)
        return 0 ;

    ssize_t off = sbl_element_byid(secname, (unsigned int)(n - 1)) ;

    return off < 0 ? 0 : secname->s + off ;
}

/** Reassemble a section body (raw "key=val" lines) into "name:k=v:k=v" and add
 * it to @dir as one "-e" element string. */
static void ns_rebuild_rule_for_entry(strbuf *dir, char const *name, char const *body)
{
    _alloc_strbuf_(el, 256) ;

    if (!strbuf_cats(&el, name))
        log_die_nomem("rule") ;

    _alloc_sbl_(lines, strlen(body) + 1) ;

    if (!strbuf_cats(&lines, body) || !strbuf_terminate(&lines))
        log_die_nomem("rule") ;

    if (!sbl_split_string_in_nline(&lines))
        log_dieu(LOG_EXIT_SYS, "split section body") ;

    size_t pos = 0 ;

    FOREACH_SBL(&lines, pos) {
        char const *raw = lines.s + pos ;
        size_t rl = strlen(raw) ;
        char buf[rl + 2] ;
        memcpy(buf, raw, rl + 1) ;
        char *l = rule_trim(buf) ;

        if (!*l || *l == '#')
            continue ;

        if (!strbuf_cats(&el, ":") || !strbuf_cats(&el, l))
            log_die_nomem("rule") ;
    }

    if (!strbuf_terminate(&el))
        log_die_nomem("rule") ;

    if (!sbl_add(dir, el.s))
        log_die_nomem("dir") ;
}

/** Dispatch a section: "[include]" recurses into the listed rule files, a
 * "[/path]" section becomes an "-e" element (unless an earlier -e/rule already
 * claimed the same path). */
static void ns_split_from_section(strbuf *dir, char const *name, char const *body, char const *rule_dir)
{
    if (!name || !*name)
        log_die(LOG_EXIT_USER, "empty section name in rule file") ;

    if (!strcmp(name, "include")) {

        _alloc_sbl_(files, strlen(body) + 1) ;

        if (!strbuf_cats(&files, body) || !strbuf_terminate(&files))
            log_die_nomem("rule") ;

        if (!sbl_split_string_in_nline(&files))
            log_dieu(LOG_EXIT_SYS, "split include section") ;

        size_t pos = 0 ;

        FOREACH_SBL(&files, pos) {
            char const *raw = files.s + pos ;
            size_t rl = strlen(raw) ;
            char buf[rl + 2] ;
            memcpy(buf, raw, rl + 1) ;

            char *f = rule_trim(buf) ;

            if (*f && *f != '#')
                ns_collect_rule(dir, f, rule_dir) ;
        }

        return ;
    }

    if (name[0] != '/')
        log_die(LOG_EXIT_USER, "rule section path must be absolute: ", name) ;

    // an earlier -e (or rule) for the same path wins
    if (dir_has_path(dir, name))
        return ;

    ns_rebuild_rule_for_entry(dir, name, body) ;
}

void ns_collect_rule(strbuf *dir, char const *name, char const *rule_dir)
{
    log_flow() ;

    _alloc_strbuf_(content, 1) ;

    // relative/absolute path first, then rule_dir/name
    if (!strbuf_read_file(&content, name)) {

        int ok = 0 ;
        if (rule_dir && *rule_dir) {

            _alloc_strbuf_(p, SS_MAX_PATH) ;

            if (!strbuf_cats(&p, rule_dir))
                log_die_nomem("rule") ;

            if (p.len && p.s[p.len - 1] != '/')
                strbuf_cats(&p, "/") ;

            if (!strbuf_cats(&p, name) || !strbuf_terminate(&p))
                log_die_nomem("rule") ;

            content.len = 0 ;
            ok = strbuf_read_file(&content, p.s) ;
        }
        if (!ok)
            log_dieusys(LOG_EXIT_SYS, "open rule file: ", name) ;
    }

    if (!content.len)
        log_die(LOG_EXIT_USER, "empty rule file: ", name) ;

    /** Walk the sections on the raw buffer: each section's body is the slice
     * between its header and the next one. (ported from original ns_parse_rule) */
    _alloc_sbl_(secname, 256) ;
    size_t clen = strlen(content.s) ;
    size_t pos = 0, start = 0 ;

    while (pos < clen) {

        start = pos ;
        int found = ns_get_section(&secname, content.s, &pos) ;
        if (found == -1)
            log_die_nomem("rule") ;

        if (!found && !start)
            log_die(LOG_EXIT_USER, "invalid rule file (no section): ", name) ;

        if (!found) {
            // no more sections: the body is the rest of the file
            ns_split_from_section(dir, ns_section_name(&secname), content.s + start + 1, rule_dir) ;
            break ;
        }

        /* locate the next header to bound the current section's body, then drop
         * its name (keep the current one) */
        start = pos ;
        size_t keep = secname.len ;
        found = ns_get_section(&secname, content.s, &pos) ;
        secname.len = keep ;
        if (found == -1)
            log_die_nomem("rule") ;

        if (!found) {
            ns_split_from_section(dir, ns_section_name(&secname), content.s + start + 1, rule_dir) ;
            break ;
        }

        ssize_t r = get_rlen_until(content.s, '\n', pos - 1) ;
        if (r < 0 || (size_t)r <= start)
            log_die(LOG_EXIT_USER, "malformed rule file: ", name) ;

        size_t blen = (size_t)r - start ;
        char body[blen + 1] ;
        memcpy(body, content.s + start + 1, blen) ;
        body[blen] = 0 ;

        ns_split_from_section(dir, ns_section_name(&secname), body, rule_dir) ;

        // restart the search at the next section header just found
        pos = start ;
    }
}
