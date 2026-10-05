/*
 * ctx.c
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

#include <string.h>
#include <unistd.h>

#include <oblibs/log.h>
#include <oblibs/string.h>

#include <66/config.h>
#include <66/utils.h>

#include "ctx.h"

static ctx_t g_ctx ;
static int g_ctx_ready = 0 ;

int ctx_init(ctx_t *c)
{
	c->owner = geteuid() ;

	if (!set_ownersysdir_stack(c->base, c->owner))
		log_warnusys_return(LOG_EXIT_ZERO, "resolve 66 base directory for uid") ;

	g_ctx = *c ;
	g_ctx_ready = 1 ;

	return 1 ;
}

ctx_t const *ctx_get(void)
{
	return g_ctx_ready ? &g_ctx : NULL ;
}

int service_src_dir(uid_t owner, char *stack)
{
	if (!owner) {
		auto_strings(stack, SS_SERVICE_ADMDIR) ;
		return 1 ;
	}

	if (!set_ownerhome_stack_byuid(stack, owner))
		return 0 ;

	auto_strings(stack + strlen(stack), SS_SERVICE_USERDIR) ;

	return 1 ;
}

int service_src_dirs(uid_t owner, char const *paths[SERVICE_SRC_DIRS_MAX], int removable[SERVICE_SRC_DIRS_MAX], size_t *n, char *store)
{
	size_t i = 0 ;

	if (!owner) {

		paths[i] = SS_SERVICE_ADMDIR ; removable[i++] = 1 ; // /etc/66/service/
		paths[i] = SS_SERVICE_SYSDIR ; removable[i++] = 0 ; // /usr/share/66/service/

	} else {

		if (!set_ownerhome_stack_byuid(store, owner))
			return 0 ;
		auto_strings(store + strlen(store), SS_SERVICE_USERDIR) ; // $HOME/.66/service/
		paths[i] = store ; removable[i++] = 1 ;
		paths[i] = SS_SERVICE_ADMDIR_USER ; removable[i++] = 1 ; // /etc/66/service/user/
		paths[i] = SS_SERVICE_SYSDIR_USER ; removable[i++] = 0 ; // /usr/share/66/service/user/
	}

	*n = i ;

	return 1 ;
}
