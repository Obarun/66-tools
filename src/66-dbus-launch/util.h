/*
 * util.h
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

#ifndef DBS_UTILS_H_INCLUDE
#define DBS_UTILS_H_INCLUDE

#include <sys/types.h>

#include "launcher.h"

extern pid_t async_spawn(char **cmd) ;
extern int spawn_wait(pid_t p) ;
extern int sync_spawn(char **cmd) ;
extern int handle_signal(launcher_t *launcher, int signo) ;

#endif

