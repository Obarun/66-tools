66-olexec(1)

# NAME

66-olexec - Opens the active (or specified) tty, locks it and executes a program.

# SYNOPSIS

66-olexec [ *-h* ] [ *-d* _tty_ ] _program_

# DESCRIPTION

66-olexec opens the active _tty_ or the one specified by the *-d* modifier,
locks it, and executes a _program_ as a child process of that _tty_.

- It determines the active _tty_ if the *-d* option is not specified.
- It closes the file descriptor stdin and stdout.
- It tries to open the _tty_ with read/write permissions.
- It reopens stdout at the _tty_.
- It closes stderr and reopens it at the _tty_.
- It gets an exclusive advisory lock on the _tty_.
- It forks and executes _program_ as child and waits until the _program_ exits. If _program_ crashs or get exited, 66-olexec sends a message to stderr with the exit code of _program_.
- It releases the lock and exits.

# OPTIONS

*-h*
	Prints this help.

*-d*
	specifies the _tty_ to use in place of the active one.
	An absolute path is expected.

# EXIT STATUS

*0*		Success.++
*111*		Failure.

# EXAMPLE

```
66-olexec -d /dev/tty3 cryptsetup open /dev/sda1 cryptroot
```
