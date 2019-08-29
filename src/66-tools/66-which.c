/* 
 * 66-which.c
 * 
 * Copyright (c) 2019 Dyne.org Foundation, Amsterdam
 * Copyright (c) 2019 Eric Vidal <eric@obarun.org>
 * 
 * Written by:
 *  - Danilo Spinella <danyspin97@protonmail.com>
 *  - Eric Vidal <eric@obarun.org>
 * 
 * All rights reserved.
 * 
 * This file is part of Obarun. It is subject to the license terms in
 * the LICENSE file found in the top-level directory of this
 * distribution.
 * This file may not be copied, modified, propagated, or distributed
 * except according to the terms contained in the LICENSE file./
 *
 * */

#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <skalibs/genalloc.h>
#include <skalibs/sgetopt.h>
#include <skalibs/buffer.h>
#include <skalibs/bytestr.h>
#include <skalibs/strerr2.h>
#include <skalibs/config.h>
#include <oblibs/error2.h>
#include <oblibs/string.h>

#define USAGE "66-which [ -h ] [ -a | -q ] commands..."

static inline void info_help (void)
{
  static char const *help =
"66-which <options> commands...\n"
"\n"
"options :\n"
"	-h: print this help\n" 
"	-a: print all matching executable in PATH\n"
"	-q: quiet, do not print anything to stdout\n"
;
	if (buffer_putsflush(buffer_1, help) < 0)
		strerr_diefu1sys(111, "write to stdout") ;
}

int check_executable(char const* filepath) {
	struct stat sb ;
	return (stat(filepath, &sb) == 0 && sb.st_mode & S_IXUSR
			&& !S_ISDIR(sb.st_mode)) ? 1 : 0 ;
}
int parse_path(genalloc* folders, char* path) {
	char* rp = NULL ;
	size_t i, len, s ;
	int found ;
	stralloc filepath = STRALLOC_ZERO ;
	while (path) {
		s = str_chr(path, ':') ;
		if (!stralloc_copyb(&filepath, path, s)
			|| !stralloc_0(&filepath))
			strerr_diefu1sys(111, "append stralloc with PATH") ;
		rp = realpath(filepath.s, NULL);
		if (rp != NULL) {
			char const** ss = genalloc_s(char const*, folders);
			found = 0;
			len = genalloc_len(char const*, folders);
			for ( i = 0 ; i < len ; i++) {
				if (obstr_equal(ss[i], rp)) {
					found = 1 ;
					break ;
				}
			}
			if (!found) {
				if (!genalloc_append(char const*, folders, &rp))
					strerr_diefu1sys(111, "append genalloc") ;
			} else {
				free(rp);
			}
		}
		if (s == strlen(path)) break ;
		path += s + 1;
	}
	stralloc_free(&filepath) ;
	return genalloc_len(char const*, folders) ;
}

int handle_string(char const* name, char const* env_path, genalloc_ref paths,
				  int quiet, int printall) {
	size_t len = genalloc_len(char const*, paths) ;
	int found = 0 ;

	stralloc filepath = STRALLOC_ZERO ;
	char const** ss = genalloc_s(char const*, paths) ;

	for (size_t i = 0 ; i < len ; i++) {
		if (!stralloc_copys(&filepath, ss[i])
			|| !stralloc_cats(&filepath, "/")
			|| !stralloc_cats(&filepath, name)
			|| !stralloc_0(&filepath))
				retstralloc(111, "filepath");

		if (check_executable(filepath.s)) {
			if (!quiet && (buffer_puts(buffer_1small, filepath.s) < 0
				|| buffer_put(buffer_1small, "\n", 1) < 0
				|| !buffer_flush(buffer_1small)))
				strerr_diefu1sys(111, "write to stdout") ;
			found = 1;
			if (!printall) break ;
		}
	}

	if (found == 0 && !quiet)
		strerr_warnw5x("no ",name," in (",env_path,")") ;

	stralloc_free(&filepath);
	return found == 1 ? 0 : 111;
}

int handle_path(char const* path, int quiet) {
	char* rp = realpath(path, NULL) ;
	if (rp != NULL && check_executable(rp)) {
		if (!quiet && (buffer_puts(buffer_1small, rp) < 0
			|| buffer_put(buffer_1small, "\n", 1) < 0
			|| !buffer_flush(buffer_1small)))
			strerr_diefu1sys(111, "write to stdout") ;
		return 0 ;
	}

	size_t len = strlen(path) ;
	char base[len+1] ;
	char dir[len+1] ;
	if (!basename(base, path))
		strerr_diefu1sys(111, "get basename") ;
	if (!dirname(dir, path))
		strerr_diefu1sys(111, "get dirname") ;

	if (!quiet) strerr_warnw5x("no ",base," in (",dir,")") ;

	return 111 ;
}

int main (int argc, char const *const *argv)
{
	int printall = 0 ;
	int quiet = 0 ;
	char* path = 0 ;
	int ret = 0;
	genalloc paths = GENALLOC_ZERO ; // char const *
	PROG = "66-which" ;
	{
		subgetopt_t l = SUBGETOPT_ZERO ;
		for (;;)
		{
			int opt = subgetopt_r(argc, argv, "haq", &l) ;
			if (opt == -1) break ;
			switch (opt)
			{
				case 'h': info_help() ; return 0 ;
				case 'a': printall = 1 ; break ;
				case 'q': quiet = 1 ; break ;
				default : strerr_dieusage(110, USAGE) ;
			}
		}
		argc -= l.ind ; argv += l.ind ;
	}

	if (printall && quiet) strerr_dieusage(110, USAGE) ;

	path = getenv("PATH") ;
	if (!path) path = SKALIBS_DEFAULTPATH ;

	if (argc < 0) strerr_dieusage(110, USAGE) ;
	if (!parse_path(&paths, path))
		strerr_diefu1sys(111, "PATH is empty or contains non valid values") ;

	for ( ; *argv ; argv++) {
		if ((*argv)[0] == '/'
			|| (*argv)[0] == '~' || ((*argv)[0] == '~' && (*argv)[1] == '/')
			|| (*argv)[0] == '.' || ((*argv)[0] == '.' && (*argv)[1] == '/')
			|| ((*argv)[0] == '.' || ((*argv)[1] == '.' && (*argv)[2] == '/')))
			ret = handle_path(*argv, quiet);
		else
			ret = handle_string(*argv, path, &paths, quiet, printall);
	}

	genalloc_free(char const *, &paths) ;
	return ret ;
}
