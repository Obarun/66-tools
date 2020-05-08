title: The 66-tools Suite: 66-gnwenv
author: Eric Vidal <eric@obarun.org>

[66-tools](index.html)

[Software](https://web.obarun.org/software)

[obarun.org](https://web.obarun.org)

# 66-gnwenv

66-gnwenv gets and writes the environment variables of a process name at a specific file location.

## Interface

```
	66-gnwenv [ -h ] [ -x ] [ -m mode ] process dir file
```

- *66-gnwenv* executes into `66-getenv -x process 66-writenv -m mode dir file`. It does nothing else: it is just a convenience program. [66-getenv](66-getenv.html) read the environment variable of the process, and [66-writenv](66-writenv.html) will write these variables into *dir/file* location.

## Exit codes

- *0* success
- *100* wrong usage
- *111* system call failed

## Options

- **-h** : prints this help.

- **-x** : match exactly with the process name. It can be a complete command line, in such case its must be double-quoted.

- **-m** *mode* : create *dir* with mode *mode* if it doesn't exist yet. Default is `0755`. 
