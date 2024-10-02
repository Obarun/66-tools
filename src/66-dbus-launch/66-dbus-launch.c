/*
 * 66-dbus-broker-launch.c
 *
 * Copyright (c) 2018-2024 Eric Vidal <eric@obarun.org>
 *
 * All rights reserved.
 *
 * This file is part of Obarun. It is subject to the license terms in
 * the LICENSE file found in the top-level directory of this
 * distribution.
 * This file may not be copied, modified, propagated, or distributed
 * except according to the terms contained in the LICENSE file./
 */

#include <sys/types.h>
#include <sys/signal.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>

#include "service.h"
#include "launcher.h"
#include "util.h"
#include "dbus.h"

#include <oblibs/log.h>
#include <oblibs/io.h>

#include <skalibs/tai.h>
#include <skalibs/iopause.h>
#include <skalibs/djbunix.h>
#include <skalibs/sgetopt.h>
#include <skalibs/types.h>
#include <skalibs/selfpipe.h>
#include <skalibs/buffer.h>
#include <skalibs/sig.h>

#define USAGE "66-dbus-launch [ -h ] [ -z ] [ -v verbosity ] [ -d notif ]"

static inline void info_help (void)
{
    static char const *help =
        "66-dbus-launch <options> prog\n"
        "\n"
        "options:\n"
        "   -h: print this help\n"
        "   -z: use color\n"
        "   -v: increase/decrease verbosity\n"
		"   -d: notify readiness on file descriptor notif\n"
        "\n"
        ;

    if (buffer_putsflush(buffer_1, help) < 0)
        log_dieusys(LOG_EXIT_SYS, "write to stdout") ;
}

static int notifier_isvalid(const char *str)
{
	unsigned int u ;

	if (!uint0_scan(str, &u))
		log_usage(USAGE) ;

	if (u < 3)
		log_die(LOG_EXIT_USER, "file descriptor must be 3 or more") ;

	if (fcntl(u, F_GETFD) < 0)
		log_diesys(LOG_EXIT_USER, "invalid file descriptor") ;

	return u ;
}

int main(int argc, char const *const *argv)
{
	unsigned int notif = 0 ;
	int r, istty ;
	struct service_s *hservice = NULL ;
	dbs_cleanup_(launcher_freep) launcher_t *launcher = 0 ;

	log_color = &log_color_disable ;
	istty = isatty(1) ;

	set_clock_enable(1) ;

	PROG = "66-dbus-launch" ;
	{
		subgetopt l = SUBGETOPT_ZERO ;
		for (;;) {
			int opt = subgetopt_r(argc, argv, "hzv:d:", &l) ;
            if (opt == -1)
				break ;
            switch (opt) {
				case 'h':
                    info_help() ;
					return 0 ;
				case 'z':
                    log_color = !istty ? &log_color_disable : &log_color_enable ;
                    break ;
                case 'v':
                    if (!uint0_scan(l.arg, &VERBOSITY))
                        log_usage(USAGE) ;
                    break ;
				case 'd':
                    notif = notifier_isvalid(l.arg) ;
					break ;
				default:
                    log_usage(USAGE) ;
			}
		}
		argc -= l.ind ; argv += l.ind ;
	}

	if (!fd_sanitize())
		log_dieusys(LOG_EXIT_SYS, "sanitize standards I/O") ;

	/** bind and listen dbus socket */
	int socket = dbs_socket_bind() ;

	if (dbs_setenv_dbus_address() < 0)
		log_dieusys(LOG_EXIT_SYS, "set ", !getuid() ? "DBUS_SYSTEM_BUS_ADDRESS" : "DBUS_SESSION_BUS_ADDRESS") ;

	if (!fd_ensure_open(notif, notif))
		log_dieusys(LOG_EXIT_SYS, "reverse fd for notification") ;

	int spfd = selfpipe_init() ;
	if (spfd < 0)
        log_dieusys(LOG_EXIT_SYS, "selfpipe_init") ;

	if (!selfpipe_trap(SIGCHLD) ||
        !selfpipe_trap(SIGINT) ||
		!selfpipe_trap(SIGQUIT) ||
        !selfpipe_trap(SIGHUP) ||
        !selfpipe_trap(SIGTERM) ||
        !sig_altignore(SIGPIPE))
            log_dieusys(LOG_EXIT_SYS, "selfpipe_trap") ;

	/** populate launcher struct */
	r = launcher_new(&launcher, &hservice, socket, spfd) ;
	if (r < 0)
		log_dieu(LOG_EXIT_SYS, "make new launcher") ;

	r = service_load(launcher) ;
	if (r <= 0)
		log_dieu(LOG_EXIT_SYS, "collect service") ;

	r = launcher_run(launcher) ;
	if (r < 0)
		log_dieu(LOG_EXIT_SYS, "run launcher") ;

	// notify right before the loop
	if (notif) {
		write(notif, "\n", 1) ;
		close(notif) ;
	}

	r = launcher_loop(launcher) ;
	if (r < 0)
		log_dieu(LOG_EXIT_SYS, "loop launcher") ;

	selfpipe_finish() ;
	/** tear down all services from tree dbus */
	service_discard_tree() ;

	return 0 ;
}
