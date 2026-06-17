# 66-olexec

66-olexec (open,lock and execute) opens the active (or specified) *tty*, locks it and executes a program.

## Interface

```
	66-olexec [ -h ] [ -d tty ] program
```

This tool opens the active tty or the one specified by the **-d** modifier, locks it, and executes a program as a child process of that *tty*.

- It determines the active *tty* if the **-d** option is not specified.

- It closes the file descriptor `stdin` and `stdout`.

- It tries to open the *tty* with read/write permissions.

- It reopens `stdout` at the *tty*.

- It closes `stderr` and reopens it at the *tty*.

- It gets an exclusive advisory lock on the *tty*.

- It forks and executes *program* as child and waits until the *program* exits. If *program* crashes or get exited, 66-olexec sends a message to `stderr` with the exit code of the *program*.

- It releases the lock and exits.


## Exit codes

- *0* success
- *100* wrong usage
- *111* system call failed

## Options

- **-h**, **--help** : prints this help.

- **-d**, **--tty** *tty* : specifies the *tty* to use in place of the active one. An absolute path is expected.

## Usage example

```
	66-olexec -d /dev/tty3 cryptsetup open /dev/sda1 cryptroot
``` 
