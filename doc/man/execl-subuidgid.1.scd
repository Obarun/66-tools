execl-subuidgid(1)

# NAME

execl-subuidgid - Substitute GID UID with the current owner GID UID

# SYNOPSIS

execl-subuidgid [ *-o* _owner_ ] _prog_

# DESCRIPTION

*execl-subuidgid* substitutes the variable UID GID on _prog_.

# OPTIONS

*-o* _owner_
	Set UID GID of _owner_ instead of the current one.

# EXAMPLES

```
execl-subuidgid
if { mkdir -p /run/user }
chown -R $UID:$GID /run/user 

execl-subuidgid -o root
if { mkdir /run }
chmow -R $UID:$GID /run/user 
```
