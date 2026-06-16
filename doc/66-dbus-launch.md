# 66-dbus-launch

*66-dbus-launch* is a tool for launching, supervising, and reacting to [dbus-broker](https://github.com/bus1/dbus-broker) events emitted by relevant D-Bus signals.

## Interface

```
    66-dbus-launch [ -h ] [ -z ] [ -v verbosity ] [ -d notif ]
```

*66-dbus-launch* acts as a launcher for the [dbus-broker](https://github.com/bus1/dbus-broker), spawning and managing a D-Bus Message Bus.

On receiving D-Bus or kernel signals, the launcher executes tasks based on the signal received(see [Execution tasks](#execution-tasks)).
Each instance of *66-dbus-launch* manages exactly one message bus. Each message bus is independent.

When started by a regular user, *66-dbus-launch* will drop privileges before executing [dbus-broker](https://github.com/bus1/dbus-broker). The launcher only manages services and environments for the process owner, meaning a user launcher cannot manage root services or environments and vice versa.

This program is only built if the `--enable-dbus=` option is passed during compilation (see [Build Requirements](#build-requirements)).

This tool is inspired by the [dbus-controllers](https://github.com/st3r4g/dbus-controllers) project and the original [dbus-broker](https://github.com/bus1/dbus-broker) program.

## Exit codes

- *0* success
- *100* wrong usage
- *111* system call failed

## Options

- **-h** : prints this help.

- **-z** : enable color. If *66-dbus-launch* is not launched from a terminal, the color is automatically disabled and the option has no effect.

- **-v** *verbosity*: increases/decreases the verbosity of the command.
    * *0*: only print error messages.
    * *1*: also, print informative messages. This is the default.
    * *2*: also, print warning messages.
    * *3*: also, print tracing messages.
    * *4*: also, print function name and line code of the messages.
    * *5*: also, display the sequence of the current process function by function.
    *verbosity* are also propagated to the *66* command invocation to activate or desactivate a service.

- **-d** *notif* : notify readiness on file descriptor *notif*. Has no effect when launched from a terminal. Notification occurs before entering the [loop](#running-time), ensuring that both the broker and launcher are fully synchronized.

## Execution Tasks

### Start Time

At startup, 66-dbus-launch performs the following tasks:

- Creates, binds, and listens to a socket at `/run/dbus/%%dbus_system_name%%` or `/run/user/UID/%%dbus_session_name%%`, depending on the process owner. The socket name can be customized at compile time with `--with-dbus-system-name=NAME` and `--with-dbus-session-name=NAME`.

- Sets the `DBUS_SYSTEM_BUS_ADDRESS=unix:path=/run/dbus/%%dbus_system_name%%` and `DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/UID/%%dbus_session_name%%` environment variables according to the owner of the process.

- For each D-Bus service file found in `%%dbus_system_service%%` or `%%dbus_session_service%%`, it reads, parses, translates, and writes a corresponding 66 frontend file. An example:

    ```
    [Main]
    Type = classic
    Description "org.freedesktop.Consolekit.dbus dbus service"
    User = ( root )
    Version = 0.0.1
    InTree = dbus
    MaxDeath = 5
    TimeoutStart = 3000
    TimeoutStop = 3000

    [Start]
    Execute = (
        execl-envfile -l ${ImportFile}
        /usr/bin/console-kit-daemon --nodaemon
    )

    [Environment]
    ImportFile=/etc/66/environment/0000-dbus
    ```

It ensures that you use the latest *D-Bus* file declarations by overwriting any existing frontend files with the same name.

- Creates an unbound socket pair and forks the command `/usr/bin/dbus-broker --controller FD --machine-id MachineId`, with *FD* being the file descriptor of the connected socket's standard input. If `/etc/machine-id` is unreadable, it defaults to `00000000000000000000000000000001`. For regular users, privileges are dropped before execution.

- Notifies readiness (if -`d` is used) and enters a loop, listening and supervising the broker.

### Running time

*66-dbus-launch* responds to kernel signals:

A `SIGHUP` signal triggers a reading, parsing, translating and writting process.
Synchronization of services can also be manually triggered via `66 reload dbus` or `66 signal -s HUP dbus`.
It also synchronizes the available list of services between the launcher and the broke as follows:

- If a *D-Bus* service file is removed, the launcher deactivates from the broker, executes `66 remove <service>`, and erases the corresponding *66* frontend file.
- If a new *D-Bus* service is found, the launcher triggers the reading, parsing, translating and writting process, and activates the service in the broker.
- For an existing service with the same name, the launcher executes `66 parse -f <service>` to ensure the latest *D-Bus* service declaration is used. Note that the service need to be restarted for any changes to take effect.

*66-dbus-launch* reacts on D-Bus signals:

- Activation requested invoke a `66 start <service>` command.
- A `org.freedesktop.DBus.UpdateActivationEnvironment()` request trigger the update environment variables, writing them by default to `/etc/66/environment/0000-dbus` or `$HOME/.66/environment/0000-dbus`, depending on ownership. The file is overwritten for each update. You can also use the [dbus-update-activation-environment](https://dbus.freedesktop.org/doc/dbus-update-activation-environment.1.html) program to trigger this update. Note that you may need to define the `DBUS_SESSION_BUS_ADDRESS` before launching the *dbus-update-activation-environment* program. For root, use `DBUS_SESSION_BUS_ADDRESS=unix:path=/run/dbus/%%dbus_system_name%%`, and for regular user, use `DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/UID/%%dbus_session_name%%`.

- A `org.freedesktop.DBus.ReloadConfig()` request triggers the same process as a `SIGHUP` signal.

### Stop time

Stopping the launcher invokes a `66 tree free dbus` command, as each service frontend file declares [`InTree=dbus`](#start-time).

As the frontend file of each service declare `InTree=dbus`, stopping the launcher invoke a `66 tree free dbus` command. This behavior may change in the future. For now, this reduce the surface of attack and guarantee you to always use a fresh installation of the service.

## Example of 66-dbus-launch frontend file for 66

The following frontend file can be used to start a 66-dbus-launch daemon, either as root or as a regular user, by setting `User=(root)` or `User=(user)` in the example below:

```
[Main]
Type=classic
Description="Dbus-broker launcher for @U account"
User=(root)
Version=0.0.1
TimeoutStart=3000
TimeoutStop=3000
MaxDeath=5
Notify=3

[Start]
Execute=(66-dbus-launch -d3 -v${Verbosity})

[Environment]
Verbosity=!3
```

By default, the frontend file can be installed at `%%datarootdir%%/66/service` (for root) and `%%datarootdir%%/66/service/user` (for regular users).

## Build requirements

- The `--enable-dbus=` option is required at compile time, with either `basu` or `elogind` as the provider of `sd_bus` functions. The use of the `sd_bus` library may change at any time without warning.

- For the runtime, the system must have [dbus-broker](https://github.com/bus1/dbus-broker) installed for *66-dbus-launch* to function properly.

## Caveats

[dbus-broker](https://github.com/bus1/dbus-broker)'s policy API isn't yet stable, allowing broad access for now.
Currently, *66-dbus-launch* doesn't handle configuration files for D-Bus (e.g., `/usr/share/dbus-1/{system,session}.conf`).