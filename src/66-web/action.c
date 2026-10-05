/*
 * action.c
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
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <oblibs/environ.h>
#include <oblibs/fd.h>
#include <oblibs/io.h>
#include <oblibs/log.h>
#include <oblibs/process.h>
#include <oblibs/sse.h>
#include <oblibs/strbuf.h>
#include <oblibs/string.h>

#include <66/config.h>

#include "action.h"

#define BIN_66 "66"
#define OUT_CHUNK 4096

static char const *const SERVICE_VERBS[] = {
	"start", "stop", "restart", "reload",
	"enable", "disable", "reconfigure", "free", "remove",
} ;
#define NVERBS nb_el(SERVICE_VERBS)

static int verb_allowed(char const *verb)
{
	for (size_t i = 0 ; i < NVERBS ; i++)
		if (!strcmp(SERVICE_VERBS[i], verb))
			return 1 ;
	return 0 ;
}

static char const *const TREE_VERBS[] = {
	"start", "stop", "free", "enable", "disable", "current", "init", "remove",
} ;
#define NTREEVERBS nb_el(TREE_VERBS)

typedef struct action_job_s action_job_t ;
struct action_job_s {
	sse_epoll_t *loop ;
	sse_watcher_t wchild ; // pidfd of the exec'd 66
	sse_watcher_t  wpipe ; // read end of its stdout+stderr
	strbuf out ; // capture, grown as the child writes
	int exit_code ; // -1 until on_child_exit decodes a status
	uint8_t reaped ; // the child exited and its status is decoded
	uint8_t drained ; // the pipe reached EOF and is closed
	uint8_t finishing ; // the completion callback already ran
	action_done_t *done ;
	action_abandon_t *abandon ;
	void *ctx ;
	action_job_t *next ;
} ;

static action_job_t *g_jobs ;

static int tree_verb_allowed(char const *verb)
{
	for (size_t i = 0 ; i < NTREEVERBS ; i++)
		if (!strcmp(TREE_VERBS[i], verb))
			return 1 ;
	return 0 ;
}

int action_name_valid(char const *s)
{
	if (!s || !*s || strlen(s) >= 256)
		return 0 ;

	if (*s == '-')
		return 0 ; // never an option to 66

	if (strstr(s, ".."))
		return 0 ;

	for (char const *p = s ; *p ; p++) {
		unsigned char c = (unsigned char)*p ;
		if (c == '/' || c <= ' ' || c == 0x7f)
			return 0 ; // no '/', space or control
	}

	return 1 ;
}

static void argv_clear(action_argv_t *out)
{
	out->len = 0 ;
	out->n = 0 ;
}

static int argv_add(action_argv_t *out, char const *arg)
{
	size_t l = strlen(arg) + 1 ;

	if (l > ACTION_ARGV_BYTES - out->len)
		return (errno = E2BIG, 0) ;

	memcpy(out->buf + out->len, arg, l) ;
	out->len += l ;
	out->n++ ;
	return 1 ;
}

int action_argv_run(action_argv_t *out, char const *action_name, char const *service_name)
{
	if (!action_name || !verb_allowed(action_name) || !action_name_valid(service_name))
		return (errno = EINVAL, 0) ;

	argv_clear(out) ;
	return argv_add(out, BIN_66) &&
		   argv_add(out, action_name) &&
		   argv_add(out, service_name) ;
}

int action_argv_run_tree(action_argv_t *out, char const *action_name, char const *tree_name)
{
	if (!action_name || !tree_verb_allowed(action_name) || !action_name_valid(tree_name))
		return (errno = EINVAL, 0) ;

	argv_clear(out) ;
	return argv_add(out, BIN_66) &&
		   argv_add(out, "tree") &&
		   argv_add(out, action_name) &&
		   argv_add(out, tree_name) ;
}

int action_argv_config(action_argv_t *out, char const *service_name)
{
	if (!action_name_valid(service_name))
		return (errno = EINVAL, 0) ;

	argv_clear(out) ;
	return argv_add(out, BIN_66) &&
		   argv_add(out, "configure") &&
		   argv_add(out, "-L") &&
		   argv_add(out, service_name) ;
}

int action_argv_enable(action_argv_t *out, char const *service_name, char const *tree_name, int start)
{
	if (!action_name_valid(service_name))
		return (errno = EINVAL, 0) ;

	if (tree_name && !action_name_valid(tree_name))
		return (errno = EINVAL, 0) ;

	argv_clear(out) ;

	if (!argv_add(out, BIN_66))
		return 0 ;

	if (tree_name && (!argv_add(out, "-t") || !argv_add(out, tree_name)))
		return 0 ;

	if (!argv_add(out, "enable"))
		return 0 ;

	if (start && !argv_add(out, "-S"))
		return 0 ;

	return argv_add(out, service_name) ;
}

static void job_link(action_job_t *job)
{
	job->next = g_jobs ;
	g_jobs = job ;
}

static void job_unlink(action_job_t *job)
{
	action_job_t **p = &g_jobs ;

	while (*p && *p != job) p = &(*p)->next ;
	if (*p) *p = job->next ;
	job->next = NULL ;
}

static void action_job_free(void *arg)
{
	action_job_t *job = (action_job_t *)arg ;

	strbuf_free(&job->out) ;
	free(job) ;
}

static void job_complete(action_job_t *job)
{
	if (job->finishing || !job->reaped || !job->drained)
		return ;

	job->finishing = 1 ;
	job_unlink(job) ;

	job->done(job->ctx, job->exit_code,
	           job->out.s ? job->out.s : "",
	           job->out.len) ;

	if (!sse_defer(job->loop, &action_job_free, job)) {
		log_warnusys("defer the release of a 66 action") ;
		action_job_free(job) ;
	}
}

static void on_pipe_read(sse_watcher_t *w, void *cbdata, int event)
{
	(void)w ;
	(void)event ;
	action_job_t *job = (action_job_t *)cbdata ;
	char buf[OUT_CHUNK] ;

	// EOF closed this pipe earlier in the very cycle that queued this read.
	if (job->drained)
		return ;

	for (;;) {
		ssize_t n = io_read(job->wpipe.fd, buf, OUT_CHUNK) ;
		if (n > 0) {
			if (strbuf_catb(&job->out, buf, (size_t)n))
				continue ;
			/* Nothing can grow the capture anymore. Report no output at
			 * all rather than a silently mutilated one, and close the
			 * pipe so the child stops filling a buffer nobody can hold. */
			log_warnusys("capture the output of a 66 action") ;
			strbuf_free(&job->out) ;
			break ;
		}

		if (!n)
			break ; // EOF: the child let go

		if (error_isagain(errno))
			return ; // drained for now, more may come

		log_warnusys("read the output of a 66 action") ;
		break ;
	}

	if (job->out.len && !strbuf_uncounted(&job->out)) {
		log_warnusys("terminate the output of a 66 action") ;
		strbuf_free(&job->out) ;
	}

	sse_free_io(&job->wpipe) ; // closes the read end
	job->drained = 1 ;

	job_complete(job) ;
}

static void on_child_exit(sse_watcher_t *w, void *cbdata, int event)
{
	(void)w ;
	(void)event ;
	action_job_t *job = (action_job_t *)cbdata ;

	if (job->reaped)
		return ;

	/* The dispatch layer waits the child itself and leaves the raw status in
	 * sdata. A pidfd only reports readable once the process is gone, so a
	 * watcher still active here means waitpid() found nothing to collect and
	 * the status is not knowable anymore: exit_code keeps its -1. */
	sse_child_t const *ch = (sse_child_t *)job->wchild.sdata ;
	if (ch && !sse_watcher_active(&job->wchild)) {
		if (WIFEXITED(ch->status))
			job->exit_code = WEXITSTATUS(ch->status) ;
		else if (WIFSIGNALED(ch->status))
			job->exit_code = 128 + WTERMSIG(ch->status) ;
	}

	sse_free_child(&job->wchild) ;
	job->reaped = 1 ;

	job_complete(job) ;
}

static void abort_child(pid_t pid)
{
	kill(pid, SIGKILL) ;
	process_wait(pid, NULL) ;
}

void action_shutdown(void)
{
	action_job_t *job = g_jobs ;

	while (job) {
		action_job_t *next = job->next ;

		job->abandon(job->ctx) ;

		if (!job->reaped)  sse_free_child(&job->wchild) ;
		if (!job->drained) sse_free_io(&job->wpipe) ;

		action_job_free(job) ;
		job = next ;
	}

	g_jobs = NULL ;
}

int action_start(sse_epoll_t *loop, action_argv_t const *argv, action_done_t *done, action_abandon_t *abandon, void *ctx)
{
	if (!loop || !argv || !argv->n || !done || !abandon)
		return (errno = EINVAL, 0) ;

	// Every argument takes one byte at least, so n never exceeds len.
	if (argv->len > ACTION_ARGV_BYTES || argv->n > argv->len)
		return (errno = EINVAL, 0) ;

	char const *av[argv->n + 3] ;

	size_t first = strlen(argv->buf) + 1 ;
	if (first > argv->len)
		return (errno = EINVAL, 0) ;

	av[0] = argv->buf ;
	av[1] = "-v" ;
	av[2] = "2" ;
	if (!environ_make(av + 3, argv->n - 1, argv->buf + first, argv->len - first))
		return 0 ;

	action_job_t *job = calloc(1, sizeof(*job)) ;
	if (!job)
		return (errno = ENOMEM, 0) ;

	int pipefd[2] ;
	if (pipe2(pipefd, O_CLOEXEC) < 0) {
		int e = errno ;
		free(job) ;
		return (errno = e, 0) ;
	}

	pid_t pid = fork() ;
	if (pid < 0) {
		int e = errno ;
		close_fd(pipefd[0]) ;
		close_fd(pipefd[1]) ;
		free(job) ;
		return (errno = e, 0) ;
	}

	if (pid == 0) {

		/* Redirect stdout + stderr into the pipe. */
		if (copy_fd(1, pipefd[1]) < 0 || copy_fd(2, pipefd[1]) < 0)
			log_dieusys(LOG_EXIT_SYS, "redirect the output of a 66 action") ;
		close_fd(pipefd[0]) ;
		close_fd(pipefd[1]) ;

		// be paranoid
		(void)close_range(3, ~0U, 0) ;

		/* The daemon ignores SIGPIPE for its SSE subscribers, and that
		 * disposition survives execve. Restore the default so 66 behaves
		 * like it does under a shell. */
		signal(SIGPIPE, SIG_DFL) ;

		sigset_t emptyset ;
		sigemptyset(&emptyset) ;
		sigprocmask(SIG_SETMASK, &emptyset, 0) ;

		execv(SS_BINPREFIX "66", (char *const *)av) ;
		log_dieusys(LOG_EXIT_SYS, "exec ", SS_BINPREFIX "66") ;
	}

	close_fd(pipefd[1]) ;

	job->loop      = loop ;
	job->out       = (strbuf)STRBUF_ZERO ;
	job->exit_code = -1 ;
	job->done      = done ;
	job->abandon   = abandon ;
	job->ctx       = ctx ;

	/* Only this end goes non-blocking: the callback drains it until it would
	 * block, while the child keeps a blocking write end so a burst of output
	 * waits for us instead of being lost. */
	if (!io_set_nonblock(pipefd[0])) {
		int e = errno ;
		close_fd(pipefd[0]) ;
		abort_child(pid) ;
		free(job) ;
		return (errno = e, 0) ;
	}

	if (!sse_start_child(loop, &job->wchild, &on_child_exit, job, pid, 0, false)) {
		int e = errno ;
		close_fd(pipefd[0]) ;
		abort_child(pid) ;
		free(job) ;
		return (errno = e, 0) ;
	}

	if (!sse_start_io(loop, &job->wpipe, &on_pipe_read, job, pipefd[0], SSE_READ, 0)) {
		int e = errno ;
		sse_free_child(&job->wchild) ;
		close_fd(pipefd[0]) ;
		abort_child(pid) ;
		free(job) ;
		return (errno = e, 0) ;
	}

	job_link(job) ;

	return 1 ;
}
