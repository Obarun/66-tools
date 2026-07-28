# pam_userd

*pam_userd.so* is the PAM module of [66-userd](66-userd.html). It reports the opening and the closing of a login session to the daemon over a Unix socket.

**This step is required.** Installing 66-tools puts `pam_userd.so` in `%%userd_pam_dir%%` and changes nothing else. Until the module is referenced from the PAM login stack of the system, no session is ever reported, the daemon never sees a login, and no user service is ever started. The package deliberately does not wire this up—see [Packaging rules](#packaging-rules).

## Interface

```
-session   optional   pam_userd.so
```

| PAM hook | Action |
|---|---|
| `pam_sm_open_session` | sends **REGISTER** to the daemon with the session context, stores the returned session id, exports the `XDG_*` variables into the session environment |
| `pam_sm_close_session` | sends **RELEASE** for the stored session id |

It is an account-neutral module: it performs no authentication and no authorization. It only reports.

Both modifiers of the line matter:

- **`optional`** : the result of the module never affects the outcome of the PAM session. A login must succeed even when *66-userd* is down. The module already returns `PAM_SUCCESS` on every internal failure path; `optional` makes that guarantee structural rather than merely a property of the current code.

- **`-` prefix** : if `pam_userd.so` is missing—uninstalled, or a partial upgrade—PAM skips it silently instead of logging an error on every single login.

## Where to add it

In the file that the login services already share for their session stack. On an Arch-derived system such as Obarun this is typically:

```
/etc/pam.d/system-login
```

Put it where the `pam_systemd` or `pam_elogind` line would live on a systemd or elogind system—that is, near the end of the session stack, after `pam_limits.so` and after any module that populates the environment (`pam_env.so`), since the daemon reads `XDG_VTNR`, `XDG_SEAT`, `XDG_SESSION_TYPE` and `XDG_SESSION_CLASS` from the PAM environment when they are present.

If the login services do not share a common file, add the line to each one that should be tracked—typically `login`, `sshd`, and the service of the display manager (`sddm`, `gdm`, `lightdm`, ...).

*pam_userd.so* must **not** coexist with `pam_systemd.so` or `pam_elogind.so` in the same stack. Both would register the same login as a session, both would try to own the runtime directory of the user, and both would export a conflicting `XDG_SESSION_ID`. Pick one. On an Obarun system the elogind/logind line should not be there in the first place.

## What is sent to the daemon

At `open_session`, the module builds the REGISTER payload from the PAM items and the PAM environment:

| Field | Source |
|---|---|
| `UID` | resolved from `PAM_USER` |
| `LEADER` | the pid of the PAM-calling process |
| `TTY` | `PAM_TTY` |
| `DISPLAY` | `PAM_XDISPLAY` |
| `SERVICE` | `PAM_SERVICE` |
| `REMOTE_USER` | `PAM_RUSER` |
| `REMOTE_HOST` | `PAM_RHOST` |
| `REMOTE` | `1` when `PAM_RHOST` is set and non-empty |
| `VTNR` | `XDG_VTNR` from the PAM environment |
| `SEAT` | `XDG_SEAT` from the PAM environment |
| `TYPE` | `XDG_SESSION_TYPE` from the PAM environment |
| `CLASS` | `XDG_SESSION_CLASS` from the PAM environment |

Absent or empty values are simply omitted. The daemon then normalizes the context—it is the daemon, not the greeter or sshd, that decides the final seat, VT number, type and class.

The `LEADER` pid is what the daemon watches: if that process dies without a clean `close_session`, the session is garbage-collected as if the user had logged out.

## What is exported into the session

After a successful REGISTER, the module puts these into the PAM environment, so that the session of the user inherits them:

| Variable | Value |
|---|---|
| `XDG_SESSION_ID` | the session id assigned by the daemon |
| `XDG_SESSION_TYPE` | the daemon-normalized type (*tty*, *x11*, *wayland*, *unspecified*) |
| `XDG_SESSION_CLASS` | the daemon-normalized class (*user*, *greeter*, *lock-screen*, *background*) |
| `XDG_SEAT` | the normalized seat, when non-empty |
| `XDG_VTNR` | the normalized VT number, only when greater than 0 |
| `XDG_RUNTIME_DIR` | `%%userd_runtime_base%%/<uid>`, **only** after verifying that the directory exists and is owned by this uid |

`XDG_RUNTIME_DIR` has no fallback on purpose. If the daemon did not bring the runtime directory up, or if it is not owned by the logging-in user, the module sets nothing and logs a warning. An `XDG_RUNTIME_DIR` pointing at a directory that does not exist, or that belongs to someone else, is worse than none at all.

Note that `XDG_VTNR` is only ever passed through, never invented: *66-userd* does not query the X server to derive a VT number from a display. A session whose display manager did not set `XDG_VTNR` keeps `VTNR 0` and is still correctly attached to its seat.

## When the daemon is down

`open_session` does not block and does not fail. The module logs to syslog:

```
66-userd unreachable; session not tracked (login continues)
```

and returns success. The login proceeds normally; the session is simply **unserved**—it is not tracked, no user service is started for it, and no `XDG_*` variable is exported. When the daemon comes back it will not retroactively learn about that session.

The same applies to a refused REGISTER and to every internal error: the policy of the module is that a session tracker must never be the reason a user cannot log in.

## Packaging rules

The package ships **`pam_userd.so` and nothing else**. It must never write into the PAM files of a distribution—`/etc/pam.d/login`, `/etc/pam.d/system-login`, and so on—and must never append to them at install time.

This is not squeamishness: PAM has no drop-in mechanism to *append a line* to an existing service. The vendor directory `/usr/lib/pam.d/` only replaces whole service files, so a package that wanted to add one line would have to take ownership of the entire file and fight the base PAM package over it on every update.

The line therefore belongs to the base PAM package—exactly where the `pam_systemd` and `pam_elogind` lines already live. The end state is an Obarun `pambase` that ships it, so that it survives updates, applies to all logins and produces no `.pacnew`. Until then it is a documented manual edit, accepting `.pacnew` merges—the same situation elogind and turnstile are in, whose upstreams also leave this to the distribution.

## Verifying it works

After adding the line, log in on a fresh session and check from another shell:

```
66-userctl list sessions
66-userctl list users
```

The login should appear. To inspect one session and confirm the normalized context:

```
66-userctl status sessions <id>
66-userctl status users <name>
```

Inside the tracked session itself, the exported variables should be present:

```
echo "$XDG_SESSION_ID $XDG_SESSION_TYPE $XDG_SEAT $XDG_RUNTIME_DIR"
```

Then log out and confirm that the session disappears from `66-userctl list sessions`, and that on the *last* session closing, the *66* services of the user are stopped.

If nothing appears, check syslog for messages from `pam_userd`—every failure path logs there, since the module cannot report to the user without breaking the login.

## See also

- [66-userd](66-userd.html) : the daemon the module talks to.
- [66-userctl](66-userctl.html) : the query CLI.
