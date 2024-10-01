BIN_TARGETS := \
66-clock \
66-getenv \
66-gnwenv \
66-ns \
66-olexec \
66-which \
66-writenv \
66-yeller \
execl-cmdline \
execl-subuidgid \
execl-toc

RULE_TARGET := $(shell find examples/rule -type f)

LIBEXEC_TARGETS :=

LIB_DEFS :=

ifneq ($(DBUS_IMPL),)

BIN_TARGETS += 66-dbus-launch

ifeq ($(DBUS_IMPL),basu)

DBUS_LIB := -lbasu

else ifeq ($(DBUS_IMPL),elogind)

DBUS_LIB := -lelogind

else

DBUS_LIB := $(error invalid DBUS_IMPL. Please configure with --enable-dbus=basu or --enable-dbus=elogind.)

endif
endif