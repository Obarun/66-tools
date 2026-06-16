# 66-clock

66-clock gets the system time and writes it with a specific format.

## Interface

```
	66-clock [ -h ] [ -m message ] [ -n ] tai|iso
```

66-clock writes the current system time as a *TAIN64* or *ISO* timestamp depending of the argument past to stdout.

## Exit codes

- *0* success
- *100* wrong usage
- *111* system call failed

## Options

- **-h** : prints this help.
- **-m** *message* : prints *message* after the system time.
- **-n** : output a trailing newline.
