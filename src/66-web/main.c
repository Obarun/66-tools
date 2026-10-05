/*
 * main.c
 *
 * Copyright (c) 2026 Eric Vidal <eric@obarun.org>
 *
 * All rights reserved.
 *
 * This file is part of Obarun. It is subject to the license terms in
 * the LICENSE file found in the top-level directory of this
 * distribution.
 * This file may not be copied, modified, propagated, or distributed
 * except according to the terms contained in the LICENSE file.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <oblibs/fd.h>
#include <oblibs/log.h>
#include <oblibs/opt.h>
#include <oblibs/socket.h>
#include <oblibs/sse.h>
#include <oblibs/strbuf.h>
#include <oblibs/string.h>
#include <oblibs/types.h>

#include <66/config.h>
#include <66/constants.h>
#include <66/utils.h>

#include "action.h"
#include "auth.h"
#include "ctx.h"
#include "events.h"
#include "http.h"

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 8066

#define AUTH_FILE_ENV "66_WEB_AUTH_FILE"
#define AUTH_FILE_ADMIN "/etc/66/web/auth"
#define AUTH_FILE_USER ".config/66-web/auth"

typedef struct web_opts_s web_opts_t ;
struct web_opts_s {
	char host[64] ;
	uint16_t port ;
	char const *auth_flag ; // --auth-file, 0 = environment or per-uid default
	uint8_t loopback ;
	uint8_t insecure ;
} ;

#define WEB_OPTS_ZERO { DEFAULT_HOST, DEFAULT_PORT, 0, 1, 0 }

static opt_t const opts_root[] = {
	{ .id = OPT_ID_HELP, .shortname = 'h', .longname = "help",      .arg = OPT_NONE,                          .help = "print this help" },
	{ .id = 'v',         .shortname = 'v', .longname = "verbosity", .arg = OPT_REQUIRED, .argname = "number", .help = "increase/decrease verbosity" },
} ;

static opt_t const opts_serve[] = {
	{ .id = OPT_ID_HELP, .shortname = 'h', .longname = "help",            .arg = OPT_NONE,                             .help = "print this help" },
	{ .id = 'l',         .shortname = 'l', .longname = "listen",          .arg = OPT_REQUIRED, .argname = "host:port", .help = "bind address, loopback only (default " DEFAULT_HOST ":8066)" },
	{ .id = 'L',         .shortname = 'L', .longname = "insecure-listen", .arg = OPT_REQUIRED, .argname = "host:port", .help = "bind a non-loopback address, traffic is unencrypted" },
	{ .id = 'a',         .shortname = 'a', .longname = "auth-file",       .arg = OPT_REQUIRED, .argname = "file",      .help = "credential file" },
} ;

static opt_t const opts_passwd[] = {
	{ .id = OPT_ID_HELP, .shortname = 'h', .longname = "help",      .arg = OPT_NONE,                        .help = "print this help" },
	{ .id = 'a',         .shortname = 'a', .longname = "auth-file", .arg = OPT_REQUIRED, .argname = "file", .help = "credential file" },
} ;

static char const passwd_epilog[] =
	"the credential file defaults to " AUTH_FILE_ADMIN " for root and to\n"
	"$HOME/" AUTH_FILE_USER " otherwise. The " AUTH_FILE_ENV " environment\n"
	"variable overrides the default, --auth-file overrides both.\n" ;

static char const serve_epilog[] =
	"--insecure-listen serves plain HTTP: put an HTTPS reverse proxy in front.\n"
	"\n"
	"the credential file defaults to " AUTH_FILE_ADMIN " for root and to\n"
	"$HOME/" AUTH_FILE_USER " otherwise. The " AUTH_FILE_ENV " environment\n"
	"variable overrides the default, --auth-file overrides both.\n" ;


static int auth_file_path(char const *flag, char *store, size_t cap)
{
	char const *env ;
	uid_t owner = geteuid() ;
	char home[SS_MAX_PATH_LEN + 1] ;

	if (!flag) {
		env = getenv(AUTH_FILE_ENV) ;
		if (env && *env)
			flag = env ;
	}

	if (flag) {

		size_t len = strlen(flag) ;

		if (len + 1 > cap)
			return (errno = ENAMETOOLONG, 0) ;

		memcpy(store, flag, len + 1) ;

		return 1 ;
	}

	if (!owner) {

		if (sizeof(AUTH_FILE_ADMIN) > cap)
			return (errno = ENAMETOOLONG, 0) ;

		memcpy(store, AUTH_FILE_ADMIN, sizeof(AUTH_FILE_ADMIN)) ;

		return 1 ;
	}

	if (!set_ownerhome_stack_byuid(home, owner))
		return 0 ;

	if (strlen(home) + sizeof(AUTH_FILE_USER) > cap)
		return (errno = ENAMETOOLONG, 0) ;

	auto_strings(store, home, AUTH_FILE_USER) ;

	return 1 ;
}

static int parse_listen(char const *arg, char *host, size_t hostlen, uint16_t *port)
{
	if (!arg || !host || !port)
		return (errno = EINVAL, 0) ;

	char const *colon = strrchr(arg, ':') ;
	if (!colon || colon == arg)
		return (errno = EINVAL, 0) ;

	size_t hlen = (size_t)(colon - arg) ;
	if (hlen + 1 > hostlen)
		return (errno = ENAMETOOLONG, 0) ;

	memcpy(host, arg, hlen) ;
	host[hlen] = 0 ;

	uint16_t p = 0 ;
	size_t sn = u16_scan(colon + 1, &p) ;

	if (!sn || colon[1 + sn] || p == 0) // reject trailing garbage after the port
		return (errno = EINVAL, 0) ;

	*port = p ;
	return 1 ;
}

static int host_is_ipv4(char const *host)
{
	struct in_addr addr ;

	return inet_pton(AF_INET, host, &addr) == 1 ;
}

static int open_listen_socket(char const *host, uint16_t port)
{
	int fd = socket_private(AF_INET, SOCK_STREAM, 0, O_NONBLOCK | O_CLOEXEC) ;
	if (fd < 0)
		return -1 ;

	int one = 1 ;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
		close_fd(fd) ;
		return -1 ;
	}

	struct sockaddr_in sin ;
	memset(&sin, 0, sizeof(sin)) ;
	sin.sin_family = AF_INET ;
	sin.sin_port = htons(port) ;

	int r = inet_pton(AF_INET, host, &sin.sin_addr) ;
	if (r != 1) {
		if (!r) errno = EINVAL ;
		close_fd(fd) ;
		return -1 ;
	}

	if (bind(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0 || socketunix_listen(fd, SOCKETUNIX_BACKLOG) < 0) {
		close_fd(fd) ;
		return -1 ;
	}

	return fd ;
}

static void check_resolve_version(char const *base)
{
	char path[SS_MAX_PATH_LEN + SS_SYSTEM_LEN + sizeof("/.version")] ;
	auto_strings(path, base, SS_SYSTEM, "/.version") ;

	_cleanup_strbuf_ strbuf ver = STRBUF_ZERO ;
	if (!strbuf_read_file(&ver, path)) {

		if (errno == ENOENT) {
			log_warn("resolve version marker absent (", path, "); serving unverified") ;
		} else {
			log_dieusys(LOG_EXIT_SYS, "read resolve version marker: ", path) ;
		}

		return ;
	}

	if (ver.len != strlen(SS_VERSION) || memcmp(ver.s, SS_VERSION, ver.len))
		log_die(LOG_EXIT_SYS, "resolve format mismatch: on disk ", ver.s, ", built for " SS_VERSION " -- rebuild 66-web against this 66, or run 66 to migrate the state") ;
}

static void on_signal(sse_watcher_t *w, void *cbdata, int event)
{
	(void)cbdata ;
	(void)event ;

	sse_signal_t *s = (sse_signal_t *)w->sdata ;

	if (s)
		log_info("caught signal ", s->si.ssi_signo == SIGINT ? "SIGINT" : "SIGTERM") ;

	w->p->running = false ;
}


static int on_root(int id, char const *arg, void *data)
{
	(void)data ;

	if (id == 'v' && !u32_scan_strict(arg, &VERBOSITY))
		log_die(LOG_EXIT_USER, "invalid verbosity level: ", arg) ;

	return 0 ;
}

static int on_serve(int id, char const *arg, void *data)
{
	web_opts_t *o = data ;

	switch (id) {

		case 'l' :

			if (!parse_listen(arg, o->host, sizeof(o->host), &o->port))
				log_dieusys(LOG_EXIT_USER, "parse --listen value: ", arg) ;

			if (!host_is_ipv4(o->host))
				log_die(LOG_EXIT_USER, "listen host is not a numeric IPv4 address: ", o->host) ;

			if (!host_is_loopback(o->host))
				log_die(LOG_EXIT_USER, "non-loopback listen address: ", o->host,
				        " -- credentials and traffic would cross the network in clear text; "
				        "use --insecure-listen instead, behind an HTTPS reverse proxy") ;

			o->loopback = 1 ;
			o->insecure = 0 ;
			break ;

		case 'L' :

			if (!parse_listen(arg, o->host, sizeof(o->host), &o->port))
				log_dieusys(LOG_EXIT_USER, "parse --insecure-listen value: ", arg) ;

			if (!host_is_ipv4(o->host))
				log_die(LOG_EXIT_USER, "listen host is not a numeric IPv4 address: ", o->host) ;

			/* A reverse proxy legitimately rewrites Host, so the
			 * rebinding check is its job from here on. */
			o->loopback = 0 ;
			o->insecure = 1 ;
			break ;

		case 'a' :

			o->auth_flag = arg ;
			break ;
	}

	return 0 ;
}

static int on_passwd(int id, char const *arg, void *data)
{
	web_opts_t *o = data ;

	if (id == 'a')
		o->auth_flag = arg ;

	return 0 ;
}

static int do_passwd(int argc, char const *const *argv, void *data)
{
	web_opts_t const *o = data ;
	char authpath[SS_MAX_PATH_LEN + 1] ;

	if (argc)
		log_die(LOG_EXIT_USER, "unexpected argument: ", argv[0]) ;

	if (!auth_file_path(o->auth_flag, authpath, SS_MAX_PATH_LEN + 1))
		log_dieusys(LOG_EXIT_USER, "resolve the credential file path") ;

	return auth_passwd_interactive(authpath) ? 0 : LOG_EXIT_SYS ;
}

static int do_serve(int argc, char const *const *argv, void *data)
{
	web_opts_t const *o = data ;
	char authpath[SS_MAX_PATH_LEN + 1] ;

	if (argc)
		log_die(LOG_EXIT_USER, "unexpected argument: ", argv[0]) ;

	ctx_t ctx ;
	if (!ctx_init(&ctx))
		log_dieusys(LOG_EXIT_SYS, "initialize 66 context") ;

	// Refuse to serve a resolve format we were not built to read.
	check_resolve_version(ctx.base) ;

	// No credential, no server -- loopback included.
	auth_cred_t cred ;
	if (!auth_file_path(o->auth_flag, authpath, SS_MAX_PATH_LEN + 1))
		log_dieusys(LOG_EXIT_USER, "resolve the credential file path") ;

	if (!auth_load(authpath, &cred))
		log_die(LOG_EXIT_USER, "no credentials configured in ", authpath, "; run: 66-web passwd") ;

	/* Ignore SIGPIPE so a dead peer, an SSE subscriber or an HTTP client,
	 * does not kill the daemon on the next write to its socket. */
	signal(SIGPIPE, SIG_IGN) ;

	int listen_fd = open_listen_socket(o->host, o->port) ;
	if (listen_fd < 0)
		log_dieusys(LOG_EXIT_SYS, "open listen socket on ", o->host) ;

	char portstr[U16_FMT] ;
	portstr[u16_fmt(portstr, o->port)] = 0 ;

	if (o->insecure)
		log_warn("!! INSECURE LISTEN !! serving plain HTTP on ", o->host, ":", portstr,
		         " -- Basic credentials and every response travel unencrypted; "
		         "put an HTTPS reverse proxy in front and keep this address off untrusted networks") ;

	sse_epoll_t loop = SSE_EPOLL_ZERO ;
	if (!sse_new(&loop, SSE_MAX_EVENTS))
		log_dieusys(LOG_EXIT_SYS, "initialize event loop") ;

	sse_watcher_t sig_w = SSE_WATCHER_ZERO ;
	if (!sse_start_signal(&loop, &sig_w, &on_signal, NULL, 0))
		log_dieusys(LOG_EXIT_SYS, "start signal watcher") ;

	if (!sse_attach_signal(&sig_w, SIGINT))
		log_dieusys(LOG_EXIT_SYS, "attach SIGINT") ;

	if (!sse_attach_signal(&sig_w, SIGTERM))
		log_dieusys(LOG_EXIT_SYS, "attach SIGTERM") ;

	sse_watcher_t accept_w = SSE_WATCHER_ZERO ;
	if (!sse_start_io(&loop, &accept_w, &on_accept, &loop, listen_fd, SSE_READ, 0))
		log_dieusys(LOG_EXIT_SYS, "start accept watcher") ;

	events_init(&loop) ; // non-fatal on failure

	http_init(o->host, o->port, o->loopback, &cred, &loop, &accept_w) ;

	log_info("listening on http://", o->host, ":", portstr) ;

	if (!sse_poll(&loop, SSE_TIMEOUT_INFINITE))
		log_dieusys(LOG_EXIT_SYS, "event loop") ;

	events_shutdown() ;
	action_shutdown() ; // before sse_free: the loop still lists their watchers
	http_shutdown() ;
	sse_free(&loop) ;
	close_fd(listen_fd) ;

	log_info("shutdown complete") ;
	return 0 ;
}

static opt_cmd_t const cmd_sub[] = {
	{
		.name = "serve",
		.help = "run the web interface",
		.epilog = serve_epilog,
		.opts = opts_serve,
		.nopts = OPT_COUNT(opts_serve),
		.on_option = &on_serve,
		.fn = &do_serve,
	},
	{
		.name = "passwd",
		.help = "set the user name and password",
		.epilog = passwd_epilog,
		.opts = opts_passwd,
		.nopts = OPT_COUNT(opts_passwd),
		.on_option = &on_passwd,
		.fn = &do_passwd,
	},
} ;

static opt_cmd_t const cmd_root = {
	.name = "66-web",
	.help = "web interface to manage 66 services",
	.opts = opts_root,
	.nopts = OPT_COUNT(opts_root),
	.on_option = &on_root,
	.sub = cmd_sub,
	.nsub = OPT_COUNT(cmd_sub),
} ;

int main(int argc, char const *const *argv)
{
	PROG = "66-web" ;

	web_opts_t o = WEB_OPTS_ZERO ;

	return opt_dispatch(argc, argv, &cmd_root, &o) ;
}
