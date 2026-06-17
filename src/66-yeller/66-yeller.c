/*
 * 66-yeller.c
 *
 * Copyright (c) 2020 Eric Vidal <eric@obarun.org>
 *
 * All rights reserved.
 *
 * This file is part of Obarun. It is subject to the license terms in
 * the LICENSE file found in the top-level directory of this
 * distribution.
 * This file may not be copied, modified, propagated, or distributed
 * except according to the terms contained in the LICENSE file./
 */

#include <string.h> //strcmp
#include <stdlib.h> //getenv
#include <sys/types.h> //ssize_t
#include <errno.h>
#include <fcntl.h> //O_RDONLY
#include <unistd.h> //getppid, isatty

#include <oblibs/log.h>
#include <oblibs/string.h>
#include <oblibs/strbuf.h>
#include <oblibs/opt.h>
#include <oblibs/types.h>
#include <oblibs/io.h>
#include <oblibs/fd.h>

#define MAXBUF 4096

static opt_t const opts[] = {
    { .id = OPT_ID_HELP, .shortname = 'h', .longname = "help", .help = "prints this help" },
    { .id = 'd', .shortname = 'd', .longname = "double", .help = "sets double output" },
    { .id = 's', .shortname = 's', .longname = "switch", .help = "switch stdout and stderr" },
    { .id = 'S', .shortname = 'S', .longname = "stdin", .help = "read from stdin" },
    { .id = '1', .shortname = '1', .longname = "out", .arg = OPT_REQUIRED, .argname = "file", .help = "redirect stdout to file" },
    { .id = '2', .shortname = '2', .longname = "err", .arg = OPT_REQUIRED, .argname = "file", .help = "redirect stderr to file" },
    { .id = 'z', .shortname = 'z', .longname = "color", .help = "enable color" },
    { .id = 'n', .shortname = 'n', .longname = "no-newline", .help = "disable trailing new line" },
    { .id = 'c', .shortname = 'c', .longname = "no-time", .help = "disable time display" },
    { .id = 'p', .shortname = 'p', .longname = "program", .arg = OPT_REQUIRED, .argname = "prog", .help = "specifies name of the program" },
    { .id = 'v', .shortname = 'v', .longname = "verbosity", .arg = OPT_REQUIRED, .argname = "verbosity", .help = "increase/decrease verbosity level" },
    { .id = 'i', .shortname = 'i', .longname = "no-info", .help = "do not print information before msg" },
    { .id = 'w', .shortname = 'w', .longname = "warning", .help = "prints a warning message" },
    { .id = 'W', .shortname = 'W', .longname = "warning-force", .help = "prints a warning message regardless the VERBOSITY level" },
    { .id = 't', .shortname = 't', .longname = "trace", .help = "prints a tracing message" },
    { .id = 'T', .shortname = 'T', .longname = "trace-force", .help = "prints a tracing message regardless the VERBOSITY level" },
    { .id = 'f', .shortname = 'f', .longname = "fatal", .help = "prints a fatal message and die" },
    { .id = 'F', .shortname = 'F', .longname = "fatal-force", .help = "prints a fatal message without dying" },
} ;

static char const yeller_epilog[] =
"\n"
"msg color options:\n"
"   %w: set color to white\n"
"   %b: set color to blue\n"
"   %g: set color to green\n"
"   %y: set color to yellow\n"
"   %r: set color to red\n"
"   %l: enable blinking\n"
"   %n: reset color to normal" ;

static opt_cmd_t const cmd = {
    .name = "66-yeller",
    .operands = "msg...",
    .epilog = yeller_epilog,
    .opts = opts,
    .nopts = OPT_COUNT(opts),
} ;

static int read_line(strbuf *dst, char const *line)
{
    char b[MAXBUF] ;

    int fd = io_open(line, O_RDONLY) ;
    if (fd == -1) return 0 ;

    unsigned int n = io_allread(fd, b, MAXBUF - 1) ;
    close_fd(fd) ;

    if(n)
    {
        int i = n ;
        // remove trailing zeroes
        while (i && b[i-1] == '\0') --i ;
        while (i--)
            if (b[i] == '\n' || b[i] == '\0') b[i] = ' ' ;

        if (b[n-1] == ' ') b[n-1] = '\0' ;
    }
    b[n] = '\0';

    if (!strbuf_cats(dst,b) ||
        !strbuf_terminate(dst)) log_die_nomem("strbuf") ;
    return n ;
}

static void build_msg(strbuf *list, int argc,char const *const *argv)
{
    int el = 0, first = 0 ;
    for ( ; el < argc ; el++)
    {
        if (!first) {
            if (!auto_strbuf(list,argv[el]))
                log_die_nomem("strbuf") ;
        }
        else {
            if (!auto_strbuf(list," ",argv[el]))
                log_die_nomem("strbuf") ;
        }
        first++ ;
    }
}

static void rebuild_without_escape(strbuf *list)
{
    size_t pos = 0 ;
    _cleanup_strbuf_ strbuf t = STRBUF_ZERO ;

    for (;pos < list->len;pos++)
    {
        char c = 0 ;
        if (list->s[pos] == '\\') {
            c = 7 + str_search("abtnvfr", list->s[pos+1]) ;
            if (!strbuf_catb(&t,&c,1)) log_die_nomem("strbuf") ;
            if (((pos + 2) >= list->len) || ((pos + 1) >= list->len) ) break ;
            if (list->s[pos+2] == ' ') pos += 2 ;
            else pos++ ;
        }
        else if (list->s[pos] == '%') {
            if (list->s[pos+1] == 'w')
            {
                if (!strbuf_cats(&t,log_color->info)) log_die_nomem("strbuf") ;
            }
            else if (list->s[pos+1] == 'b')
            {
                if (!strbuf_cats(&t,log_color->blue)) log_die_nomem("strbuf") ;
            }
            else if (list->s[pos+1] == 'g')
            {
                if (!strbuf_cats(&t,log_color->valid)) log_die_nomem("strbuf") ;
            }
            else if (list->s[pos+1] == 'y')
            {
                if (!strbuf_cats(&t,log_color->warning)) log_die_nomem("strbuf") ;
            }
            else if (list->s[pos+1] == 'r')
            {
                if (!strbuf_cats(&t,log_color->error)) log_die_nomem("strbuf") ;
            }
            else if (list->s[pos+1] == 'l')
            {
                if (!strbuf_cats(&t,log_color->ablink)) log_die_nomem("strbuf") ;
            }
            else if (list->s[pos+1] == 'n')
            {
                if (!strbuf_cats(&t,log_color->off)) log_die_nomem("strbuf") ;
            }
            if ((pos + 1) >= list->len) break ;
            else pos++ ;
        }
        else {
            c = list->s[pos] ;
            if (!strbuf_catb(&t,&c,1)) log_die_nomem("strbuf") ;
        }
    }
    if (!strbuf_terminate(&t)) log_die_nomem("strbuf") ;
    t.len-- ;
    if (!strbuf_copy(list,&t)) log_die_nomem("strbuf") ;

}

static void display_list(strbuf *list, uint8_t level)
{
    if (level == 1) log_info(list->s) ;
    else if (level == 2) log_warn(list->s) ;
    else if (level == 3) log_1_warn(list->s) ;
    else if (level == 4) log_trace(list->s) ;
    else if (level == 5) log_1_trace(list->s) ;
    else if (level == 6) log_die(LOG_EXIT_SYS,list->s) ;
    else if (level == 7) log_fatal(list->s) ;
}

int main(int argc, char const *const *argv)
{
    uint8_t level = 0, newline = 1, read_stdin = 0 ;
    int iverbo = -1 ;
    unsigned int iclock = 1, idble = 0, itimestamp, icolor = 0 ;
    char const *prog = 0, *verbo = 0, *redir1 = 0, *redir2 = 0, *clock = 0, *dble = 0, *timestamp = 0, *color = 0 ;
    char proc[4096] ;

    _cleanup_strbuf_ strbuf list = STRBUF_ZERO ;
    _cleanup_strbuf_ strbuf saproc = STRBUF_ZERO ;

    log_color = &log_color_disable ;

    /** by default log_out() write on stderr
     * switch it to sdtout */
    set_switch_stream(1) ;

    auto_strings(proc,"/proc/") ;

    PROG = "66-yeller" ;
    {
        opt_scan_t st = OPT_SCAN_ZERO ;

        for (;;)
        {
            int o = opt_scan(argc, argv, opts, OPT_COUNT(opts), &st) ;
            if (o == OPT_END) break ;
            switch (o)
            {
                case OPT_ID_HELP :  return opt_emit_help(cmd.name, &cmd) ;
                case 'v' :  { uint32_t v ; if (!u32_scan_strict(st.arg, &v)) return opt_emit_usage(cmd.name, &cmd) ; iverbo = (int)v ; } break ;
                case 'd' :  idble = 1 ; break ;
                case 's' :  set_switch_stream(0) ; break ;
                case 'S' :  read_stdin = 1 ; break ;
                case '1' :  redir1 = st.arg ; break ;
                case '2' :  redir2 = st.arg ; break ;
                case 'z' :  icolor = 1 ; break ;
                case 'n' :  newline = 0 ; set_trailing_newline(0) ; break ;
                case 'i' :  set_default_msg(0) ; break ;
                case 'p' :  prog = st.arg ; break ;
                case 'c' :  iclock = 0 ; break ;
                case 'w' :  if (level) return opt_emit_usage(cmd.name, &cmd) ; level = 2 ; break ;
                case 'W' :  if (level) return opt_emit_usage(cmd.name, &cmd) ; level = 3 ; break ;
                case 't' :  if (level) return opt_emit_usage(cmd.name, &cmd) ; level = 4 ; break ;
                case 'T' :  if (level) return opt_emit_usage(cmd.name, &cmd) ; level = 5 ; break ;
                case 'f' :  if (level) return opt_emit_usage(cmd.name, &cmd) ; level = 6 ; break ;
                case 'F' :  if (level) return opt_emit_usage(cmd.name, &cmd) ; level = 7 ; break ;
                default :   return opt_emit_error(cmd.name, &cmd, o, &st) ;
            }
        }
        argc -= st.ind ; argv += st.ind ;
    }
    if (!argc && !read_stdin) return opt_emit_usage(cmd.name, &cmd) ;

    if (!color)
    {
        color = getenv("COLOR_ENABLED") ;
        if (color)
            if (!u32_scan_strict(color,&icolor))
                log_die(LOG_EXIT_SYS,"invalid format of COLOR_ENABLED environment variable") ;
    }
    if (icolor) log_color = !isatty(1) ? &log_color_disable : &log_color_enable ;

    if (!idble)
    {
        dble = getenv("DOUBLE_OUTPUT") ;
        if (dble)
            if (!u32_scan_strict(dble,&idble))
                log_die(LOG_EXIT_SYS,"invalid format of DOUBLE_OUTPUT environment variable") ;
    }
    set_double_output(idble) ;

    if (iclock)
    {
        clock = getenv("CLOCK_ENABLED") ;
        if (clock)
            if (!u32_scan_strict(clock,&iclock))
                log_die(LOG_EXIT_SYS,"invalid format of CLOCK_ENABLED environment variable") ;
    }
    set_clock_enable(iclock) ;

    timestamp = getenv("CLOCK_TIMESTAMP") ;
    if (timestamp) {
        if (!u32_scan_strict(timestamp,&itimestamp))
                log_die(LOG_EXIT_SYS,"invalid format of CLOCK_TIMESTAMP environment variable") ;
        set_clock_timestamp(itimestamp) ;
    }

    if (!level) level = 1 ;

    char fmt[PID_FMT] ;
    fmt[pid_format(fmt, getppid())] = 0 ;

    auto_string_from(proc,6,fmt,"/comm") ;

    read_line(&saproc,proc) ;

    if (!prog){
        PROG = getenv("PROG") ;
        if (!PROG) PROG = saproc.s ;
    }
    else PROG = prog ;

    if (iverbo == -1)
    {
        verbo = getenv("VERBOSITY") ;
        if (verbo){
            if (!u32_scan_strict(verbo, &VERBOSITY))
                log_die(LOG_EXIT_SYS,"invalid format of VERBOSITY environment variable") ;
            if (VERBOSITY >= 3) VERBOSITY = 3 ;
        }
    }
    else VERBOSITY=iverbo ;

    if (!redir1) {
        redir1 = getenv("REDIRFD_1") ;
        if (redir1) set_redirfd_1(redir1) ;
    }
    else set_redirfd_1(redir1) ;

    if (!redir2) {
        redir2 = getenv("REDIRFD_2") ;
        if (redir2) set_redirfd_2(redir2) ;
    }
    else set_redirfd_2(redir2) ;

    if (read_stdin)
    {
        _cleanup_strbuf_ strbuf tmp = STRBUF_ZERO ;
        if (argc) {
            build_msg(&tmp,argc,argv) ;
            if (!auto_strbuf(&tmp," ")) log_die_nomem("strbuf") ;
            rebuild_without_escape(&tmp) ;
        }

        char buf[2] = {0} ;
        ssize_t r = 1 ;
        while(r > 0)
        {
            r = io_read(0,buf,1) ;
            if (r <= 0) break ;
            if (buf[0] !='\n')
            {
                if (!auto_strbuf(&list,buf)) log_die_nomem("strbuf") ;
            }
            else
            {
                if (tmp.len){
                    if (!strbuf_insert(&list,0,&tmp)) log_die_nomem("strbuf") ;
                    if (!newline)
                        if (!auto_strbuf(&list," ")) log_die_nomem("strbuf") ;
                    if (!strbuf_terminate(&list)) log_die_nomem("strbuf") ;
                }
                display_list(&list,level) ;
                list.len = 0 ;
            }
        }
    }
    else
    {
        build_msg(&list,argc,argv) ;
        if (!newline)
            if (!strbuf_cats(&list," ")) log_die_nomem("strbuf") ;

        if (!strbuf_terminate(&list)) log_die_nomem("strbuf") ;
        rebuild_without_escape(&list) ;
        display_list(&list,level) ;
    }

    return 0 ;
}
