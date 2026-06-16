/*
 * 66-which.c
 *
 * Copyright (c) 2019 Dyne.org Foundation, Amsterdam
 * Copyright (c) 2020 Eric Vidal <eric@obarun.org>
 *
 * Written by:
 *  - Danilo Spinella <danyspin97@protonmail.com>
 *  - Eric Vidal <eric@obarun.org>
 *
 * All rights reserved.
 *
 * This file is part of Obarun. It is subject to the license terms in
 * the LICENSE file found in the top-level directory of this
 * distribution.
 * This file may not be copied, modified, propagated, or distributed
 * except according to the terms contained in the LICENSE file./
 *
 * */

#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <oblibs/log.h>
#include <oblibs/string.h>
#include <oblibs/genbuf.h>
#include <oblibs/opt.h>
#include <oblibs/stream.h>
#include <oblibs/strbuf.h>
#include <oblibs/exec.h>

static opt_t const opts[] = {
    { .id = OPT_ID_HELP, .shortname = 'h', .longname = "help", .arg = OPT_NONE, .help = "print this help" },
    { .id = 'a',         .shortname = 'a', .longname = "all",  .arg = OPT_NONE, .help = "print all matching executable in PATH" },
    { .id = 'q',         .shortname = 'q', .longname = "quiet", .arg = OPT_NONE, .help = "quiet, do not print anything to stdout" },
} ;

static opt_cmd_t const cmd = {
    .name = "66-which",
    .operands = "commands...",
    .opts = opts,
    .nopts = OPT_COUNT(opts),
} ;

int check_executable(char const* filepath)
{
    struct stat sb ;
    return (stat(filepath, &sb) == 0 && sb.st_mode & S_IXUSR
            && !S_ISDIR(sb.st_mode)) ? 1 : 0 ;
}

int parse_path(genbuf* folders, char* path)
{
    char* rp = NULL ;
    size_t i, len, s ;
    int found ;
    _cleanup_strbuf_ strbuf filepath = STRBUF_ZERO ;

    while (path) {

        s = str_search(path, ':') ;
        if (!strbuf_copyb(&filepath, path, s)
            || !strbuf_terminate(&filepath))
                log_dieusys(LOG_EXIT_SYS, "append strbuf with PATH") ;

        rp = realpath(filepath.s, NULL);

        if (rp != NULL) {

            char const** ss = genbuf_s(char const*, folders);
            found = 0;
            len = genbuf_len(char const*, folders);
            for ( i = 0 ; i < len ; i++) {
                if (!strcmp(ss[i], rp)) {
                    found = 1 ;
                    break ;
                }
            }
            if (!found) {
                if (!genbuf_append(char const*, folders, &rp))
                    log_dieusys(LOG_EXIT_SYS, "append genbuf") ;
            } else {
                free(rp);
            }
        }
        if (s == strlen(path)) break ;
        path += s + 1;
    }

    return genbuf_len(char const*, folders) ;
}

int handle_string(char const* name, char const* env_path, genbuf_ref paths, int quiet, int printall)
{
    size_t len = genbuf_len(char const*, paths) ;
    int found = 0 ;

    strbuf filepath = STRBUF_ZERO ;
    char const** ss = genbuf_s(char const*, paths) ;

    for (size_t i = 0 ; i < len ; i++) {

        filepath.len = 0 ;
        if (!auto_strbuf(&filepath, ss[i], "/", name))
            log_die_nomem("strbuf");

        if (check_executable(filepath.s)) {
            if (!quiet && (!ostream_puts(ostream_1, filepath.s)
                || !ostream_put(ostream_1, "\n", 1)
                || !ostream_flush(ostream_1))) log_dieusys(LOG_EXIT_SYS, "write to stdout") ;
            found = 1;
            if (!printall)
                break ;
        }
    }

    if (found == 0 && !quiet)
        log_warn("no ",name," in (",env_path,")") ;

    strbuf_free(&filepath) ;
    return found == 1 ? 0 : 111;
}

int handle_path(char const* path, int quiet) {

    char* rp = realpath(path, NULL) ;

    if (rp != NULL && check_executable(rp)) {

        if (!quiet && (!ostream_puts(ostream_1, rp)
            || !ostream_put(ostream_1, "\n", 1)
            || !ostream_flush(ostream_1)))
                log_dieusys(LOG_EXIT_SYS, "write to stdout") ;
        free(rp) ;
        return 0 ;
    }

    free(rp) ;

    size_t len = strlen(path) ;
    char base[len+1] ;
    char dir[len+1] ;

    if (!ob_basename(base, path))
        log_dieusys(LOG_EXIT_SYS, "get basename") ;

        if (!ob_dirname(dir, path))
        log_dieusys(LOG_EXIT_SYS, "get dirname") ;

    if (!quiet)
        log_warn("no ",base," in (",dir,")") ;

    return 111 ;
}

int main (int argc, char const *const *argv)
{
    int printall = 0 ;
    int quiet = 0 ;
    char* path = 0 ;
    int ret = 0;
    genbuf paths = GENBUF_ZERO ; // char const *

    PROG = "66-which" ;
    {
        opt_scan_t st = OPT_SCAN_ZERO ;

        for (;;)
        {
            int o = opt_scan(argc, argv, opts, OPT_COUNT(opts), &st) ;
            if (o == OPT_END) break ;
            switch (o)
            {
                case OPT_ID_HELP : return opt_emit_help(cmd.name, &cmd) ;
                case 'a': printall = 1 ; break ;
                case 'q': quiet = 1 ; break ;
                default : return opt_emit_error(cmd.name, &cmd, o, &st) ;
            }
        }
        argc -= st.ind ; argv += st.ind ;
    }

    if (printall && quiet) return opt_emit_usage(cmd.name, &cmd) ;

    path = getenv("PATH") ;
    if (!path)
        path = EXEC_DEFAULTPATH ;

    if (argc < 0)
        return opt_emit_usage(cmd.name, &cmd) ;

    if (!parse_path(&paths, path))
        log_dieusys(LOG_EXIT_SYS, "PATH is empty or contains non valid values") ;

    for ( ; *argv ; argv++) {

        if ((*argv)[0] == '/'
            || (*argv)[0] == '~' || ((*argv)[0] == '~' && (*argv)[1] == '/')
            || (*argv)[0] == '.' || ((*argv)[0] == '.' && (*argv)[1] == '/')
            || ((*argv)[0] == '.' || ((*argv)[1] == '.' && (*argv)[2] == '/')))
            ret = handle_path(*argv, quiet) ;
        else
            ret = handle_string(*argv, path, &paths, quiet, printall) ;
    }

    char const **ss = genbuf_s(char const *, &paths) ;
    size_t n = genbuf_len(char const *, &paths) ;

    for (size_t i = 0 ; i < n ; i++)
        free((void *)ss[i]) ;

    genbuf_free(&paths) ;

    return ret ;
}
