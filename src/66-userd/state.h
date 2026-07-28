/*
 * state.h
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

#ifndef USERD_STATE_H
#define USERD_STATE_H

#include <sys/types.h>
#include <stddef.h>

#include "session.h"
#include "userd.h"

/**
 * @brief Create the on-disk state tree under @statedir.
 *
 * Creates `@statedir/sessions` and `@statedir/users` (and any missing parent
 * components of @statedir itself), each with mode `0755`, via
 * `dir_create_parent()`. An already-existing directory is not an error
 * (`EEXIST` from the underlying `mkdir()` is swallowed). Call once at daemon
 * startup before any save/load.
 *
 * @param[in] statedir Root of the state tree (`USERD_STATEDIR`); must not be NULL.
 * @return Status code:
 *      - 1 on success (both subdirectories exist afterwards)
 *      - 0 on error; errno is set to the `mkdir()` errno of the first component
 *        that could not be created (e.g., `EACCES`, `ENOSPC`, `ENOTDIR`), with
 *        `EEXIST` excluded since pre-existing directories are accepted
 */
extern int state_init(char const *statedir) ;

/**
 * @brief Persist a session to `@statedir/sessions/<id>`.
 *
 * Serializes @s with `session_serialize()` into the `KEY=value` envfile form and
 * writes it to `@statedir/sessions/` under the filename `s->id` via
 * `file_write()`. The write is NOT atomic: the destination is opened with
 * `O_TRUNC | O_CREAT` and truncated up front, so a process crash between truncate
 * and the final write — or a `write()` that itself fails — leaves a partial or
 * empty file (it is not unlinked).
 *
 * @param[in] statedir Root of the state tree; must not be NULL.
 * @param[in] s        Session to persist; must not be NULL and `s->id` must be a
 *                     non-empty, valid filename component.
 * @return Status code:
 *      - 1 on success
 *      - 0 on error; errno is set. Either `session_serialize()` failed building
 *        the buffer (allocation failure, errno from the underlying `strbuf`
 *        growth), or the underlying `file_write()` failed and forwards its
 *        `open()`/`write()` errno (e.g., `EACCES`, `ENOSPC`, `ENOENT` if
 *        `@statedir/sessions` does not exist).
 */
extern int state_save_session(char const *statedir, session_t const *s) ;

/**
 * @brief Load `@statedir/sessions/<id>` into @s.
 *
 * Reads the envfile at `@statedir/sessions/<id>` and dispatches its `KEY=value`
 * entries onto @s via `session_from_env()`. The on-disk block is this daemon's
 * own output and carries the daemon-assigned fields (`LEADER_STARTTIME`, `STATE`,
 * `TIMESTAMP`), which are restored along with the rest. Keys absent from the file
 * leave the corresponding @s fields untouched; @s is not zeroed first, so the
 * caller passes a struct already initialized (e.g., with its `id` set).
 *
 * @param[in]     statedir Root of the state tree; must not be NULL.
 * @param[in]     id       Session id (filename component); must not be NULL.
 * @param[in,out] s        Destination; must not be NULL. Fields named by present
 *                         keys are overwritten, others are left as supplied.
 * @return Status code:
 *      - 1 on success
 *      - 0 on error; errno is set.
 * @retval 0 errno is set to `ENOENT` if `@statedir/sessions/<id>` does not exist
 *         (a missing file is an error), `EINVAL` if the file exists but is empty,
 *         `ENAMETOOLONG` if it exceeds the envfile size cap (`MAXENV`), or another
 *         errno forwarded from the underlying read (`environ_merge_file()`).
 * @retval 0 errno is set to `EOVERFLOW` if a string value in the file
 *         (`SEAT`, `TTY`, `DISPLAY`, `SERVICE`, `REMOTE_USER`, `REMOTE_HOST`)
 *         is too long for its fixed field; `session_from_env()` rejects it rather
 *         than truncating.
 */
extern int state_load_session(char const *statedir, char const *id, session_t *s) ;

/**
 * @brief Remove `@statedir/sessions/<id>`.
 *
 * Unlinks the session file. A missing file is treated as success.
 *
 * @param[in] statedir Root of the state tree; must not be NULL.
 * @param[in] id       Session id (filename component); must not be NULL.
 * @return Status code:
 *      - 1 on success, or if the file was already absent (`unlink()` errno
 *        `ENOENT` is swallowed)
 *      - 0 on error; errno is set to the `unlink()` errno (e.g., `EACCES`,
 *        `ENOTDIR`) for any failure other than `ENOENT`
 */
extern int state_remove_session(char const *statedir, char const *id) ;

/**
 * @brief Persist a user to `@statedir/users/<uid>`.
 *
 * Writes a `KEY=value` envfile carrying `NAME`, `GID`, `STATE`, `NSESSIONS`,
 * then (only when `u->guardian_pid > 0`) `GUARDIAN_PID`/`GUARDIAN_STARTTIME`, and
 * finally `TIMESTAMP`. The session-id list is supplied externally as @sessions
 * and written verbatim under the `SESSIONS` key, because `user_t` has no
 * in-memory sessions field. The filename is the decimal @uid. The write is NOT
 * atomic (`file_write()` opens with `O_TRUNC | O_CREAT` and truncates up
 * front; a crash mid-write or a failed `write()` can leave a partial file).
 *
 * The guardian identity is persisted so a restart can re-adopt the supervised
 * guardian (re-pidfd plus start-time check; the two keys are absent
 * when no guardian exists.
 *
 * @param[in] statedir Root of the state tree; must not be NULL.
 * @param[in] u        User to persist; must not be NULL.
 * @param[in] sessions Space-separated list of the user's live session ids, or
 *                     NULL/empty. NULL and "" both write no `SESSIONS` value
 *                     (`sf_str()` emits nothing for an empty string).
 * @return Status code:
 *      - 1 on success
 *      - 0 on error; errno is set. Either a `sf_str()`/`sf_u64()` field-append
 *        failed (allocation failure, errno from `strbuf` growth), or the
 *        underlying `file_write()` failed and forwards its `open()`/
 *        `write()` errno (e.g., `EACCES`, `ENOSPC`, `ENOENT` if
 *        `@statedir/users` does not exist).
 */
extern int state_save_user(char const *statedir, user_t const *u, char const *sessions) ;

/**
 * @brief Load `@statedir/users/<uid>` into @u.
 *
 * Reads the envfile at `@statedir/users/<uid>` and parses its keys onto @u:
 * `NAME` (into `u->name`), `GID`, `STATE`, `NSESSIONS`, `TIMESTAMP`. Sets
 * `u->uid` to @uid up front. The counterpart of state_save_user(); it
 * externalizes the session-id list the same way state_save_user() stored it.
 *
 * Numeric/enum keys are parsed best-effort: a malformed `GID`/`NSESSIONS`/
 * `TIMESTAMP` value leaves the corresponding @u field unchanged and is NOT an
 * error, and an unrecognized `STATE` string maps to `USER_STATE_OFFLINE` via
 * `user_state_from()`. Keys absent from the file leave the matching @u field
 * untouched (@u is not zeroed first).
 *
 * @param[in]     statedir    Root of the state tree; must not be NULL.
 * @param[in]     uid         User id; its decimal form is the filename.
 * @param[in,out] u           Destination; must not be NULL. `u->uid` is set; other
 *                            fields are overwritten only for keys present in the file.
 * @param[out]    sessions    If non-NULL, receives the space-separated `SESSIONS`
 *                            value ("" when the key is absent), NUL-terminated.
 *                            May be NULL to skip reading the list.
 * @param[in]     sessionsmax Capacity in bytes of @sessions (including the NUL);
 *                            ignored when @sessions is NULL.
 * @return Status code:
 *      - 1 on success
 *      - 0 on error; errno is set.
 * @retval 0 errno is set to `ENOENT` if `@statedir/users/<uid>` does not exist
 *         (a missing user file is an error), `EINVAL` if the file exists but is
 *         empty, `ENAMETOOLONG` if it exceeds the envfile size cap (`MAXENV`), or
 *         another errno forwarded from `environ_merge_file()`.
 * @retval 0 errno is set to `EOVERFLOW` if the `NAME` value does not fit
 *         `u->name`, or if the `SESSIONS` value (including its NUL) does not fit
 *         @sessionsmax; the value is rejected rather than truncated.
 */
extern int state_load_user(char const *statedir, uid_t uid, user_t *u, char *sessions, size_t sessionsmax) ;

/**
 * @brief Read ONLY the persisted guardian identity of `@statedir/users/<uid>`.
 *
 * Reads the user file and extracts solely `GUARDIAN_PID` and
 * `GUARDIAN_STARTTIME` into @pid and @starttime, without touching any in-memory
 * `user_t`. Used at restart reconciliation to re-adopt a supervised guardian.
 * Both outputs are pre-set to 0 before parsing, so when the keys
 * are absent (no guardian was running) `*@pid` is 0 — which is NOT an error.
 * `GUARDIAN_PID` is parsed best-effort: a malformed value leaves `*@pid` at 0.
 *
 * @param[in]  statedir  Root of the state tree; must not be NULL.
 * @param[in]  uid       User id; its decimal form is the filename.
 * @param[out] pid       Receives `GUARDIAN_PID`, or 0 if absent/unparsable;
 *                       must not be NULL.
 * @param[out] starttime Receives `GUARDIAN_STARTTIME`, or 0 if absent; must not
 *                       be NULL.
 * @return Status code:
 *      - 1 on success (the user file was read; `*@pid` may legitimately be 0)
 *      - 0 on error; errno is set.
 * @retval 0 errno is set to `ENOENT` if `@statedir/users/<uid>` does not exist
 *         (a missing file is an error), `EINVAL` if the file exists but is empty,
 *         `ENAMETOOLONG` if it exceeds the envfile size cap (`MAXENV`), or another
 *         errno forwarded from `environ_merge_file()`.
 */
extern int state_load_user_guardian(char const *statedir, uid_t uid, pid_t *pid, unsigned long *starttime) ;

/**
 * @brief Remove `@statedir/users/<uid>`.
 *
 * Unlinks the user file (named by the decimal @uid). A missing file is treated
 * as success.
 *
 * @param[in] statedir Root of the state tree; must not be NULL.
 * @param[in] uid      User id; its decimal form is the filename.
 * @return Status code:
 *      - 1 on success, or if the file was already absent (`unlink()` errno
 *        `ENOENT` is swallowed)
 *      - 0 on error; errno is set to the `unlink()` errno (e.g., `EACCES`,
 *        `ENOTDIR`) for any failure other than `ENOENT`
 */
extern int state_remove_user(char const *statedir, uid_t uid) ;

#endif
