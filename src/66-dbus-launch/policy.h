/*
 * policy.h
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

#ifndef DBS_POLICY_H_INCLUDE
#define DBS_POLICY_H_INCLUDE

#include "dbus.h"

#define POLICY_T_BATCH                                                          \
                "bt"                                                            \
                "a(btbs)"                                                       \
                "a(btssssuutt)"                                                 \
                "a(btssssuutt)"

#define POLICY_T                                                                \
                "a(u(" POLICY_T_BATCH "))"                                      \
                "a(buu(" POLICY_T_BATCH "))"                                    \
                "a(ss)"                                                         \
                "b"                                                             \
                "s"

#define POLICY_PRIORITY_DEFAULT (UINT64_C(1))

extern int policy(sd_bus_message *m);

#endif

