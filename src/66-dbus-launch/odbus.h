/*
 * odbus.h
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

#ifndef DBS_ODBUS_H_INCLUDE
#define DBS_ODBUS_H_INCLUDE

#include <stddef.h>
#include <stdint.h>

typedef struct odbus_s odbus ;
typedef struct odbus_message_s odbus_message ;
typedef int (*odbus_message_handler_t)(odbus_message *m, void *userdata) ;
typedef struct odbus_method_s odbus_method ;

struct odbus_method_s
{
	char const *member ;
	odbus_message_handler_t handler ;
} ;

/* Bus lifetime */

extern int odbus_open(odbus **ret, int fd) ;
extern odbus *odbus_free(odbus *bus) ;

/* Registration */

extern int odbus_set_filter(odbus *bus, odbus_message_handler_t callback, void *userdata) ;
extern int odbus_set_object(odbus *bus, char const *path, char const *interface, odbus_method const *methods, void *userdata) ;

/* Event loop and calls */

extern int odbus_process(odbus *bus) ;
extern int odbus_call(odbus *bus, odbus_message *m, unsigned int timeout_ms) ;
extern int odbus_call_method(odbus *bus, char const *path, char const *interface, char const *member, unsigned int timeout_ms, char const *types, ...) ;
extern int odbus_reply_method_return(odbus_message *call, char const *types, ...) ;
extern char const *odbus_error_name(odbus const *bus) ;

/* Messages */

extern int odbus_message_new_method_call(odbus *bus, odbus_message **m, char const *path, char const *interface, char const *member) ;
extern odbus_message *odbus_message_free(odbus_message *m) ;
extern int odbus_message_append(odbus_message *m, char const *types, ...) ;
extern int odbus_message_open_container(odbus_message *m, char type, char const *contents) ;
extern int odbus_message_close_container(odbus_message *m) ;
extern int odbus_message_read(odbus_message *m, char const *types, ...) ;
extern int odbus_message_enter_container(odbus_message *m, char type, char const *contents) ;
extern int odbus_message_exit_container(odbus_message *m) ;
extern int odbus_message_at_end(odbus_message *m) ;
extern int odbus_message_is_signal(odbus_message *m, char const *interface, char const *member) ;
extern char const *odbus_message_get_path(odbus_message *m) ;

#endif
