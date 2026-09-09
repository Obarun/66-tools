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
#include <time.h>

#include "launcher.h"
#include "service.h"

#include <oblibs/log.h>
#include <oblibs/spawn.h>
#include <oblibs/process.h>
#include <oblibs/environ.h>

#define BROKER_REAP_TRIES 1000

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

static int reap_broker(pid_t pid, int *wstat)
{
	struct timespec ms = { .tv_sec = 0, .tv_nsec = 1000000 } ;
	unsigned int i = 0 ;

	for (; i < BROKER_REAP_TRIES ; i++) {

		pid_t r = waitpid(pid, wstat, WNOHANG) ;

		if (r > 0)
			return 1 ;

		if (r < 0)
			return 0 ;

		nanosleep(&ms, NULL) ;
	}

	return 0 ;
}

void report_broker_death(launcher_t *launcher, int wstat)
{
	log_flow() ;

	/** @wstat is the only account of why the broker is gone: say it here or it
	 * is lost, and carry it out of the loop as the code the launcher will take.
	 * A zeroed bpid is what tells the rest of the loop the broker was reaped. */
	if (WIFSIGNALED(wstat))
		flog_warn("the dbus broker was killed by signal %d", WTERMSIG(wstat)) ;
	else if (WEXITSTATUS(wstat))
		flog_warn("the dbus broker exited with code %d", WEXITSTATUS(wstat)) ;
	else
		log_info("the dbus broker exited normally") ;

	launcher->loopret = compute_exit(wstat) ;
	launcher->bpid = 0 ;
}

void collect_broker_death(launcher_t *launcher)
{
	log_flow() ;

	int wstat ;

	/** A zeroed bpid means the signal watcher got there first and has already
	 * said and recorded everything. */
	if (!launcher->bpid)
		return ;

	if (reap_broker(launcher->bpid, &wstat)) {

		report_broker_death(launcher, wstat) ;

	} else {

		log_warn("the controller bus went down while the dbus broker is still alive") ;

		launcher->loopret = DBS_EXIT_FATAL ;
	}
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
				if (cpid == launcher->bpid) {

					report_broker_death(launcher, wstat) ;

					return compute_exit(wstat) ;
				}
			}
			break ;
		default : log_warn("unexpected signal") ; return DBS_EXIT_WARN ;
	}

	return DBS_EXIT_MAIN ;
}
