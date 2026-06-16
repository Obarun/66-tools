/*
 * 66-clock.c
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

#include <stdint.h>
#include <string.h>

#include <oblibs/log.h>
#include <oblibs/clock.h>
#include <oblibs/opt.h>
#include <oblibs/stream.h>

static opt_t const opts[] = {
    { .id = OPT_ID_HELP, .shortname = 'h', .longname = "help",    .arg = OPT_NONE,                           .help = "print this help" },
    { .id = 'n',         .shortname = 'n', .longname = "newline", .arg = OPT_NONE,                           .help = "output a trailing newline" },
    { .id = 'm',         .shortname = 'm', .longname = "message", .arg = OPT_REQUIRED, .argname = "message", .help = "print message after the time system" },
} ;

static opt_cmd_t const cmd = {
    .name = "66-clock",
    .operands = "tai|iso",
    .opts = opts,
    .nopts = OPT_COUNT(opts),
} ;

void display_clock(unsigned int flags, uint8_t nl, char const *msg)
{
    size_t slen = !msg ? 0 : strlen(msg) ;
    char stamp[CLOCK_LOCAL_LEN + 1 + slen + 1 + 1] ;
    size_t len = 0 ;
    struct timespec now ;

    if (!clock_now(&now))
        log_dieusys(LOG_EXIT_SYS, "get current time") ;

    if (flags & 1)
        len = clock_tai64n_fmt(stamp, &now) ;
    else if (flags & 2)
        len = clock_local_fmt(stamp, &now) ;

    if (msg) {
        stamp[len++] = ' ' ;
        memcpy(stamp + len, msg, slen) ;
        len += slen ;
    }
    stamp[len++] = nl ? '\n' : ' ' ;

    if (!ostream_putflush(ostream_1, stamp, len))
        log_dieusys(LOG_EXIT_SYS, "write to stdout") ;
}

int main (int argc, char const *const *argv)
{
    unsigned int flag = 0 ;
    uint8_t nl = 0 ;
    char const *msg = 0 ;

    PROG = "66-clock" ;
    {
        opt_scan_t st = OPT_SCAN_ZERO ;

        for (;;)
        {
            int o = opt_scan(argc, argv, opts, OPT_COUNT(opts), &st) ;
            if (o == OPT_END) break ;
            switch (o) {
                case OPT_ID_HELP: return opt_emit_help(cmd.name, &cmd) ;
                case 'm': msg = st.arg ; break ;
                case 'n': nl = 1 ; break ;
                default : return opt_emit_error(cmd.name, &cmd, o, &st) ;
            }
        }
        argc -= st.ind ; argv += st.ind ;
    }

    if (!argc) return opt_emit_usage(cmd.name, &cmd) ;

    if (!strcmp(*argv,"tai")) flag |= 1 ;
    else if (!strcmp(*argv,"iso")) flag |= 2 ;
    else log_die(LOG_EXIT_USER, "invalid format -- must be tai or iso") ;

    display_clock(flag, nl, msg) ;

    return 0 ;
}
