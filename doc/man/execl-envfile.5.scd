execl-envfile(5)

# NAME

execl-envfile - _key=value_ format for configuration and environment files

# SYNTAX

_file_ is a text file containing lines of pairs with the syntax being:
	key = value
Whitespace is permitted before and after _key_, and before or after _value_.
Quoting is also possible.

Quoting is also possible. In this case, the quoting field is treated as one
word.

Empty lines, or lines containing only whitespace, are ignored. Lines beginning
with *#* (also after whitespace) are ignored and typically used for
comments. Comments are *not* possible at the end of lines:

```
key = value # comment not valid
```

Empty _value_ is *not* permitted.
If _val_ begin by a *!* character:
	key=*!*value
the _key_ will be removed from the environment after the substitution.

# LIMITS
_src_ cannot exceed more than 100 files. Each file cannot contain more than
4095 bytes or more than 50 _key=value_ pairs.

# SEE ALSO

*execl-envfile*(1)
