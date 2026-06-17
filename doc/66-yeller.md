# 66-yeller

*66-yeller* is a powerful, specialized echo tool.

## Interface

```
	66-yeller [ -h ] [ -d ] [ -s|S ] [ -1 file ] [ -2 file ] [ -z ] [ -n ] [ -c ] [ -p prog ] [ -v verbosity ] [ -i|w|W|t|T|f|F ] msg...
```

*66-yeller* writes the current system time, the name of the program, a colorized informative message (*msg*), depending on the options passed to one or two streams. By default, *stream_1* points to **stdout** and *stream_2* points to **stderr**. The *\a*, *\b*, *\t*, *\n*, *\v*, *\f*, and *\r* sequences are recognized in quoted strings, and are converted to the ASCII numbers 7, 8, 9, 10, 11, 12 and 13 respectively.

## Exit codes

- *0* success
- *100* wrong usage
- *111* system call failed

## Options

- **-h**, **--help** : prints this help.

- **-d**, **--double** : double-output. It writes on *stream_1* and *stream_2*.

- **-s**, **--switch** : switches streams. *Stream_1* becomes *stream_2* and *stream_2* becomes *stream_1*.

- **-S**, **--stdin** : reads *msg* from stdin.

- **-1**, **--out** *file* : redirects *stream_1* to *file*. The file is opened for appending and created if it doesn't exist.

- **-2**, **--err** *file* : redirects *stream_2* to *file*. The file is opened for appending and created if it doesn't exist.

- **-z**, **--color** : enable color. If the *stream_1* does not point to a terminal, the color is automatically disabled and the option has no effects.

- **-n**, **--no-newline** : does not output a trailing newline.

- **-c**, **--no-time** : does not write the current sytem time. By default the current system time as an *ISO* timestamp. It can be set to a *TAIN64*(see [Environment variable](66-yeller.html#Environment variables)).

- **-p**, **--program** *prog* : use *prog* as the program name to display. By default *66-yeller* tries to find the name of the calling process by reading and parsing the `/proc/<pid>/comm` file. This option tells to *66-yeller* to use *prog* as the default program name to display.

- **-v**, **--verbosity** *verbosity*: increases/decreases the verbosity of the command.
    * *0*: only print error messages.
    * *1*: also, print informative messages. This is the default.
    * *2*: also, print warning messages.
    * *3*: also, print tracing messages.
    * *4*: also, print function name and line code of the messages.
    * *5*: also, display the sequence of the current process function by function.

- **-i**, **--no-info** : does not write the informative message.

- **-w**, **--warning** : prints a warning message.

- **-W**, **--warning-force** : prints a warning message regardless the verbosity level. *66-yeller* follows the exact same verbosity rule as any other 66 tools. A *verbosity* set to `1` will not write a warning message. This option forces *66-yeller* to write a warning message even if the *verbosity* is less than `2`.

- **-t**, **--trace** : prints a tracing message.

- **-T**, **--trace-force** : prints a tracing message regardless the verbosity level. *66-yeller* follows the exact same verbosity rule as any other 66 tools. A *verbosity* set to `2` will not write a tracing message. This option forces *66-yeller* to write a tracing message even if the *verbosity* is less than `3`.

- **-f**, **--fatal** : prints a fatal message and dies with an `111` as exit code.

- **-F**, **--fatal-force** : prints a fatal message without dying.

## msg color options

- **%w** : set color to white
- **%b** : set color to blue
- **%g** : set color to green
- **%y** : set color to yellow
- **%r** : set color to red
- **%l** : enable blinking
- **%n** : reset color to normal

## Default informative message format

- A `info`(default) message will be:

	```
		<sytem time> <prog>: info: <msg>
	```

	where `info` is written in green color.


- A `warning` message will be:

	```
		<sytem time> <prog>: warning: <msg>
	```

	where `warning` is written in yellow color.

- A `tracing` message will be:

	```
		<sytem time> <prog>: tracing: <msg>
	```

	where `tracing` is written in white color.

- A `fatal` message will be:

	```
		<sytem time> <prog>: fatal: <msg>
	```

	where `fatal` is written in red color.

## Environment variables

The following environment variable can be set to configure the default *66-yeller* behavior. The corresponding options set at commandline overwrite the environment variable.

- ### PROG

	Corresponds to the **-p** option. Specifies the name of the program to display.

- ### VERBOSITY

	Corresponds to the **-v** option. Specifies the verbosity level to use.

- ### COLOR_ENABLED

	Corresponds to the **-z** option. Tells to *66-yeller* to use color. A value of `1` enable the color, where `0` disable it.

- ### DOUBLE_OUTPUT

	Corresponds to the **-d** option. Tells to *66-yeller* to use double-output. A value of `1` switches on the double-output, where `0` switches off the double-output.

- ### CLOCK_ENABLED

	Corresponds to the **-c** option. Tells to *66-yeller* to output the current system time. A value of `1` outputs the system time where `0` does not.

- ### CLOCK_TIMESTAMP

	Specifies the timestamp to use for the system time display. A value of `1` output the system time as *ISO* timestamp where `0` outputs the system time as *TAIN64* timestamp.

- ### REDIRFD_1

	Corresponds to the **-1** option. Redirects the *stream_1* to its value(e.g. REDIRFD_1=/dev/console). A value of `0` resets the `stream_1` to its default value.

- ### REDIRFD_2

	Corresponds to the **-2** option. Redirects the *stream_2* to its value(e.g. REDIRFD_2=/dev/null). A value of `0` resets the `stream_2` to its default value.

## Usage examples

Call 66-yeller from a terminal where the shell is `zsh`:

```
	% 66-yeller this is an info message
	2020-05-06 10:49:02.71 zsh: info: this is an info message
```

Same things but specifying the name of the program to print:

```
	% 66-yeller -p MyProg this is a warning message
	2020-05-06 10:49:02.71 MyProg: info: this is a info message
```

This following message will not be displayed:

```
	% 66-yeller -p MyProg -w this following message will not be displayed
```

Where this one will be:

```
	% 66-yeller -p MyProg -W this following message will be displayed
	2020-05-06 10:49:02.71 MyProg: warning: this following message will be displayed
```

Also, it can be:

```
	% 66-yeller -v2 -p MyProg -w this following message will be displayed
	2020-05-06 10:49:02.71 MyProg: warning: this following message will be displayed
```

Do not display the system time:

```
	% 66-yeller -c -p MyProg what time is it\?
	MyProg: info: what time is it?
```

Also do not display the informative message:

```
	% 66-yeller -ic -p MyProg what time is it\?
	what time is it?
```

Colorize the output where `what` will be displayed in red color and `time` in yellow color:

```
	% 66-yeller -ic %r what %y time %n is it\?
	what time is it?
```

Alos, it can be:

```
	% 66-yeller -ic %rwhat%y time%n is it\?
	what time is it?
```

Append a trailing newline and a tabulation:

```
	% 66-yeller -ic what "time\nis\tit?"
	what time
	is	it?
```

Use the double-output and redirects *stream_2* to `myfile` file:

```
	% 66-yeller -d -2 myfile -p MyProg reads myfile to see again this message
	2020-05-18 09:33:16.47 MyProg: info: reads myfile to see again this message
```

Reads *msg* from stdin:

```
	% ls -la /tmp | 66-yeller -p MyProg -S
	2020-05-18 09:37:59.15 MyProg: info: total 840K
	2020-05-18 09:37:59.15 MyProg: info: drwxrwxrwt  6 root   root   220 May 18 09:07 ./
	2020-05-18 09:37:59.15 MyProg: info: drwxr-xr-x 19 root   root  4.0K May 16 09:13 ../
	2020-05-18 09:37:59.15 MyProg: info: drwxr-xr-t  2 root   root    40 May 18 06:57 .ICE-unix/
	2020-05-18 09:37:59.15 MyProg: info: drwxrwxrwt  2 root   root    60 May 18 06:57 .X11-unix/

```

Reads message from stdin. Does not display informative message and the system time. Uses double output and redirects *stream_2* to `myfile`

```
	% ls -la /tmp | 66-yeller -cdi2 /tmp/myfile -S /tmp contents "->"
	/tmp contents -> total 844K
	/tmp contents -> drwxrwxrwt  6 root   root   240 May 18 09:40 ./
	/tmp contents -> drwxr-xr-x 19 root   root  4.0K May 16 09:13 ../
	/tmp contents -> drwxr-xr-t  2 root   root    40 May 18 06:57 .ICE-unix/
	/tmp contents -> drwxrwxrwt  2 root   root    60 May 18 06:57 .X11-unix/
```

Use *66-yeller* in a `sh` script called `script.sh` with a pre-defined behavior set by the environment variables:

```
	#!/bin/sh
	export PROG=my_awesome_script
	export VERBOSITY=2
	export DOUBLE_OUTPUT=1
	export REDIRFD_2=/tmp/my_awesome_script.log

	set -e

	if ! [ -e /etc/66/init.conf ]; then
		66-yeller -f "file /etc/66/init.conf doesn't exist"
	else
		cat /etc/66/init.conf | 66-yeller -Sic "%y->%n "
	fi

	if ! [ -e /etc/66/toto.conf ]; then
		66-yeller -f "file /etc/66/toto.conf doesn't exist"
	else
		cat /etc/66/toto.conf | 66-yeller -Sic "%y->%n "
	fi
	66-yeller you will never see this message
```

```
	% ./script.sh
	-> VERBOSITY=0
	-> LIVE=/run/66
	-> PATH=/usr/bin:/usr/sbin:/bin:/sbin:/usr/local/bin
	-> TREE=boot
	-> RCINIT=/etc/66/rc.init
	-> RCSHUTDOWN=/etc/66/rc.shutdown
	-> RCSHUTDOWNFINAL=/etc/66/rc.shutdown.final
	-> UMASK=0022
	-> RESCAN=0
	-> ISHELL=/etc/66/ishell
	2020-05-18 09:57:16.83 my_awesome_script: fatal: file /etc/66/toto.conf doesn't exist

	% cat /tmp/my_awesome_script
	cat /tmp/my_awesome_script.log
	-> VERBOSITY=0
	-> LIVE=/run/66
	-> PATH=/usr/bin:/usr/sbin:/bin:/sbin:/usr/local/bin
	-> TREE=boot
	-> RCINIT=/etc/66/rc.init
	-> RCSHUTDOWN=/etc/66/rc.shutdown
	-> RCSHUTDOWNFINAL=/etc/66/rc.shutdown.final
	-> UMASK=0022
	-> RESCAN=0
	-> ISHELL=/etc/66/ishell
	2020-05-18 10:03:28.94 my_awesome_script: fatal: file /etc/66/toto.conf doesn't exist

```
