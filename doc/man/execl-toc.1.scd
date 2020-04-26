execl-toc(1)

# NAME

Execl-toc - Tests an element and creates it if it does not exist yet 
with the default or specified options, then executes a program by default. 

# SYNOPSIS

execl-toc [ *-h* ] [ *-v* _verbosity_ ] [ *-D* ] [ *-X* ] [ *-d*|*p*|*S*|*m*|*L*|*e*|*b*|*c*|*k*|*n*|*g*|*r*|*s*|*t*|*u*|*w*|*x*|*f*|*z*|*O*|*U*|*N*|*V*|*E* _element_ ] [ *-o* _opts_ ] [ *-t* _type_ ] [ *-d* _device_ ] [ *-g* _gid_ ] [ *-u* _uid_ ] [ *-m* _mode_ ] [ *-s*|*D*|*B* ] [ *-b* _backlog_ ] _prog_...

# DESCRIPTION

Execl-toc (Test Or Create) tests an _element_ and creates it if it does not exist yet, where an _element_ can be a directory, a fifo, a socket, a mountpoint or a symlink, for testing and creation, and additionally a file and a string for testing only. Then it execs into the rest of its command line.
The options are separated in three parts: 1) _main_options_, 2) _test_options_ and 3) _create_options_, and must ordered at the command line in that sequence.
The creation of an _element_ can be controlled with the help of _create_options_ options. Some of these _create_options_ are specific for a particular _element_ where others are general. 

# MAIN OPTIONS

_Main_options_ are not mandatory.

*-h*
	Prints this help.

*-v*
	increases/decreases the verbosity of the command. 0(Default): Only print error messages. 
	1: Also print information messages. 2: Also print warning messages. 
	3: Also print tracing messages. 4: Also print debugging messages.

*-D*
	performs the test without creating the _element_ if it does not exist yet.

*-X*
	do not execute _prog_.

# TEST OPTIONS

_Test_options_ are mandatory and *must* precede the _create_options_. 
Parent directories of an _element_ will also be created at creation process, 
if it doesn't exist with 0755 as permissions, if *-m* is not specified.

*Test and create:*++
	*-d*++
		true if _element_ is a directory.++
	*-p*++
		true if _element_ is a fifo.++
	*-S*++
		true if _element_ is a socket.++
	*-m*++
		true if _element_ is a mountpoint.++
	*-L*++
		true if _element_ is a symlink.

*Test only:*++
	*-e*++
		true if _element_ exist.++
	*-b*++
		true if _element_ is a block.++
	*-c*++
		true if _element_ is a character.++
	*-k*++
		true if  sticky bit is set for _element_.++
	*-n*++
		true if the length of _element_ is non-zero. .++
	*-g*++
		true if _element_ is set-group-id.++
	*-r*++
		true if _element_ is a readable.++
	*-s*++
		true if the size of _element_ is greater than zero.++
	*-t*++
		true if _element_ is open and refers to a terminal.++
	*-u*++
		true if set-user-id bit is set for _element_.++
	*-w*++
		true if _element_ is a writable.++
	*-x*++
		true if _element_ is a executable.++
	*-f*++
		true if _element_ is a regular file.++
	*-z*++
		true if the length of _element_ is zero.++
	*-O*++
		true if _element_ is owned by the effective user id.++
	*-U*++
		true if _element_ is owned by the effective group id.++
	*-N*++
		true if _element_ has been modified since it was last read.++
	*-V*++
		true if _element_ exist on the environment.++
	*-E*++
		true if _element_ is an empty directory. The test performs a check++
		of an existing block, character, fifo, regular file, directory or symlink, inside an _element_.

# CREATE OPTIONS

Depending on the _element_ to create, _create_options_ may or may not be mandatory.
The following explanation specifies the mandatory ones.

*-o*
	mount options (correspond to mount -o). *Mandatory* is the case of 
	mountpoint creation. This option has no effect in other cases. 

*-t*
	_type_ mount options (correspond to mount -t). This also is used in 
	case of symlink creation as a target option. *Mandatory* is the case 
	of mountpoint or symlink creation. This option has no effect in other cases. 

*-d*
	_device_ mount options (correspond to mount -t type device /directory). 
	*Mandatory* is the case of mountpoint creation. This option has no effect in other cases. 

*-u*
	changes _element_'s owner to (numeric) _uid_ after the creation. 
	Default is the uid of the owner of the process. This options is used
	for directory and fifos creation and it has no effects in other cases. 

*-g*
	changes _element_'s owner to (numeric) _gid_ after the creation. 
	Default is the gid of the owner of the process. This options is used
	for directory and fifos creation and it has no effects in other cases. 

*-m*
	create _element_'s permissions to (numeric) _mode_ after the creation. 
	Default is 0755, 0666 and 0777 for a directory, fifo and socket creation respectively. 
	This options is used for directory, fifos and sockets creation and it has no effects in other cases. 

*-s*
	_element_ will be SOCK_DGRAM where _element_ is a socket. 
	Default SOCK_STREAM. This options is used for socket creation and it has no effects in other cases.  

*-D*
	disallow instant rebinding of _element_ to the same path where an 
	_element_ is a socket. Default is allow. This options is used for socket 
	creation and it has no effects in other cases. 

*-B*
	the _element_ will be blocking where _element_ is a socket. Default is unlocked. 
	This options is used for socket creation and it has no effects in other cases.

*-b*
	set the maximum of _backlog_ connection on the _element_ where an _element_ is a socket. 
	Default SOMAXCONN. This options is used for socket creation and it has no effects in other cases. 

# EXIT CODES

*0* Success.++
*110* Bad usage.++
*111* System error.

# USAGE EXAMPLES

Test and create a directory if it does not exist yet:


```
		#!/usr/bin/execlineb -P
		execl-toc -d /run/user/1000 -m 0700
		66-echo -- "/run/user was created"
```

Test a directory but do not create it if it does not exist:


```
		#!/usr/bin/execlineb -P
		execl-toc -D -d /run/user/1000
		66-echo -- "/run/user already exist"
	
```

Test a directory, do not create if it does not exit, and do not execute prog


```
		#!/usr/bin/execlineb -P
		if -n { execl-toc -X -D -d /run/user/1001 }
		66-echo -- "/run/user/1001 doesn't exist"
```

Test a mountpoint and create it if it does not exist


```
		#!/usr/bin/execlineb -P
		execl-toc -m /dev/hugepage -o noatime,nodev,noexec,nosuid -t hugetlbfs -d hugepages
		66-echo -- "/dev/hugepage created and mounted successfully"
```

Test a socket and create it if it doesn't exist


```
		#!/usr/bin/execlineb -P
		execl-toc -S /run/user/bus
		66-echo -- "you can now use the socket"
```

Test if a variable is set on the environment


```
		#!/usr/bin/execlineb -P
		execl-toc -V PATH
		66-echo -- "PATH variable is set"
```

Test if a file exists


```
		#!/usr/bin/execlineb -P
		execl-toc -e /home/obarun/.xinitrc
		66-echo -- ".xinitrc is a regular file"
```

Test if a file is executable


```
		#!/usr/bin/execlineb -P
		execl-toc -x /home/obarun/.xinitrc
		66-echo -- ".xinitrc is executable"
```

Test if a directory is empty


```
		#!/usr/bin/execlineb -P
		execl-toc -E /mnt
		66-echo -- "/mnt is emtpy"
```

