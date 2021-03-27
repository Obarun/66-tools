title: The 66-tools Suite: 66-getenv
author: Eric Vidal <eric@obarun.org>

[66-tools](index.html)

[Software](https://web.obarun.org/software)

[obarun.org](https://web.obarun.org)

# 66-getenv

66-getenv gets and displays the environment variables of a process name.

## Interface

```
    66-getenv [ -h ] [ -x ] [ -d delim ] process
```

- It find the corresponding pid of the *process* name applying a regex search.

- It open and read `/proc/<pid>/environ` file and displays its contain.

## Exit codes

- *0* success
- *100* wrong usage
- *111* system call failed

## Options

- **-h** : prints this help.

- **-x** : match exactly with the process name. It can be a complete command line, in such case its must be double-quoted.

- **-d** *delim* : specify output delimiter. The default is `\n` character.

## Usage example

```
    66-getenv -x jwm
    66-getenv "ck-launch-session"
```

## Note

The file `/proc/<pid>/environ` contains the initial environment that was set when the currently executing program was started via *execve(2)*. If, after an *execve(2)*, the process modifies its environment (e.g., by calling functions such as *putenv(3)* or modifying the *environ(7)* variable directly), this file will *not* reflect those changes—see *proc(5)* for futher informations.
