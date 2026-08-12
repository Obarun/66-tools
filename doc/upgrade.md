# Changelog for 66-tools

---

# In 0.2.0.1

## Overview

This is a bug fix release.

## Bug Fixes

- [pam_userd](pam_userd.html): export `DBUS_SESSION_BUS_ADDRESS` into the login
  session (`87cb9cf`). The guardian builds that address for the scandir branch,
  but the session of the user never received it, so a D-Bus client started from
  the session was left to the fallback of its implementation. The module now
  exports the very address the guardian gives to the scandir, gated on the same
  runtime directory check as `XDG_RUNTIME_DIR`.
- `66-olexec`: suppress a compilation warning on the return value of `dup()`
  (`e5e2283`).
- Fix the version comparison of the documentation deployment (`7841edb`).

---

# In 0.2.0.0

- Adapt to `oblibs` 0.4.0.0
- Depends on `lib66` 0.9.0.0

## Overview

This release makes 66-tools fully `oblibs`-native. The direct dependencies on
`skalibs` and `execline` are both gone, the legacy `configure`/`make` build has
been removed in favor of meson, and `66-ns` has been rewritten with proper
supervision under a pid namespace.

## Breaking changes

- The legacy `configure` and `make` build system has been removed. The project
  is now built exclusively with meson (`1c01296`). See `INSTALL.md`.
- The `skalibs` dependency has been dropped entirely; `oblibs` is self-contained
  and does not depend on it (`ff6bfbf`).
- The `execline` dependency has been dropped: `execl-cmdline` no longer links it
  (`796df87`). No binary links `execline` anymore.
- `66-ns` now needs `lib66` at build time (`1ab255e`): it includes
  `<66/config.h>` for the compile-time paths. It links no symbol from it, so
  `lib66` is not a runtime dependency of that binary.
- `lib66` is now a mandatory dependency of the project, and `libpam` becomes a
  new one: `66-userd`, `66-userctl` and `pam_userd.so` are built
  unconditionally.

## New features

- [66-userd](66-userd.html), [66-userctl](66-userctl.html) and
  [pam_userd](pam_userd.html): session and user tracking for a 66 system,
  without D-Bus. A PAM module reports the opening and the closing of a login
  session to a daemon which, on the first login of a user, mounts the
  runtime directory of that user and starts their `66-scandir` and enabled trees,
  and on the last logout stops them. `66-userctl` is the query and power-action
  CLI. The module is installed but never wired into the PAM stack by the package: see
  [pam_userd](pam_userd.html), that step is mandatory.

    When a scandir is already running for that user, a previous guardian died and
    left its own behind, the `scandir@` module is still enabled, or one was started
    by hand — it is adopted rather than started a second time, and the enabled trees
    are brought up on it. Services already up are left running.

- [66-ns](66-ns.html): supervision under a pid namespace. When a pid namespace
  is requested, `66-ns` runs as pid 1 and acts as a transparent proxy for the
  supervised daemon: it forwards the catchable control signals to the tracked
  main, follows the real daemon across a double-fork, mirrors its exit code back
  to the supervisor, and tears the namespace down once the service is gone. The
  new `-p`, `--pidfile` option makes supervision authoritative (`4e8f4be`,
  `a38366d`).
- Long options are now available for every tool, e.g. `-h`, `--help`
  (`92436f3`, `cdc900b`). `execl-toc` is intentionally left short-only, as its
  options mirror `mount(8)`.

## Bug Fixes

- `execl-toc`: detect a group name lookup failure that previously went unnoticed
  (`4b4a0ba`).
- `66-dbus-launch`: bind the socket behind an exclusive lock, so a concurrent
  launch on the same path gets a clean `EBUSY` instead of stealing the path from
  the running instance (`3e4c272`).
- `66-dbus-launch`: reject a notification file descriptor greater than
  `INT_MAX` (`ca039f2`).
- `66-which`: fix a leak of the `realpath` result (`fa9db26`).
- `66-dbus-launch`: fix a swapped `io_read` argument order (`fa9db26`).

## Enhancements

- Ported every tool from `skalibs` to the new `oblibs` interfaces (`fa9db26`).
- `66-ns` has been rewritten as a set of focused, modular translation units
  backed by `oblibs` and `lib66` (`1ab255e`).
- `execl-cmdline` reimplements `el_semicolon`/`el_getstrict` natively on
  `oblibs` (`796df87`).
- `66-dbus-launch` migrates its service hash table from uthash to the `oblibs`
  intrusive hash table (`6866b5e`).
- Documentation and CI are aligned on the `66` project: lowdown HTML/man rules,
  a mkdocs stack and matching pipeline stages (`dcaa800`), and the build
  instructions are unified into a single `INSTALL.md` (`7925ab1`).

---

# In 0.1.2.0

- Adapt to `oblibs` 0.3.4.0

## Overview

This is a bug fix and enhancement release.

## Bug Fixes

- Fixed type behavior and adapted to `oblibs` for `hash.h` file (`0182864`).
- Suppressed compiler warning (`0182864`).
- Fixed typos in documentation (`0182864`, `a536fb9`).

## Enhancements

- Switched to Meson build system for improved cross-platform support and efficiency (`378e67f`). Traditional `configure` and `make` remain functional during the transition but consider it as deprecated. See `INSTALL_MESON.md` for details.

---

# In 0.1.1.0

- Adapt to `oblibs` 0.3.1.0

## New features

- [66-dbus-launch](66-dbus-launch.html): A tool for launching, supervising, and reacting to [dbus-broker](https://github.com/bus1/dbus-broker) events emitted by relevant D-Bus signals. This tool is not build by default, see `./configure --help` at `Dbus support` section and [66-dbus-launch documentation](66-dbus-launch.html) for build requirements and use.

## Bug Fixes

- Typo fix

---
# In 0.1.0.2

- Adapt to `oblibs` 0.3.0.0

- Bugs fix:

    - fix `-r` options at *66-ns* program.
    - reset ignore options if element exist to avoid skipping it.

---

# In 0.1.0.1

- Bugs fix

- Adapt to skalibs 2.14.1.0

- Adapt to oblibs 0.2.0.2

---

# In 0.1.0.0

- Adapt to skalibs 2.14.0.1

- Adapt to execline 2.9.4.0

- Adapt to oblibs 0.2.0.0

---

# In 0.0.8.0

- Adapt to skalibs 2.11.0.0

- Adapt to execline 2.8.1.0

- Adapt to oblibs 0.1.4.0

- Remove slashpackage convention.

---

# In 0.0.7.3

- Behavior changes:
    *execl-cmdline*: accept empty value for a key with -s options

- Bugs fix:
    *66-getenv*: do not split single/double quoted value

---

# In 0.0.7.2

- Bugs fix:
    *66-ns*: Fix overeating cpu usage

---

# In 0.0.7.1

- Fix musl build

---

# In 0.0.7.0

- Adapt to skalibs 2.10.0.0

- Adapt to execline 2.7.0.0

- Adapt to oblibs 0.1.2.0

- New tool:
    - *66-ns*: setup a namespace and execs a program inside it.

- html documentation is now versionned.

---

# In 0.0.6.2

- bug fix:
    - *execl-toc* respect **-t** **-n** options if the test crash

---

# In 0.0.6.1

- Bugs fix: fix *execl-toc* **-t** **-n** main_options behavior

---

# In 0.0.6.0

***WARNING***: execl-envfile binary was **removed**. It now a part of [66](https://framagit.org/obarun/66.git) software.

- adapt to oblibs v0.0.9.0

- execl-toc:
    - Add *-M* options: create the parent directories of an element with a specific *mode*.
    - Allow to set the uid/gid at *-u* and *-g* by numeric or name value.

- 66-yeller:
    - *-z* option mean now enable color instead of disable color. It was a big mistake to do the contrary. Sorry for this inconvenient.

- documentation installation: calling the makefile with `make install` install now the documentation by default.

---

# In 0.0.5.1

- Bug fix: fix wrong umask at creation time

---

# In 0.0.5.0

- Adapt to oblibs v0.0.8.0
- add -m option to 66-clock tool
- New tool:
    - execl-toc:
        - this tool allow to check an element and create it if it not exist.
    - 66-yeller:
        - A powerfull, specialized echo tool.

---

# In 0.0.4.0

- Minor bugs fix

- Adapt to oblibs v0.0.6.0

- New 66-clock tool:
    * get and write sytem time to stdout.

---

# In 0.0.3.1

- Adapt to oblibs v0.0.5.0

- Fix parse of double-quote at execl-cmdline

---

# In 0.0.3.0

- Adapt to oblibs v0.0.4.0
- Supports relative path at execl-envfile
- New 66-olexec tool:
    * opens, locks a tty and execs a program

---

# In 0.0.2.0

- Remove deprecated 66-enfvile tools

- Adapt to skalibs 2.9.1.0, execline 2.5.3.0, oblibs 0.0.3.1
