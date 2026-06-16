/*
 * ns_session.c
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
 *
 * New-session handling: setsid() detaches from the controlling terminal, which
 * also closes the TIOCSTI input-injection hole (the sandbox can no longer push
 * characters into the launching terminal). Optionally re-acquire a ctty.
 */

#include <unistd.h>
#include <sys/ioctl.h>
#include <termios.h>

#include <oblibs/log.h>

#include "ns.h"
#include "ns_internal.h"

void ns_session_setup(ns_t *ns)
{
    log_flow() ;

    if (!ns->new_session)
        return ;

    /* The clone child / pid-namespace grandchild is never a process-group
     * leader (its pid differs from the inherited pgid), so setsid() succeeds. */
    if (setsid() == -1) {
        log_warnusys("setsid") ;
        return ;
    }

    if (ns->take_ctty)
        if (ioctl(STDIN_FILENO, TIOCSCTTY, 0) == -1)
            log_warnusys("acquire controlling terminal") ;
}
