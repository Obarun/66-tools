66-getenv(1)

# NAME
66-getenv - Get and display the environment variables of a process name

# SYNOPSIS

66-getenv [ *-h* ] [ *-x* ] [ *-d* _delim_ ] _process_


# DESCRIPTION

66-getenv find the corresponding pid of the _process_ name applying a regex
search.

It open and read */proc/<pid>/environ* file and displays its contain.

# OPTIONS

*-h*
	Prints this help.

*-x*
	Match exactly with the process name. It can be a complete command line, in
	such case its must be double-quoted.

*-d* _delim_
	Specify output delimiter. The default is *\n* character.

# EXIT STATUS

*0*		Success.++
*111*		Failure.

# EXAMPLE

```
66-getenv -x jwm
66-getenv "ck-launch-session jwm"
```

# NOTES

The file */proc/<pid>/environ* contains the initial environment that was set
when the currently executing program was started via *execve*(2). If, after an
*execve*(2), the process modifies its environment (e.g., by calling functions
such as *putenv*(3) or modifying the *environ*(7) variable directly), this file
will *not* reflect those changes&mdash;see *proc*(5) for futher informations.
