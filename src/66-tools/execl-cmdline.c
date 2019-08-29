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

#include <oblibs/string.h>
#include <oblibs/stralist.h>
#include <oblibs/error2.h>

#include <skalibs/stralloc.h>
#include <skalibs/genalloc.h>
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
    strerr_diefu1sys(111, "write to stdout") ;
}

int clean_val_doublequoted(genalloc *ga,char const *line)
{
	size_t slen = strlen(line) ;
	size_t tl ;
	char s[slen+1] ;
	memcpy(s,line,slen) ;
	s[slen] = 0 ;
	int f = 0, r = 0 , prev = 0 ;
	r = get_len_until(s,'"') ;
	if (r < 0)
	{
		if (!clean_val(ga,s)) return 0 ;
		return 1 ;
	}
	for (int i = 0 ; i < slen ; i++)
	{
		if (s[i] == '"')
		{
			if (f)
			{
				char t[slen+1] ;
				tl = i-1 ;
				memcpy(t,s+prev,tl-prev+1) ;
				t[tl-prev+1] = 0 ;
				if (!stra_add(ga,t)) return 0 ;
				f = 0 ; prev = i+1 ;
			}
			else
			{
				if (i > 0)
				{
					char t[slen+1] ;
					tl = i ;
					if (prev == tl){ f++ ; continue ; }
					memcpy(t,s+prev,tl-prev) ;
					t[tl-prev] = 0 ;
					if (!clean_val(ga,t)) return 0 ;
					f++ ; prev = i+1 ;
				}
				else f++ ; 
			}
		}
		else
		if (i+1 == slen)
		{
			char t[slen+1] ;
			tl = i - 1 ;
			memcpy(t,s+prev,slen-prev) ;
			t[slen-prev] = 0 ;
			if (!clean_val(ga,t)) return 0 ;
			break ;
		}
	}
	if (f) strerr_dief2x(111,"odd number of double quote in: ",line) ;

	return 1 ;
}

int main(int argc, char const **argv, char const *const *envp)
{

	int r, argc1, split ;

	PROG = "execl-cmdline" ;
	
	stralloc tmodifs = STRALLOC_ZERO ;
	stralloc modifs = STRALLOC_ZERO ;
	genalloc ga = GENALLOC_ZERO ;
	
	r =  argc1 = split = 0 ;
	
	{
		subgetopt_t l = SUBGETOPT_ZERO ;
		for (;;)
		{
		  int opt = subgetopt_r(argc, argv, "hs", &l) ;
		  if (opt == -1) break ;
		  switch (opt)
		  {
			case 'h' : info_help() ; return 0 ;
			case 's' : split = 1 ; break ;
			default : exitusage(USAGE) ;
		  }
		}
		argc -= l.ind ; argv += l.ind ;
	}
	if (!argc) exitusage(USAGE) ;
	argc1 = el_semicolon(argv) ;
	if (argc1 >= argc) strerr_dief1x(100, "unterminated block") ;
	argv[argc1] = 0 ;
	
	char const **newargv = argv ;
	for (int i = 0; i < argc1 ; i++, newargv++)
	{	
		if (!*newargv[0])
			continue ;
		
		if (!stralloc_cats(&tmodifs,*newargv)) retstralloc(111,"tmodifs") ;
		if (split)
		{
			if (!stralloc_cats(&tmodifs," ")) retstralloc(111,"tmodifs") ;
		}
		else if (!stralloc_0(&tmodifs)) retstralloc(111,"tmodifs_0") ;
		r++;
	}
	
	if (split)
	{
		if (!clean_val_doublequoted(&ga,tmodifs.s)) strerr_diefu2x(111,"clean val: ",tmodifs.s) ;
		for (unsigned int i = 0 ; i < genalloc_len(stralist,&ga) ; i++)
		{
			stralloc_cats(&modifs,gaistr(&ga,i)) ;
			stralloc_0(&modifs) ;
		}
		r = genalloc_len(stralist,&ga) ;
		genalloc_deepfree(stralist,&ga,stra_free) ;
	}
	else if (!stralloc_copy(&modifs,&tmodifs)) retstralloc(111,"copy") ;
	
	stralloc_free(&tmodifs) ;
	
	char const *newarg[r + 1] ;
    if (!env_make(newarg, r, modifs.s, modifs.len)) strerr_diefu1sys(111, "env_make") ;
    newarg[r] = 0 ;

	xpathexec_run(newarg[0],newarg,envp) ;
}
