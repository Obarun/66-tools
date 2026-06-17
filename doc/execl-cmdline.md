# execl-cmdline

This command parses a command line into an [execline](https://skarnet.org/software/execline) script. 

## Interface

As [execlineb](https://www.skarnet.org/software/execline/execlineb.html) script:

```
	execl-cmdline -s { command }
```

- It reads the command and removes all whitespace or `no word` before executing. 

- It exits with the exit code of the command

## Options

- **-h**, **--help** : prints this help.

- **-s**, **--split** : splits a command considered by execline scripting language as one word into separate words. 

## Note and usage examples

*execl-cmdline* should be used at the end of *execline* scripts. If you want to run another program after the call of execl-cmdline you would need to use an [if](https://skarnet.org/software/execline/if.html) command, [foreground](https://skarnet.org/software/execline/foreground.html), [background](https://skarnet.org/software/execline/background.html) or the likes.

The following command:

```
	execl-cmdline { /usr/bin/ntpd -d "" -S }
```

will result in:

```
	/usr/bin/ntpd "-d" "-S"
```

This command:

```
	execl-cmdline -s { /usr/bin/ntpd "-d -S" }
```	

will result in:

```
	/usr/bin/ntpd "-d" "-S"
```

To run another program after the call of *execl-cmdline* program:

```
	foreground { execl-cmdline { /usr/bin/ntpd -d "" -S } }
```
