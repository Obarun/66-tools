/*
 * execl-cmdline.c
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

#include <stdlib.h>
#include <string.h>

#include <oblibs/log.h>
#include <oblibs/sbl.h>
#include <oblibs/string.h>
#include <oblibs/environ.h>
#include <oblibs/strbuf.h>
#include <oblibs/opt.h>
#include <oblibs/exec.h>
#include <oblibs/types.h>

/*
 * Native reimplementation of execline's el_semicolon()/el_getstrict(), the only
 * two execline functions this tool relied on.
 */

static unsigned int ecl_getstrict (void)
{
    static uint32_t strict = 0 ;
    static int first = 1 ;
    if (first) {
        char const *x = getenv("EXECLINE_STRICT") ;
        first = 0 ;
        if (x)
            u32_scan_strict(x, &strict) ;
    }

    return strict ;
}

static int ecl_semicolon (char const **argv)
{
    static unsigned int nblock = 0 ;
    unsigned int strict = ecl_getstrict() ;
    nblock++ ;
    unsigned int i = 0 ;

    for (; argv[i] ; i++) {

        if (!argv[i][0]) {
            return i ;
        } else if (argv[i][0] == ' ') {
            argv[i]++ ;
        } else if (strict) {

            if (strict >= 2) {
                flog_die(LOG_EXIT_USER, "unquoted argument %s at block %u position %u", argv[i], nblock, i) ;
            } else {
                flog_1_warn("unquoted argument %s at block %u position %u", argv[i], nblock, i) ;
            }
        }
    }

    return i + 1 ;
}

static opt_t const opts[] = {
    { .id = OPT_ID_HELP, .shortname = 'h', .longname = "help",  .help = "print this help" },
    { .id = 's',         .shortname = 's', .longname = "split", .help = "split command" },
} ;

static opt_cmd_t const cmd = {
    .name = "execl-cmdline",
    .operands = "{ command... }",
    .opts = opts,
    .nopts = OPT_COUNT(opts),
} ;

void clean_string(strbuf *modifs, strbuf *tmodifs)
{
    _cleanup_strbuf_ strbuf tmp = STRBUF_ZERO ;
    size_t pos = 0 ;
    for (;pos < tmodifs->len; pos += strlen(tmodifs->s + pos)+1)
    {
        tmp.len = 0 ;
        if (!sbl_clean_string(&tmp,tmodifs->s+pos) && (strlen(tmodifs->s + pos)))
            log_dieu(LOG_EXIT_SYS,"clean element of: ",tmodifs->s) ;
        if (!sbl_rebuild_oneline(&tmp)) log_dieu(LOG_EXIT_SYS,"rebuild line: ",tmp.s) ;
        if (tmp.len)
            if (!sbl_add(modifs,tmp.s)) log_dieu(LOG_EXIT_SYS,"rebuild final line: ",tmp.s) ;
    }
}

int clean_val_doublequoted(strbuf *sa,char const *line)
{
    size_t slen = strlen(line) , f = 0, prev = 0 , tl = 0 , pos = 0 , i = 0 ;
    char t[slen+1] ;

    _cleanup_strbuf_ strbuf tmp = STRBUF_ZERO ;

    for (; i < slen ; i++)
    {
        if (line[i] == '"')
        {
            if (f)
            {
                tl = i-1 ;
                memcpy(t,line+prev,tl-prev+1) ;
                t[tl-prev+1] = 0 ;
                if (!sbl_add(sa,t)) return 0 ;
                f = 0 ; prev = i+1 ;
            }
            else
            {
                if (i > 0)
                {
                    tmp.len = 0 ;
                    tl = i ;
                    if (prev == tl){ f++ ; continue ; }
                    memcpy(t,line+prev,tl-prev) ;
                    t[tl-prev] = 0 ;
                    if (!sbl_clean_string(&tmp,t)) return 0 ;
                    for (pos = 0 ;pos < tmp.len; pos += strlen(tmp.s + pos)+1)
                        if (!sbl_add(sa,tmp.s+pos)) return 0 ;
                    f++ ; prev = i+1 ;
                }
                else f++ ;
            }
        }
        else
        if (i+1 == slen)
        {
            tmp.len = 0 ;
            tl = i - 1 ;
            memcpy(t,line+prev,slen-prev) ;
            t[slen-prev] = 0 ;
            if (!sbl_clean_string(&tmp,t)) return 0 ;
            for (pos = 0 ;pos < tmp.len; pos += strlen(tmp.s + pos)+1)
                if (!sbl_add(sa,tmp.s+pos)) return 0 ;
            break ;
        }
    }
    if (f) log_die(LOG_EXIT_SYS,"odd number of double quote in: ",line) ;
    return 1 ;
}

int main(int argc, char const **argv, char const *const *envp)
{
    int r, argc1, split ;
    size_t pos = 0 ;

    strbuf tmodifs = STRBUF_ZERO ;
    strbuf modifs = STRBUF_ZERO ;
    strbuf tmp = STRBUF_ZERO ;

    PROG = "execl-cmdline" ;

    r =  argc1 = split = 0 ;

    {
        opt_scan_t st = OPT_SCAN_ZERO ;

        for (;;)
        {
          int o = opt_scan(argc, argv, opts, OPT_COUNT(opts), &st) ;
          if (o == OPT_END) break ;
          switch (o)
          {
            case OPT_ID_HELP :  return opt_emit_help(cmd.name, &cmd) ;
            case 's' :  split = 1 ; break ;
            default :   return opt_emit_error(cmd.name, &cmd, o, &st) ;
          }
        }
        argc -= st.ind ; argv += st.ind ;
    }
    if (!argc) return opt_emit_usage(cmd.name, &cmd) ;
    argc1 = ecl_semicolon(argv) ;
    if (argc1 >= argc) log_die(LOG_EXIT_USER, "unterminated block") ;
    argv[argc1] = 0 ;

    if (!environ_import_arguments(&tmodifs, argv, argc1))
        log_dieu(LOG_EXIT_SYS, "import arguments to environment") ;

    clean_string(&modifs,&tmodifs) ;

    if (split)
    {
        size_t tpos ;
        tmp.len = 0 ;
        for (pos = 0 ;pos < modifs.len; pos += strlen(modifs.s + pos) + 1)
        {
            char *line = modifs.s + pos ;
            r = get_len_until(line,'"') ;
            if (r >= 0)
            {
                if (!clean_val_doublequoted(&tmp,line))
                    log_dieu(LOG_EXIT_SYS,"parse quote of: ",line) ;
            }
            else
            {
                tmodifs.len = 0 ;
                strbuf_cats(&tmodifs,line) ;
                strbuf_terminate(&tmodifs) ;
                if (!sbl_clean_string(&tmodifs, tmodifs.s))
                    log_dieu(LOG_EXIT_SYS,"split element of: ",tmodifs.s) ;
                for (tpos = 0 ; tpos < tmodifs.len ; tpos += strlen(tmodifs.s + tpos) + 1)
                    if (!sbl_add(&tmp,tmodifs.s+tpos)) log_dieu(LOG_EXIT_SYS,"add line: ", tmodifs.s) ;
            }
        }
        strbuf_copy(&modifs,&tmp) ;
    }
    strbuf_free(&tmp) ;
    strbuf_free(&tmodifs) ;

	/* TODO
	 * strbuf modifs cannot be freed before exec
	 * due of the environ_make behavior.
	 * The environ_make() should be remade for our needs */
    r = sbl_count(&modifs) ;
    char const *newarg[r + 1] ;
    if (!environ_make(newarg, r, modifs.s, modifs.len)) log_dieusys(LOG_EXIT_SYS, "environ_make") ;
    newarg[r] = 0 ;

    exec_path_die(newarg[0],newarg,envp) ;
}
