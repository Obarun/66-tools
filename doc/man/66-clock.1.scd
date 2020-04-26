66-clock(1)

# NAME

66-clock - Gets the system time and writes it with a specific format.

# SYNOPSIS

66-clock [ *-h* ] [ *-m* _message_ ] [ *-n* ] _tai|iso_

# DESCRIPTION

66-clock writes the current system time as a TAIN64 or ISO timestamp 
depending of the argument past to stdout.

# OPTIONS

*-h*
	Prints this help.

*-m*
	Prints _message_ after the system time.

*-n*
	output a trailing new line.

# EXIT STATUS

*0*		Success.++
*111*		Failure.

