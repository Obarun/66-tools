/* 
 * 66-gnwenv.c
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
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

#include <oblibs/string.h>

#include <skalibs/types.h>
#include <skalibs/buffer.h>
#include <skalibs/sgetopt.h>
#include <skalibs/strerr2.h>
#include <skalibs/env.h>
#include <skalibs/djbunix.h>

#define MAX_ENV 4095
static char const *pattern = 0 ;
static unsigned int EXACT = 0 ;

#define USAGE "66-gnwenv [ -h ] [ -x ] [ -m mode ] process dir file"
#define dieusage() strerr_dieusage(100, USAGE)

static inline void info_help (void)
{
  static char const *help =
"66-gnwenv <options> process dir file\n"
"\n"
"options :\n"
"	-h: print this help\n" 
"	-m: create dir with given mode\n"
"	-x: match exactly with the process name\n"
;

 if (buffer_putsflush(buffer_1, help) < 0)
    strerr_diefu1sys(111, "write to stdout") ;
}

static void string_env(char *tmp,char const *s,size_t len)
{
	int pos = 0 ;
	ssize_t r = 0 ;
	
	while ((pos < len) && (r != -1))
	{
		r = get_len_until(s+pos,'\n') ;
		memcpy(tmp+pos,s+pos,r) ;
		tmp[pos+r] = 0 ;
		pos += ++r ;/**+1 to skip the \n character*/
	}
}

int main (int argc, char const *const *argv, char const *const *envp)
{
	int r = 0 , pf, rm = 0, m = 0, fd[2], did = 0 ;
	ssize_t slen = 0 ; 
	
	unsigned int mode = S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH ;
	
	char const *dir = 0 , *file = 0 ;
	char const *newargv[6+1] ;
	char const *newread[6+1] ;
	char md[UINT32_FMT] ;
	char buf[MAX_ENV+1] ;
	char tmp[MAX_ENV+1] ;
	
	PROG = "66-gnwenv" ;
	{
		subgetopt_t l = SUBGETOPT_ZERO ;
		for (;;)
		{
			int opt = subgetopt_r(argc, argv, "hxm:", &l) ;
			if (opt == -1) break ;
			switch (opt)
			{
				case 'h' : info_help() ; return 0 ;
				case 'x' : EXACT = 1 ; break ;
				case 'm' : if (!uint0_oscan(l.arg, &mode)) dieusage() ; did = 1 ; break ;
				default : dieusage() ;
			}
		}
		argc -= l.ind ; argv += l.ind ;
	}
	if (argc < 3) dieusage() ;
	pattern = argv[0] ;
	dir = argv[1] ;
	if (dir[0] != '/') strerr_dief2x(111,dir," must be an absolute path") ;
	file = argv[2] ;
	
	newread[rm++] = "66-getenv" ;
	if (EXACT)
		newread[rm++] = "-x" ;
	newread[rm++] = pattern ;
	newread[rm++] = dir ;
	newread[rm++] = file ;
	newread[rm++] = 0 ;
	
	if (pipe(fd) < 0) strerr_diefu1sys(111,"pipe") ;
	pf = fork() ;
	if (pf < 0) strerr_diefu1sys(111,"fork") ;
	if (!pf)
	{
		dup2(fd[1],1) ;
		pathexec(newread) ;
	}
	else
	{	
		wait(NULL) ;
		slen = read(fd[0],buf,MAX_ENV) ;
		if (!slen) return 0 ;
		buf[slen] = 0 ;
	}
 	r = get_nbline(buf,slen) ;
 	
 	string_env(tmp,buf,slen) ;
	
	md[uint32_fmt(md,mode)] = 0 ;

	newargv[m++] = "66-writenv" ;
	if (did)
	{
		newargv[m++] = "-m" ;
		newargv[m++] = md ;
	}
	newargv[m++] = dir ;
	newargv[m++] = file ;
	newargv[m++] = 0 ;
		
	char const *v[r + 1] ;
	if (!env_make (v, r ,tmp, slen)) strerr_diefu1sys(111,"make environment") ;
	v[r] = 0 ;
	
	xpathexec_fromenv (newargv, v, r) ;
}



