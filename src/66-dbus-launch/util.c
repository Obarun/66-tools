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
#include <signal.h>
#include <stddef.h>
#include <sys/wait.h>

#include "launcher.h"
#include "service.h"

#include <oblibs/log.h>
#include <oblibs/spawn.h>
#include <oblibs/process.h>

extern char **environ;

/**
 * make a proper environment
 *
*/

pid_t async_spawn(char **cmd)
{
	log_flow() ;
	return spawn_path(cmd[0], (char const *const *)cmd, (char const *const *)environ) ;
}

int spawn_wait(pid_t p)
{
	log_flow() ;

	int wstat ;

	if (process_wait(p, &wstat) < 0)
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

int handle_signal(launcher_t *launcher, int signo)
{
	int wstat ;
	pid_t cpid ;

	switch (signo) {

		case SIGHUP:
			log_info("caught SIGHUP signal, reloading services and configuration") ;
			service_reload(launcher) ;
			return DBS_EXIT_CHILD ;
		case SIGTERM:
		case SIGINT:
		case SIGQUIT:
			return DBS_EXIT_MAIN ;
		case SIGCHLD:
			/** We can have multiple pid as long as we spawn
			 * a process to start a service. */
			for (;;) {

				do cpid = waitpid(-1, &wstat, WNOHANG) ;
				while (cpid < 0 && errno == EINTR) ;

				if (cpid < 0) {
					if (errno == ECHILD) break ;
					else log_warnusys_return(DBS_EXIT_FATAL,"wait for children") ;
				} else if (!cpid) return DBS_EXIT_CHILD ;

				/** launcher */
				if (cpid == launcher->bpid)
					return compute_exit(wstat) ;
			}
			break ;
		default : log_warn("unexpected signal") ; return DBS_EXIT_WARN ;
	}

	return DBS_EXIT_MAIN ;
}
