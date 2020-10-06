# Changelog for 66-tools

---

# In 0.0.6.2

- bug fix: 
	- *execl-toc* respect **-t** **-n** options if the test crash

---

# In 0.0.6.1

- Bugs fix: fix *execl-toc* **-t** **-n** main_options behavior

---

# In 0.0.6.0

***WARNING***: execl-envfile binary was **removed**. It now a part of [66](https://framagit.org/obarun/66.git) software.

- adapt to oblibs v0.0.9.0

- execl-toc:
	- Add *-M* options: create the parent directories of an element with a specific *mode*.
	- Allow to set the uid/gid at *-u* and *-g* by numeric or name value.

- 66-yeller:
	- *-z* option mean now enable color instead of disable color. It was a big mistake to do the contrary. Sorry for this inconvenient.

- documentation installation: calling the makefile with `make install` install now the documentation by default.

---

# In 0.0.5.1

- Bug fix: fix wrong umask at creation time

---

# In 0.0.5.0

- Adapt to oblibs v0.0.8.0
- add -m option to 66-clock tool
- New tool:
	- execl-toc:
		- this tool allow to check an element and create it if it not exist.
	- 66-yeller:
		- A powerfull, specialized echo tool.

---

# In 0.0.4.0

- Minor bugs fix

- Adapt to oblibs v0.0.6.0

- New 66-clock tool: 
	* get and write sytem time to stdout.

---

# In 0.0.3.1

- Adapt to oblibs v0.0.5.0

- Fix parse of double-quote at execl-cmdline

---
	
# In 0.0.3.0

- Adapt to oblibs v0.0.4.0
- Supports relative path at execl-envfile
- New 66-olexec tool: 
	* opens, locks a tty and execs a program

---

# In 0.0.2.0

- Remove deprecated 66-enfvile tools

- Adapt to skalibs 2.9.1.0, execline 2.5.3.0, oblibs 0.0.3.1
