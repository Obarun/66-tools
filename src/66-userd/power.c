/*
 * power.c
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
#include <sys/wait.h>

#include <oblibs/log.h>
#include <oblibs/process.h>
#include <oblibs/spawn.h>

#include <66/config.h>

#include "power.h"

extern char **environ ;

static char const *const action_table[6] = { "poweroff", "reboot", "halt", "suspend", "hibernate", 0 } ;

int power_action_from(char const *s)
{
    log_flow() ;

    for (int i = 0 ; action_table[i] ; i++)
        if (!strcmp(action_table[i], s))
            return i ;

    return -1 ;
}

char const *power_action_str(power_action_t action)
{
    return action <= POWER_HIBERNATE ? action_table[action] : "" ;
}

power_policy_t power_policy(int is_root, int has_local_session, int allowed, int has_other_users, int force)
{
    if (is_root)
        return POWER_ALLOW ;

    if (!has_local_session)
        return POWER_DENY_NO_SESSION ;

    if (!allowed)
        return POWER_DENY_NOT_ALLOWED ;

    if (has_other_users && !force)
        return POWER_DENY_OTHER_USERS ;

    return POWER_ALLOW ;
}

char const *power_policy_str(power_policy_t verdict)
{
    switch (verdict) {
        case POWER_DENY_NO_SESSION :  return "no active local session" ;
        case POWER_DENY_NOT_ALLOWED : return "not authorized to power off" ;
        case POWER_DENY_OTHER_USERS : return "other users are logged in (use --force)" ;
        default :                     return "allowed" ;
    }
}

static char const *hpr_flag(power_action_t action)
{
    switch (action) {
        case POWER_REBOOT :    return "-r" ;
        case POWER_HALT :      return "-h" ;
        case POWER_SUSPEND :   return "-s" ;
        case POWER_HIBERNATE : return "-i" ;
        default :              return "-p" ;
    }
}

int power_trigger(power_action_t action)
{
    log_flow() ;

    pid_t pid = fork() ;
    if (pid < 0)
        return 0 ;

    if (!pid) {
        char const *argv[] = { "66-hpr", hpr_flag(action), "-l", SS_LIVE, 0 } ;
        pid_t worker = spawn_path(SS_BINPREFIX "66-hpr", argv, (char const *const *)environ) ;
        // the worker reparents to PID 1; this child only reports the spawn result.
        if (!worker)
            log_dieusys(LOG_EXIT_SYS, "spawn: ", SS_BINPREFIX "66-hpr") ;

        _exit(0) ;
    }

    int wstat ;
    process_wait(pid, &wstat) ; // reap the short-lived intermediate child

    return WIFEXITED(wstat) && !WEXITSTATUS(wstat) ;
}


