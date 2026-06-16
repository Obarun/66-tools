/*
 * 66-getenv.c
 *
 * Copyright (c) 2018 Eric Vidal <eric@obarun.org>
 *
 * All rights reserved.
 *
 * This file is part of Obarun. It is subject to the license terms in
 * the LICENSE file found in the top-level directory of this
 * distribution.
 * This file may not be copied, modified, propagated, or distributed
 * except according to the terms contained in the LICENSE file./
 */

#include <string.h>
#include <unistd.h>//getpid
#include <stdlib.h>//malloc
#include <fcntl.h>//O_RDONLY
#include <sys/stat.h>
#include <regex.h>

#include <oblibs/sbl.h>
#include <oblibs/log.h>
#include <oblibs/opt.h>
#include <oblibs/strbuf.h>
#include <oblibs/stream.h>
#include <oblibs/types.h>
#include <oblibs/io.h>
#include <oblibs/fd.h>
#include <oblibs/string.h>

#define MAXBUF 1024*64*2

static char const *delim = "\n" ;
static char const *pattern = 0 ;
static unsigned int EXACT = 0 ;

static opt_t const opts[] = {
    { .id = OPT_ID_HELP, .shortname = 'h', .longname = "help",  .arg = OPT_NONE,                         .help = "print this help" },
    { .id = 'x',         .shortname = 'x', .longname = "exact", .arg = OPT_NONE,                         .help = "match exactly with the process name" },
    { .id = 'd',         .shortname = 'd', .longname = "delim", .arg = OPT_REQUIRED, .argname = "delim", .help = "specify output delimiter" },
} ;

static opt_cmd_t const cmd = {
    .name = "66-getenv",
    .operands = "process",
    .opts = opts,
    .nopts = OPT_COUNT(opts),
} ;

static int read_line(strbuf *dst, char const *line, char subdelim)
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
            if (b[i] == '\n' || b[i] == '\0')
                 b[i] = subdelim ;

        if (b[n-1] == ' ') b[n-1] = '\0' ;
    }
    b[n] = '\0';

    if (!strbuf_cats(dst,b) ||
        !strbuf_terminate(dst)) log_die_nomem("strbuf") ;
    return n ;
}

static regex_t *regex_cmp (void)
{
    regex_t *preg = 0 ;
    size_t plen = strlen(pattern) ;
    char re[plen + 4 + 1] ;
    char errbuf[256] ;
    int r ;

    preg = malloc (sizeof (regex_t)) ;
    if (!preg) log_dieusys(LOG_EXIT_SYS,"allocate preg") ;
    if (EXACT)
        auto_strings(re, "^(", pattern, ")$") ;
    else
        auto_strings(re, pattern) ;

    r = regcomp (preg, re, REG_EXTENDED | REG_NOSUB) ;
    if (r)
    {
        regerror (r, preg, errbuf, sizeof(errbuf)) ;
        log_dieu(LOG_EXIT_SYS,errbuf) ;
    }

    return preg ;
}

void get_procs ()
{
    char *proc = "/proc" ;
    char *cmdline = "/cmdline" ;
    char *environ = "/environ" ;
    size_t proclen = 5, linelen = 8, i = 0, len ;
    char myself [PID_FMT] ;
    myself[pid_format(myself,getpid())] = 0 ;
    regex_t *preg ;
    preg = regex_cmp() ;
    _cleanup_strbuf_ strbuf satmp = STRBUF_ZERO ;
    _cleanup_strbuf_ strbuf saproc = STRBUF_ZERO ;
    char const *exclude[1] = { 0 } ;

    if (!sbl_dir_get(&satmp,proc,exclude,S_IFDIR)) log_dieusys(LOG_EXIT_SYS,"get content of /proc") ;

    i = 0, len = satmp.len ;
    for (;i < len; i += strlen(satmp.s + i) + 1)
    {
        char *name = satmp.s + i ;
        char c = name[0] ;
        // keep only pid directories
        if ( c >= '0' && c <= '9' )
            if (!strbuf_catb(&saproc,name,strlen(name) + 1)) log_dieusys(LOG_EXIT_SYS,"append strbuf") ;
    }

    i = 0, len = saproc.len ;
    for (;i < len; i += strlen(saproc.s + i) + 1)
    {
        satmp.len = 0 ;
        char subdelim = ' ' ;
        int found = 1 ;
        char *name = saproc.s + i ;
        size_t namelen = strlen(name) ;
        if (!strcmp(name,myself)) continue ;
        char tmp[proclen + 1 + namelen + linelen + 1] ;
        auto_strings(tmp, proc, "/", name, cmdline) ;
        if (!read_line(&satmp,tmp,subdelim)) continue ;

        if (regexec (preg, satmp.s, 0, NULL, 0) != 0)
            found = 0 ;

        satmp.len = 0 ;
        auto_strings(tmp, proc, "/", name, environ) ;
        subdelim = '\n' ;
        if (!read_line(&satmp,tmp,subdelim)) continue ;

        if (found)
        {
            /** ensure to have an empty end line */
            if (!strbuf_catb(&satmp,"\n",1))
                log_die_nomem("strbuf") ;

            size_t j = 0 ;
            for(;j < satmp.len; j++)
            {
                char ch[2] = { satmp.s[j], 0 } ;
                if (satmp.s[j] == '\n')
                {
                    if (!ostream_putflush(ostream_1, delim, strlen(delim))) log_dieusys(LOG_EXIT_SYS, "write to stdout") ;
                }
                else if (!ostream_puts(ostream_1, ch)) log_dieusys(LOG_EXIT_SYS, "write to stdout") ;
            }
            break ;
        }
    }

    regfree(preg) ;
    free(preg) ;
}

int main (int argc, char const *const *argv)
{
    PROG = "66-getenv" ;
    {
        opt_scan_t st = OPT_SCAN_ZERO ;

        for (;;)
        {
            int o = opt_scan(argc, argv, opts, OPT_COUNT(opts), &st) ;
            if (o == OPT_END) break ;
            switch (o)
            {
                case OPT_ID_HELP : return opt_emit_help(cmd.name, &cmd) ;
                case 'x' :  EXACT = 1 ; break ;
                case 'd' :  delim = st.arg ; break ;
                default :   return opt_emit_error(cmd.name, &cmd, o, &st) ;
            }
        }
        argc -= st.ind ; argv += st.ind ;
    }
    if (argc < 1) return opt_emit_usage(cmd.name, &cmd) ;
    pattern = *argv ;

    get_procs() ;

    return 0 ;
}
