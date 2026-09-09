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

static int policy_export_connect(odbus_message *m)
{
    return odbus_message_append(m, "bt", true, POLICY_PRIORITY_DEFAULT) ;
}

static int policy_export_own(odbus_message *m)
{
    int r ;

    if (!(r = odbus_message_open_container(m, 'a', "(btbs)"))) return 0 ;
    if (!(r = odbus_message_open_container(m, 'r', "btbs"))) return 0 ;
    if (!(r = odbus_message_append(m, "btbs", true, POLICY_PRIORITY_DEFAULT, true, ""))) return 0 ;
    if (!(r = odbus_message_close_container(m))) return 0 ;
    return odbus_message_close_container(m) ;
}

static int policy_export_xmit(odbus_message *m)
{
    int r ;

    if (!(r = odbus_message_open_container(m, 'a', "(btssssuutt)"))) return 0 ;
    if (!(r = odbus_message_open_container(m, 'r', "btssssuutt"))) return 0 ;
    if (!(r = odbus_message_append(m, "btssssuutt", true, POLICY_PRIORITY_DEFAULT, "", "", "", "", 0, 0, UINT64_C(0), (uint64_t) - 1))) return 0 ;
    if (!(r = odbus_message_close_container(m))) return 0 ;
    return odbus_message_close_container(m) ;
}

int policy(odbus_message *m)
{
    log_flow() ;

    int r ;

    if (!(r = odbus_message_open_container(m, 'v', "(" POLICY_T ")"))) return 0 ;
    if (!(r = odbus_message_open_container(m, 'r', POLICY_T))) return 0 ;
    if (!(r = odbus_message_open_container(m, 'a', "(u(" POLICY_T_BATCH "))"))) return 0 ;
    if (!(r = odbus_message_open_container(m, 'r', "u(" POLICY_T_BATCH ")"))) return 0 ;
    if (!(r = odbus_message_append(m, "u", (uint32_t) - 1))) return 0 ;
    if (!(r = odbus_message_open_container(m, 'r', POLICY_T_BATCH))) return 0 ;

    if (!(r = policy_export_connect(m))) return 0 ;
    if (!(r = policy_export_own(m))) return 0 ;
    if (!(r = policy_export_xmit(m))) return 0 ;
    if (!(r = policy_export_xmit(m))) return 0 ;

    if (!(r = odbus_message_close_container(m))) return 0 ;
    if (!(r = odbus_message_close_container(m))) return 0 ;
    if (!(r = odbus_message_close_container(m))) return 0 ;

    if (!(r = odbus_message_open_container(m, 'a', "(buu(" POLICY_T_BATCH "))"))) return 0 ;
    if (!(r = odbus_message_close_container(m))) return 0 ;

    if (!(r = odbus_message_open_container(m, 'a', "(ss)"))) return 0 ;
    if (!(r = odbus_message_close_container(m))) return 0 ;

    if (!(r = odbus_message_append(m, "b", false))) return 0 ;

    /** From a cursory reading of the source code, it seems this is
     * only relevant if you're using MAC. Until the policy API gets
     * stabilized, using this field doesn't make sense.*/
    if (!(r = odbus_message_append(m, "s", "n/a"))) return 0 ;

    if (!(r = odbus_message_close_container(m))) return 0 ;
    return odbus_message_close_container(m) ;
}
