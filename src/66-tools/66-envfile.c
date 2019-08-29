/* 
 * 66-envfile.c
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

#include <skalibs/djbunix.h>
#include <skalibs/strerr2.h>

int main (int argc, char const *const *argv, char const *const *envp)
{
	PROG = "66-envfile" ;
	int i = 0 ;
	strerr_warnw1x("the 66-envfile is obsolescent, please use execl-envfile instead") ;
	argv++ ;
	char const *cmd[argc] ;
	cmd[0] = "execl-envfile" ;
	for(; i < argc ;i++)
		cmd[i+1] = argv[i] ;
	cmd[i+1] = 0 ;
	pathexec_run(cmd[0],cmd,envp) ;
}
