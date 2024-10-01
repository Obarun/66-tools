/*
 * launcher.c
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

#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <grp.h>
#include <sys/prctl.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>

#include "launcher.h"
#include "dbus.h"
#include "service.h"
#include "util.h"
#include "policy.h"
#include "macro.h"

#include <oblibs/log.h>
#include <oblibs/string.h>
#include <oblibs/stack.h>
#include <oblibs/sastr.h>
#include <oblibs/files.h>
#include <oblibs/io.h>

#include <skalibs/iopause.h>
#include <skalibs/selfpipe.h>

#include <66-tools/config.h>

#include <66/constants.h>
#include <66/config.h>

launcher_t *launcher_free(launcher_t *launcher)
{
	log_flow() ;

	if (!launcher)
		return NULL ;
	close(launcher->fd_dbus) ;
    dbs_close_unref(launcher->bus_controller) ;
    dbs_close_unref(launcher->bus_regular) ;
	service_hash_free(launcher->hservice) ;
	close(launcher->fd_controller_in) ;
	close(launcher->fd_controller_out) ;
	free(launcher) ;
	return NULL ;
}

int launcher_new(launcher_t_ref *plauncher, struct service_s **hservice, int socket, int sfpd)
{
	log_flow() ;

	dbs_cleanup_(launcher_freep) launcher_t *launcher = NULL ;

	launcher = calloc(1, sizeof(*launcher)) ;
	if (!launcher)
		log_warn_return(DBS_EXIT_FATAL, "launcher") ;

	launcher->fd_dbus = socket ;
	launcher->spfd = sfpd ;
	launcher->fd_controller_in= -1 ;
	launcher->fd_controller_out= -1 ;
	launcher->uid = getuid() ;
	launcher->gid = getgid() ;
	launcher->nservice = 1 ;
	launcher_get_machine_id(launcher) ;

	launcher->hservice = hservice ;

	*plauncher = launcher ;
	launcher = NULL ;

	return 1 ;
}

int launcher_run(launcher_t *launcher)
{
	int r, controller[2] ;

	if (socketpair(PF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, controller) < 0)
		log_warnu_return(DBS_EXIT_FATAL, "socketpair") ;

	launcher->fd_controller_in = controller[0] ;
	launcher->fd_controller_out = controller[1] ;

	if (pipe(launcher->sync) < 0)
		log_warnu_return(DBS_EXIT_FATAL, "pipe") ;

	r = launcher_fork(launcher) ;
	if (r < 0)
		return r ;

	r = launcher_setup(launcher) ;
	if (r < 0)
		log_warnu_return(r, "setup launcher") ;

	return 1 ;
}

int launcher_fork(launcher_t *launcher)
{
	int r ;
	pid_t pid = fork();
	if (pid == -1)
		log_warnusys_return(DBS_EXIT_FATAL, "fork") ;

	if (pid == 0) {

		close(launcher->sync[0]) ;
		selfpipe_finish() ;
		close(launcher->fd_controller_in) ;

		launcher_run_broker(launcher) ;
	}

	close(launcher->fd_controller_out) ;

	launcher->bpid = pid ;

	{
		// synchronize with child
		close(launcher->sync[1]) ;
		int dummy ;
		do r = read(launcher->sync[0], &dummy, 1) ;
		while ((r < 0) && (errno == EINTR)) ;
		if (r < 0)
			log_warnu_return(DBS_EXIT_FATAL, "synchronize with child") ;
		close(launcher->sync[0]) ;
	}

	return 1 ;
}

int launcher_setup(launcher_t *launcher)
{
	log_flow() ;

	int r ;
	if (sd_bus_new(&launcher->bus_controller) < 0)
	 	log_warn_return(DBS_EXIT_FATAL, "sd_bus_new") ;

	if (sd_bus_set_fd(launcher->bus_controller, launcher->fd_controller_in, launcher->fd_controller_in) < 0)
		log_warnu_return(DBS_EXIT_FATAL, "set the file descriptors to use for bus communication") ;

	if (sd_bus_add_object_vtable(launcher->bus_controller, NULL, "/org/bus1/DBus/Controller", "org.bus1.DBus.Controller", launcher_vtable, launcher) < 0)
		log_warnusys_return(DBS_EXIT_FATAL, "sd_bus_add_object_vtable") ;

	if (sd_bus_start(launcher->bus_controller) < 0)
		log_warnusys_return(DBS_EXIT_FATAL, "sd_bus_start") ;

	if (sd_bus_add_filter(launcher->bus_controller, NULL, launcher_on_message, launcher) < 0)
		log_warnusys_return(DBS_EXIT_FATAL, "sd_bus_add_filter") ;

	if (!launcher_add_listener(launcher))
		log_warnsys("AddListener failed") ;

	if (launcher_connect(launcher) < 0)
	 	log_warnusys_return(DBS_EXIT_FATAL, "connect to dbus socket") ;

	service_sync_launcher_broker(launcher) ;

	r = launcher_drop_permissions(launcher) ;
	if (r < 0)
		log_warnusys_return(DBS_EXIT_FATAL, "drop permissions") ;

	return 1 ;
}

int launcher_connect(launcher_t *launcher)
{
	log_flow() ;

	if (launcher->uid) {
		if (sd_bus_open_user(&launcher->bus_regular) < 0)
			log_warnusys_return(DBS_EXIT_FATAL, "set user dbus address") ;
	} else {
		if (sd_bus_open_system(&launcher->bus_regular) < 0)
			log_warnusys_return(DBS_EXIT_FATAL, "set system dbus address") ;
	}

	return 1  ;
}

int launcher_run_broker(launcher_t *launcher)
{
	log_flow() ;

	int r, flags ;
	char fd[INT_FMT] ;
	fd[int_fmt(fd, launcher->fd_controller_out)] = 0 ;

	const char *const nargv[] = {
		"/usr/bin/dbus-broker",
		"--controller",
		fd,
		"--machine-id",
		launcher->machineid,
		// "--max-matches", "1000000",
        // "--max-objects", "1000000",
        // "--max-bytes", "1000000000",
		0
	} ;

	r = launcher_drop_permissions(launcher) ;
	if (r < 0){
		log_warnusys("drop permissions") ;
		goto exit ;
	}

	// die if parent process exit
	if (prctl(PR_SET_PDEATHSIG, SIGTERM)) {
		log_warnusys("prctl") ;
		goto exit ;
	}

	flags = fcntl(launcher->fd_controller_out, F_GETFD) ;
    if (flags < 0) {
		log_warnusys("get flags of fd_controller_out") ;
		goto exit ;
	}

	if (fcntl(launcher->fd_controller_out, F_SETFD, flags & ~FD_CLOEXEC) < 0){
		log_warnusys("remove FD_CLOEXEC flag on fd_controller_out") ;
		goto exit ;
	}

	{
		// synchronize with parent
		do r = write(launcher->sync[1], "\n", 1) ;
		while (r < 0 && (errno = EINTR)) ;
		if (r < 0) {
			log_warnusys("synchronize with parent") ;
			goto exit ;
		}
		close(launcher->sync[1]) ;
	}

	execve(nargv[0], (char *const *) nargv, (char *const *) environ) ;
	log_warnusys_return(DBS_EXIT_FATAL, "exec dbus-broker") ;

	exit:
		_exit(1) ;
}

int launcher_add_listener(launcher_t *launcher)
{
	log_flow() ;

	sd_bus_message *m = NULL ;

	if (sd_bus_message_new_method_call(launcher->bus_controller,
									   &m,
									   NULL,
									   "/org/bus1/DBus/Broker",
									   "org.bus1.DBus.Broker",
									   "AddListener") < 0)
		log_warnusys_return(DBS_EXIT_WARN, "call method org.bus1.DBus.Broker") ;

	if (sd_bus_message_append(m, "oh", "/org/bus1/DBus/Listener/0", launcher->fd_dbus) < 0)
		log_warnusys_return(DBS_EXIT_WARN, "append message") ;

	if (policy(m) < 0)
		log_warnusys_return(DBS_EXIT_WARN, "export policy") ;

	sd_bus_error error = SD_BUS_ERROR_NULL ;
	if (sd_bus_call(launcher->bus_controller, m, 0, &error, NULL) < 0)
		log_warnu_return(DBS_EXIT_WARN, "sd_bus_call failed: ", error.name," ", error.message) ;

	sd_bus_message_unref(m) ;

	return 1 ;
}

int launcher_loop(launcher_t *launcher)
{
	int r ;
	tain deadline = tain_infinite_relative ;

	iopause_fd x[2] = {
		{ .fd = launcher->spfd, .events = IOPAUSE_READ, .revents = 0 },
		{ .fd = launcher->fd_controller_in, .events = IOPAUSE_READ, .revents = 0 }
	} ;

	tain_now_set_stopwatch_g() ;
    tain_add_g(&deadline, &deadline) ;

	for (;;) {

		r = iopause_g(x, 2, &deadline) ;
        if (r < 0)
            log_warnusys_return(DBS_EXIT_FATAL, "iopause") ;
        if (!r)
			// never reached
            log_warnusys_return(DBS_EXIT_FATAL, "timeout") ;

		if (x[1].revents & IOPAUSE_READ) {

			do r = sd_bus_process(launcher->bus_controller, NULL) ;
			while (r < 0 && errno == EINTR) ;
			if (r < 0)
				log_warnusys_return(DBS_EXIT_FATAL, "process bus");
			if (r > 0) /* we processed a request, try to process another one, right-away */
				continue ;
		}

		if (x[0].revents & IOPAUSE_READ) {

			r = handle_signal(launcher, launcher->bpid) ;
			if (r == DBS_EXIT_MAIN)
				break ;
			if (r == DBS_EXIT_FATAL)
				return DBS_EXIT_FATAL ;
			continue ;
        }
	}

	return 1 ;
}

// https://github.com/bus1/dbus-broker/blob/main/src/launch/launcher.c#L491
int launcher_on_message(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
	log_flow() ;

	launcher_t *launcher = userdata ;

	const char *obj_path ;
	int suffix ;

	obj_path = sd_bus_message_get_path(m) ;

	if (!obj_path)
		return 0 ;

	suffix = str_start_with(obj_path, "/org/bus1/DBus/Name/") ;

	if (!suffix) {

		if (sd_bus_message_is_signal(m, "org.bus1.DBus.Name", "Activate")) {

			uint64_t serial;
			int r = sd_bus_message_read(m, "t", &serial);

			_alloc_stk_(stk, strlen(obj_path) + 1) ;

			if (!ob_basename(stk.s, obj_path))
				log_warnu_return(DBS_EXIT_WARN, "get basename of: ", obj_path) ;

			r = service_activate(launcher, atoi(stk.s)) ;

			if (r != 0) {
				r = sd_bus_call_method(launcher->bus_controller, NULL, obj_path, "org.bus1.DBus.Name", "Reset", NULL, NULL, "t", serial);
			}
		}

	} else if (!strcmp(obj_path, "/org/bus1/DBus/Broker")) {

		if (sd_bus_message_is_signal(m, "org.bus1.DBus.Broker", "SetActivationEnvironment"))
			launcher_update_environment(launcher, m) ;
	}

	return 0 ;
}

// https://github.com/bus1/dbus-broker/blob/main/src/launch/launcher.c#L459
void launcher_update_environment(launcher_t *launcher, sd_bus_message *m)
{
	log_flow() ;

	char home[SS_MAX_PATH_LEN + strlen(SS_ENVIRONMENT_USERDIR) + 9] ;
	_alloc_sa_(sa) ;

	memset(home, 0, sizeof(char) * SS_MAX_PATH_LEN + strlen(SS_ENVIRONMENT_USERDIR) + 9) ;

	int r = sd_bus_message_enter_container(m, 'a', "{ss}") ;
	if (r != 1) {
		log_warnusys("enter in container") ;
		goto exit ;
	}

	while (!sd_bus_message_at_end(m, false)) {

		const char *key, *value;

		r = sd_bus_message_read(m, "{ss}", &key, &value);
		if (r < 0) {
			log_warnusys("read environment key=value pair") ;
			goto exit ;
		}

		if (!auto_stra(&sa, key, "=", value, "\n")) {
			log_warnusys("stralloc") ;
			goto exit ;
		}
	}

	if (!service_environ_file_name(home, launcher))
		goto exit ;

	if (!file_write_unsafe_g(home, sa.s))
		log_warnusys("write file: ", home) ;

	exit:
		r = sd_bus_message_exit_container(m) ;
		if (r != 1)
			log_warnusys("exit from container") ;
}

void launcher_get_machine_id(launcher_t *launcher)
{
	log_flow() ;

	int fd = io_open("/etc/machine-id", O_RDONLY) ;
	if (fd < 0) {
		memcpy(launcher->machineid, "00000000000000000000000000000001", 32) ;
        goto exit ;
    }

    int r = io_read(launcher->machineid, fd, 32) ;
    if (r < 0)
        memcpy(launcher->machineid, "00000000000000000000000000000001", 32) ;

    exit:
	    r = 32 ;
	    close(fd) ;

        launcher->machineid[r + 1] = 0 ;
}

int launcher_drop_permissions(launcher_t *launcher)
{
	if (launcher->uid > 0) {
		/*
		* For compatibility to dbus-daemon, this must be
		* non-fatal.
		*/
		setgroups(0, NULL) ;

		if (setgid(launcher->gid) < 0)
			log_warnusys_return(DBS_EXIT_FATAL, "setgid") ;

		if (setuid(launcher->uid) < 0)
			log_warnusys_return(DBS_EXIT_FATAL, "setuid") ;
	}

	return 1 ;
}


