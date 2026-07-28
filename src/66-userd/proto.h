/*
 * proto.h
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

#ifndef USERD_PROTO_H
#define USERD_PROTO_H

#include <stdint.h>

/**
 * @brief Maximum payload size, in bytes, of a single message.
 *
 * Upper bound on the `size` field of `proto_header_t`. The daemon rejects any
 * incoming frame whose declared `size` exceeds this value (`handle_client()`
 * replies `PROTO_ERROR` "payload too large" and drops the connection); the
 * client rejects a reply whose `size` exceeds its own receive buffer. Both
 * sides size their payload buffers from this constant. A serialized session is
 * roughly 200 bytes, so 4096 leaves ample headroom for one record per frame.
 */
#define PROTO_PAYLOAD_MAX 4096

/**
 * @brief Client -> daemon request opcodes (sent in `proto_header_t.opcode`).
 *
 * - `PROTO_REGISTER_SESSION`: register a new login session. Payload is the
 *   session context as a `KEY=value` block (UID, LEADER, VTNR, REMOTE, TYPE,
 *   CLASS, TTY, DISPLAY, SEAT, SERVICE, REMOTE_USER, REMOTE_HOST; the
 *   daemon-assigned LEADER_STARTTIME/STATE/TIMESTAMP are not sent by the
 *   client). Reply is `PROTO_SESSION_REGISTERED` on success, `PROTO_ERROR` on
 *   failure. Privileged: the daemon rejects this opcode from a non-root peer
 *   (`SO_PEERCRED` check).
 * - `PROTO_RELEASE_SESSION`: release a session by id. Payload is `ID=<id>`.
 *   Reply is `PROTO_OK` on success, `PROTO_ERROR` on failure. Privileged:
 *   rejected from a non-root peer.
 * - `PROTO_LIST_SESSIONS`: list all session ids. No payload. Reply is
 *   `PROTO_SESSION_LIST`.
 * - `PROTO_GET_SESSION`: fetch one session record by id. Payload is `ID=<id>`.
 *   Reply is `PROTO_SESSION_INFO` on success, `PROTO_ERROR` if unknown.
 * - `PROTO_POWER`: request a power action (poweroff/reboot/halt) for the machine.
 *   Payload is `ACTION=poweroff|reboot|halt`. NON-privileged opcode: a non-root
 *   peer may send it; the daemon applies its power policy (caller is an active
 *   local session, the shutdown.allow whitelist, the multiple-sessions guard)
 *   before delegating the mechanism to 66's init. Reply is `PROTO_OK` if the
 *   action was accepted, `PROTO_ERROR` (with a reason) if denied or on failure.
 */
#define PROTO_REGISTER_SESSION 1
#define PROTO_RELEASE_SESSION  2
#define PROTO_LIST_SESSIONS    3
#define PROTO_GET_SESSION      4
#define PROTO_POWER            5    // payload: ACTION=poweroff|reboot|halt

/**
 * @brief Daemon -> client reply opcodes (returned in `proto_header_t.opcode`).
 *
 * - `PROTO_SESSION_REGISTERED`: success reply to `PROTO_REGISTER_SESSION`.
 *   Payload is the assigned session id (raw id string, not NUL-terminated on
 *   the wire).
 * - `PROTO_OK`: generic success reply (e.g. to `PROTO_RELEASE_SESSION`). No
 *   payload (`size` == 0).
 * - `PROTO_SESSION_LIST`: reply to `PROTO_LIST_SESSIONS`. Payload is the session
 *   ids, one per line, `\n`-separated. If the full list would exceed one frame
 *   it is truncated to whole ids and a warning is logged daemon-side.
 * - `PROTO_SESSION_INFO`: reply to `PROTO_GET_SESSION`. Payload is the
 *   serialized session record as a `KEY=value` block.
 * - `PROTO_ERROR`: failure reply to any request. Payload is a human-readable
 *   error message (not NUL-terminated on the wire; its length is `size`).
 */
#define PROTO_SESSION_REGISTERED 128    // payload: assigned session id
#define PROTO_OK                 129    // no payload
#define PROTO_SESSION_LIST       130    // payload: one session id per line
#define PROTO_SESSION_INFO       131    // payload: serialized session (KEY=value)
#define PROTO_ERROR              255    // payload: human-readable message

/**
 * @struct proto_header_s
 * @brief Fixed binary frame header that prefixes every protocol message.
 *
 * Every request and every reply begins with this header, sent as raw struct
 * bytes (no padding-portable serialization, no byte-order conversion — the
 * channel is same-host, same-endianness). The header is immediately followed by
 * `size` payload bytes. The two-byte fields keep the layout naturally packed on
 * the supported platforms.
 *
 * @param opcode
 * The message type. For a request this is one of the `PROTO_*` client-to-daemon
 * opcodes (`PROTO_REGISTER_SESSION`, `PROTO_RELEASE_SESSION`,
 * `PROTO_LIST_SESSIONS`, `PROTO_GET_SESSION`). For a reply it is one of the
 * daemon-to-client opcodes (`PROTO_SESSION_REGISTERED`, `PROTO_OK`,
 * `PROTO_SESSION_LIST`, `PROTO_SESSION_INFO`, `PROTO_ERROR`). The field is 16
 * bits wide; all defined opcodes fit in its low 8 bits. The daemon replies
 * `PROTO_ERROR` "unknown opcode" to any request value it does not recognize.
 *
 * @param size
 * The number of payload bytes that follow this header. `0` denotes a
 * header-only message. Must not exceed `PROTO_PAYLOAD_MAX`; a larger declared
 * size is rejected by the receiver. This count does not include any NUL
 * terminator — none is sent on the wire.
 */
typedef struct proto_header_s proto_header_t ;
struct proto_header_s {
    uint16_t opcode ;
    uint16_t size ;     // number of payload bytes following the header
} ;

// Zero-initializer for a `proto_header_t` (opcode 0, size 0).
#define PROTO_HEADER_ZERO { 0, 0 }

/**
 * @brief Run one client transaction against the userd daemon socket.
 *
 * Opens a fresh connection to the Unix socket at @path, writes one request frame
 * (`opcode` + @plen payload bytes), reads exactly one reply frame, and closes the
 * connection. The reply opcode is returned in @rop and the reply payload
 * (never NUL-terminated on the wire) is copied into @rbuf with its byte count in @rlen.
 *
 * @param[in] path    Daemon socket path to connect to.
 * @param[in] opcode  Request opcode (`PROTO_*` client-to-daemon).
 * @param[in] payload Request payload bytes; may be NULL when @plen is 0.
 * @param[in] plen    Number of payload bytes to send.
 * @param[out] rop    Receives the reply opcode.
 * @param[out] rbuf   Receive buffer for the reply payload; must hold @rcap bytes.
 * @param[in] rcap    Capacity of @rbuf. A reply larger than @rcap is rejected.
 * @param[out] rlen   Receives the reply payload byte count.
 *
 * @return 1 on a completed transaction (@rop, @rbuf, @rlen set).
 * @return 0 on any failure (socket create/connect, short read/write, or a reply
 *         exceeding @rcap); the connection is closed and errno is left by the
 *         failing call.
 */
extern int userd_call(char const *path, uint16_t opcode, char const *payload, uint16_t plen, uint16_t *rop, char *rbuf, uint16_t rcap, uint16_t *rlen) ;

#endif
