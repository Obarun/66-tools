# 66-web

*66-web* is a web interface to the *66* service manager. A single binary serves an embedded web UI and a JSON API to list services and trees, read their state, follow their changes live, and act on them.

## Interface

```
66-web [ -h ] [ -v verbosity ] serve [ -h ] [ -l host:port ] [ -L host:port ] [ -a file ]
66-web [ -h ] [ -v verbosity ] passwd [ -h ] [ -a file ]
```

*66-web* manages exactly the services of the uid that runs it. Launched as root, it manages the system services; launched as a user, it manages the services of that user. It never changes its privileges, never reaches outside that scope, and there is no separate root daemon behind it.

Nothing is served without a credential. Run `66-web passwd` once, as the uid that will run the server, before the first `66-web serve`. See [Credential file](#credential-file).

The HTML, CSS and JavaScript of the UI are compiled into the binary. There is nothing else to install or deploy.

## Exit codes

- *0* : success.
- *100* : wrong usage, bad argument, or no usable credential file.
- *111* : a system call failed.

## Options

These options are global and come before the command, as for *66*.

- **-h**, **--help** : print the help and exit. Every command also accepts **-h**.

- **-v**, **--verbosity** *verbosity* : set the verbosity level, from *0* to *5*.
    * *0* : only print error messages.
    * *1* : also print informative messages. This is the default.
    * *2* : also print warning messages.
    * *3* : also print tracing messages.
    * *4* : also print the function name and line of the messages.
    * *5* : also display the sequence of the current process function by function.

Called without a command, *66-web* prints the help and exits *100*.

## Commands

### serve

```
66-web serve [ -h ] [ -l host:port ] [ -L host:port ] [ -a file ]
```

Runs the HTTP server until it receives `SIGINT` or `SIGTERM`.

- **-l**, **--listen** *host:port* : address to bind. Default `127.0.0.1:8066`. The host must be a loopback address, that is a `127.0.0.0/8` address. Any other host is refused with exit code *100*, pointing at **--insecure-listen**.

- **-L**, **--insecure-listen** *host:port* : same as **--listen**, but accepts a non-loopback host. The server speaks plain HTTP, so credentials and every response cross the network unencrypted. A warning is logged at bind time, visible from `-v 2` on. Use it only behind an HTTPS reverse proxy. It also turns off the `Host` and `Origin` checks, which become the job of the proxy, see [Security](#security).

- **-a**, **--auth-file** *file* : credential file to load. See [Credential file](#credential-file).

For both options, the host must be an IPv4 address in numeric form. A name such as `localhost`, or an IPv6 address such as `::1`, is refused with exit code *100*. The port must be between *1* and *65535*, with nothing after it.

When **--listen** and **--insecure-listen** are both given, the last one wins.

Before binding, *66-web* checks that the resolve format on disk is the one it was built for, by comparing the `.version` marker of the *66* system directory with the version of *66* it was compiled against. On a mismatch it exits *111*: rebuild *66-web* against the installed *66*, or run *66* once to migrate the state. When the marker is absent, it logs a warning and serves anyway.

### passwd

```
66-web passwd [ -h ] [ -a file ]
```

Prompts for a user name, a password and its confirmation on stdin, then writes the credential file and exits.

- **-a**, **--auth-file** *file* : credential file to write. See [Credential file](#credential-file).

When stdin is a terminal, echo is disabled while the password is typed. When stdin is a pipe, the three lines are read as they come, which allows a scripted setup:

```
printf 'admin\nsecret\nsecret\n' | 66-web passwd
```

An empty user name, a user name containing `:`, an empty password or a confirmation that does not match is refused. The password is never printed nor logged.

Running `passwd` again replaces the credential. A running server keeps the credential it loaded at startup: restart it to apply the new one.

## Credential file

*66-web* holds a single user name and password, used through HTTP Basic authentication.

The file is chosen in this order:

1. the **--auth-file** option;
2. the `66_WEB_AUTH_FILE` environment variable, when set and not empty;
3. `/etc/66/web/auth` when running as root, `$HOME/.config/66-web/auth` otherwise.

`serve` and `passwd` apply the same rule, so both find the same file when they run as the same uid with the same environment.

The file holds one line:

```
user:<salt_hex>:<hash_hex>
```

The password itself is never stored. The salt is 16 random bytes read from `/dev/urandom`. The hash is SHA-256 iterated 100000 times: `h = SHA256(salt || password)`, then `h = SHA256(salt || h)` repeated. The file is created mode `0600`, its parent directory `0700` if it does not exist, and it is written under a temporary name then renamed into place, so a reader never sees a partial credential.

`66-web serve` refuses to start, exit code *100*, when the file is absent, empty or malformed. This holds on loopback too: there is no anonymous mode.

## Security

Every request is authenticated, the UI assets, the JSON API and the event stream alike.

- **Authentication** : a request without a valid `Authorization: Basic` header gets `401` with `WWW-Authenticate: Basic realm="66-web", charset="UTF-8"`, which makes the browser show its login prompt. The user name and the password are both compared in constant time, so a wrong user and a wrong password cannot be told apart by timing.

- **Host check** : when bound with **--listen**, the `Host` header must name this server: the listen host, `127.0.0.1`, `localhost` or `[::1]`, with the listen port or no port. Anything else gets `403`. This defeats DNS rebinding, where an attacker's domain resolves to `127.0.0.1`: the browser still sends the attacker's name, which matches nothing.

- **Origin check** : when bound with **--listen**, a `POST` carrying an `Origin` header must name this server, with an `http` or `https` scheme and the same set of names as the `Host` check. An `Origin` without a scheme, `null` included, gets `403`. A `POST` without `Origin` is accepted.

- **CSRF header** : every `POST` must carry an `X-66-Web` header, whatever its value, in every listen mode, or it gets `403`. A cross-site form cannot set a custom header, and a script that tries forces a CORS preflight, which *66-web* answers `405`. No `Access-Control-Allow-Origin` is ever sent. `GET` requests and the event stream do not need it.

- **Behind a reverse proxy** : with **--insecure-listen**, the `Host` and `Origin` checks are off, since the proxy legitimately rewrites them. The proxy then owns TLS, the `Host` filtering and the `Origin` filtering. Basic authentication and the `X-66-Web` header still apply.

*66-web* has no TLS of its own, no multiple users, no roles and no sessions. One credential gives full control over the scope of the uid running the server.

## Scope

The running uid decides everything:

| | root | user |
|---|---|---|
| services and trees | system ones | the user's ones |
| new frontends written to | `/etc/66/service/` | `$HOME/.66/service/` |
| frontends listed from | `/etc/66/service/`, then `/usr/share/66/service/` | `$HOME/.66/service/`, then `/etc/66/service/user/`, then `/usr/share/66/service/user/` |
| default credential file | `/etc/66/web/auth` | `$HOME/.config/66-web/auth` |

Frontends under `/usr/share/66/service/` are owned by the package manager: *66-web* lists them but never deletes them.

## Web interface

Point a browser at the listen address, `http://127.0.0.1:8066/` by default, and log in with the credential set by `66-web passwd`.

The UI offers four views:

- **services** : every service of every tree, with its state, and a detail panel giving its resolve information, its dependencies, the tail of its log, its frontend and its configuration. The panel runs the service actions.
- **trees** : every tree, its flags, its dependencies, and the tree actions.
- **graph** : the dependency graph of the services.
- **available** : the frontends found in the source directories that are not parsed yet, to enable, start or delete them.

**+ new service** opens an editor to write a new frontend, either a single service or a module, and optionally enable or start it right away.

The views refresh by themselves when the state of a service changes, through the event stream described below.

## HTTP API

### Conventions

- Only `GET` and `POST` are accepted. Any other method gets `405`.
- Every response closes the connection (`Connection: close`), except the event stream.
- API responses are `application/json; charset=utf-8`. An API error carries `{"error":"<message>"}`. Errors detected by the HTTP layer itself, before routing, carry a one-line `text/plain` body such as `404 Not Found`.
- A query string is ignored. The path is percent-decoded before routing; a malformed escape gets `400`.
- Names in a path or a body must be non-empty, shorter than 256 bytes, must not start with `-`, and must not contain `/`, `..`, whitespace or control characters. `@` is allowed for instances and templates.
- Actions are never run inside *66-web*. Each one execs the `66` binary of the bindir *66-web* was built for, with `-v 2` added so that its warnings reach the captured output. The server keeps serving while the command runs; the HTTP response is sent when the command has exited.

### Read routes

#### GET /api/trees

```json
{
  "base": "/var/lib/66/",
  "trees": [
    {
      "name": "global",
      "enabled": true,
      "current": true,
      "init": true,
      "supervised": true,
      "group": "admin",
      "depends": [],
      "requiredby": [],
      "allow": []
    }
  ]
}
```

`base` is the *66* base directory of the running uid.

#### GET /api/services

Every service of every tree:

```json
{
  "base": "/var/lib/66/",
  "services": [
    {
      "name": "ntpd",
      "tree": "global",
      "type": 0,
      "typestr": "classic",
      "inns": null,
      "logger": "ntpd-log",
      "status": "up",
      "pid": 812,
      "who": "user",
      "since": 1790000000,
      "depends": [],
      "requiredby": [],
      "contents": [],
      "provide": [],
      "conflict": []
    }
  ]
}
```

- `typestr` is one of `classic`, `oneshot`, `module`, `event`.
- `inns` is the module holding the service, or `null`.
- `logger` is the name of the logger service, or `null`.
- `status` is the word *66* uses for the state of the service. `who` tells what caused it: `self`, `user`, `event`, `boot` or `shutdown`.
- `since` is the date of the last state change in seconds since the epoch, or `null`.
- `contents` lists the services a module bundles.

A service listed by its tree whose resolve file cannot be read is still present, with `null` in place of its type, links, status and date, and empty arrays.

#### GET /api/services/*name*

The detail of one service, with more keys than its entry in `GET /api/services`:

```json
{
  "name": "ntpd",
  "description": "ntpd daemon",
  "version": "0.0.1",
  "type": 0,
  "typestr": "classic",
  "source": "/etc/66/service/ntpd",
  "intree": "global",
  "treename": "global",
  "enabled": true,
  "islog": false,
  "earlier": false,
  "config": true,
  "status": "up",
  "pid": 812,
  "who": "user",
  "since": 1790000000,
  "depends": [],
  "requiredby": [],
  "contents": [],
  "provide": [],
  "conflict": []
}
```

- `description`, `version` : the `Description` and `Version` keys of the frontend.
- `source` : path of the frontend file the service was parsed from.
- `intree` : the tree the frontend asks for with its `InTree` key, empty when it has none.
- `treename` : the tree the service actually belongs to.
- `enabled` : `true` when the service is enabled.
- `islog` : `true` when the service is itself a logger, such as `ntpd-log`.
- `earlier` : `true` when the frontend carries the `earlier` flag, starting the service as soon as the scandir is up.
- `config` : `true` when the service has an environment configuration, readable through `GET /api/services/`*name*`/config`.
- `type`, `typestr`, `status`, `pid`, `who`, `since` and the five arrays : same meaning as in `GET /api/services`.

`404` when the service is unknown.

#### GET /api/services/*name*/log

```json
{ "name": "ntpd", "log": "2026-10-03 10:00:00.000000000 ...\n" }
```

The last 5 lines of the log of the service, with TAI64N stamps converted to local time. `log` is empty when the service logs nowhere *66-web* can read, or has never run.

#### GET /api/services/*name*/frontend

```json
{ "name": "ntpd", "path": "/etc/66/service/ntpd", "content": "[Main]\n..." }
```

The frontend the service was parsed from. When that file cannot be read, `content` is `(unable to read frontend)`.

#### GET /api/services/*name*/config

```json
{ "name": "ntpd", "config": "..." }
```

The output of `66 configure -L` *name*. Always `200`, whatever the exit code of the command.

#### GET /api/available

The frontends of the source directories that are not parsed yet, listed in the precedence order of [Scope](#scope). A name present in several directories is listed once, from the first.

```json
{
  "base": "/var/lib/66/",
  "available": [
    {
      "name": "sshd",
      "kind": "service",
      "type": "classic",
      "description": "OpenSSH daemon",
      "instanced": false,
      "source": "/usr/share/66/service/",
      "removable": false
    }
  ]
}
```

- `kind` is `service` or `module`.
- `instanced` is `true` for a template, a name ending in `@`, and for every module.
- `removable` is `true` when the directory is not package-managed and the running uid can write in it.

#### GET /api/frontend/*name*

```json
{ "name": "sshd", "source": "/usr/share/66/service/", "content": "[Main]\n..." }
```

The content of a frontend found in the source directories, parsed or not. For an instance, `name@instance`, it answers the content of the template `name@`. `404` when no directory holds it.

### Action routes

Every `POST` needs the `X-66-Web` header, see [Security](#security). Bodies are JSON, 256 KiB at most.

#### POST /api/services/*name*/*action*

Runs `66` *action* *name*. *action* is one of `start`, `stop`, `restart`, `reload`, `enable`, `disable`, `reconfigure`, `free`, `remove`. Any other verb gets `400`.

```json
{ "name": "ntpd", "action": "start", "exit": 0, "output": "..." }
```

`200` when `66` exits *0*, `503` otherwise. `output` holds its stdout and stderr.

#### POST /api/trees/*name*/*action*

Runs `66 tree` *action* *name*. *action* is one of `start`, `stop`, `free`, `enable`, `disable`, `current`, `init`, `remove`. Any other verb gets `400`.

```json
{ "tree": "global", "action": "start", "exit": 0, "output": "..." }
```

`200` when `66` exits *0*, `503` otherwise.

Machine-level commands, `boot`, `halt`, `poweroff`, `reboot`, are not reachable through the API.

#### POST /api/services

Writes a new frontend file into the writable source directory, see [Scope](#scope).

```json
{
  "name": "myservice",
  "content": "[Main]\nType = classic\n...",
  "action": "enablestart",
  "tree": "global",
  "overwrite": false
}
```

- `name`, `content` : required.
- `action` : what to run once the file is written.
    * `none` : nothing. This is the default, and the value for any unknown word.
    * `enable` : `66 enable`, which parses the frontend first.
    * `enablestart` : `66 enable -S`.
    * `start` : `66 start`.
- `tree` : tree to enable into, passed as `66 -t`. When absent, *66* decides.
- `overwrite` : replace an existing file of that name. Without it, an existing file gets `409`.

```json
{ "name": "myservice", "path": "/etc/66/service/myservice", "action": "enablestart", "exit": 0, "output": "..." }
```

`201` when the file is written and the action, if any, succeeded. `503` when `66` failed; the file is kept, so a parse error can be fixed and the request sent again with `overwrite`.

#### POST /api/modules

Writes a new module directory into the writable source directory.

```json
{
  "name": "mymodule",
  "content": "[Main]\nType = module\n...",
  "members": [ { "local": "worker", "content": "[Main]\n..." } ],
  "activated": [ "worker" ],
  "depends": [ "network" ],
  "requiredby": [],
  "configure": "#!/bin/sh\n...",
  "instance": "one",
  "action": "enable",
  "tree": "global",
  "overwrite": false
}
```

- `name` : module name, without `@`. The module frontend is written as `name/name@`.
- `content` : the module frontend, required.
- `members` : services bundled by the module, written under `frontend/`.
- `activated`, `depends`, `requiredby` : written under `activated/`, `activated/depends/` and `activated/requiredby/`.
- `configure` : the `configure/configure` script, made executable.
- `action`, `tree` : as for `/api/services`, applied to `name@instance`. Any action other than `none` requires `instance`, without `@`.
- `overwrite` : replace an existing module directory. Without it, an existing one gets `409`.

When writing the directory fails halfway, it is removed. The response is the same as for `/api/services`, `path` naming the module directory.

#### POST /api/enable

Enables or starts a frontend already present in the source directories.

```json
{ "name": "sshd", "tree": "global", "action": "enable" }
```

`action` is `enable`, the default, `enablestart` or `start`.

```json
{ "name": "sshd", "exit": 0, "output": "..." }
```

`200` when `66` exits *0*, `503` otherwise.

#### POST /api/remove/*name*

Deletes an unparsed frontend, file or module directory, from the first source directory that holds it, is not package-managed, and is writable by the running uid. No body.

```json
{ "name": "myservice", "source": "/etc/66/service/", "removed": true }
```

`404` when no such directory holds it. To remove a parsed service, use `POST /api/services/`*name*`/remove`.

### Event stream

#### GET /api/events

A [Server-Sent Events](https://html.spec.whatwg.org/multipage/server-sent-events.html) stream, `text/event-stream`. It opens with a `: connected` comment, then sends:

```
data: {"type":"state_changed"}
```

each time something changes in the live state directory of the running uid under the *66* live directory. The event carries no detail: a client re-fetches what it shows. A client too slow to read misses a frame rather than receiving a late one.

At most 64 streams are served at once; one more gets `503`. When the live state directory cannot be watched, a warning is logged and the stream stays open but never emits.

### Status codes

| Code | When |
|---|---|
| `200` | success |
| `201` | frontend or module written |
| `400` | malformed request, bad name, unknown verb, invalid JSON |
| `401` | missing or wrong credential |
| `403` | `Host`, `Origin` or `X-66-Web` check failed |
| `404` | unknown route, service, tree or frontend |
| `405` | method other than `GET` or `POST` |
| `408` | the request header was not complete within 15 seconds |
| `409` | the frontend or module already exists and `overwrite` is not set |
| `413` | body larger than 256 KiB |
| `414` | path of 1024 bytes or more |
| `431` | header block larger than 16 KiB, or more than 64 header fields |
| `500` | internal failure |
| `503` | the `66` command exited non-zero, or the event stream is full |

### Limits

- 256 connections at once.
- 15 seconds to send the request header block.
- 30 seconds without the client reading one more byte of a response, and 10 minutes in all to deliver it.

### Examples

The examples use [curl](https://curl.se) and [jq](https://jqlang.org), with the server on its default address and the credential `admin` / `secret`. With `-u admin` alone, curl prompts for the password instead of leaving it in the shell history.

List the trees:

```
curl -s -u admin:secret http://127.0.0.1:8066/api/trees | jq .
```

Show the services that are not up, one per line:

```
curl -s -u admin:secret http://127.0.0.1:8066/api/services | jq -r '.services[] | select(.status != "up") | "\(.name) \(.status)"'
```

```
numlockx waiting
printenv down
```

Show one service, then the tail of its log:

```
curl -s -u admin:secret http://127.0.0.1:8066/api/services/clipit | jq .
curl -s -u admin:secret http://127.0.0.1:8066/api/services/clipit/log | jq -r .log
```

```
2026-10-01 02:01:20.661064816  (clipit:16578): Gtk-WARNING **: 02:01:20.661: cannot open display: :0
2026-10-02 00:13:40.658753078  Clipboard is null, recovering ...
```

List the frontends not parsed yet, with the directory holding them:

```
curl -s -u admin:secret http://127.0.0.1:8066/api/available | jq -r '.available[] | "\(.name)\t\(.kind)\t\(.source)"'
```

Print a frontend:

```
curl -s -u admin:secret http://127.0.0.1:8066/api/frontend/clipit | jq -r .content
```

Restart a service, then stop a tree. A `POST` needs the `X-66-Web` header; `-i` shows the status line, `200` on success and `503` when `66` failed:

```
curl -s -i -u admin:secret -H 'X-66-Web: 1' -X POST http://127.0.0.1:8066/api/services/clipit/restart
curl -s -u admin:secret -H 'X-66-Web: 1' -X POST http://127.0.0.1:8066/api/trees/global/stop | jq -r .output
```

Without the header, the server answers `403 Forbidden`; with a verb outside the whitelist, `400`:

```
{"error":"unknown action or invalid service name"}
```

Create a service from a local frontend file `myservice`, enable it in the tree `global` and start it. `jq --rawfile` builds the JSON body and escapes the content of the file, `--data-binary @-` sends it unchanged:

```
jq -n --rawfile c myservice '{name: "myservice", content: $c, action: "enablestart", tree: "global"}' | curl -s -u admin:secret -H 'X-66-Web: 1' -H 'Content-Type: application/json' --data-binary @- http://127.0.0.1:8066/api/services | jq .
```

Enable a frontend already present in a source directory:

```
jq -n '{name: "sshd", tree: "global", action: "enable"}' | curl -s -u admin:secret -H 'X-66-Web: 1' --data-binary @- http://127.0.0.1:8066/api/enable | jq .
```

Follow the event stream. `-N` turns off the buffering of curl, so each event shows as it arrives:

```
curl -s -N -u admin:secret http://127.0.0.1:8066/api/events
```

```
: connected

data: {"type":"state_changed"}
```

## Running it under 66

An example frontend for the system instance:

```
[Main]
Type = classic
Description = "66-web interface"

[Start]
Execute = ( /usr/bin/66-web serve )
```

Run `66-web passwd` as root first, otherwise the service fails at start. For a user instance, write the same frontend into `$HOME/.66/service/`, run `66-web passwd` as that user, and pick a different port with **--listen** if the system instance is also running.

## Logging

*66-web* logs through *oblibs* to stderr, at the verbosity set by **-v**. Under *66*, that stream goes to the logger of the service. The listen address, the watched live state directory, shutdown, and every failure path are reported there. Request bodies and credentials are never logged.

## See also

- [66](https://docs.obarun.org/66/latest) : the service manager *66-web* drives.
