title: The 66-tools Suite: 66-writenv
author: Eric Vidal <eric@obarun.org>

[66-tools](index.html)

[Software](https://web.obarun.org/software)

[obarun.org](https://web.obarun.org)

# 66-writenv

66-writenv stores its environment variables at a specific file location.

## Interface

```
	66-writenv [ -h ] [ -m mode ] dir file
```

- This tool write its environment variables into *dir/file* under the classic format `key=value` pair and thoses one per line. *dir* must be an absolute path. 

## Exit codes

- *0* success
- *100* wrong usage
- *111* system call failed

## Options

- **-h** : prints this help.

- **-m** *mode* : create dir with mode *mode* if it doesn't exist yet. Default is `0755`.
