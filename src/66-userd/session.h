/*
 * session.h
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

#ifndef USERD_SESSION_H
#define USERD_SESSION_H

#include <sys/types.h>
#include <stddef.h>
#include <stdint.h>

#include <oblibs/hash.h>
#include <oblibs/sse.h>
#include <oblibs/strbuf.h>

#include "constants.h"
#include "userd.h"

/**
 * @brief Session medium. Serialized as the `TYPE` key via session_type_str().
 *
 * The integer value is the index into session.c's `session_type_table`, so it
 * doubles as the map to the wire/disk string. `SESSION_TYPE_UNSPECIFIED` is the
 * zero default and the value session_type_from() returns for any unknown string.
 */
enum session_type_e
{
    SESSION_TYPE_UNSPECIFIED = 0,   // "unspecified": no medium known yet
    SESSION_TYPE_TTY,               // "tty": text console / pty
    SESSION_TYPE_X11,               // "x11": X11 graphical display
    SESSION_TYPE_WAYLAND            // "wayland": Wayland graphical session
} ;
typedef enum session_type_e session_type_t ;

/**
 * @brief Session class. Serialized as the `CLASS` key via session_class_str().
 *
 * The integer value is the index into session.c's `session_class_table`, so it
 * doubles as the map to the wire/disk string. `SESSION_CLASS_USER` is the zero
 * default and the value session_class_from() returns for any unknown string.
 */
enum session_class_e
{
    SESSION_CLASS_USER = 0,         // "user": ordinary interactive user session
    SESSION_CLASS_GREETER,          // "greeter": display-manager login screen
    SESSION_CLASS_LOCK_SCREEN,      // "lock-screen": screen locker
    SESSION_CLASS_BACKGROUND        // "background": non-interactive (e.g. cron)
} ;
typedef enum session_class_e session_class_t ;

/**
 * @brief Session lifecycle state. Serialized as the `STATE` key via
 *        session_state_str() (a daemon-written key, absent in a REGISTER payload).
 *
 * The integer value is the index into session.c's `session_state_table`, so it
 * doubles as the map to the wire/disk string. `SESSION_STATE_OPENING` is the zero
 * default and the value session_state_from() returns for any unknown string.
 */
enum session_state_e
{
    SESSION_STATE_OPENING = 0,  // registered, services not yet up
    SESSION_STATE_ONLINE,       // services up, not foreground
    SESSION_STATE_ACTIVE,       // foreground on its seat
    SESSION_STATE_CLOSING       // release in progress
} ;
typedef enum session_state_e session_state_t ;

/**
 * @struct session_s
 * @brief A tracked login session, indexed by its decimal string id.
 *
 * One record per session the daemon tracks. String fields are fixed inline
 * buffers (no heap per field); an empty string (`field[0] == 0`) means "absent"
 * and is omitted from the serialized form by sf_str(). A value too long for its
 * buffer is rejected at REGISTER (session_set() / session_from_env() return 0,
 * errno `EOVERFLOW`), never truncated. The node lives in an intrusive hash table
 * keyed on @id; allocate with session_new() and release with session_free().
 *
 * @param id
 * Session id, NUL-terminated, capacity `SESSION_ID_MAX`. The hash key. Set once
 * by session_new() (which rejects an empty or over-long id); never derived from the
 * `UID`/etc. fields.
 *
 * @param uid
 * Owning user's uid. Deserialized from the `UID` key.
 *
 * @param leader
 * Session leader pid as reported by PAM. Deserialized from the `LEADER` key.
 *
 * @param leaderfd
 * pidfd on @leader, armed in the event loop for dead-leader garbage collection;
 * -1 when none. Initialized to -1 by session_new(); NOT touched by the
 * (de)serializer.
 *
 * @param leader_starttime
 * @leader start time in clock ticks since boot (`/proc/<pid>/stat` field 22),
 * used to detect pid reuse. Deserialized from the `LEADER_STARTTIME` key (a
 * daemon-written key, absent in a REGISTER payload, present on disk).
 *
 * @param leaderw
 * Event-loop watcher arming @leaderfd. Owned by the daemon's
 * event loop; NOT touched by the (de)serializer.
 *
 * @param vtnr
 * Virtual-terminal number, 0 if none. Deserialized from the `VTNR` key; may also
 * be derived by session_normalize() from a `ttyN` @tty.
 *
 * @param remote
 * Boolean (0/1): the session is remote. Deserialized from the `REMOTE` key (any
 * non-zero scanned value becomes 1).
 *
 * @param type
 * Session medium (`session_type_t`). Deserialized from the `TYPE` key; may be
 * defaulted by session_normalize().
 *
 * @param class
 * Session class (`session_class_t`). Deserialized from the `CLASS` key; may be
 * defaulted by session_normalize().
 *
 * @param state
 * Lifecycle state (`session_state_t`). Deserialized from the `STATE` key (a
 * daemon-written key); session_new() initializes it to `SESSION_STATE_OPENING`.
 *
 * @param tty
 * PAM_TTY, NUL-terminated, capacity `SESSION_FIELD_MAX`. Deserialized from the
 * `TTY` key; rewritten by session_normalize() (colon-displays moved to @display,
 * `cron`/`ssh` mapped, `/dev/` prefix stripped).
 *
 * @param display
 * PAM_XDISPLAY (e.g. ":0"), NUL-terminated, capacity `SESSION_FIELD_MAX`.
 * Deserialized from the `DISPLAY` key.
 *
 * @param seat
 * Seat id (e.g. "seat0"), NUL-terminated, capacity `SESSION_FIELD_MAX`.
 * Deserialized from the `SEAT` key; may be set to "seat0" by session_normalize().
 *
 * @param service
 * PAM service name, NUL-terminated, capacity `SESSION_FIELD_MAX`. Deserialized
 * from the `SERVICE` key.
 *
 * @param remote_user
 * PAM_RUSER, NUL-terminated, capacity `SESSION_FIELD_MAX`. Deserialized from the
 * `REMOTE_USER` key.
 *
 * @param remote_host
 * PAM_RHOST, NUL-terminated, capacity `SESSION_FIELD_MAX`. Deserialized from the
 * `REMOTE_HOST` key.
 *
 * @param timestamp
 * Creation time in realtime microseconds. Deserialized from the `TIMESTAMP` key
 * (a daemon-written key, absent in a REGISTER payload, present on disk).
 *
 * @param node
 * Intrusive hash node; the table is indexed by @id. Managed by the hash module.
 */
typedef struct session_s session_t, *session_t_ref;
struct session_s {
    char id[SESSION_ID_MAX] ; // hash key
    uid_t uid ;
    pid_t leader ;
    int leaderfd ;  // pidfd on leader for GC, -1 if none
    unsigned long leader_starttime ; // leader start time (ticks since boot, /proc/<pid>/stat field 22)
    sse_watcher_t leaderw ;
    uint32_t vtnr ; // VT number, 0 if none
    uint8_t remote ; // boolean: remote session
    session_type_t type ;
    session_class_t class ;
    session_state_t state ;
    char tty[SESSION_FIELD_MAX] ;
    char display[SESSION_FIELD_MAX] ;
    char seat[SESSION_FIELD_MAX] ;
    char service[SESSION_FIELD_MAX] ;
    char remote_user[SESSION_FIELD_MAX] ;
    char remote_host[SESSION_FIELD_MAX] ;
    uint64_t timestamp ;
    hash_node_t node ;
};

/**
 * @brief Allocate and minimally initialize a session keyed on @id.
 *
 * Validates @id, then `calloc`s a zeroed session, copies @id into the inline
 * @id buffer, sets @leaderfd to -1 and @state to `SESSION_STATE_OPENING`. All
 * other fields are left zero (empty strings = absent). The caller owns the
 * returned node and must release it with session_free(); it is not yet inserted
 * in any hash table.
 *
 * @param[in] id  Session id, must be non-NULL, non-empty, and shorter than
 *                `SESSION_ID_MAX` (the NUL must fit). Copied, not retained.
 *
 * @return Pointer to the new session on success.
 * @return 0 (NULL) on error:
 *      - errno is set to EINVAL if @id is NULL, empty, or its length is
 *        >= `SESSION_ID_MAX`.
 *      - errno is set to ENOMEM if the allocation fails.
 */
extern session_t *session_new(char const *id) ;

/**
 * @brief Free a session allocated by session_new().
 *
 * Calls `free(s)`. Holds no other resource: @leaderfd and @leaderw are owned by
 * the daemon's event loop and are NOT closed/disarmed here — the caller must have
 * released them beforehand. A NULL @s is a no-op (`free(NULL)`). The node must not
 * still be linked in a hash table.
 *
 * @param[in] s  Session to free, or NULL.
 */
extern void session_free(session_t *s) ;

/**
 * @brief Copy @value into the inline string field @field of capacity @cap, or
 *        clear it.
 *
 * A NULL or empty @value clears the field (`field[0] = 0`) and succeeds. Otherwise
 * @value (including its NUL) must fit in @cap bytes; it is rejected, never
 * truncated, if it does not.
 *
 * @param[out] field  Destination inline buffer of at least @cap bytes. Must not
 *                    be NULL (dereferenced unconditionally).
 * @param[in]  cap    Capacity of @field in bytes, including the terminating NUL.
 * @param[in]  value  Source string, or NULL/empty to clear @field.
 *
 * @return 1 on success (value copied, or field cleared).
 * @return 0 if @value does not fit; errno is set to EOVERFLOW. @field is left
 *         unchanged in that case.
 */
extern int session_set(char *field, size_t cap, char const *value) ;

/**
 * @brief Derive the canonical SEAT/VTNR/TYPE/CLASS of a deserialized session.
 *
 * Brings an already-populated @s to the daemon's canonical form (logind parity)
 * so the daemon — not the upstream greeter/agetty/sshd —
 * owns the truth for these fields. Explicit values always win over derived ones.
 * In order it: rewrites @tty (a colon-display is moved into @display unless one is
 * already set, then @tty is cleared; `cron` -> background/unspecified, `ssh` ->
 * user/tty, both clearing @tty; a `/dev/` prefix is stripped); derives @vtnr from
 * a fully numeric `ttyN` @tty when @vtnr is still 0; puts a local session (a VC
 * with a vtnr, or a local @display) with no explicit @seat on "seat0"; clears
 * @vtnr unless @seat is "seat0"; defaults @type from @display (x11) or @tty (tty)
 * only when it is `SESSION_TYPE_UNSPECIFIED`; and, only when @class_provided is 0,
 * sets @class to background for an unspecified type, user otherwise.
 *
 * Pure by design: no `getenv`, no I/O, and in particular no query to the X server
 * for a display's VTNR. A local X session that carried no `XDG_VTNR` therefore
 * ends up on seat0 with vtnr 0.
 *
 * @param[in,out] s  Session to normalize in place. Must not be NULL.
 * @param[in] class_provided  1 iff the REGISTER payload carried a `CLASS` key, 0
 *                    otherwise. Required because the @class enum cannot distinguish
 *                    a defaulted `SESSION_CLASS_USER` from an explicit `CLASS=user`;
 *                    when 0, @class is overwritten by the derived default.
 *
 * @note Always succeeds (no return value). The internal session_set() calls copy
 *       between equal-capacity fields or the literal "seat0", so they never fail.
 */
extern void session_normalize(session_t *s, int class_provided) ;

/**
 * @brief Map a `session_type_t` to its canonical string.
 *
 * @param[in] t  An in-range `session_type_t`.
 * @return The matching table string ("unspecified", "tty", "x11", "wayland").
 * @note No bounds check: a value outside the enum reads past `session_type_table`
 *       (undefined). Pass only values produced by the API.
 */
extern char const *session_type_str(session_type_t t) ;

/**
 * @brief Map a string to a `session_type_t`.
 *
 * @param[in] s  String to look up. Must not be NULL (`strcmp`'d unconditionally).
 * @return The matching `session_type_t` for an exact table match; otherwise
 *         `SESSION_TYPE_UNSPECIFIED` (an unknown string is never an error here).
 */
extern session_type_t session_type_from(char const *s) ;

/**
 * @brief Map a `session_class_t` to its canonical string.
 *
 * @param[in] c  An in-range `session_class_t`.
 * @return The matching table string ("user", "greeter", "lock-screen",
 *         "background").
 * @note No bounds check: a value outside the enum reads past
 *       `session_class_table` (undefined). Pass only values produced by the API.
 */
extern char const *session_class_str(session_class_t c) ;

/**
 * @brief Map a string to a `session_class_t`.
 *
 * @param[in] s  String to look up. Must not be NULL (`strcmp`'d unconditionally).
 * @return The matching `session_class_t` for an exact table match; otherwise
 *         `SESSION_CLASS_USER` (an unknown string is never an error here).
 */
extern session_class_t session_class_from(char const *s) ;

/**
 * @brief Map a `session_state_t` to its canonical string.
 *
 * @param[in] st  An in-range `session_state_t`.
 * @return The matching table string ("opening", "online", "active", "closing").
 * @note No bounds check: a value outside the enum reads past
 *       `session_state_table` (undefined). Pass only values produced by the API.
 */
extern char const *session_state_str(session_state_t st) ;

/**
 * @brief Map a string to a `session_state_t`.
 *
 * @param[in] s  String to look up. Must not be NULL (`strcmp`'d unconditionally).
 * @return The matching `session_state_t` for an exact table match; otherwise
 *         `SESSION_STATE_OPENING` (an unknown string is never an error here).
 */
extern session_state_t session_state_from(char const *s) ;

/**
 * @brief Serialize @s as a `KEY=value` envfile block into @sb.
 *
 * Resets @sb (`sb->len = 0`) and appends one field per session attribute in a
 * fixed order via sf_u64()/sf_str(): `UID`, `LEADER`, `LEADER_STARTTIME`, `SEAT`,
 * `VTNR`, `TTY`, `DISPLAY`, `TYPE`, `CLASS`, `REMOTE`, `SERVICE`, `REMOTE_USER`,
 * `REMOTE_HOST`, `STATE`, `TIMESTAMP`. Empty string fields are omitted (sf_str()
 * skips them); every numeric field is always emitted, including 0. Values
 * containing `"` or `\` are double-quoted and escaped. The block is the same form
 * used on the wire and on disk.
 *
 * @param[in]  s   Session to serialize. Must not be NULL.
 * @param[out] sb  Destination buffer; reset before writing, left NUL-terminated
 *                 with `sb->len` excluding the NUL. Must not be NULL.
 *
 * @return 1 on success (block written).
 * @return 0 if growing @sb fails; errno is set by the underlying allocator to
 *         ENOMEM (the strbuf ERANGE overflow path is unreachable for these
 *         bounded fields). On failure the partially appended bytes are NOT rolled
 *         back; discard @sb.
 */
extern int session_serialize(session_t const *s, strbuf *sb) ;

/**
 * @brief Populate @s from the parsed envfile entries in @env.
 *
 * Walks @env, a buffer of `KEY=value` entries each terminated by a NUL (as
 * produced by `environ_merge_*`), in a single pass; key order is irrelevant.
 * Each entry is split in place at its first `=` (so @env is mutated). Recognized
 * keys are dispatched: `UID`/`LEADER` parsed as uid/pid, `LEADER_STARTTIME`/
 * `TIMESTAMP` scanned as u64, `VTNR` as u32, `REMOTE` as a 0/1 boolean, `TYPE`/
 * `CLASS`/`STATE` mapped via the *_from() tables, and the string fields `SEAT`,
 * `TTY`, `DISPLAY`, `SERVICE`, `REMOTE_USER`, `REMOTE_HOST` copied via
 * session_set(). An entry with no `=`, and any unknown key, is ignored. A numeric
 * scan that fails leaves that field unchanged and is not an error. Fields absent
 * from @env keep whatever value @s already held.
 *
 * @param[out]    s    Session to fill. Must not be NULL. Only the keys present in
 *                     @env are written; other fields are left as-is.
 * @param[in,out] env  Parsed entries; mutated in place (each first `=` is
 *                     NUL'd). Must not be NULL.
 *
 * @return 1 on success (all recognized entries dispatched).
 * @return 0 on the first over-long string value; errno is set to EOVERFLOW by the
 *         underlying session_set(). Parsing stops at that entry; fields dispatched
 *         before it are already written.
 */
extern int session_from_env(session_t *s, strbuf *env) ;

/**
 * @brief Populate @s from a NUL-terminated `KEY=value` text block (wire payload
 *        or on-disk state file).
 *
 * Parses @block through `environ_merge_string` (envfile grammar: trims, merges)
 * into a temporary buffer, then dispatches it through session_from_env(). The
 * daemon-written keys (`LEADER_STARTTIME`, `STATE`, `TIMESTAMP`) are absent in a
 * REGISTER payload and present on disk; either way, absent keys leave @s's
 * existing field values intact.
 *
 * @param[out] s      Session to fill. Must not be NULL. Only keys present in
 *                    @block are written.
 * @param[in]  block  NUL-terminated `KEY=value` block. Must not be NULL
 *                    (`environ_merge_string` returns 0 / errno EINVAL on NULL).
 *
 * @return 1 on success.
 * @return 0 on error. errno is whatever the failing stage left:
 *      - from `environ_merge_string`: EINVAL if @block is NULL, ENAMETOOLONG if it
 *        is >= MAXENV bytes, EOVERFLOW on an internal buffer copy failure, or the
 *        errno left by its trim/merge step.
 *      - from session_from_env(): EOVERFLOW on an over-long string value.
 */
extern int session_deserialize(session_t *s, char const *block) ;

#endif
