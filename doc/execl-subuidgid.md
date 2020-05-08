title: The 66-tools Suite: execl-subuidgid
author: Eric Vidal <eric@obarun.org>

[66-tools](index.html)

[Software](https://web.obarun.org/software)

[obarun.org](https://web.obarun.org)

# execl-subuidgid

Substitutes a literal GID UID with the UID GID of the current owner of the process. 

## Interface

```
	execl-subuidgid [ -o owner ] prog
```

- Substitutes the variable `UID` `GID` on *prog*.

## Exit codes

- *0* success
- *100* wrong usage
- *111* system call failed

## Options

- **-o** *owner* : set `UID` `GID` of *owner* instead of the current one.

## Usage examples

```
	execl-subuidgid
	if { mkdir -p /run/user }
	chown -R $UID:$GID /run/user 
```
```
	execl-subuidgid -o root
	if { mkdir /run }
	chmow -R $UID:$GID /run/user 
```
