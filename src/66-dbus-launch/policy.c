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
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#include "dbus.h"
#include "odbus_internal.h"
#include "policy.h"

#include <oblibs/genbuf.h>
#include <oblibs/log.h>
#include <oblibs/strbuf.h>

#include <66-tools/config.h>

#define POLICY_WILDCARD "*"
#define POLICY_BROADER ", the effective policy is broader than the file requests"

/*
 * The syntax of the policy is not stable yet: refer to
 * https://github.com/bus1/dbus-broker/blob/main/docs/dbus-broker.rst
 * dbus-broker/src/launch/policy.c and
 * dbus-broker/src/bus/policy.c
 *
 * For Type format https://man.archlinux.org/man/sd_bus_message_append.3.en
 */

static void policy_entries_free(policy_entries *e)
{
	genbuf_free(&e->connect) ;
	genbuf_free(&e->own) ;
	genbuf_free(&e->send) ;
	genbuf_free(&e->recv) ;
}

static int policy_entries_empty(policy_entries const *e)
{
	return !e->connect.len && !e->own.len && !e->send.len && !e->recv.len ;
}

static void policy_nodes_free(genbuf *nodes)
{
	size_t n = genbuf_len(policy_node, nodes) ;

	for (size_t i = 0 ; i < n ; i++)
		policy_entries_free(&genbuf_s(policy_node, nodes)[i].entries) ;

	genbuf_free(nodes) ;
}

void policy_free(policy_t *p)
{
	strbuf_free(&p->strings) ;
	policy_entries_free(&p->default_entries) ;
	policy_entries_free(&p->at_console) ;
	policy_entries_free(&p->no_console) ;
	policy_nodes_free(&p->uids) ;
	policy_nodes_free(&p->gids) ;
	p->counter = 0 ;
}

static policy_entries *policy_node_at(genbuf *nodes, uint32_t id)
{
	policy_node const node = { .id = id, .entries = { GENBUF_ZERO, GENBUF_ZERO, GENBUF_ZERO, GENBUF_ZERO } } ;
	size_t n = genbuf_len(policy_node, nodes), i = 0 ;

	while (i < n && genbuf_s(policy_node, nodes)[i].id < id)
		i++ ;

	if (i < n && genbuf_s(policy_node, nodes)[i].id == id)
		return &genbuf_s(policy_node, nodes)[i].entries ;

	if (!strbuf_insertb(nodes, i * sizeof(policy_node), (char const *)&node, sizeof(policy_node)))
		return NULL ;

	return &genbuf_s(policy_node, nodes)[i].entries ;
}

static int policy_skip(policy_rule const *r, char const *reason)
{
	flog_warn("%s:%zu: <%s> is ignored: %s", r->file, r->line, r->allow ? "allow" : "deny", reason) ;

	return 1 ;
}

static int policy_skip_account(policy_rule const *r)
{
	flog_warn("%s:%zu: <%s> is ignored: unknown%s%s%s%s%s", r->file, r->line, r->allow ? "allow" : "deny",
		r->user_unknown ? " user " : "", r->user_unknown ? r->user_unknown : "",
		r->group_unknown ? " group " : "", r->group_unknown ? r->group_unknown : "",
		r->allow ? "" : POLICY_BROADER) ;

	return 1 ;
}

static int policy_skip_unapplied(policy_rule const *r)
{
	flog_warn("%s:%zu: <%s> is ignored: dbus-broker does not apply%s%s%s%s", r->file, r->line, r->allow ? "allow" : "deny",
		r->send_error ? " send_error" : "", r->recv_error ? " receive_error" : "",
		r->send_destination_prefix ? " send_destination_prefix" : "",
		r->allow ? "" : POLICY_BROADER) ;

	return 1 ;
}

static int policy_rule_sends(policy_rule const *r)
{
	return r->send_interface || r->send_member || r->send_destination || r->send_path || r->send_typed || r->send_broadcast || r->send_requested_reply ;
}

static int policy_rule_receives(policy_rule const *r)
{
	return r->recv_interface || r->recv_member || r->recv_sender || r->recv_path || r->recv_typed || r->recv_requested_reply ;
}

static int policy_rule_unresolved(policy_rule const *r)
{
	return (r->context == POLICY_CONTEXT_USER || r->context == POLICY_CONTEXT_GROUP) && r->id == POLICY_ID_NONE ;
}

static policy_verdict policy_verdict_next(policy_t *p, policy_rule const *r)
{
	policy_verdict const v = {
		.allow = r->allow,
		.priority = UINT64_MAX / POLICY_CONTEXT_ENDOFKEY * r->context + ++p->counter
	} ;

	return v ;
}

static policy_entries *policy_entries_of(policy_t *p, policy_rule const *r)
{
	switch (r->context) {

		case POLICY_CONTEXT_USER :

			return policy_node_at(&p->uids, r->id) ;

		case POLICY_CONTEXT_GROUP :

			return policy_node_at(&p->gids, r->id) ;

		case POLICY_CONTEXT_AT_CONSOLE :

			return &p->at_console ;

		case POLICY_CONTEXT_NO_CONSOLE :

			return &p->no_console ;

		default :

			return &p->default_entries ;
	}
}

static int policy_string_add(policy_t *p, char const *s, uint8_t wildcard, size_t *offset)
{
	if (!s || (wildcard && !strcmp(s, POLICY_WILDCARD))) {
		*offset = POLICY_STRING_NONE ;
		return 1 ;
	}

	*offset = p->strings.len ;

	return strbuf_catb(&p->strings, s, strlen(s) + 1) ;
}

char const *policy_string(policy_t const *p, size_t offset)
{
	return offset == POLICY_STRING_NONE ? "" : p->strings.s + offset ;
}

int policy_connect_self(policy_t *p, uid_t uid)
{
	log_flow() ;

	policy_entries *e = policy_node_at(&p->uids, (uint32_t)uid) ;
	policy_verdict v = { .allow = 1, .priority = 0 } ;

	if (!e)
		return 0 ;

	v.priority = ++p->counter ;

	return genbuf_append(policy_verdict, &e->connect, &v) ;
}

static int policy_import_connect(policy_t *p, policy_rule const *r)
{
	policy_entries *e = &p->default_entries ;
	policy_verdict v ;

	if (r->user == r->group || r->own || r->own_prefix || policy_rule_sends(r) || policy_rule_receives(r))
		return policy_skip(r, "invalid combination of attributes") ;

	if (r->context == POLICY_CONTEXT_USER || r->context == POLICY_CONTEXT_GROUP)
		return policy_skip(r, "a connection rule is not allowed in a user or group policy") ;

	if (r->user && r->uid != POLICY_ID_NONE)
		e = policy_node_at(&p->uids, r->uid) ;
	else if (r->group && r->gid != POLICY_ID_NONE)
		e = policy_node_at(&p->gids, r->gid) ;

	if (!e)
		return 0 ;

	v = policy_verdict_next(p, r) ;

	return genbuf_append(policy_verdict, &e->connect, &v) ;
}

static int policy_import_own(policy_t *p, policy_rule const *r)
{
	policy_own own = { .prefix = 1, .name = POLICY_STRING_NONE } ;
	policy_entries *e ;

	if ((r->own && r->own_prefix) || policy_rule_sends(r) || policy_rule_receives(r))
		return policy_skip(r, "invalid combination of attributes") ;

	if (policy_rule_unresolved(r))
		return 1 ;

	e = policy_entries_of(p, r) ;
	if (!e)
		return 0 ;

	if (r->own_prefix) {

		if (!policy_string_add(p, r->own_prefix, 0, &own.name))
			return 0 ;

	} else if (strcmp(r->own, POLICY_WILDCARD)) {

		own.prefix = 0 ;

		if (!policy_string_add(p, r->own, 0, &own.name))
			return 0 ;
	}

	own.verdict = policy_verdict_next(p, r) ;

	return genbuf_append(policy_own, &e->own, &own) ;
}

static int policy_import_xmit(policy_t *p, policy_rule const *r, uint8_t send)
{
	policy_xmit x = {
		.type = send ? r->send_type : r->recv_type,
		.broadcast = send ? r->send_broadcast : POLICY_TRISTATE_UNSET,
		.min_fds = r->min_fds,
		.max_fds = r->max_fds
	} ;
	policy_entries *e ;

	if (x.type == ODBUS_MESSAGE_METHOD_RETURN || x.type == ODBUS_MESSAGE_ERROR)
		return 1 ;

	if (r->eavesdrop == POLICY_TRISTATE_YES && !r->allow)
		return 1 ;

	if (policy_rule_unresolved(r))
		return 1 ;

	e = policy_entries_of(p, r) ;
	if (!e)
		return 0 ;

	if (!policy_string_add(p, send ? r->send_destination : r->recv_sender, 1, &x.name) ||
		!policy_string_add(p, send ? r->send_path : r->recv_path, 1, &x.path) ||
		!policy_string_add(p, send ? r->send_interface : r->recv_interface, 1, &x.interface) ||
		!policy_string_add(p, send ? r->send_member : r->recv_member, 1, &x.member))
			return 0 ;

	x.verdict = policy_verdict_next(p, r) ;

	return genbuf_append(policy_xmit, send ? &e->send : &e->recv, &x) ;
}

int policy_import(policy_t *p, policy_rule const *r)
{
	if (r->context == POLICY_CONTEXT_NONE)
		return policy_skip(r, "its policy has no context") ;

	if (r->user_unknown || r->group_unknown)
		return policy_skip_account(r) ;

	if (r->send_error || r->recv_error || r->send_destination_prefix)
		return policy_skip_unapplied(r) ;

	if (r->user || r->group)
		return policy_import_connect(p, r) ;

	if (r->own || r->own_prefix)
		return policy_import_own(p, r) ;

	if (policy_rule_sends(r)) {

		if (policy_rule_receives(r))
			return policy_skip(r, "invalid combination of attributes") ;

		return policy_import_xmit(p, r, 1) ;
	}

	if (policy_rule_receives(r) || r->eavesdrop)
		return policy_import_xmit(p, r, 0) ;

	return policy_skip(r, "no attribute defines what it applies to") ;
}

static policy_verdict const *policy_top(genbuf const *list)
{
	policy_verdict const *top = NULL ;
	size_t n = genbuf_len(policy_verdict, list) ;

	for (size_t i = 0 ; i < n ; i++)
		if (!top || genbuf_s(policy_verdict, list)[i].priority > top->priority)
			top = &genbuf_s(policy_verdict, list)[i] ;

	return top ;
}

static policy_verdict const *policy_shrink(genbuf *list, policy_verdict const *keep)
{
	if (!keep) {
		list->len = 0 ;
		return NULL ;
	}

	genbuf_s(policy_verdict, list)[0] = *keep ;
	list->len = sizeof(policy_verdict) ;

	return genbuf_s(policy_verdict, list) ;
}

static void policy_optimize_nodes(genbuf *nodes, policy_verdict const *top_default)
{
	size_t n = genbuf_len(policy_node, nodes), kept = 0 ;
	policy_node *s = genbuf_s(policy_node, nodes) ;

	for (size_t i = 0 ; i < n ; i++) {

		policy_verdict const *top = policy_top(&s[i].entries.connect) ;

		if (top && top_default && top->priority <= top_default->priority)
			top = NULL ;

		policy_shrink(&s[i].entries.connect, top) ;

		if (policy_entries_empty(&s[i].entries)) {
			policy_entries_free(&s[i].entries) ;
			continue ;
		}

		s[kept++] = s[i] ;
	}

	nodes->len = kept * sizeof(policy_node) ;
}

void policy_optimize(policy_t *p)
{
	log_flow() ;

	policy_verdict const *top = policy_shrink(&p->default_entries.connect, policy_top(&p->default_entries.connect)) ;
	policy_optimize_nodes(&p->uids, top) ;
	policy_optimize_nodes(&p->gids, top) ;
}

static int policy_export_connect(odbus_message *m, genbuf const *inherited, genbuf const *own)
{
	policy_verdict const *top = own ? policy_top(own) : NULL ;

	if (!top && inherited)
		top = policy_top(inherited) ;

	if (!top)
		return odbus_message_append(m, "bt", false, POLICY_PRIORITY_DEFAULT) ;

	return odbus_message_append(m, "bt", (int)top->allow, top->priority) ;
}

static int policy_export_own_list(policy_t const *p, odbus_message *m, genbuf const *list)
{
	size_t n = list ? genbuf_len(policy_own, list) : 0 ;

	for (size_t i = 0 ; i < n ; i++) {

		policy_own const *o = &genbuf_s(policy_own, list)[i] ;

		if (!odbus_message_append(m, POLICY_T_OWN, (int)o->verdict.allow, o->verdict.priority, (int)o->prefix, policy_string(p, o->name)))
			return 0 ;
	}

	return 1 ;
}

static int policy_export_own(policy_t const *p, odbus_message *m, genbuf const *inherited, genbuf const *own)
{
	return odbus_message_open_container(m, 'a', POLICY_T_OWN) &&
		policy_export_own_list(p, m, inherited) &&
		policy_export_own_list(p, m, own) &&
		odbus_message_close_container(m) ;
}

static int policy_export_xmit_list(policy_t const *p, odbus_message *m, genbuf const *list)
{
	size_t n = list ? genbuf_len(policy_xmit, list) : 0 ;

	for (size_t i = 0 ; i < n ; i++) {

		policy_xmit const *x = &genbuf_s(policy_xmit, list)[i] ;

		if (!odbus_message_append(m, POLICY_T_XMIT, (int)x->verdict.allow, x->verdict.priority,
			policy_string(p, x->name), policy_string(p, x->path), policy_string(p, x->interface), policy_string(p, x->member),
			x->type, x->broadcast, x->min_fds, x->max_fds))
				return 0 ;
	}

	return 1 ;
}

static int policy_export_xmit(policy_t const *p, odbus_message *m, genbuf const *inherited, genbuf const *own)
{
	return odbus_message_open_container(m, 'a', POLICY_T_XMIT) &&
		policy_export_xmit_list(p, m, inherited) &&
		policy_export_xmit_list(p, m, own) &&
		odbus_message_close_container(m) ;
}

static int policy_export_batch(policy_t const *p, odbus_message *m, policy_entries const *connect, policy_entries const *inherited, policy_entries const *own)
{
	return odbus_message_open_container(m, 'r', POLICY_T_BATCH) &&
		policy_export_connect(m, connect ? &connect->connect : NULL, own ? &own->connect : NULL) &&
		policy_export_own(p, m, inherited ? &inherited->own : NULL, own ? &own->own : NULL) &&
		policy_export_xmit(p, m, inherited ? &inherited->send : NULL, own ? &own->send : NULL) &&
		policy_export_xmit(p, m, inherited ? &inherited->recv : NULL, own ? &own->recv : NULL) &&
		odbus_message_close_container(m) ;
}

static int policy_export_uid(policy_t const *p, odbus_message *m, uint32_t uid, policy_entries const *own)
{
	return odbus_message_open_container(m, 'r', "u(" POLICY_T_BATCH ")") &&
		odbus_message_append(m, "u", uid) &&
		policy_export_batch(p, m, &p->default_entries, &p->default_entries, own) &&
		odbus_message_close_container(m) ;
}

static int policy_export_uids(policy_t const *p, odbus_message *m)
{
	size_t n = genbuf_len(policy_node, &p->uids) ;

	if (!odbus_message_open_container(m, 'a', "(u(" POLICY_T_BATCH "))") ||
		!policy_export_uid(p, m, POLICY_UID_DEFAULT, NULL))
			return 0 ;

	for (size_t i = 0 ; i < n ; i++) {

		policy_node const *node = &genbuf_s(policy_node, &p->uids)[i] ;

		if (!policy_export_uid(p, m, node->id, &node->entries))
			return 0 ;
	}

	return odbus_message_close_container(m) ;
}

static int policy_export_range(policy_t const *p, odbus_message *m, uint8_t group, uint32_t first, uint32_t last, policy_entries const *connect, policy_entries const *own)
{
	return odbus_message_open_container(m, 'r', "buu(" POLICY_T_BATCH ")") &&
		odbus_message_append(m, "buu", (int)group, first, last) &&
		policy_export_batch(p, m, connect, NULL, own) &&
		odbus_message_close_container(m) ;
}

static int policy_export_ranges(policy_t const *p, odbus_message *m)
{
	size_t n = genbuf_len(policy_node, &p->gids) ;

	if (!odbus_message_open_container(m, 'a', "(buu(" POLICY_T_BATCH "))"))
		return 0 ;

	for (size_t i = 0 ; i < n ; i++) {

		policy_node const *node = &genbuf_s(policy_node, &p->gids)[i] ;

		if (!policy_export_range(p, m, 1, node->id, node->id, &p->default_entries, &node->entries))
			return 0 ;
	}

	if (!policy_entries_empty(&p->no_console) &&
		!policy_export_range(p, m, 0, 0, (uint32_t)SS_TOOLS_DBS_SYSTEM_UID_MAX, NULL, &p->no_console))
			return 0 ;

	if (!policy_entries_empty(&p->at_console) &&
		!policy_export_range(p, m, 0, (uint32_t)SS_TOOLS_DBS_SYSTEM_UID_MAX + 1, UINT32_MAX, NULL, &p->at_console))
			return 0 ;

	return odbus_message_close_container(m) ;
}

int policy_export(policy_t const *p, odbus_message *m)
{
	log_flow() ;

	if (!odbus_message_open_container(m, 'v', "(" POLICY_T ")") ||
		!odbus_message_open_container(m, 'r', POLICY_T) ||
		!policy_export_uids(p, m) ||
		!policy_export_ranges(p, m) ||
		!odbus_message_open_container(m, 'a', "(ss)") ||
		!odbus_message_close_container(m) ||
		!odbus_message_append(m, "b", false))
			return 0 ;

	/** From a cursory reading of the source code, it seems this is
	 * only relevant if you're using MAC. Until the policy API gets
	 * stabilized, using this field doesn't make sense.*/
	if (!odbus_message_append(m, "s", "n/a"))
		return 0 ;

	return odbus_message_close_container(m) && odbus_message_close_container(m) ;
}
