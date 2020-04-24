/* 
 * 66-writenv.c
 * 
 * Copyright (c) 2018-2020 Eric Vidal <eric@obarun.org>
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
#include <sys/stat.h>
#include <unistd.h>//fsync,close
#include <errno.h>

#include <oblibs/log.h>

#include <skalibs/types.h>
#include <skalibs/bytestr.h>
#include <skalibs/buffer.h>
#include <skalibs/sgetopt.h>
#include <skalibs/djbunix.h>

#define MAX_ENV 4095
#define USAGE "66-writenv [ - h ] [ -m mode ] dir file"

static inline void info_help (void)
{
  static char const *help =
"66-writenv <options> dir file\n"
"\n"
"options :\n"
"	-h: print this help\n" 
"	-m: create dir with given mode\n"
;

 if (buffer_putsflush(buffer_1, help) < 0)
    log_dieusys(LOG_EXIT_SYS, "write to stdout") ;
}

int main (int argc, char const *const *argv, char const *const *envp)
{
	unsigned int mode = S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH ;
	size_t dirlen, filen ;
	buffer b ;
	int fd ;
	char const *dir = 0 , *file = 0 ;
	char buf[MAX_ENV+1] ;
	PROG = "66-writenv" ;
	{
		subgetopt_t l = SUBGETOPT_ZERO ;
		for (;;)
		{
			int opt = subgetopt_r(argc, argv, "hm:", &l) ;
			if (opt == -1) break ;
			switch (opt)
			{
				case 'h' : 	info_help() ; return 0 ;
				case 'm' :	if (!uint0_oscan(l.arg, &mode)) log_usage(USAGE) ; break ;
				default : 	log_usage(USAGE) ;
			}
		}
		argc -= l.ind ; argv += l.ind ;
	}
	if (argc < 2) log_usage(USAGE) ;
	dir = argv[0] ;
	if (dir[0] != '/') log_die(LOG_EXIT_USER,dir," must be an absolute path") ;
	file = argv[1] ;
	
	if (mkdir(dir, mode) < 0)
	{
		struct stat st ;
		if (errno != EEXIST) log_dieusys(LOG_EXIT_SYS, "mkdir ", dir) ;
		if (stat(dir, &st) < 0)
			log_dieusys(LOG_EXIT_SYS, "stat ", dir) ;
		if (!S_ISDIR(st.st_mode))
		{
			errno = ENOTDIR ;
			log_dieusys(LOG_EXIT_SYS, "mkdir ", dir) ;
		}
	}
	dirlen = strlen(dir) ;
	filen = strlen(file) ;
	char fn[dirlen + 1 + filen + 1] ;
	memcpy(fn,dir,dirlen) ;
	fn[dirlen] = '/' ;
	memcpy(fn + dirlen + 1, file ,filen) ;
	fn[dirlen + 1 + filen] = 0 ;
	fd = open_trunc(fn) ;
	if (fd < 0) log_dieusys(LOG_EXIT_SYS,"open trunc: ",fn) ;
 	buffer_init(&b,&buffer_write,fd,buf,MAX_ENV) ;
 	for (; *envp ; envp++)
	{
		if ((buffer_put(&b, *envp,strlen(*envp)) < 0) ||
		(buffer_put(&b, "\n",1) < 0)) { close(fd) ; log_dieusys(LOG_EXIT_SYS,"write buffer") ; }
	}
	if(!buffer_flush(&b)){ close(fd) ; log_dieusys(LOG_EXIT_SYS,"flush") ; }
	if (fsync(fd) < 0){ close(fd) ; log_dieusys(LOG_EXIT_SYS,"sync") ; }
	close(fd) ;
	return 0 ;
}
