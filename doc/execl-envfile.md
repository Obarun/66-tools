title: The 66-tools Suite: execl-envfile
author: Eric Vidal <eric@obarun.org>

[66-tools](index.html)

[Software](https://web.obarun.org/software)

[obarun.org](https://web.obarun.org)

# execl-envfile

A mix of [s6-envdir](https://skarnet.org/software/s6/s6-envdir.html) and [importas](https://skarnet.org/software/execline/importas.html). Reads files containing variable assignments in the given *file/directory*, adds the variables to the environment and then executes a program.

## Interface

```
	execl-envfile [ -h ] [ -l ] src prog
```
This tool expects to find a regular file or a directory in *src* containing one or multiple `key=value` pair(s). It will parse that file, import the `key=value` and then exec the given *prog* with the modified environment. In case of directory for each file found it apply the same process. *src* can be an absolute or a relative path.

- It opens and reads a file.

- It parses that file.

- It imports the found `key=value` pair(s).

- It substitutes each corresponding *key* with value from that file.

- It unexports the variable(s) if requested.

- It execs *prog* with the modified environment.


## Exit codes

- *0* success
- *100* wrong usage
- *111* system call failed

## Options

- **-h** : prints this help.

- **-l** : loose; do nothing and execute *prog* directly if *src* does not contain any regular file(s) or *src* does not exist.

## File syntax

*src* is a text file or a directory containing lines of pairs with the syntax being: `key = value`
Whitespace is permitted before and after *key*, and before or after *value*. Quoting is also possible.

Empty lines or lines containing only whitespace are ignored. Lines beginning with `#` (also after whitespace) are ignored and typically used for comments. Comments are not possible at the end of lines: `key = value # comment` is not a valid comment.

If val begin by a `!` character: `key=!value` the key will be removed from the environment after the substitution. 

## Usage example

```
	#!/usr/bin/execlineb -P
	fdmove -c 2 1
	execl-envfile %%service_admconf%%/ntpd
	foreground { mkdir -p  -m 0755 ${RUNDIR} } 
	execl-cmdline -s { ntpd ${CMD_ARGS} }
	
```

The equivalent with s6-envdir and importas would be:

```
	#!/usr/bin/execlineb -P
	fdmove -c 2 1
	s6-envdir %%service_admconf%%
	importas -u RUNDIR RUNDIR
	importas -u CMD_ARGS CMD_ARGS
	foreground { mkdir -p  -m 0755 ${RUNDIR} } 
	execl-cmdline -s { ntpd ${CMD_ARGS} }
```

where `%%service_admconf%%` contains two named files `RUNDIR` and `CMD_ARGS` written with `/run/openntpd` and `-d -s` respectively. 

## Limits

*src* can not exceed more than `100` files. Each file can not contain more than `4095` bytes or more than `50` `key=value` pairs. 
