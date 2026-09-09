fs = import('fs')

conf = configuration_data()
conf.set_quoted('SS_TOOLS_VERSION', meson.project_version())
conf.set_quoted('SS_TOOLS_NSRULE', ns_rule_dir + '/')
conf.set_quoted('SS_TOOLS_DBS_SYSTEM_SERVICE', dbus_system_service_dir + '/')
conf.set_quoted('SS_TOOLS_DBS_SESSION_SERVICE', dbus_session_service_dir + '/')
conf.set_quoted('SS_TOOLS_DBS_SYSTEM_NAME', dbus_system_name)
conf.set_quoted('SS_TOOLS_DBS_SESSION_NAME', dbus_session_name)
conf.set_quoted('SS_TOOLS_USERD_RUNTIME_BASE', userd_runtime_base)
conf.set_quoted('SS_TOOLS_USERD_RUNTIME_SIZE', userd_runtime_size)
conf.set_quoted('SS_TOOLS_USERD_DBUS_ADDR_PREFIX', userd_dbus_addr_prefix)
conf.set_quoted('SS_TOOLS_USERD_DEFAULT_PATH', userd_default_path)
conf.set_quoted('SS_TOOLS_BINPREFIX', BINDIR + '/')
conf.set_quoted('SS_TOOLS_EXTBINPREFIX', '')
conf.set_quoted('SS_TOOLS_LIBEXECPREFIX', LIBEXECDIR + '/')

configure_file(
  input: 'config.h.in',
  output: 'config.h',
  configuration: conf,
  install: true,
  install_dir: INCLUDEDIR
)
