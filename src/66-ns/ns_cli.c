/*
 * ns_cli.c
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
 * Command-line wrapper: parse argv into an ns_t, then ns_run(). This is the
 * 66-ns program body and the shape of the future 66 sub-command handler.
 */

#include <stdint.h>

#include <oblibs/opt.h>
#include <oblibs/log.h>
#include <oblibs/types.h>
#include <oblibs/strbuf.h>
#include <oblibs/sbl.h>

#include <66/config.h>

#include "ns.h"

extern char **environ ;

/* Default directory where bare .rule names are looked up. */
#define NS_RULE_DIR SS_SCRIPT_SYSDIR "ns"

static opt_t const ns_opts[] = {
    { .id = OPT_ID_HELP, .shortname = 'h', .longname = "help",      .arg = OPT_NONE,                              .help = "print this help" },
    { .id = 'z',         .shortname = 'z', .longname = "color",     .arg = OPT_NONE,                              .help = "enable color" },
    { .id = 'v',         .shortname = 'v', .longname = "verbosity", .arg = OPT_REQUIRED, .argname = "number",     .help = "set verbosity level (0..5)" },
    { .id = 'd',         .shortname = 'd', .longname = "notify",    .arg = OPT_REQUIRED, .argname = "number",     .help = "notify readiness on file descriptor notif" },
    { .id = 'o',         .shortname = 'o', .longname = "options",   .arg = OPT_REQUIRED, .argname = "ns_options", .help = "comma-separated namespace options (repeatable)" },
    { .id = 'e',         .shortname = 'e', .longname = "element",   .arg = OPT_REQUIRED, .argname = "element",    .help = "colon-separated element to handle (repeatable)" },
    { .id = 'r',         .shortname = 'r', .longname = "rule",      .arg = OPT_REQUIRED, .argname = "path",       .help = "rule file to apply (repeatable)" },
    { .id = 'p',         .shortname = 'p', .longname = "pidfile",   .arg = OPT_REQUIRED, .argname = "path",       .help = "authoritative in-ns pidfile naming the daemon's main process (needs unshare=pid)" },
} ;

static opt_cmd_t const ns_cmd = {
    .name = "66-ns",
    .operands = "prog...",
    .opts = ns_opts,
    .nopts = OPT_COUNT(ns_opts),
    .epilog =
        "namespace options (-o), comma-separated:\n"
        "    flag=flag           final / propagation: shared|slave|private|unbindable\n"
        "    unshare=flag        namespaces: pid, net, ipc, uts, cgroup, user, all\n"
        "                        'user' enables rootless mode (no root needed)\n"
        "    uid=number          rootless: inner uid the caller maps to (default 0)\n"
        "    gid=number          rootless: inner gid the caller maps to (default 0)\n"
        "    hostname=name       set hostname (implies unshare=uts)\n"
        "    nonewprivileges     set PR_SET_NO_NEW_PRIVS\n"
        "    newsession          setsid() before exec (detach controlling tty)\n"
        "\n"
        "element options (-e), colon-separated, first field is an absolute path:\n"
        "    target=path         mount destination inside the ns (default: same path)\n"
        "    type=type           tmpfs|hidden|recursive|clone|proc|dev|sys (default: bind)\n"
        "    options=options     mount options (kernel flags folded in, rest passed as data)\n"
        "    ignore=yes|no       skip the element if its source is missing (default no)\n"
        "    create=yes|no       create the target if missing (default yes)\n"
        "\n"
        "supervision (with unshare=pid, 66-ns is pid 1 of the namespace):\n"
        "    foreground and self-backgrounding daemons are supervised automatically,\n"
        "    no option needed; when the main exits, the namespace is torn down.\n"
        "    --pidfile is optional: if the daemon can write one, it makes supervision\n"
        "    authoritative (the named pid is the main); it never breaks anything.",
} ;

int ns_main(int argc, char const *const *argv, void *data)
{
    (void)data ;

    _cleanup_ns_ ns_t ns = NS_T_ZERO ;
    opt_scan_t st = OPT_SCAN_ZERO ;
    int r ;

    /* dir   : ordered list of pending element strings (-e first, then rules).
     * rules : rule names/paths to expand after option scanning. */
     _cleanup_strbuf_ strbuf dir = STRBUF_ZERO ;
     _cleanup_strbuf_ strbuf rules = STRBUF_ZERO ;

    for (;;) {

        int o = opt_scan(argc, argv, ns_opts, OPT_COUNT(ns_opts), &st) ;
        if (o == OPT_END) break ;

        switch (o) {

            case OPT_ID_HELP :
                return opt_emit_help(ns_cmd.name, &ns_cmd) ;

            case 'z' :
                log_color = &log_color_enable ;
                break ;

            case 'v' : {

                uint32_t v ;
                if (!u32_scan_strict(st.arg, &v))
                    return opt_emit_usage(ns_cmd.name, &ns_cmd) ;

                VERBOSITY = v ;
                break ;
            }

            case 'd' : {

                uint32_t fd ;
                if (!u32_scan_strict(st.arg, &fd))
                    return opt_emit_usage(ns_cmd.name, &ns_cmd) ;

                ns.notif_fd = (int)fd ;
                break ;
            }

            case 'o' :
                ns_parse_options(&ns, st.arg) ;
                break ;

            case 'e' :
                ns_collect_element(&dir, st.arg) ;
                break ;

            case 'r' :
                if (!sbl_add(&rules, st.arg))
                    log_die_nomem("rules") ;
                break ;

            case 'p' :
                ns.pidfile = (ssize_t)ns_arena_add(&ns, st.arg) ;
                break ;

            default :
                return opt_emit_error(ns_cmd.name, &ns_cmd, o, &st) ;
        }
    }

    argc -= st.ind ;
    argv += st.ind ;

    if (argc < 1)
        return opt_emit_usage(ns_cmd.name, &ns_cmd) ;

    /* Rules are expanded after -e elements so an -e wins over a rule for the
     * same path (ns_collect_rule skips a path already present in dir). */
    char const *rdir = ns.rule_dir ? ns.rule_dir : NS_RULE_DIR ;
    {
        size_t pos = 0 ;
        FOREACH_SBL(&rules, pos)
            ns_collect_rule(&dir, rules.s + pos, rdir) ;
    }

    ns_build_entries(&ns, &dir) ;

    ns.prog = argv ;
    ns.envp = (char const *const *)environ ;

    r = ns_run(&ns) ;

    return r ;
}

int main(int argc, char const *const *argv)
{
    PROG = "66-ns" ;
    return ns_main(argc, argv, 0) ;
}
