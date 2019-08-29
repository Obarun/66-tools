execl-envfile(1)

# NAME

execl-envfile - A blend of *s6-envdir* and *importas* program.

# SYNOPSIS

execl-envfile [ *-h* ] [ *-l* ] _src_ _prog_

# DESCRIPTION

It reads files containing variable assignments on the given file/directory, adds the
variables to the environment, then executes a program.

This tool expects to find a regular file or a directory in _src_ containing
one or multiple _key=value_ pair(s). It will parse that file,
import the _key=value_ and then exec the given _prog_ with the modified
environment. In case of directory for each file found it apply the same process.

- It opens and reads the file.
- It parses the file.
- It imports the _key=value_ pair(s).
- It substitutes each corresponding _key_ with _value_ from that file.
- It unexports the variable if requested.
- It execs _prog_ with the modified environment.

# OPTIONS

*-h*
	Prints this help.

*-f* _file_
	Only parses the _file_ found in _src_. *deprecated option*

*-l*
	Loose. Do nothing and executes _prog_ directly if _src_ does not contain
	any regular file(s) or _src_ doesn't exist.

# EXAMPLE

```
	#!/usr/bin/execlineb -P
	fdmove -c 2 1
	execl-envfile /etc/66/conf/ntpd
	foreground { mkdir -p  -m 0755 ${RUNDIR} } 
	execl-cmdline -s { ntpd ${CMD_ARGS} }
```

The strict equivalent with s6-envdir and importas command will be:

```
	#!/usr/bin/execlineb -P
	fdmove -c 2 1
	s6-envdir /etc/66/conf
	importas -u RUNDIR RUNDIR
	importas -u CMD_ARGS CMD_ARGS
	foreground { mkdir -p  -m 0755 ${RUNDIR} } 
	execl-cmdline -s { ntpd ${CMD_ARGS} }
```

where */etc/66/conf* directory contains two named files *RUNDIR* and *CMD_ARGS* written with */run/openntpd* and *-d -s* respectively.

# SEE ALSO

*execl-envfile*(5)
