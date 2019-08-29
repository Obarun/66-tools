execl-cmdline(1)

# NAME

execl-cmdline - Parse a command line into an execline script.

# SYNOPSYS

As *execlineb* script:

```
    execl-cmdline -S { command }
```

# DESCRIPTION

*execl-cmdline* reads the _command_ and removes all whitespace or 'no word'
before executing.

OPTIONS

*-s*
	Splits a command considered by *execline* scripting language as one word
	into separate words.

# EXIT STATUS

*execl-cmdline* exits with the exit code of the _command_.

# NOTES

*execl-cmdline* should be used at the end of *execline* scripts. If you want to
run another program after the call of *execl-cmdline* you would need to use an
*if* command, *foreground*, *background* or the likes.

# EXAMPLE

The following _command_:

```
    execl-cmdline { /usr/bin/ntpd -d "" -S }
```

will result in:

```

    /usr/bin/ntpd "-d" "-S"

```

This _command_:

```

    execl-cmdline -S { /usr/bin/ntpd "-d -S" }

```

will result in:

```

    /usr/bin/ntpd "-d" "-S"

```

To run another program after the call of *execl-cmdline* program:

```

    foreground { execl-cmdline { /usr/bin/ntpd -d "" -S } }

```
