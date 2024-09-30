/*
 * util.c
 *
 * Copyright (c) 2024 Eric Vidal <eric@obarun.org>
 *
 * All rights reserved.
 *
 * This file is part of Obarun. It is subject to the license terms in
 * the LICENSE file found in the top-level directory of this
 * distribution.
 * This file may not be copied, modified, propagated, or distributed
 * except according to the terms contained in the LICENSE file./
 */

#include <unistd.h>
#include <errno.h>
#include <spawn.h>
#include <signal.h>
#include <stddef.h>

#include "launcher.h"
#include "service.h"

#include <oblibs/log.h>

#include <skalibs/selfpipe.h>
#include <skalibs/djbunix.h>

extern char **environ;

int fdmove(int to, int from) {

	log_flow() ;

	int r ;
	if (to == from) return 0 ;
	do r = dup2(from, to) ;
	while ((r == -1) && (errno == EINTR)) ;
	if (r < 0) return r ;
	close(from) ;
	return 0 ;
}

/**
 * make a proper environment
 *
*/

pid_t async_spawn(char **cmd)
{
	log_flow() ;
	pid_t p ;
	if ((errno = posix_spawnp(&p, cmd[0], NULL, NULL, cmd, environ)))
		return 0 ;
	return p ;
}

int spawn_wait(pid_t p)
{
	log_flow() ;

	int r, wstat ;

	do r = waitpid(p, &wstat, 0) ;
	while (r < 0 && errno == EINTR) ;

	if (r < 0)
		return DBS_EXIT_FATAL ;

	if (WIFEXITED(wstat) && WEXITSTATUS(wstat) == 0)
		return 0 ;
	else
		return WIFSIGNALED(wstat) ? 128 + WTERMSIG(wstat) : WEXITSTATUS(wstat) ;

}

int sync_spawn(char **cmd)
{
	log_flow() ;

	pid_t p = async_spawn(cmd) ;
	if (p == 0)
		return DBS_EXIT_FATAL ;

	return spawn_wait(p) ;
}

static int compute_exit(int wstat)
{
	log_flow() ;

    if (WIFEXITED(wstat) && WEXITSTATUS(wstat) == 0)
        return DBS_EXIT_MAIN ;

    return WIFSIGNALED(wstat) ? 128 + WTERMSIG(wstat) : WEXITSTATUS(wstat) ;
}

int handle_signal(launcher_t *launcher, pid_t ppid)
{
	int ok = DBS_EXIT_MAIN, wstat ;
	pid_t cpid ;

    for (;;) {
		int r = selfpipe_read() ;

		switch (r) {

            case -1 : log_dieusys(LOG_EXIT_ZERO, "selfpipe_read") ;
            case 0 : return DBS_EXIT_MAIN ;
			case SIGHUP:
				log_info("caught SIGHUP signal, reloading services and configuration") ;
				service_load(launcher) ;
				return DBS_EXIT_CHILD ;
			case SIGTERM:
            case SIGINT:
            case SIGQUIT:
				return DBS_EXIT_MAIN ;
            case SIGCHLD:
				/** We can have multiple pid as long as we spawn
				 * a process to start a service. */
				for (;;) {

					cpid = wait_nohang(&wstat) ;

					if (cpid < 0) {
						if (errno == ECHILD) break ;
						else log_warnusys_return(DBS_EXIT_FATAL,"wait for children") ;
					} else if (!cpid) return DBS_EXIT_CHILD ;

					/** launcher */
					if (cpid == ppid)
						return compute_exit(wstat) ;
				}
                break ;
			default : log_warn("unexpected data in selfpipe") ; return DBS_EXIT_WARN ;
		}
    }

    return ok ;
}
