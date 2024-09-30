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

ifneq ($(SDBUS_IMPL),)

BIN_TARGETS += 66-dbus-launch

ifeq ($(SDBUS_IMPL),basu)

SDBUS_LIB := -lbasu

else ifeq ($(SDBUS_IMPL),elogind)

SDBUS_LIB := -lelogind

else

SDBUS_LIB := $(error invalid SDBUS_IMPL. Please configure with --enable-sdbus=basu or --enable-sdbus=elogind.)

endif
endif