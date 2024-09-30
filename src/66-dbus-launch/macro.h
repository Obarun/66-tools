/*
 * macro.h
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
#ifndef DBS_MACRO_H_INCLUDE
#define DBS_MACRO_H_INCLUDE

#define dbs_cleanup_(func) __attribute__((__cleanup__(func)))

#define DBS_DEFINE_CLEANUP(_type, _func)			\
	static inline void _func ## p(_type *p) {   \
		if (*p)                                 \
		    _func(*p) ;                 		\
	} struct force_semicolon

#define DBS_EXIT_FATAL -1
#define DBS_EXIT_WARN 0
#define DBS_EXIT_MAIN 0
#define DBS_EXIT_CHILD 1

#endif