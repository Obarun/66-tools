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

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <oblibs/genbuf.h>
#include <oblibs/strbuf.h>

#include "dbus.h"

#define POLICY_T_OWN "(btbs)"
#define POLICY_T_XMIT "(btssssuutt)"

#define POLICY_T_BATCH                                                          \
                "bt"                                                            \
                "a" POLICY_T_OWN                                                \
                "a" POLICY_T_XMIT                                               \
                "a" POLICY_T_XMIT

#define POLICY_T                                                                \
                "a(u(" POLICY_T_BATCH "))"                                      \
                "a(buu(" POLICY_T_BATCH "))"                                    \
                "a(ss)"                                                         \
                "b"                                                             \
                "s"

#define POLICY_PRIORITY_DEFAULT (UINT64_C(1))
#define POLICY_ID_NONE UINT32_MAX
#define POLICY_UID_DEFAULT UINT32_MAX
#define POLICY_STRING_NONE SIZE_MAX
#define POLICY_TYPE_ANY 0

typedef enum policy_context_e policy_context_t ;
enum policy_context_e
{
	POLICY_CONTEXT_NONE = 0,
	POLICY_CONTEXT_DEFAULT,
	POLICY_CONTEXT_GROUP,
	POLICY_CONTEXT_USER,
	POLICY_CONTEXT_AT_CONSOLE,
	POLICY_CONTEXT_NO_CONSOLE,
	POLICY_CONTEXT_MANDATORY,
	POLICY_CONTEXT_ENDOFKEY
} ;

typedef enum policy_tristate_e policy_tristate_t ;
enum policy_tristate_e
{
	POLICY_TRISTATE_UNSET = 0,
	POLICY_TRISTATE_YES,
	POLICY_TRISTATE_NO
} ;

typedef struct policy_verdict_s policy_verdict, *policy_verdict_ref ;
struct policy_verdict_s
{
	uint8_t allow ;
	uint64_t priority ;
} ;

typedef struct policy_own_s policy_own, *policy_own_ref ;
struct policy_own_s
{
	policy_verdict verdict ;
	uint8_t prefix ;
	size_t name ;
} ;

typedef struct policy_xmit_s policy_xmit, *policy_xmit_ref ;
struct policy_xmit_s
{
	policy_verdict verdict ;
	size_t name ;
	size_t path ;
	size_t interface ;
	size_t member ;
	uint32_t type ;
	uint32_t broadcast ;
	uint64_t min_fds ;
	uint64_t max_fds ;
} ;

typedef struct policy_entries_s policy_entries, *policy_entries_ref ;
struct policy_entries_s
{
	genbuf connect ;
	genbuf own ;
	genbuf send ;
	genbuf recv ;
} ;

typedef struct policy_node_s policy_node, *policy_node_ref ;
struct policy_node_s
{
	uint32_t id ;
	policy_entries entries ;
} ;

typedef struct policy_s policy_t, *policy_t_ref ;
struct policy_s
{
	uint64_t counter ;
	strbuf strings ;
	policy_entries default_entries ;
	policy_entries at_console ;
	policy_entries no_console ;
	genbuf uids ;
	genbuf gids ;
} ;

typedef struct policy_rule_s policy_rule, *policy_rule_ref ;
struct policy_rule_s
{
	char const *file ;
	size_t line ;
	uint8_t allow ;
	policy_context_t context ;
	uint32_t id ;
	uint8_t user ;
	uint32_t uid ;
	char const *user_unknown ;
	uint8_t group ;
	uint32_t gid ;
	char const *group_unknown ;
	char const *own ;
	char const *own_prefix ;
	char const *send_interface ;
	char const *send_member ;
	char const *send_error ;
	char const *send_destination ;
	char const *send_destination_prefix ;
	char const *send_path ;
	uint8_t send_typed ;
	uint32_t send_type ;
	uint32_t send_broadcast ;
	uint32_t send_requested_reply ;
	char const *recv_interface ;
	char const *recv_member ;
	char const *recv_error ;
	char const *recv_sender ;
	char const *recv_path ;
	uint8_t recv_typed ;
	uint32_t recv_type ;
	uint32_t recv_requested_reply ;
	uint32_t eavesdrop ;
	uint64_t min_fds ;
	uint64_t max_fds ;
} ;

#define POLICY_ZERO { .counter = 0, .strings = STRBUF_ZERO, .default_entries = { GENBUF_ZERO, GENBUF_ZERO, GENBUF_ZERO, GENBUF_ZERO }, .at_console = { GENBUF_ZERO, GENBUF_ZERO, GENBUF_ZERO, GENBUF_ZERO }, .no_console = { GENBUF_ZERO, GENBUF_ZERO, GENBUF_ZERO, GENBUF_ZERO }, .uids = GENBUF_ZERO, .gids = GENBUF_ZERO }

extern void policy_free(policy_t *p) ;
extern int policy_connect_self(policy_t *p, uid_t uid) ;
extern int policy_import(policy_t *p, policy_rule const *r) ;
extern void policy_optimize(policy_t *p) ;
extern char const *policy_string(policy_t const *p, size_t offset) ;
extern int policy_export(policy_t const *p, odbus_message *m) ;

#endif
