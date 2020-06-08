/* 
 * execl-envfile.c
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
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <oblibs/string.h>
#include <oblibs/log.h>
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

#define USAGE "execl-envfile [ -h ] [ -l ] src prog"

static inline void info_help (void)
{
  static char const *help =
"execl-envfile <options> src prog\n"
"\n"
"options :\n"
"	-h: print this help\n" 
"	-l: loose\n"
;

 if (buffer_putsflush(buffer_1, help) < 0)
    log_dieusys(LOG_EXIT_SYS, "write to stdout") ;
}

void clean_n_unexport(stralloc *modifs, stralloc *dst, stralloc *src)
{
	if (!environ_clean_envfile(modifs,src)) log_dieu(LOG_EXIT_SYS,"prepare modified environment of: ",src->s) ;
	if (!sastr_split_string_in_nline(modifs)) log_dieu(LOG_EXIT_SYS,"build environment line of: ",src->s) ;
	if (!stralloc_cats(dst,src->s)) log_die_nomem("stralloc") ;
}

static int cmpnsort(stralloc *sa)
{
	size_t pos = 0 ;
	if (!sa->len) return 0 ;
	size_t salen = sa->len ;
	size_t nel = sastr_len(sa), idx = 0, a = 0, b = 0 ;
	char names[nel][MAXENV + 1] ;
	char tmp[MAXENV + 1] ;

	for (; pos < salen && idx < nel ; pos += strlen(sa->s + pos) + 1,idx++)
	{
		memcpy(names[idx],sa->s + pos,strlen(sa->s + pos)) ;
		names[idx][strlen(sa->s+pos)] = 0 ;
	}
	for (; a < nel - 1 ; a++)
	{
		for (b = a + 1 ; b < idx ; b++)
		{
			if (strcmp(names[a],names[b]) > 0)
			{
				strcpy(tmp,names[a]) ;
				strcpy(names[a],names[b]);
				strcpy(names[b],tmp);
			}
		}
	}
	sa->len = 0 ;
	for (a = 0 ; a < nel ; a++)
	{
		if (!sastr_add_string(sa,names[a])) return 0 ;
	}
	return 1 ;
}

int main (int argc, char const *const *argv, char const *const *envp)
{
	int r = 0, unexport = 0, insist = 1, nvar = 0 ;
	size_t pos = 0 ;
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
			int opt = subgetopt_r(argc,argv, "hl", &l) ;
			if (opt == -1) break ;
			switch (opt)
			{
				case 'h' : 	info_help(); return 0 ;
				case 'l' : 	insist = 0 ; break ;
				default : 	log_usage(USAGE) ; 
			}
		}
		argc -= l.ind ; argv += l.ind ;
	}
	if (argc < 2) log_usage(USAGE) ;
	
	path = *argv ;
	argv++;
	argc--;
	
	r = scan_mode(path,S_IFREG) ;
	if (r > 0)
	{
		if (!ob_basename(tfile,path)) log_dieu(LOG_EXIT_SYS,"get file name of: ",path) ;
		file = tfile ;
		if (!ob_dirname(tpath,path)) log_dieu(LOG_EXIT_SYS,"get parent path of: ",path) ;
		path = tpath ;
	}
		
	if (path[0] == '.')
	{
		if (!dir_beabsolute(tpath,path) && insist) 
			log_dieusys(LOG_EXIT_SYS,"find absolute path of: ",path) ;
		path = tpath ;
	}
		
	r = sastr_dir_get(&toparse,path,"",S_IFREG) ;
	if (!r && insist) log_dieusys(LOG_EXIT_SYS,"get file from: ",path) ;
	else if ((!r && !insist) || !toparse.len)
	{
		xpathexec_run(argv[0],argv,envp) ;
	}
	if (file)
	{
		ssize_t	r = sastr_cmp(&toparse,file) ;
		if (r < 0) 
		{
			if (insist) log_dieu(LOG_EXIT_SYS,"find: ",path,file) ;
			else
			{
				xpathexec_run(argv[0],argv,envp) ;
			}
		}
		if (!file_readputsa(&src,path,file)) log_dieusys(LOG_EXIT_SYS,"read file: ",path,file) ;
		clean_n_unexport(&modifs,&dst,&src) ;
		nvar = environ_get_num_of_line(&src) ;
		if (nvar == -1) log_dieusys(LOG_EXIT_SYS,"get number of line of:",path,toparse.s+pos) ;
		if (nvar > MAXVAR) log_dieusys(LOG_EXIT_SYS,"to many variables in file: ",path,toparse.s+pos) ;
	}
	else
	{
		if (sastr_nelement(&toparse) > MAXFILE) log_die(LOG_EXIT_SYS,"to many file to parse in: ",path) ;
		if (!cmpnsort(&toparse)) log_dieu(LOG_EXIT_SYS,"sort environment file list from: ",path) ;
		for (;pos < toparse.len; pos += strlen(toparse.s + pos) + 1)
		{
			src.len = 0 ;
			if (!file_readputsa(&src,path,toparse.s+pos)) log_dieusys(LOG_EXIT_SYS,"read file: ",path,toparse.s+pos) ;
			clean_n_unexport(&modifs,&dst,&src) ;
			nvar = environ_get_num_of_line(&src) ;
			if (nvar == -1) log_dieusys(LOG_EXIT_SYS,"get number of line of:",path,toparse.s+pos) ;
			if (nvar > MAXVAR) log_die(LOG_EXIT_SYS,"to many variables in file: ",path,toparse.s+pos) ;
		}
	}
	stralloc_free(&src) ;
	
	/** be able to freed the stralloc before existing */
	char tmp[modifs.len+1] ;
	memcpy(tmp,modifs.s,modifs.len) ;
	tmp[modifs.len] = 0 ;
	
	size_t n = env_len(envp) + 1 + byte_count(modifs.s,modifs.len,'\0') ;
	if (n > MAXENV) log_die(LOG_EXIT_SYS,"environment string too long") ;
	char const *newenv[n + 1] ;
	if (!env_merge (newenv, n ,envp,env_len(envp),tmp, modifs.len)) log_dieusys(LOG_EXIT_SYS,"build environment") ;
	
	if (!sastr_split_string_in_nline(&dst)) log_dieusys(LOG_EXIT_SYS,"split line") ;
	pos = 0 ;
	
	while (pos < dst.len)
	{		
		unexport = 0 ;
		key.len = val.len = 0 ;
		if (!stralloc_copy(&key,&dst) ||
		!stralloc_copy(&val,&dst)) log_die_nomem("stralloc") ;
		
		if (!environ_get_key_nclean(&key,&pos)) log_dieusys(LOG_EXIT_SYS,"get key from line: ",key.s + pos) ;
		pos-- ;// retrieve the '=' character
		if (!environ_get_val(&val,&pos)) log_dieusys(LOG_EXIT_SYS,"get value from line: ",val.s + pos) ;
		
		char *uval = val.s ;
		if (val.s[0] == VAR_UNEXPORT)
		{
			uval = val.s+1 ;
			unexport = 1 ;
		}
		if(sastr_cmp(&info.vars,key.s) == -1)
			if (!environ_substitute(key.s,uval,&info,newenv,unexport)) 
				log_dieu(LOG_EXIT_SYS,"substitute value of: ",key.s," by: ",uval) ;
	}
	stralloc_free(&key) ;
	stralloc_free(&val) ;
	stralloc_free(&dst) ;
	stralloc_free(&modifs) ;
	
	if (!env_string (&modifs, argv, (unsigned int) argc)) log_dieu(LOG_EXIT_SYS,"make environment string") ;
	r = el_substitute (&dst, modifs.s, modifs.len, info.vars.s, info.values.s,
		genalloc_s (elsubst_t const, &info.data),genalloc_len (elsubst_t const, &info.data)) ;
	if (r < 0) log_dieusys(LOG_EXIT_SYS,"el_substitute") ;
	else if (!r) _exit(0) ;

	stralloc_free(&modifs) ;

	char const *v[r + 1] ;
	if (!env_make (v, r ,dst.s, dst.len)) log_dieusys(LOG_EXIT_SYS,"make environment") ;
	v[r] = 0 ;
	
	pathexec_r (v, newenv, env_len(newenv),info.modifs.s,info.modifs.len) ;
}
