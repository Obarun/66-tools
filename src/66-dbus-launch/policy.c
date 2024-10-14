/*
 * policy.c
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

#include <stdbool.h>
#include <stdint.h>

#include "dbus.h"
#include "policy.h"

#include <oblibs/log.h>

/*
 * At the moment, we just allow everything
 * The syntax of the policy is not stable yet: refer to
 * https://github.com/bus1/dbus-broker/blob/main/docs/dbus-broker.rst
 * dbus-broker/src/launch/policy.c and
 * dbus-broker/src/bus/policy.c
 *
 * For Type format https://man.archlinux.org/man/sd_bus_message_append.3.en
 */

static void policy_export_connect(sd_bus_message *m)
{
    sd_bus_message_append(m, "bt", true, POLICY_PRIORITY_DEFAULT) ;
}

static void policy_export_own(sd_bus_message *m)
{
    sd_bus_message_open_container(m, 'a', "(btbs)") ;
    sd_bus_message_open_container(m, 'r', "btbs") ;
    sd_bus_message_append(m, "btbs", true, POLICY_PRIORITY_DEFAULT, true, "") ;
    sd_bus_message_close_container(m) ;
    sd_bus_message_close_container(m) ;
}

static void policy_export_xmit(sd_bus_message *m)
{
    sd_bus_message_open_container(m, 'a', "(btssssuutt)") ;
    sd_bus_message_open_container(m, 'r', "btssssuutt") ;
    sd_bus_message_append(m, "btssssuutt", true, POLICY_PRIORITY_DEFAULT, "", "", "", "", 0, 0, UINT64_C(0), (uint64_t) - 1) ;
    sd_bus_message_close_container(m) ;
    sd_bus_message_close_container(m) ;
}

int policy(sd_bus_message *m)
{
    log_flow() ;

    int r;
    r = sd_bus_message_open_container(m, 'v', "(" POLICY_T ")") ;
    r = sd_bus_message_open_container(m, 'r', POLICY_T) ;
    r = sd_bus_message_open_container(m, 'a', "(u(" POLICY_T_BATCH "))") ;
    r = sd_bus_message_open_container(m, 'r', "u(" POLICY_T_BATCH ")") ;
    r = sd_bus_message_append(m, "u", (uint32_t) - 1) ;
    r = sd_bus_message_open_container(m, 'r', POLICY_T_BATCH) ;
    policy_export_connect(m) ;
    policy_export_own(m) ;
    policy_export_xmit(m) ;
    policy_export_xmit(m) ;

    r = sd_bus_message_close_container(m) ;
    r = sd_bus_message_close_container(m) ;
    r = sd_bus_message_close_container(m) ;

    r = sd_bus_message_open_container(m, 'a', "(buu(" POLICY_T_BATCH "))") ;
    r = sd_bus_message_close_container(m) ;

    r = sd_bus_message_open_container(m, 'a', "(ss)") ;
    r = sd_bus_message_close_container(m) ;

    r = sd_bus_message_append(m, "b", false) ;

    /** From a cursory reading of the source code, it seems this is
     * only relevant if you're using MAC. Until the policy API gets
     * stabilized, using this field doesn't make sense.*/
    r = sd_bus_message_append(m, "s", "n/a") ;
    r = sd_bus_message_close_container(m) ;
    r = sd_bus_message_close_container(m) ;

    return r ;
}
