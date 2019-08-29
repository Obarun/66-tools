66-writenv(1)

# NAME

66-writenv - Store environment variables at a specific file location.

# SYNOPSYS

66-writenv [ *-h* ] [ *-m* _mode_ ] _dir_ _file_

This tool write its environment variables into _dir/file_ under the classic
format *key=value* pair and thoses one per line. _dir_ must be an absolute path.

# OPTIONS

*-h*
	Prints this help.

*-m* _mode_
	Create _dir_ with mode _mode_ if it doesn't exist yet. Default is *0755*.

# EXIT STATUS

*0*		Successful execution
*111*		Failure
