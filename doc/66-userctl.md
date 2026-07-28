# 66-userctl

*66-userctl* queries and controls what [66-userd](66-userd.html) tracks.

## Interface

```
66-userctl [ -h ] [ -z ] [ -v verbosity ] command [ subcommand ] [ operands ]
```

*66-userctl* reads the persisted state of the daemon under `/run/66/userd/` directly, and talks to the daemon over its socket only for power actions.

## Exit codes

- *0* : success.
- *100* : wrong usage—bad argument, bad usage, or an operation refused because of the state of the caller.
- *111* : a system call failed, or the daemon is unreachable.

## Options

- **-h**, **--help** : print the help and exit. Available at every level, including on subcommands.

- **-z**, **--color** : enable color. If *66-userctl* is not launched from a terminal, color is automatically disabled and the option has no effect.

- **-v**, **--verbosity** *verbosity* : set the verbosity level of the command, from *0* to *5*.
    * *0* : only print error messages.
    * *1* : also print informative messages. This is the default.
    * *2* : also print warning messages.
    * *3* : also print tracing messages.
    * *4* : also print the function name and line of the messages.
    * *5* : also display the sequence of the current process function by function.

## Commands

### list

```
66-userctl list users
66-userctl list sessions
```

`list users` shows the tracked users: uid, login name, state (*offline*, *opening*, *online*, *closing*) and live session count.

`list sessions` shows the tracked sessions with their owning user and where they are attached.

### status

```
66-userctl status users <id|name>
66-userctl status sessions <id>
```

`status users` accepts either a uid or a login name, and shows the user together with its sessions.

`status sessions` shows one session in full: id, uid, user, leader pid, seat, VT number, type, class, tty, display, remote flag, PAM service, remote user, remote host, state and creation timestamp.

### access

```
66-userctl access list
66-userctl access allow <user>
66-userctl access deny <user>
```

Manages `/etc/66/shutdown.allow`, the whitelist of users permitted to power the machine off. The file is the one of *66*—one username per line—and is shared with the rest of the *66* tooling.

`allow` and `deny` are **root only**; a non-root caller gets a user error. `list` is readable by anyone.

If the file does not exist at all, there is no whitelist restriction: the gate is simply not applied. Creating it with a single `access allow` therefore *tightens* the policy for everyone else—worth knowing before running it the first time.

### Power actions

```
66-userctl poweroff [ -f ]
66-userctl reboot [ -f ]
66-userctl halt [ -f ]
66-userctl suspend [ -f ]
66-userctl hibernate [ -f ]
```

- **-f**, **--force** : proceed even if other users are logged in.

These do not act on the hardware themselves. The request goes to the daemon which, if the policy allows it, dispatches `66-hpr` with the matching flag. `poweroff`, `reboot` and `halt` go through the fifo of `66-shutdownd`; `suspend` and `hibernate` write `/sys/power/state` and block until resume. Success means *the request was dispatched*, not *the machine has finished acting on it*.

The policy is evaluated in this order:

1. **root is always allowed**—no further check.

2. Otherwise the caller must have an **active local session**. A purely remote caller—an ssh login with no local session—is refused with `no active local session`.

3. Then, if `/etc/66/shutdown.allow` exists, the caller must be listed in it. Refused with `not authorized to power off`.

4. Finally, if **another user** has an active session, **--force** is required. Refused with `other users are logged in (use --force)`.

This is the one operation the daemon accepts from a non-root peer; REGISTER and RELEASE are root-only.

## Notes

`list` and `status` never contact the daemon. They read the serialized records under `/run/66/userd/sessions/` and `/run/66/userd/users/`, so they work—and show the last known state—even while the daemon is stopped. What they cannot do in that situation is tell you that the state is stale.

An empty `list sessions` on a system where users are clearly logged in almost always means the PAM module is not wired into the login stack. See [pam_userd](pam_userd.html).

## See also

- [pam_userd](pam_userd.html) : PAM integration, required for anything to be tracked.
- [66-userd](66-userd.html) : the daemon and its state.
