/* 
 * execl-envfile.c
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
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
//#include <stdio.h>

#include <oblibs/string.h>
#include <oblibs/stralist.h>
#include <oblibs/error2.h>
#include <oblibs/types.h>
#include <oblibs/directory.h>
#include <oblibs/files.h>
#include <oblibs/environ.h>
#include <oblibs/sastr.h>

#include <skalibs/bytestr.h>
#include <skalibs/stralloc.h>
#include <skalibs/env.h>
#include <skalibs/djbunix.h>
#include <skalibs/env.h>
#include <skalibs/sgetopt.h>

#include <execline/execline.h>

#define USAGE "execl-envfile [ -h help ] [ -l ] src prog"

static inline void info_help (void)
{
  static char const *help =
"execl-envfile <options> src prog\n"
"\n"
"options :\n"
"	-h: print this help\n" 
"	-f: file to parse(deprecated)\n"
"	-l: loose\n"
;

 if (buffer_putsflush(buffer_1, help) < 0)
    strerr_diefu1sys(111, "write to stdout") ;
}

void clean_n_unexport(stralloc *modifs, stralloc *dst, stralloc *src)
{
	if (!environ_clean_envfile(modifs,src)) strerr_diefu2sys(111,"prepare modified environment of: ",src->s) ;		
	if (!sastr_split_string_in_nline(modifs)) strerr_diefu2sys(111,"build environment line of: ",src->s) ; ;
	if (!stralloc_cats(dst,src->s)) exitstralloc("clean_n_unexport") ;
}

int main (int argc, char const *const *argv, char const *const *envp)
{
	int r = 0, unexport = 0, insist = 1, nfile = 0, nvar = 0 ;
	size_t pos = 0, pathlen = 0 ;
	char const *path = 0, *file = 0 ;
	char tpath[MAXENV + 1], tfile[MAXENV+1] ;
	stralloc src = STRALLOC_ZERO ;
	stralloc modifs = STRALLOC_ZERO ;
	stralloc dst = STRALLOC_ZERO ;
	stralloc toparse = STRALLOC_ZERO ; 
	stralloc key = STRALLOC_ZERO ; 
	stralloc val = STRALLOC_ZERO ; 
	
	exlsn_t info = EXLSN_ZERO ;
		
	PROG = "execl-envfile" ;
	{
		subgetopt_t l = SUBGETOPT_ZERO ;

		for (;;)
		{
			int opt = subgetopt_r(argc,argv, "hlf:", &l) ;
			if (opt == -1) break ;
			switch (opt)
			{
				case 'h' : 	info_help(); return 0 ;
				case 'f' :  file = l.arg ;  break ;
				case 'l' : 	insist = 0 ; break ;
				default : exitusage(USAGE) ; 
			}
		}
		argc -= l.ind ; argv += l.ind ;
	}
	if (argc < 2) exitusage(USAGE) ;
	
	path = *argv ;
	argv++;
	argc--;
	
	/** Mark -f as deprecated */	
	if (file)
	{
		strerr_warnw1x("-f options is deprecated") ;
		if (path[0] != '/') strerr_dief3x(111,"path of: ",path,": must be absolute") ;
	}
	else
	{
		r = scan_mode(path,S_IFREG) ;
		if (!r && insist) strerr_diefu2sys(111,"find: ",path) ;
		if (r < 0)
		{
			r = scan_mode(path,S_IFDIR) ;
			if (!r && insist) strerr_diefu2sys(111,"find: ",path) ;
			if (r < 0) strerr_dief2x(111,"invalid format of: ", path) ;
			if (path[0] == '.')
			{
				if (!dir_beabsolute(tpath,path)) 
					strerr_diefu2sys(111,"find absolute path of: ",path) ;
			}
			else if (path[0] != '/') strerr_dief3x(111,"path of: ",path,": must be absolute") ;
			else
			{
				pathlen = strlen(path) ;
				memcpy(tpath,path,pathlen);
				tpath[pathlen] = 0 ;
			}
			path = tpath ;
		}
		else
		{
			if (!basename(tfile,path)) strerr_diefu2x(111,"get file name of: ",path) ;
			file = tfile ;
			if (!dirname(tpath,path)) strerr_diefu2x(111,"get parent path of: ",path) ;
			path = tpath ;
		}
	}
	
	r = sastr_dir_get(&toparse,path,"",S_IFREG) ;
	if (!r && insist) strerr_diefu2sys(111,"get file from: ",path) ;
	else if ((!r && !insist) || !toparse.len)
	{
		xpathexec_run(argv[0],argv,envp) ;
	}
	if (file)
	{
		ssize_t	r = sastr_cmp(&toparse,file) ;
		if (r < 0) 
		{
			if (insist) strerr_diefu2x(111,"find: ",file) ;
			else
			{
				xpathexec_run(argv[0],argv,envp) ;
			}
		}
		if (!file_readputsa(&src,path,file)) strerr_diefu4sys(111,"read file: ",path,"/",file) ;
		clean_n_unexport(&modifs,&dst,&src) ;
	}
	else
	{
		for (;pos < toparse.len; pos += strlen(toparse.s + pos) + 1)
		{
			nfile++;
			src.len = 0 ;
			if (nfile > MAXFILE) strerr_dief2x(111,"to many file to parse in: ",path) ;
			if (!file_readputsa(&src,path,toparse.s+pos)) strerr_diefu4sys(111,"read file: ",path,"/",toparse.s+pos) ;
			clean_n_unexport(&modifs,&dst,&src) ;
			nvar = environ_get_num_of_line(&src) ;
			if (nvar == -1) strerr_diefu4sys(111,"get number of line of:",path,"/",toparse.s+pos) ;
			if (nvar > MAXVAR) strerr_dief4x(111,"to many variables in file: ",path,"/",toparse.s+pos) ;
		}
	}
	stralloc_free(&src) ;
	
	/** be able to freed the stralloc before existing */
	char tmp[modifs.len+1] ;
	memcpy(tmp,modifs.s,modifs.len) ;
	tmp[modifs.len] = 0 ;
	
	size_t n = env_len(envp) + 1 + byte_count(modifs.s,modifs.len,'\0') ;
	if (n > MAXENV) strerr_dief1x(111,"environment string too long") ;
	char const *newenv[n + 1] ;
	if (!env_merge (newenv, n ,envp,env_len(envp),tmp, modifs.len)) strerr_diefu1sys(111,"build environment") ;
	
	if (!sastr_split_string_in_nline(&dst)) strerr_diefu1x(111,"split line") ;
	pos = 0 ;
	while (pos < dst.len)
	{		
		unexport = 0 ;
		key.len = val.len = 0 ;
		if (!stralloc_copy(&key,&dst) ||
		!stralloc_copy(&val,&dst)) retstralloc(111,"main") ;
		
		if (!environ_get_key_nclean(&key,&pos)) strerr_diefu2x(111,"get key from line: ",key.s) ;
		pos-- ;// retrieve the '=' character
		if (!environ_get_val(&val,&pos)) strerr_diefu2x(111,"get value from line: ",val.s) ;

		char *uval = val.s ;
		if (val.s[0] == VAR_UNEXPORT)
		{
			uval = val.s+1 ;
			unexport = 1 ;
		}
		if(sastr_cmp(&info.vars,key.s) == -1)
			if (!environ_substitute(key.s,uval,&info,newenv,unexport)) 
				strerr_diefu4x(111,"substitute value of: ",key.s," by: ",uval) ;
	}
	stralloc_free(&key) ;
	stralloc_free(&val) ;
	stralloc_free(&dst) ;
	stralloc_free(&modifs) ;
	
	if (!env_string (&modifs, argv, (unsigned int) argc)) strerr_diefu1x(111,"make environment string") ;
	r = el_substitute (&dst, modifs.s, modifs.len, info.vars.s, info.values.s,
		genalloc_s (elsubst_t const, &info.data),genalloc_len (elsubst_t const, &info.data)) ;
	if (r < 0) strerr_diefu1sys(111,"el_substitute") ;
	else if (!r) _exit(0) ;

	stralloc_free(&modifs) ;

	char const *v[r + 1] ;
	if (!env_make (v, r ,dst.s, dst.len)) strerr_diefu1sys(111,"make environment") ;
	v[r] = 0 ;
	
	pathexec_r (v, newenv, env_len(newenv),info.modifs.s,info.modifs.len) ;
}
