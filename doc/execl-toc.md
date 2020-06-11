title: The 66-tools Suite: execl-toc
author: Eric Vidal <eric@obarun.org>

[66-tools](index.html)

[Software](https://web.obarun.org/software)

[obarun.org](https://web.obarun.org)

# execl-toc

Tests an element and creates it, if it does not exist yet, with the default or specified options, then executes a program by default. 

## Interface

```
	execl-toc [ -h ] [ -v verbosity ] [ -n ] [ -t ] [ -D ] [ -X ] [ -d|p|S|m|L|e|b|c|k|n|g|r|s|t|u|w|x|f|z|O|U|N|V|E element ] [ -o opts ] [ -t type ] [ -d device ] [ -g gid ] [ -u uid ] [ -m mode ] [ -M mode ] [ -s|D|B ] [ -b backlog ] prog...
```

*Execl-toc* (Test Or Create) tests an *element* and creates it, if it does not exist yet, where an *element* can be a directory, a fifo, a socket, a mountpoint or a symlink, for testing and creation, and additionally a file and a string for testing only. Then it execs into the rest of its command line.
The options are separated in three parts: 

- (1) main_options

- (2) test_options

- (3) create_options

and must be ordered at the command line in that sequence.

The creation of an *element* can be controlled with the help of `create_options` options. Some of these `create_options` are specific for a particular *element*, where others are general. 

## Exit codes

- *0* success
- *100* wrong usage
- *111* system call failed

## Main options

`Main_options` are not mandatory.

- **-h**  : prints this help.

- **-v** *verbosity* : increases/decreases the verbosity of the command.
    * *1* : only print error messages. This is the default.
    * *2* : also print warning messages.
    * *3* : also print tracing messages.
    * *4* : also print debugging messages.

- **-n** : negate the test. Treat fail as true and vise versa.

- **-t** : exit 0 if the test fail.
 
- **-D** : performs the test without creating the *element* if it does not exist yet.

- **-X** : do not execute *prog*.

## Test_options

`Test_options` are *mandatory* and **must** precede the `create_options`. Parent directories of an *element* will also be created at creation process, if it doesn't exist with `0755` as permissions, if **-m** is not specified.



- Test and create:
  
  * **-d** : true if *element* is a directory.
  * **-p** : true if *element* is a fifo.
  * **-S** : true if *element* is a socket.
  * **-m** : true if *element* is a mountpoint.
  * **-L** : true if *element* is a symlink.

- Test only
  
  * **-e** : true if *element* exist.
  * **-b** : true if *element* is a block.
  * **-c** : true if *element* is a character.
  * **-k** : true if sticky bit is set for *element*.
  * **-n** : true if the length of *element* is non-zero.
  * **-g** : true if *element* is set-group-id.
  * **-r** : true if *element* is readable.
  * **-s** : true if the size of *element* is greater than zero.
  * **-t** : true if *element* is open and refers to a terminal.
  * **-u** : true if set-user-id bit is set for *element*.
  * **-w** : true if *element* is writable.
  * **-x** : true if *element* is executable.
  * **-f** : true if *element* is a regular file.
  * **-z** : true if the length of *element* is zero.
  * **-O** : true if *element* is owned by the effective user id.
  * **-U** : true if *element* is owned by the effective group id.
  * **-N** : true if *element* has been modified since it was last read.
  * **-V** : true if *element* exists on the environment.
  * **-E** : true if *element* is an empty directory. The test performs a check of an existing block, character, fifo, regular file, directory or symlink, inside an *element*.

## Create options

Depending on the *element* to create, `create_options` may or may not be mandatory. The following explanation specifies the mandatory ones.


- **-o** *opts* : mount options (correspond to mount -o). This option has no effect in other cases.

- **-t** *type* : type mount options (correspond to mount -t). This also is used in case of symlink creation as a target option. **Mandatory** is the case of mountpoint or symlink creation. This option has no effect in other cases.

- **-d** *device* : device mount options (correspond to mount -t type device /directory). **Mandatory** is the case of mountpoint creation. This option has no effect in other cases.

- **-u** *uid* : changes *element*'s owner to (numeric/name) uid after the creation. Default is the uid of the owner of the process. This option is used for directory and fifos creation and it has no effects in other cases. If *mode* is given by a name, it searches the user database for an entry with a matching name.

- **-g** *gid* : changes *element*'s owner to (numeric/name) gid after the creation. Default is the gid of the owner of the process. This option is used for directory and fifos creation and it has no effects in other cases. If *mode* is given by a name, it searches the user database for an entry with a matching name then it searches the corresponding group of that name. If it's not found, it searches the group database for an entry with a matching name.

- **-m** *mode* : creates *element*'s permissions to (numeric) mode. Default is `0755`, `0666` and `0777` for a directory, fifo and socket creation respectively. This option is used for directory, fifos and sockets creation and it has no effects in other cases.

- **-M** *mode* : creates parent directories of the *element* with permissions to (numeric) mode. Default is `0755`.

- **-s** : element will be `SOCK_DGRAM` where element is a socket. Default `SOCK_STREAM`. This option is used for socket creation and it has no effects in other cases.

- **-D** : disallow instant rebinding of *element* to the same path where an *element* is a socket. Default is allow. This option is used for socket creation and it has no effects in other cases.
    
- **-B** : the *element* will be blocking where *element* is a socket. Default is unlocked. This option is used for socket creation and it has no effects in other cases.


- **-b** *backlog* : set the maximum of backlog connection on the *element* where an *element* is a socket. Default `SOMAXCONN`. This option is used for socket creation and it has no effects in other cases.

## Usage examples

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

Test a directory, do not create it if it does not exist, and do not execute prog

```
	#!/usr/bin/execlineb -P
	if -n { execl-toc -X -D -d /run/user/1001 }
	66-echo -- "/run/user/1001 does not exist"
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
