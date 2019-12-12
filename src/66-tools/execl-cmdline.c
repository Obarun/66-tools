/* 
 * execl-cmdline.c
 * 
 * Copyright (c) 2018-2019 Eric Vidal <eric@obarun.org>
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

#include <skalibs/stralloc.h>
#include <skalibs/env.h>
#include <skalibs/djbunix.h>
#include <skalibs/sgetopt.h>
#include <skalibs/buffer.h>

#include <execline/execline.h>

#define USAGE "execl-cmdline [ -s ] { command... }"

static inline void info_help (void)
{
  static char const *help =
"execl-envfile <options> { command... }\n"
"\n"
"options :\n"
"	-h: print this help\n" 
"	-s: split command\n"
;

 if (buffer_putsflush(buffer_1, help) < 0)
    log_dieusys(LOG_EXIT_SYS, "write to stdout") ;
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
		subgetopt_t l = SUBGETOPT_ZERO ;
		for (;;)
		{
		  int opt = subgetopt_r(argc, argv, "hs", &l) ;
		  if (opt == -1) break ;
		  switch (opt)
		  {
			case 'h' : 	info_help() ; return 0 ;
			case 's' : 	split = 1 ; break ;
			default : 	log_usage(USAGE) ;
		  }
		}
		argc -= l.ind ; argv += l.ind ;
	}
	if (!argc) log_usage(USAGE) ;
	argc1 = el_semicolon(argv) ;
	if (argc1 >= argc) log_die(100, "unterminated block") ;
	argv[argc1] = 0 ;
	
	if (!env_string(&tmodifs,argv,argc1)) log_dieu(LOG_EXIT_SYS,"environment string") ;
		
	for (;pos < tmodifs.len; pos += strlen(tmodifs.s + pos)+1)
	{
		tmp.len = 0 ;
		if (!sastr_clean_string(&tmp,tmodifs.s+pos)) log_die(LOG_EXIT_SYS,"clean element of: ",tmp.s) ;
		if (!sastr_rebuild_in_oneline(&tmp)) log_die(LOG_EXIT_SYS,"rebuild line: ",tmp.s) ;
		if (!stralloc_0(&tmp)) log_die_nomem("stralloc") ;
		if (!stralloc_catb(&modifs,tmp.s,strlen(tmp.s) + 1)) log_die(LOG_EXIT_SYS,"rebuild final line: ",tmp.s) ;
	}
	stralloc_free(&tmp) ;
	stralloc_free(&tmodifs) ;
	if (split)
		if (!sastr_split_element_in_nline(&modifs)) log_die(LOG_EXIT_SYS,"split element of: ",modifs.s) ;
	
	r = sastr_len(&modifs) ;
	char const *newarg[r + 1] ;
    if (!env_make(newarg, r, modifs.s, modifs.len)) log_dieusys(LOG_EXIT_SYS, "env_make") ;
    newarg[r] = 0 ;
	
	xpathexec_run(newarg[0],newarg,envp) ;
}
