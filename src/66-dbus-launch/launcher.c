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
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <grp.h>
#include <sys/prctl.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include "launcher.h"
#include "dbus.h"
#include "service.h"
#include "util.h"
#include "policy.h"
#include "macro.h"

#include <errno.h>

#include <oblibs/attributes.h>
#include <oblibs/exec.h>
#include <oblibs/fd.h>
#include <oblibs/log.h>
#include <oblibs/string.h>
#include <oblibs/strbuf.h>
#include <oblibs/files.h>
#include <oblibs/io.h>
#include <oblibs/types.h>

#include <66-tools/config.h>

#include <66/constants.h>
#include <66/environ.h>
#include <66/status.h>
#include <66/config.h>

static void on_signal(sse_watcher_t *w, void *data, int event) ;
static void on_bus(sse_watcher_t *w, void *data, int event) ;

launcher_t *launcher_free(launcher_t *launcher)
{
	log_flow() ;

	if (!launcher)
		return NULL ;
	sse_free(&launcher->p) ;
	close_fd(launcher->fd_dbus) ;
	odbus_free(launcher->bus) ;
	service_hash_free(launcher->hservice) ;
	close_fd(launcher->fd_controller_in) ;
	close_fd(launcher->fd_controller_out) ;
	free(launcher) ;
	return NULL ;
}

int launcher_new(launcher_t_ref *plauncher, hash_t *hservice, int socket)
{
	log_flow() ;

	dbs_cleanup_(launcher_freep) launcher_t *launcher = NULL ;

	launcher = calloc(1, sizeof(*launcher)) ;
	if (!launcher)
		log_warn_return(DBS_EXIT_FATAL, "launcher") ;

	/** the struct is calloc'd: make the event-loop fields safe to sse_free()
	 * even if we fail before sse_new() (otherwise fd 0 would be closed). */
	launcher->p.fd = -1 ;

	launcher->fd_dbus = socket ;
	launcher->fd_controller_in= -1 ;
	launcher->fd_controller_out= -1 ;
	launcher->uid = getuid() ;
	launcher->gid = getgid() ;
	launcher->loopret = DBS_EXIT_MAIN ;
	launcher->nservice = 1 ;
	launcher_get_machine_id(launcher) ;

	launcher->hservice = hservice ;

	/** init the service table before any failure path: launcher_free ->
	 * service_hash_free -> hash_free then releases the buckets on rollback. */
	if (!hash_init(hservice, 0, offsetof(struct service_s, node)))
		log_warnusys_return(DBS_EXIT_FATAL, "init service hash") ;

	/** event loop + signal trapping (replaces selfpipe). The signalfd is set up
	 * here, before the broker is forked, so no signal is missed. */
	if (!sse_new(&launcher->p, 2))
		log_warnusys_return(DBS_EXIT_FATAL, "create event loop") ;

	if (!sse_start_signal(&launcher->p, &launcher->wsignal, on_signal, launcher, 10))
		log_warnusys_return(DBS_EXIT_FATAL, "start signal watcher") ;

	if (!sse_attach_signal(&launcher->wsignal, SIGCHLD) ||
		!sse_attach_signal(&launcher->wsignal, SIGINT) ||
		!sse_attach_signal(&launcher->wsignal, SIGQUIT) ||
		!sse_attach_signal(&launcher->wsignal, SIGHUP) ||
		!sse_attach_signal(&launcher->wsignal, SIGTERM) ||
		!sse_ignore_signal(&launcher->wsignal, SIGPIPE))
			log_warnusys_return(DBS_EXIT_FATAL, "trap signals") ;

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
		/** drop the inherited event loop (epoll + signalfd) and restore a clean
		 * signal mask for the broker (the mask is inherited across execve). */
		sse_free(&launcher->p) ;
		{
			sigset_t set ;
			sigemptyset(&set) ;
			sigaddset(&set, SIGCHLD) ;
			sigaddset(&set, SIGINT) ;
			sigaddset(&set, SIGQUIT) ;
			sigaddset(&set, SIGHUP) ;
			sigaddset(&set, SIGTERM) ;
			sigaddset(&set, SIGPIPE) ;
			sigprocmask(SIG_UNBLOCK, &set, NULL) ;
		}
		close(launcher->fd_controller_in) ;

		launcher_run_broker(launcher) ;
	}

	close(launcher->fd_controller_out) ;

	launcher->bpid = pid ;

	{
		// synchronize with child
		close_fd(launcher->sync[1]) ;
		char dummy ;
		r = io_read(launcher->sync[0], &dummy, 1) ;
		if (r < 0)
			log_warnusys_return(DBS_EXIT_FATAL, "synchronize with child") ;

		if (!r)
			log_warnu_return(DBS_EXIT_FATAL, "start the broker -- the child went away before signalling") ;

		close_fd(launcher->sync[0]) ;
	}

	return 1 ;
}

int launcher_setup(launcher_t *launcher)
{
	log_flow() ;

	int r ;

	if (!odbus_open(&launcher->bus, launcher->fd_controller_in))
		log_warnusys_return(DBS_EXIT_FATAL, "open the controller bus") ;

	if (!odbus_set_object(launcher->bus, "/org/bus1/DBus/Controller", "org.bus1.DBus.Controller", launcher_methods, launcher))
		log_warnusys_return(DBS_EXIT_FATAL, "serve the controller object") ;

	if (!odbus_set_filter(launcher->bus, launcher_on_message, launcher))
		log_warnusys_return(DBS_EXIT_FATAL, "set the controller bus filter") ;

	if (!launcher_add_listener(launcher))
		log_warnsys("AddListener failed") ;

	service_sync_launcher_broker(launcher) ;

	r = launcher_drop_permissions(launcher) ;
	if (r < 0)
		log_warnusys_return(DBS_EXIT_FATAL, "drop permissions") ;

	return 1 ;
}

void launcher_run_broker(launcher_t *launcher)
{
	log_flow() ;

	int r ;
	char fd[I32_FMT] ;
	fd[i32_fmt(fd, launcher->fd_controller_out)] = 0 ;

	const char *const nargv[] = {
		"dbus-broker",
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

	if (!uncloexec_fd(launcher->fd_controller_out)) {
		log_warnusys("keep fd_controller_out across the exec") ;
		goto exit ;
	}

	{
		// synchronize with parent
		char sign = '\n' ;

		if (io_write(launcher->sync[1], &sign, 1) < 0) {
			log_warnusys("synchronize with parent") ;
			goto exit ;
		}
		close_fd(launcher->sync[1]) ;
	}

	exec_path_die(nargv[0], nargv, (char const *const *)environ) ;

	exit:
		_exit(1) ;
}

int launcher_add_listener(launcher_t *launcher)
{
	log_flow() ;

	odbus_message *m = NULL ;
	int r ;

	if (!odbus_message_new_method_call(launcher->bus,
									   &m,
									   "/org/bus1/DBus/Broker",
									   "org.bus1.DBus.Broker",
									   "AddListener"))
		log_warnusys_return(DBS_EXIT_WARN, "call method org.bus1.DBus.Broker") ;

	if (!odbus_message_append(m, "oh", "/org/bus1/DBus/Listener/0", launcher->fd_dbus)) {
		odbus_message_free(m) ;
		log_warnusys_return(DBS_EXIT_WARN, "append message") ;
	}

	if (!policy(m)) {
		odbus_message_free(m) ;
		log_warnusys_return(DBS_EXIT_WARN, "export policy") ;
	}

	r = odbus_call(launcher->bus, m, DBS_DBUS_CALL_TIMEOUT_MS) ;
	odbus_message_free(m) ;

	if (!r) {

		char const *name = odbus_error_name(launcher->bus) ;

		if (name)
			log_warnusys_return(DBS_EXIT_WARN, "AddListener, refused with: ", name) ;

		log_warnusys_return(DBS_EXIT_WARN, "AddListener") ;
	}

	return 1 ;
}

static void on_signal(sse_watcher_t *w, void *data, int event)
{
	(void)event ;
	launcher_t *launcher = data ;
	sse_signal_t *sig = (sse_signal_t *)w->sdata ;

	int r = handle_signal(launcher, sig->si.ssi_signo) ;

	if (r == DBS_EXIT_FATAL) {
		launcher->loopret = DBS_EXIT_FATAL ;
		w->p->running = false ;
	} else if (r == DBS_EXIT_MAIN) {
		w->p->running = false ;
	}
	/* DBS_EXIT_CHILD (reload, transient child reaped) or broker death by signal:
	 * keep the loop running, exactly like the former iopause loop. */
}

static void on_bus(sse_watcher_t *w, void *data, int event)
{
	launcher_t *launcher = data ;
	int r ;

	if (event & (SSE_ERROR | SSE_HUP)) {
		log_warn("the controller bus ", (event & SSE_HUP) ? "was closed by the broker" : "reported an error", " -- stopping") ;
		collect_broker_death(launcher) ;
		w->p->running = false ;
		return ;
	}

	/* drain the controller bus: process requests until none is left */
	do {
		do r = odbus_process(launcher->bus) ;
		while (r < 0 && errno == EINTR) ;
		if (r < 0) {
			log_warnusys("process bus") ;
			launcher->loopret = DBS_EXIT_FATAL ;
			w->p->running = false ;
			return ;
		}
	} while (r > 0) ;
}

int launcher_loop(launcher_t *launcher)
{
	launcher->loopret = DBS_EXIT_MAIN ;

	if (!sse_start_io(&launcher->p, &launcher->wbus, on_bus, launcher, launcher->fd_controller_in, SSE_READ, 0))
		log_warnusys_return(DBS_EXIT_FATAL, "start controller bus watcher") ;

	if (!sse_poll(&launcher->p, SSE_TIMEOUT_INFINITE))
		log_warnusys_return(DBS_EXIT_FATAL, "event loop") ;

	return launcher->loopret ;
}

// https://github.com/bus1/dbus-broker/blob/main/src/launch/launcher.c#L491
int launcher_on_message(odbus_message *m, void *userdata)
{
	log_flow() ;

	launcher_t *launcher = userdata ;

	const char *obj_path ;
	int suffix ;

	obj_path = odbus_message_get_path(m) ;

	if (!obj_path)
		return 0 ;

	suffix = str_start_with(obj_path, "/org/bus1/DBus/Name/") ;

	if (!suffix) {

		if (odbus_message_is_signal(m, "org.bus1.DBus.Name", "Activate")) {

			uint64_t serial = 0 ;
			int r ;

			if (odbus_message_read(m, "t", &serial) != 1)
				log_warnusys_return(DBS_EXIT_WARN, "read the serial of: ", obj_path) ;

			_alloc_strbuf_(stk, strlen(obj_path) + 1) ;

			if (!ob_basename(stk.s, obj_path))
				log_warnu_return(DBS_EXIT_WARN, "get basename of: ", obj_path) ;

			r = service_activate(launcher, atoi(stk.s)) ;

			if (r != 0)
				odbus_call_method(launcher->bus, obj_path, "org.bus1.DBus.Name", "Reset", DBS_DBUS_CALL_TIMEOUT_MS, "t", serial) ;

		}

	} else if (!strcmp(obj_path, "/org/bus1/DBus/Broker")) {

		if (odbus_message_is_signal(m, "org.bus1.DBus.Broker", "SetActivationEnvironment"))
			launcher_update_environment(launcher, m) ;
	}

	return 0 ;
}

int launcher_on_reload_config(odbus_message *message, void *userdata)
{
	log_flow() ;
    launcher_t *launcher = userdata ;
	log_info("config reload requested") ;
	service_reload(launcher) ;
	return odbus_reply_method_return(message, NULL) ;
}

// https://github.com/bus1/dbus-broker/blob/main/src/launch/launcher.c#L459
static void launcher_publish_environ(char const *dir, char const *scandir, char const *key, char const *value)
{
	int r ;

	if (!env_runtime_key_isvalid(key)) {
		log_warn("skip variable: ", key, " -- not a valid name for the runtime environment") ;
		return ;
	}

	/** D-Bus carries an empty value happily, and the only sensible reading of it
	 * is that the variable no longer applies: withdraw it rather than refuse it. */
	if (!*value) {

		r = env_runtime_withdraw(dir, key) ;

		if (r < 0) {
			log_warnusys("withdraw variable: ", key) ;
			return ;
		}

		if (!r)
			return ; // it was not published, nothing to do

		log_info("withdrew variable: ", key) ;

		env_runtime_emit(scandir, STATUS_WHO_SELF, SS_LIVEENV_EVENT_GONE, key) ;

		return ;
	}

	if (*value == SS_VAR_UNEXPORT) {
		log_warn("skip variable: ", key, " -- its value starts with an exclamation mark, and the runtime environment is published verbatim") ;
		return ;
	}

	if (!env_runtime_publish(dir, key, value)) {
		log_warnusys("publish variable: ", key) ;
		return ;
	}

	log_info("published variable: ", key) ;

	env_runtime_emit(scandir, STATUS_WHO_SELF, SS_LIVEENV_EVENT, key) ;
}

void launcher_update_environment(launcher_t *launcher, odbus_message *m)
{
	log_flow() ;

	char ownerstr[UID_FMT] ;
	int r ;
	char dir[sizeof(SS_LIVE) + SS_LIVEENV_LEN + 1 + UID_FMT] ;
	char scandir[sizeof(SS_LIVE) + SS_SCANDIR_LEN + 1 + UID_FMT] ;

	ownerstr[uid_format(ownerstr, launcher->uid)] = 0 ;
	auto_strings(dir, SS_LIVE, SS_LIVEENV, "/", ownerstr) ;
	auto_strings(scandir, SS_LIVE, SS_SCANDIR, "/", ownerstr) ;

	log_info("environment update requested") ;

	r = odbus_message_enter_container(m, 'a', "{ss}") ;
	if (r != 1) {
		log_warnusys("enter in container") ;
		return ;
	}

	if (scan_mode(dir, S_IFDIR) <= 0) {
		log_warn("no runtime environment directory: ", dir, " -- the activation environment is dropped") ;

	} else {

		while (!odbus_message_at_end(m)) {

			const char *key, *value ;

			if (odbus_message_read(m, "{ss}", &key, &value) < 0) {
				log_warnusys("read environment key=value pair") ;
				break ;
			}

			launcher_publish_environ(dir, scandir, key, value) ;
		}
	}

	if (odbus_message_exit_container(m) != 1)
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

    int r = io_read(fd, launcher->machineid, 32) ;
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


