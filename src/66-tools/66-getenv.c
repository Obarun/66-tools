/* 
 * 66-getenv.c
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
#include <unistd.h>//read
#include <stdlib.h>//malloc
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <regex.h>

#include <oblibs/sastr.h>

#include <skalibs/sgetopt.h>
#include <skalibs/stralloc.h>
#include <skalibs/buffer.h>
#include <skalibs/strerr2.h>
#include <skalibs/types.h>

#define MAXBUF 1024*64*2

static char const *delim = "\n" ;
static char const *pattern = 0 ;
static unsigned int EXACT = 0 ;

#define USAGE "66-getenv [ -h ] [ -x ] [ -d delim ] process"
#define dieusage() strerr_dieusage(100, USAGE)

static inline void info_help (void)
{
  static char const *help =
"66-getenv <options> process\n"
"\n"
"options :\n"
"	-h: print this help\n" 
"	-d: specify output delimiter\n"
"	-x: match exactly with the process name\n"
;

 if (buffer_putsflush(buffer_1, help) < 0)
    strerr_diefu1sys(111, "write to stdout") ;
}

static int read_line(stralloc *dst, char const *line) 
{
	char b[MAXBUF] ;
	int fd ;
	unsigned int n = 0, m = MAXBUF ;
	
	fd = open(line, O_RDONLY) ;
	if (fd == -1) return 0 ;
	
	for(;;)
	{
		ssize_t r = read(fd,b+n,m-n);
		if (r == -1)
		{
			if (errno == EINTR) continue ;
			break ;
		}
		n += r ;
		// buffer is full
		if (n == m)
		{
			--n ;
			break ;
		}
		// end of file
		if (r == 0) break ;
	}
	close(fd) ;
	
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
	
	if (!stralloc_cats(dst,b) ||
		!stralloc_0(dst)) strerr_diefu1sys(111,"append stralloc") ;
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
	if (!preg) strerr_diefu1sys(111,"allocate preg") ;
	if (EXACT)
	{
		memcpy(re,"^(",2) ;
		memcpy(re + 2,pattern,plen) ;
		memcpy(re + 2 + plen,")$",2) ;
		re[2 + plen + 2] = 0 ;
	}
	else
	{
		memcpy(re,pattern,plen) ;
		re[plen] = 0 ;
	}

	r = regcomp (preg, re, REG_EXTENDED | REG_NOSUB) ;
	if (r)
	{
		regerror (r, preg, errbuf, sizeof(errbuf)) ;
		strerr_diefu1x(111,errbuf) ;
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
	myself[pid_fmt(myself,getpid())] = 0 ;
	regex_t *preg ;
	preg = regex_cmp() ;
	stralloc satmp = STRALLOC_ZERO ;
	stralloc saproc = STRALLOC_ZERO ;
		
	if (!sastr_dir_get(&satmp,proc,"",S_IFDIR)) strerr_diefu1sys(111,"get content of /proc") ;
	
	i = 0, len = satmp.len ;
	for (;i < len; i += strlen(satmp.s + i) + 1)
	{
		char *name = satmp.s + i ;
		char c = name[0] ;
		// keep only pid directories
		if ( c >= '0' && c <= '9' ) 
			if (!stralloc_catb(&saproc,name,strlen(name) + 1)) strerr_diefu1sys(111,"append stralloc") ;
	}
	
	i = 0, len = saproc.len ;
	for (;i < len; i += strlen(saproc.s + i) + 1)
	{
		satmp.len = 0 ;
		int found = 1 ;
		char *name = saproc.s + i ;
		size_t namelen = strlen(name) ;
		if (!strcmp(name,myself)) continue ;
		char tmp[proclen + 1 + namelen + linelen + 1] ;
		memcpy(tmp,proc,proclen) ;
		tmp[proclen] = '/' ;
		memcpy(tmp + proclen + 1,name,namelen) ;
		memcpy(tmp + proclen + 1 + namelen,cmdline,linelen) ;
		tmp[proclen + 1 + namelen + linelen] = 0 ;
		if (!read_line(&satmp,tmp)) continue ;
		
		if (regexec (preg, satmp.s, 0, NULL, 0) != 0)
			found = 0 ;
		
		satmp.len = 0 ;
		memcpy(tmp + proclen + 1 + namelen,environ,linelen) ;
		tmp[proclen + 1 + namelen + linelen] = 0 ;
		if (!read_line(&satmp,tmp)) continue ;
		
		if (found) 
		{	
			size_t j = 0 ;
			for(;j < satmp.len; j++)
			{
				char ch[2] = { satmp.s[j], 0 } ;
				if (satmp.s[j] == ' ' || satmp.s[j] == '\0')
				{
					if (buffer_putsflush(buffer_1, delim) < 0) strerr_diefu1sys(111, "write to stdout") ;
				}
				else if (buffer_puts(buffer_1, ch) < 0) strerr_diefu1sys(111, "write to stdout") ;
			}
			break ;
		}
	}
	stralloc_free(&satmp) ;
	stralloc_free(&saproc) ;
	regfree(preg) ;
}

int main (int argc, char const *const *argv, char const *const *envp)
{
	PROG = "66-getenv" ;
	{
		subgetopt_t l = SUBGETOPT_ZERO ;
		for (;;)
		{
			int opt = subgetopt_r(argc, argv, "hxd:", &l) ;
			if (opt == -1) break ;
			switch (opt)
			{
				case 'h' : info_help() ; return 0 ;
				case 'x' : EXACT = 1 ; break ;
				case 'd' : delim = l.arg ; break ;
				default : dieusage() ;
			}
		}
		argc -= l.ind ; argv += l.ind ;
	}
	if (argc < 1) dieusage() ;
	pattern = *argv ;

 	get_procs() ;
	
	return 0 ;	
}
