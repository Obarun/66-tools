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

- **-h**, **--help** : prints this help.

- **-o**, **--owner** *owner* : set `UID` `GID` of *owner* instead of the current one.

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
