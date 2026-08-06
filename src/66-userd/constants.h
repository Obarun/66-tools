/*
 * constants.h
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

#ifndef USERD_CONSTANTS_H
#define USERD_CONSTANTS_H

#include <66/config.h>

#define USERD_STATEDIR SS_LIVE "userd"
#define USERD_SOCKET_PATH USERD_STATEDIR "/s"
#define USERD_SESSIONS_SUB "/sessions"
#define USERD_SESSIONS_SUB_LEN (sizeof USERD_SESSIONS_SUB - 1)
#define USERD_USERS_SUB "/users"
#define USERD_SESSIONS_DIR USERD_STATEDIR USERD_SESSIONS_SUB
#define USERD_USERS_DIR USERD_STATEDIR USERD_USERS_SUB
#define SESSION_ID_MAX 16
#define SESSION_FIELD_MAX 256
#define USER_NAME_MAX 256

#endif
