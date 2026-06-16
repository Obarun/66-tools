/*
 * 66-dbus-broker-launch.c
 *
 * Copyright (c) 2018 Eric Vidal <eric@obarun.org>
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
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "service.h"
#include "launcher.h"
#include "util.h"
#include "dbus.h"

#include <oblibs/log.h>
#include <oblibs/io.h>
#include <oblibs/fd.h>
#include <oblibs/opt.h>
#include <oblibs/types.h>

static opt_t const opts[] = {
    { .id = OPT_ID_HELP, .shortname = 'h', .help = "print this help" },
    { .id = 'z', .shortname = 'z', .help = "use color" },
    { .id = 'v', .shortname = 'v', .arg = OPT_REQUIRED, .argname = "verbosity", .help = "increase/decrease verbosity" },
    { .id = 'd', .shortname = 'd', .arg = OPT_REQUIRED, .argname = "notif", .help = "notify readiness on file descriptor notif" },
} ;

static opt_cmd_t const cmd = {
    .name = "66-dbus-launch",
    .opts = opts,
    .nopts = OPT_COUNT(opts),
} ;

// Ensure @fd is open, opening /dev/null (write if @w, else read) when it is not.
static int dbs_fd_ensure_open(int fd, int w)
{
	int dummy ;
	if (fcntl(fd, F_GETFD, &dummy) == -1) {
		if (errno != EBADF)
			return 0 ;
		int newfd = io_open("/dev/null", w ? O_WRONLY : O_RDONLY) ;
		if (newfd == -1)
			return 0 ;
		if (move_fd(fd, newfd) == -1) {
			close_fd(newfd) ;
			return 0 ;
		}
	}
	return 1 ;
}

static int notifier_isvalid(const char *str)
{
	uint32_t u ;

	if (!u32_scan_strict(str, &u))
		log_die(LOG_EXIT_USER, "invalid notification file descriptor: ", str) ;

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
		opt_scan_t st = OPT_SCAN_ZERO ;
		for (;;) {
			int o = opt_scan(argc, argv, opts, OPT_COUNT(opts), &st) ;
			if (o == OPT_END)
				break ;
			switch (o) {
				case OPT_ID_HELP:
					return opt_emit_help(cmd.name, &cmd) ;
				case 'z':
					log_color = !istty ? &log_color_disable : &log_color_enable ;
					break ;
				case 'v':
					if (!u32_scan_strict(st.arg, &VERBOSITY))
						return opt_emit_usage(cmd.name, &cmd) ;
					break ;
				case 'd':
					notif = notifier_isvalid(st.arg) ;
					break ;
				default:
					return opt_emit_error(cmd.name, &cmd, o, &st) ;
			}
		}
		argc -= st.ind ; argv += st.ind ;
	}

	if (!ensure_stdfds())
		log_dieusys(LOG_EXIT_SYS, "sanitize standards I/O") ;

	/** bind and listen dbus socket */
	int socket = dbs_socket_bind() ;

	if (dbs_setenv_dbus_address() < 0)
		log_dieusys(LOG_EXIT_SYS, "set ", !getuid() ? "DBUS_SYSTEM_BUS_ADDRESS" : "DBUS_SESSION_BUS_ADDRESS") ;

	if (!dbs_fd_ensure_open(notif, notif))
		log_dieusys(LOG_EXIT_SYS, "reverse fd for notification") ;

	/** populate launcher struct ; this also sets up the event loop and signal
	 * trapping (before the broker is forked). */
	r = launcher_new(&launcher, &hservice, socket) ;
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

	/** tear down all services from tree dbus */
	service_discard_tree() ;

	return 0 ;
}
