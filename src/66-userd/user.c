/*
 * user.c
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

#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <oblibs/io.h>
#include <oblibs/socket.h>

#include "userd.h"
#include "proto.h"

user_t *user_new(uid_t uid)
{
    user_t *u = calloc(1, sizeof(*u)) ;
    if (!u)
        return (errno = ENOMEM, (user_t *)0) ;

    u->uid = uid ;
    u->state = USER_STATE_OFFLINE ;
    u->guardianfd = -1 ;
    u->readyw.fd = -1 ;

    return u ;
}

void user_free(user_t *u)
{
    free(u) ;
}

int userd_call(char const *path, uint16_t opcode, char const *payload, uint16_t plen, uint16_t *rop, char *rbuf, uint16_t rcap, uint16_t *rlen)
{
    int fd = socketunix_create(O_CLOEXEC) ;
    if (fd < 0)
        return 0 ;

    if (socketunix_connect(fd, path) < 0)
        return (close(fd), 0) ;

    proto_header_t h = { opcode, plen } ;
    if (io_allwrite(fd, (char *)&h, sizeof(h)) != sizeof(h) ||
        (plen && io_allwrite(fd, (char *)payload, plen) != plen))
            return (close(fd), 0) ;

    proto_header_t rh ;
    if (io_allread(fd, (char *)&rh, sizeof(rh)) != sizeof(rh) || rh.size > rcap)
        return (close(fd), 0) ;

    if (rh.size && io_allread(fd, rbuf, rh.size) != rh.size)
        return (close(fd), 0) ;

    close(fd) ;
    *rop = rh.opcode ;
    *rlen = rh.size ;

    return 1 ;
}