66-gnwenv(1)

# NAME

66-gnwenv - Get and write the environment variables of a process

# SYNOPSIS

66-gnwenv [ *-h* ] [ *-x* ] [ *-m* _mode_ ] _process_ _dir_ _file_

# DESCRIPTION

66-gnwenv executes into++
*66-getenv -x process 66-writenv -m mode dir file*++
It does nothing else: it is just a convenience program.

*66-getenv*(1) read the environment variable of the _process_, and
*66-writenv*(1) will write these variables into _dir/file_ location.

# OPTIONS

*-h*
	Prints this help.

*-x*
	Match exactly with the process name. It can be a complete command line, in
	such case its must be double-quoted.

*-m* _mode_
	Create _dir_ with mode _mode_ if it doesn't exist yet. Default is *0755*.

# EXIT STATUS

*0*		Success.++
*111*		Failure.
