/* 
 * 66-clock.c
 * 
 * Copyright (c) 2018-2020 Eric Vidal <eric@obarun.org>
 * 
 * All rights reserved.
 * 
 * This file is part of Obarun. It is subject to the license terms in
 * the LICENSE file found in the top-level directory of this
 * distribution.
 * This file may not be copied, modified, propagated, or distributed
 * except according to the terms contained in the LICENSE file./
 *
 * This file is a strict copy of s6-linux-init-umountall.c file
 * coming from skarnet software at https://skarnet.org/software/s6-linux-init.
 * All credits goes to Laurent Bercot <ska-remove-this-if-you-are-not-a-bot@skarnet.org>
 * */

#include <stdint.h>

#include <oblibs/log.h>
#include <oblibs/string.h>

#include <skalibs/buffer.h>
#include <skalibs/tai.h>
#include <skalibs/djbtime.h>
#include <skalibs/sgetopt.h>

#define USAGE "66-clock [ -n ] tai|iso"

static inline void info_help (void)
{
  static char const *help =
"66-clock <options> tai|iso\n"
"\n"
"options :\n"
"	-h: print this help\n"
"	-n: output a trailing newline\n"
;

 if (buffer_putsflush(buffer_1, help) < 0)
    log_dieusys(111,"write to stdout") ;
}
 
void display_clock(unsigned int flags,uint8_t nl)
{
	size_t hlen = 0 ;
	char hstamp[32] ;
	char tstamp[TIMESTAMP+1] ;
	char *pstr ;
	tain_t now ;
	tain_wallclock_read(&now) ;
	if (flags & 1)
	{
		timestamp_fmt(tstamp, &now) ;
		if (nl) tstamp[TIMESTAMP] = '\n' ;
		else tstamp[TIMESTAMP] = ' ' ;
		pstr = &tstamp[0] ;
	}
	if (flags & 2)
	{
		localtmn_t l ;
		localtmn_from_tain(&l, &now, 1) ;
		hlen = localtmn_fmt(hstamp, &l) ;
		if (nl) hstamp[hlen++] = '\n' ;
		else hstamp[hlen++] = ' ' ;
		pstr = &hstamp[0] ;
	}
	if (buffer_putsflush(buffer_1, pstr) < 0) 
		log_dieusys(LOG_EXIT_SYS, "write to stdout") ;
}

int main (int argc, char const *const *argv)
{
	unsigned int flag = 0 ;
	uint8_t nl = 0 ;
	
	PROG = "66-clock" ;
	{
		subgetopt_t l = SUBGETOPT_ZERO ;
		for (;;)
		{
			int opt = subgetopt_r(argc, argv, "hn", &l) ;
			if (opt == -1) break ;
			switch (opt)
			{
				case 'h': info_help() ; return 0 ;
				case 'n': nl = 1 ; break ;
				default : log_usage(USAGE) ;
			}
		}
		argc -= l.ind ; argv += l.ind ;
	}
	
	if (!argc) log_usage(USAGE) ;
	
	if (!strcmp(*argv,"tai")) {
		flag |= 1 ;
	}
	else if (!strcmp(*argv,"iso")) {
		flag |= 2 ;
	}
	else
	{
		log_die(LOG_EXIT_SYS,"invalid format -- must be tai or iso") ;
	}
	
	display_clock(flag,nl) ;

	return 0 ;
}

