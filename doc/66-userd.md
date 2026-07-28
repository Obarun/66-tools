# 66-userd

*66-userd* is a session and user tracker. It records which users are logged in and from where, owns each user's runtime directory, and drives that user's *66* services on the first login and the last logout—without D-Bus.

## Interface

```
66-userd [ -h ] [ -v verbosity ] [ -d number ]
```

*66-userd* is the only long-lived process of the trio it belongs to. It runs as root, listens on a Unix socket, and answers the session events reported by [pam_userd](pam_userd.html). It is complementary to `seatd`: `seatd` owns *seats*—the hardware context, devices and VTs—where *66-userd* owns *sessions*—who and where—and the services that follow them.

There is no runtime option to relocate the socket or the state directory. Both are fixed at compile time—see [Configuration](#configuration).

*66-userd* is inert on its own: nothing is ever tracked until `pam_userd.so` is referenced from the system PAM login stack. That step is mandatory and is described in [pam_userd](pam_userd.html).

## Exit codes

- *0* : success.
- *100* : wrong usage (invalid option, bad argument).
- *111* : a system call failed.

## Options

- **-h**, **--help** : print the help and exit.

- **-v**, **--verbosity** *verbosity* : set the verbosity level of the daemon, from *0* to *5*.
    * *0* : only print error messages.
    * *1* : also print informative messages. This is the default.
    * *2* : also print warning messages.
    * *3* : also print tracing messages.
    * *4* : also print the function name and line of the messages.
    * *5* : also display the sequence of the current process function by function.

- **-d**, **--notify** *number* : notify readiness on the file descriptor *number*. The daemon writes a single newline to that descriptor once it is listening on its socket. This matters because the PAM module connects to the socket at every login: a service declared up before the socket exists would produce a window of untracked logins at boot.

## Architecture

Three artifacts share the state kept under `/run/66/userd/`:

```
login / sshd / display manager
        │
        │  PAM session stack
        ▼
┌───────────────┐   REGISTER / RELEASE    ┌──────────────────┐
│ pam_userd.so  │ ──────────────────────► │     66-userd     │  (root)
│ (login proc)  │ ◄────────────────────── │    event loop    │
└───────────────┘   session id, context   └────────┬─────────┘
                                                    │ fork
                                                    ▼
                                            ┌────────────────┐
                                            │ guardian (uid) │  one per user
                                            └───────┬────────┘
                                                    │
                                        66-scandir + 66 tree start
```

- **`pam_userd.so`** runs inside the login process. It captures the session context PAM knows about and hands it to the daemon. It never blocks a login—see [pam_userd](pam_userd.html).
- **`66-userd`** is the only long-lived process. It owns the truth: the session and user tables, the runtime directory of each user, and the decision of when services start and stop.
- **`66-userctl`** reads the persisted state and, for power actions, talks to the daemon. It holds no state of its own—see [66-userctl](66-userctl.html).

## Session lifecycle

1. A user logs in. PAM runs the session stack; the `open_session` hook of `pam_userd.so` sends a **REGISTER** carrying uid, leader pid, tty, display, seat, vtnr, class, type and remote information.

2. The daemon normalizes that context—it, not the greeter or sshd, owns the canonical `SEAT`, `VTNR`, `TYPE` and `CLASS`—assigns a session id, and increments the session count of that user.

3. **If this is the first session for that uid**, the daemon mounts the runtime directory of the user and forks a *guardian*: a per-user process that parents the user's `66-scandir`, waits for it to signal readiness, then runs `66 tree start` for the enabled trees of that user.

4. The daemon replies with the session id. The module re-exports the normalized context into the session environment as `XDG_*` variables.

5. Further logins by the same user are tracked, but start nothing—the services are already up.

6. On logout, the `close_session` hook sends a **RELEASE**. The count drops.

7. **When the last session goes away**, the daemon signals the guardian, which runs `66 tree stop` then `66 scandir quit`—in that order, since the trees need the scandir alive to be stopped—and exits. The runtime directory is unmounted.

Services therefore start on the `0 → 1` transition of the session count of a user and stop on the `1 → 0` transition, never in between.

The daemon itself never changes its uid and never calls into *66* in-process. Every operation that must run as the user runs in a throwaway child that drops privilege first.

## Garbage collection and reconciliation

A session leader that dies without a clean `close_session`—a killed terminal, a crashed session—is not leaked. The daemon holds a pidfd on the leader and treats its death as a logout, releasing the session and, if it was the last one, tearing the services of that user down.

On start, the daemon reloads its persisted state and reconciles it with reality: sessions whose leader is gone, or whose pid was reused by a different process—detected through the recorded start time—are dropped; guardians still alive are re-adopted rather than restarted; users whose scandir is already up are re-probed rather than started again. A restart of the daemon therefore does not disturb running user sessions.

## State on disk

```
/run/66/userd/
├── s                # the listening Unix socket
├── sessions/        # one serialized record per tracked session, named by session id
└── users/           # one serialized record per tracked user, named by uid
```

The records are plain `KEY=value` text, one field per line. *66-userctl* reads them directly rather than querying the daemon.

The socket is the single rendez-vous point with the PAM module. Only a peer running as uid 0 may REGISTER or RELEASE—the daemon checks this with `SO_PEERCRED`, so an unprivileged process cannot invent or destroy sessions. Power actions are the exception, since they are meant to be requested by ordinary users; their own policy is described in [66-userctl](66-userctl.html).

## Runtime directories

The daemon owns `%%userd_runtime_base%%/UID`. On the first session of a user it creates the directory and mounts a private tmpfs over it with `mode=0700`, the uid and gid of the user, and `MS_NOSUID | MS_NODEV`. On the last session it unmounts and removes it, falling back to a lazy detach if the mount is still busy.

This is why `XDG_RUNTIME_DIR` is exported by the PAM module only after checking that the directory exists and is owned by the right user: the daemon is the only writer, and the module refuses to guess.

## Per-user environment

Before exec'ing the scandir of the user, the guardian sets up a stable per-user environment which the scandir and every service under it inherit: identity (`HOME`, `USER`, `LOGNAME`, `SHELL`), `XDG_RUNTIME_DIR`, `DBUS_SESSION_BUS_ADDRESS` built from the runtime directory, and a `PATH`.

These are **safe defaults, not a forced injection**. *66* natively merges the user's own `~/.66/environment` on top at scandir start, and that merge wins—a user can override any of them. The `PATH` the daemon inherited from PID 1 is passed through untouched; the compile-time fallback is only a floor for the abnormal case where the daemon inherited no `PATH` at all.

## Configuration

Build-time only. There is no configuration file and no environment variable tier—the socket path of a session daemon must not be redirectable by an inherited variable.

The knobs are meson options, resolved into the generated `<66-tools/config.h>`:

| Option | Default | Meaning |
|---|---|---|
| `userd-runtime-base` | `%%userd_runtime_base%%` | base directory of the runtime directory of each user |
| `userd-runtime-size` | `%%userd_runtime_size%%` | `size=` of the per-user tmpfs, as a share of RAM |
| `userd-dbus-addr-prefix` | `%%userd_dbus_addr_prefix%%` | prefix of the `DBUS_SESSION_BUS_ADDRESS` built for the user |
| `userd-default-path` | `%%userd_default_path%%` | fallback `PATH` for the services of a user, used only if the daemon inherited none |
| `userd-pam-dir` | `%%userd_pam_dir%%` | where `pam_userd.so` is installed |

The state directory and the socket path are **not** options. They are constants composed in C, anchored on the `SS_LIVE` of *66*, so that they can never drift from the live tree of *66*:

```c
#define USERD_STATEDIR     SS_LIVE "userd"
#define USERD_SOCKET_PATH  USERD_STATEDIR "/s"
```

## Running it under 66

An example frontend file is provided in `contributions/service/66-userd`:

```
[Main]
Type = classic
Description = "66-userd session and user tracker daemon"
Depends = ( seatd )

[Start]
Notify = 3
Execute = ( /usr/bin/66-userd -d 3 )

[Event]
EventType = service
From = ( seatd )
On = ( up )
Do = restart
```

`Notify = 3` and `-d 3` are the two halves of the same contract: *66* treats the service as up only once the daemon is listening on its socket.

By default, the frontend file can be installed at `%%datarootdir%%/66/service`.

## Logging

The daemon logs through *oblibs*, to stderr, at the verbosity set by **-v**. Under *66* that stream is captured by the logger of the service. Session registrations, user state transitions, guardian lifecycle events and every failure path are reported there.

Failures inside a guardian or one of its children are reported with the uid they concern, so a scandir that fails to come up for one user is identifiable in a log shared by all of them.

## Design boundaries

Two things are deliberately **not** the job of *66-userd*:

- **Seats, VTs and device access** belong to `seatd`. *66-userd* records the seat and VT number of a session as reported, but never arbitrates device access and never switches VTs.

- **The live directory of 66** belongs to *66*. Every *66* operation the daemon triggers leaves the livedir unset, so that *66* resolves its own compiled default. There is no knob to relocate it from here.

## See also

- [pam_userd](pam_userd.html) : the module that feeds the daemon; **required setup**.
- [66-userctl](66-userctl.html) : inspecting what the daemon tracks.
