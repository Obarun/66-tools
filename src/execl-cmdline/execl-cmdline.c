/*
 * execl-cmdline.c
 *
 * Copyright (c) 2018-2024 Eric Vidal <eric@obarun.org>
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

#include <oblibs/log.h>
#include <oblibs/sastr.h>
#include <oblibs/string.h>
#include <oblibs/environ.h>

#include <skalibs/stralloc.h>
#include <skalibs/env.h>
#include <skalibs/djbunix.h>
#include <skalibs/sgetopt.h>
#include <skalibs/buffer.h>
#include <skalibs/exec.h>

#include <execline/execline.h>

#define USAGE "execl-cmdline [ -s ] { command... }"

static inline void info_help (void)
{
  static char const *help =
"execl-cmdline <options> { command... }\n"
"\n"
"options :\n"
"   -h: print this help\n"
"   -s: split command\n"
;

 if (buffer_putsflush(buffer_1, help) < 0)
    log_dieusys(LOG_EXIT_SYS, "write to stdout") ;
}

void clean_string(stralloc *modifs, stralloc *tmodifs)
{
    stralloc tmp = STRALLOC_ZERO ;
    size_t pos = 0 ;
    for (;pos < tmodifs->len; pos += strlen(tmodifs->s + pos)+1)
    {
        tmp.len = 0 ;
        if (!sastr_clean_string(&tmp,tmodifs->s+pos) && (strlen(tmodifs->s + pos)))
            log_dieu(LOG_EXIT_SYS,"clean element of: ",tmodifs->s) ;
        if (!sastr_rebuild_in_oneline(&tmp)) log_dieu(LOG_EXIT_SYS,"rebuild line: ",tmp.s) ;
        if (!stralloc_0(&tmp)) log_die_nomem("stralloc") ;
        if (tmp.len > 1)
            if (!sastr_add_string(modifs,tmp.s)) log_dieu(LOG_EXIT_SYS,"rebuild final line: ",tmp.s) ;
    }
    stralloc_free(&tmp) ;
}

int clean_val_doublequoted(stralloc *sa,char const *line)
{
    size_t slen = strlen(line) , f = 0, prev = 0 , tl = 0 , pos = 0 , i = 0 ;
    char t[slen+1] ;

    stralloc tmp = STRALLOC_ZERO ;

    for (; i < slen ; i++)
    {
        if (line[i] == '"')
        {
            if (f)
            {
                tl = i-1 ;
                memcpy(t,line+prev,tl-prev+1) ;
                t[tl-prev+1] = 0 ;
                if (!sastr_add_string(sa,t)) return 0 ;
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
                    if (!sastr_clean_string(&tmp,t)) return 0 ;
                    for (pos = 0 ;pos < tmp.len; pos += strlen(tmp.s + pos)+1)
                        if (!sastr_add_string(sa,tmp.s+pos)) return 0 ;
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
            if (!sastr_clean_string(&tmp,t)) return 0 ;
            for (pos = 0 ;pos < tmp.len; pos += strlen(tmp.s + pos)+1)
                if (!sastr_add_string(sa,tmp.s+pos)) return 0 ;
            break ;
        }
    }
    if (f) log_die(LOG_EXIT_SYS,"odd number of double quote in: ",line) ;
    stralloc_free(&tmp) ;
    return 1 ;
}

int main(int argc, char const **argv, char const *const *envp)
{
    int r, argc1, split ;
    size_t pos = 0 ;

    stralloc tmodifs = STRALLOC_ZERO ;
    stralloc modifs = STRALLOC_ZERO ;
    stralloc tmp = STRALLOC_ZERO ;

    PROG = "execl-cmdline" ;

    r =  argc1 = split = 0 ;

    {
        subgetopt l = SUBGETOPT_ZERO ;

        for (;;)
        {
          int opt = subgetopt_r(argc, argv, "hs", &l) ;
          if (opt == -1) break ;
          switch (opt)
          {
            case 'h' :  info_help() ; return 0 ;
            case 's' :  split = 1 ; break ;
            default :   log_usage(USAGE) ;
          }
        }
        argc -= l.ind ; argv += l.ind ;
    }
    if (!argc) log_usage(USAGE) ;
    argc1 = el_semicolon(argv) ;
    if (argc1 >= argc) log_die(100, "unterminated block") ;
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
                stralloc_cats(&tmodifs,line) ;
                stralloc_0(&tmodifs) ;
                if (!sastr_clean_string(&tmodifs, tmodifs.s))
                    log_dieu(LOG_EXIT_SYS,"split element of: ",tmodifs.s) ;
                for (tpos = 0 ; tpos < tmodifs.len ; tpos += strlen(tmodifs.s + tpos) + 1)
                    if (!sastr_add_string(&tmp,tmodifs.s+tpos)) log_dieu(LOG_EXIT_SYS,"add line: ", tmodifs.s) ;
            }
        }
        stralloc_copy(&modifs,&tmp) ;
    }
    stralloc_free(&tmp) ;
    stralloc_free(&tmodifs) ;

	/* TODO
	 * stralloc modifs cannot be freed before exec
	 * due of the env_make behavior.
	 * The env_make() should be remade for our needs */
    r = sastr_len(&modifs) ;
    char const *newarg[r + 1] ;
    if (!env_make(newarg, r, modifs.s, modifs.len)) log_dieusys(LOG_EXIT_SYS, "env_make") ;
    newarg[r] = 0 ;

    xexec_ae(newarg[0],newarg,envp) ;
}
