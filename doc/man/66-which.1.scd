66-which(1)

# NAME

66-which - Check if a command exists in PATH or path exists and is executable

# SYNOPSIS

66-which [ *-h* ] [ *-q* | *-a* ] _command(s)_

# DESCRIPTION

66-which is a portable which command that checks if a command exists
and is executable in PATH or at specified path. It behaves slightly
different then the GNU which command.


- This tool expect to find valid command name or path, it will check
  if they exists and are executable by the current user.


- It parse PATH for valid entries, applying a substitution of each
- entry with its realpath and emilinating duplicates.

- It parses that command given checking if it is a path or a name.

- It prints each command found with the first PATH entry, otherwise
  it print an error. By passing the *-a* option, it
  searches and print command with all the PATH entries, even if it
  been already found.

# OPTIONS

*-h*
	Print this help.

*-a*
	Print all. Search and print the command using all the PATH entries, even
	after the command has already been found.

*-q*
	Quiet. Do not print anything on stdout.

# EXIT CODE

*0*		Success; all the commands have been found.++
*110*		Bad usage.++
*111*		System call failed or at least one command hasn't been found.

# EXAMPLES

Search for a command by using its name:

	$ 66-which 66-echo
	/usr/bin/66-echo


Check for a command by using its path:

	$ 66-which /usr/bin/66-echo
	/usr/bin/66-echo

Usage of *66-which* in a script:

	#!/usr/bin/execlineb -P

	if { 66-which -q vgchange }
	vgchange -ay

# NOTES

*66-which* need at least one valid entry in PATH that exists on the
system. *66-which* is often used in critical scripts, like the ones booting
the system, so it's important to check if PATH contains valid entries for the
current system.

*66-which* correctly handle the tilde *~* character for paths.
Beware that same shell, like bash, replaces this character with the
current user home path. To pass the correct string to *66-which*,
containing the tilde character, use quoting like this: *66-which*
'~/.bin/gvr'.
