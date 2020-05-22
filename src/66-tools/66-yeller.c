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
#include <sys/types.h> //ssize_t
#include <errno.h>
#include <fcntl.h> //open
#include <unistd.h> //close, read

#include <oblibs/log.h>
#include <oblibs/sastr.h>
#include <oblibs/string.h>

#include <skalibs/buffer.h>
#include <skalibs/sgetopt.h>
#include <skalibs/stralloc.h>
#include <skalibs/env.h>//env_get2
#include <skalibs/types.h>
#include <skalibs/djbunix.h>
#include <skalibs/bytestr.h>//byte_chr

#define MAXBUF 4096

#define USAGE "66-yeller [ -h ] [ -d ] [ -s|S ] [ -1 file ] [ -2 file ] [ -z ] [ -n ] [ -c ] [ -p prog ] [ -v verbosity ] [ -i|w|W|t|T|f|F ] msg..."

static inline void info_help (void)
{
	set_clock_enable(0) ;
	static char const *help =
"66-yeller <options> msg...\n"
"\n"
"options:\n"
"	-h: prints this help\n"
"	-d: set double output\n"
"	-s: switch stdout and stderr\n"
"	-S: read from stdin\n"
"	-1: redirect stdout to file\n" 
"	-2: redirect stderr to file\n" 
"	-z: disable color\n"
"	-n: disable trailing new line\n"
"	-c: disable time display\n"
"	-p: specifies name of the program\n"
"	-v: increase/decrease verbosity level\n"
"	-i: do not print information before msg\n"
"	-w: prints a warning message\n"
"	-W: prints a warning message regarless the VERBOSITY level\n"
"	-t: prints a tracing message\n"
"	-T: prints a tracing message regarless the VERBOSITY level\n"
"	-f: prints a fatal message and die\n"
"	-F: prints a fatal message without dying\n"
"\n"
"msg color options: \n"
"	%w: set color to white\n"
"	%b: set color to blue\n"
"	%g: set color to green\n"
"	%y: set color to yellow\n"
"	%r: set color to red\n"
"	%bl: enable blinking\n"
"	%n: reset color to normal\n"
"\n"
;
	if (buffer_putsflush(buffer_1, help) < 0)
		log_dieusys(LOG_EXIT_SYS, "write to stdout") ;
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
		!stralloc_0(dst)) log_die_nomem("stralloc") ;
 	return n ;
}

static void build_msg(stralloc *list, int argc,char const *const *argv)
{
	int el = 0, first = 0 ;
	for ( ; el < argc ; el++)
	{
		if (!first)	{
			if (!auto_stra(list,argv[el]))
				log_die_nomem("stralloc") ;
		}
		else {
			if (!auto_stra(list," ",argv[el]))
				log_die_nomem("stralloc") ;
		}
		first++ ;
	}
}

static void rebuild_without_escape(stralloc *list)
{
	size_t pos = 0 ;
	stralloc t = STRALLOC_ZERO ;

	for (;pos < list->len;pos++)
	{
		char c = 0 ;
		if (list->s[pos] == '\\') {
			c = 7 + byte_chr("abtnvfr", 7, list->s[pos+1]) ;
			if (!stralloc_catb(&t,&c,1)) log_die_nomem("stralloc") ;
			if (((pos + 2) >= list->len) || ((pos + 1) >= list->len) ) break ;
			if (list->s[pos+2] == ' ') pos += 2 ;
			else pos++ ;
		}
		else if (list->s[pos] == '%') {
			if (list->s[pos+1] == 'w')
			{
				if (!stralloc_cats(&t,log_color->info)) log_die_nomem("stralloc") ;
			}
			else if (list->s[pos+1] == 'b')
			{
				if (!stralloc_cats(&t,log_color->blue)) log_die_nomem("stralloc") ;
			}
			else if (list->s[pos+1] == 'g')
			{
				if (!stralloc_cats(&t,log_color->valid)) log_die_nomem("stralloc") ;
			}
			else if (list->s[pos+1] == 'y')
			{
				if (!stralloc_cats(&t,log_color->warning)) log_die_nomem("stralloc") ;
			}
			else if (list->s[pos+1] == 'r')
			{
				if (!stralloc_cats(&t,log_color->error)) log_die_nomem("stralloc") ;
			}
			else if (list->s[pos+1] == 'l')
			{
				if (!stralloc_cats(&t,log_color->ablink)) log_die_nomem("stralloc") ;
			}
			else if (list->s[pos+1] == 'n')
			{
				if (!stralloc_cats(&t,log_color->off)) log_die_nomem("stralloc") ;
			}
			if ((pos + 1) >= list->len) break ;
			else pos++ ;
		}
		else {
			c = list->s[pos] ;
			if (!stralloc_catb(&t,&c,1)) log_die_nomem("stralloc") ;
		}
	}
	if (!stralloc_0(&t)) log_die_nomem("stralloc") ;
	t.len-- ;
	if (!stralloc_copy(list,&t)) log_die_nomem("stralloc") ;

	stralloc_free(&t) ;
}

static void display_list(stralloc *list, uint8_t level)
{
	if (level == 1)	log_info(list->s) ;
	else if (level == 2) log_warn(list->s) ;
	else if (level == 3) log_1_warn(list->s) ;
	else if (level == 4) log_trace(list->s) ; 
	else if (level == 5) log_1_trace(list->s) ; 
	else if (level == 6) log_die(LOG_EXIT_SYS,list->s) ; 
	else if (level == 7) log_fatal(list->s) ;
}

int main(int argc, char const *const *argv, char const *const *envp)
{
	uint8_t level = 0, newline = 1, read_stdin = 0 ;
	int iverbo = -1 ;
	unsigned int iclock = 1, idble = 0, itimestamp ;
	char const *prog = 0, *verbo = 0, *redir1 = 0, *redir2 = 0, *clock = 0, *dble = 0, *timestamp = 0 ;
	char proc[4096] ;
	
	stralloc list = STRALLOC_ZERO ;
	stralloc saproc = STRALLOC_ZERO ;
	
	log_color = !isatty(1) ? &log_color_disable : &log_color_enable ;
	
	/** by default log_out() write on stderr
	 * switch it to sdtout */
	set_switch_stream(1) ;
	
	auto_strings(proc,"/proc/") ;
	
	PROG = "66-yeller" ;
	{
		subgetopt_t l = SUBGETOPT_ZERO ;
		for (;;)
		{
			int opt = subgetopt_r(argc, argv, "hv:dsS1:2:znip:cwWtTfF", &l) ;
			if (opt == -1) break ;
			switch (opt)
			{
				case 'h' : 	info_help() ; return 0 ;
				case 'v' : 	if (!uint0_scan(l.arg,(uint32_t *)&iverbo)) 
								log_usage(USAGE) ;	
							break ;
				case 'd' : 	idble = 1 ; break ;
				case 's' : 	set_switch_stream(0) ; break ;
				case 'S' : 	read_stdin = 1 ; break ;
				case '1' :	redir1 = l.arg ; break ;
				case '2' :	redir2 = l.arg ; break ;
				case 'z' : 	log_color = &log_color_disable ; break ;
				case 'n' : 	newline = 0 ; set_trailing_newline(0) ;	break ;
				case 'i' : 	set_default_msg(0) ; break ;
				case 'p' : 	prog = l.arg ;	break ;
				case 'c' : 	iclock = 0 ; break ;
				case 'w' : 	if (level) log_usage(USAGE) ; level = 2 ; break ;
				case 'W' : 	if (level) log_usage(USAGE) ; level = 3 ; break ;
				case 't' : 	if (level) log_usage(USAGE) ; level = 4 ; break ;
				case 'T' : 	if (level) log_usage(USAGE) ; level = 5 ; break ;
				case 'f' : 	if (level) log_usage(USAGE) ; level = 6 ; break ;
				case 'F' : 	if (level) log_usage(USAGE) ; level = 7 ; break ;
				default :  	log_usage(USAGE) ;
			}
		}
		argc -= l.ind ; argv += l.ind ;
	}
	if (!argc && !read_stdin) log_usage(USAGE) ;
	
	if (!idble)
	{
		dble = env_get2(envp,"DOUBLE_OUTPUT") ;
		if (dble)
			if (!uint0_scan(dble,&idble)) 
				log_die(LOG_EXIT_SYS,"invalid format of DOUBLE_OUTPUT environment variable") ;
	}
	set_double_output(idble) ;
	
	if (iclock)
	{
		clock = env_get2(envp,"CLOCK_ENABLED") ;
		if (clock)
			if (!uint0_scan(clock,&iclock))
				log_die(LOG_EXIT_SYS,"invalid format of CLOCK_ENABLED environment variable") ;
	}
	set_clock_enable(iclock) ;

	timestamp = env_get2(envp,"CLOCK_TIMESTAMP") ;
	if (timestamp) {
		if (!uint0_scan(timestamp,&itimestamp))
				log_die(LOG_EXIT_SYS,"invalid format of CLOCK_TIMESTAMP environment variable") ;
		set_clock_timestamp(itimestamp) ;
	}
	
	if (!level) level = 1 ;
	
	char fmt[UINT_FMT] ;
	fmt[uint_fmt(fmt, getppid())] = 0 ;
	
	auto_string_from(proc,6,fmt,"/comm") ;
		
	read_line(&saproc,proc) ;
	
	if (!prog){
		PROG = env_get2(envp,"PROG") ;
		if (!PROG) PROG = saproc.s ;
	}
	else PROG = prog ;
	
	if (iverbo == -1)
	{
		verbo = env_get2(envp,"VERBOSITY") ;
		if (verbo){
			if (!uint0_scan(verbo, &VERBOSITY))
				log_die(LOG_EXIT_SYS,"invalid format of VERBOSITY environment variable") ;
			if (VERBOSITY >= 3) VERBOSITY = 3 ;
		}
	}
	else VERBOSITY=iverbo ;
	
	if (!redir1) {
		redir1 = env_get2(envp,"REDIRFD_1") ;
		if (redir1) set_redirfd_1(redir1) ;
	}
	else set_redirfd_1(redir1) ;

	if (!redir2) {
		redir2 = env_get2(envp,"REDIRFD_2") ;
		if (redir2) set_redirfd_2(redir2) ;
	}
	else set_redirfd_2(redir2) ;

	if (read_stdin)
	{
		stralloc tmp = STRALLOC_ZERO ;
		if (argc) {
			build_msg(&tmp,argc,argv) ;
			if (!auto_stra(&tmp," ")) log_die_nomem("stralloc") ;
			rebuild_without_escape(&tmp) ;
		}
		
		char buf[2] ;
		ssize_t r = 1 ;
		while(r > 0)
		{
			r = read(0,buf,1) ;
			if (r == -1) {
				if (errno == EINTR) continue ;
				break ;
			}
			if (r <= 0) break ;
			if (buf[0] !='\n')
			{
				if (!auto_stra(&list,buf)) log_die_nomem("stralloc") ;
			}
			else
			{
				if (tmp.len){
					if (!stralloc_insert(&list,0,&tmp)) log_die_nomem("stralloc") ;
					if (!newline) 
						if (!auto_stra(&list," ")) log_die_nomem("stralloc") ;
					if (!stralloc_0(&list)) log_die_nomem("stralloc") ;
				}
				display_list(&list,level) ;
				list.len = 0 ;
			}
		}
		stralloc_free(&tmp) ;
	}
	else
	{
			
		build_msg(&list,argc,argv) ;
		if (!newline) 
			if (!stralloc_cats(&list," ")) log_die_nomem("stralloc") ;
		
		if (!stralloc_0(&list)) log_die_nomem("stralloc") ;
		rebuild_without_escape(&list) ;
		display_list(&list,level) ;
	}
	
	stralloc_free(&list) ;
	stralloc_free(&saproc) ;
	return 0 ;
}
