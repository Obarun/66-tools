/*
 * ns_net.c
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
 * Loopback configuration for a fresh network namespace. A freshly created
 * netns owns a 'lo' interface but leaves it DOWN with no address, so 127.0.0.1
 * is unreachable; like bwrap and systemd we bring it up (127.0.0.1/8 + IFF_UP).
 * Independent reimplementation of the standard rtnetlink(7) exchange.
 */

#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <oblibs/log.h>

#include "ns.h"
#include "ns_internal.h"

/* Append one rtattr of @size payload bytes to @h and return a pointer to its
 * (zeroed by the caller's memset) data area. @h->nlmsg_len is grown to cover it. */
static void *add_rta(struct nlmsghdr *h, int type, size_t size)
{
    size_t rta_len = RTA_LENGTH(size) ;
    struct rtattr *rta = (struct rtattr *)((char *)h + NLMSG_ALIGN(h->nlmsg_len)) ;

    rta->rta_type = type ;
    rta->rta_len = rta_len ;
    h->nlmsg_len = NLMSG_ALIGN(h->nlmsg_len) + rta_len ;

    return RTA_DATA(rta) ;
}

// Initialise the netlink header at @buf for a request of @size payload bytes.
static struct nlmsghdr *rtnl_request(char *buf, int type, int flags, size_t size, uint32_t seq)
{
    size_t len = NLMSG_LENGTH(size) ;
    memset(buf, 0, len) ;

    struct nlmsghdr *h = (struct nlmsghdr *)buf ;
    h->nlmsg_len = len ;
    h->nlmsg_type = type ;
    h->nlmsg_flags = flags | NLM_F_REQUEST ;
    h->nlmsg_seq = seq ;
    h->nlmsg_pid = 0 ; // let the kernel fill in our port id

    return h ;
}

// Send @h and wait for its ACK. Returns 0 on success, -1 (errno set) on failure.
static int rtnl_do(int fd, struct nlmsghdr *h)
{
    struct sockaddr_nl dst = { .nl_family = AF_NETLINK } ;

    for (;;) {

        ssize_t n = sendto(fd, h, h->nlmsg_len, 0, (struct sockaddr *)&dst, sizeof dst) ;
        if (n >= 0)
            break ;

        if (errno == EINTR)
            continue ;

        return -1 ;
    }

    char rbuf[1024] ;
    for (;;) {

        ssize_t n = recv(fd, rbuf, sizeof rbuf, 0) ;
        if (n < 0) {
            if (errno == EINTR)
                continue ;
            return -1 ;
        }

        struct nlmsghdr *r = (struct nlmsghdr *)rbuf ;
        for (; NLMSG_OK(r, (size_t)n) ; r = NLMSG_NEXT(r, n)) {

            if (r->nlmsg_seq != h->nlmsg_seq)
                continue ;

            if (r->nlmsg_type == NLMSG_ERROR) {

                struct nlmsgerr *e = NLMSG_DATA(r) ;
                if (e->error) {
                    errno = -e->error ;
                    return -1 ;
                }

                return 0 ; /* error == 0 is the ACK */
            }

            if (r->nlmsg_type == NLMSG_DONE)
                return 0 ;
        }
    }
}

void ns_loopback_up(void)
{
    log_flow() ;

    unsigned int lo = if_nametoindex("lo") ;
    if (!lo)
        log_dieusys(LOG_EXIT_SYS, "loopback: look up lo") ;

    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE) ;
    if (fd < 0)
        log_dieusys(LOG_EXIT_SYS, "loopback: open NETLINK_ROUTE socket") ;

    struct sockaddr_nl src = { .nl_family = AF_NETLINK } ;
    if (bind(fd, (struct sockaddr *)&src, sizeof src) < 0)
        log_dieusys(LOG_EXIT_SYS, "loopback: bind netlink socket") ;

    char buf[1024] ;
    uint32_t seq = 0 ;

    /* 1. add 127.0.0.1/8 to lo */
    struct nlmsghdr *h = rtnl_request(buf, RTM_NEWADDR, NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK, sizeof(struct ifaddrmsg), seq++) ;
    struct ifaddrmsg *addr = NLMSG_DATA(h) ;
    addr->ifa_family = AF_INET ;
    addr->ifa_prefixlen = 8 ;
    addr->ifa_flags = IFA_F_PERMANENT ;
    addr->ifa_scope = RT_SCOPE_HOST ;
    addr->ifa_index = lo ;

    *(struct in_addr *)add_rta(h, IFA_LOCAL, sizeof(struct in_addr)) = (struct in_addr){ .s_addr = htonl(INADDR_LOOPBACK) } ;
    *(struct in_addr *)add_rta(h, IFA_ADDRESS, sizeof(struct in_addr)) = (struct in_addr){ .s_addr = htonl(INADDR_LOOPBACK) } ;

    if (rtnl_do(fd, h) < 0)
        log_dieusys(LOG_EXIT_SYS, "loopback: RTM_NEWADDR") ;

    /* 2. bring lo up */
    h = rtnl_request(buf, RTM_NEWLINK, NLM_F_ACK, sizeof(struct ifinfomsg), seq++) ;
    struct ifinfomsg *link = NLMSG_DATA(h) ;
    link->ifi_family = AF_UNSPEC ;
    link->ifi_index = (int)lo ;
    link->ifi_flags = IFF_UP ;
    link->ifi_change = IFF_UP ;

    if (rtnl_do(fd, h) < 0)
        log_dieusys(LOG_EXIT_SYS, "loopback: RTM_NEWLINK") ;

    close(fd) ;
}
