project('66-tools',
        'c',
        version: '0.1.2.0',
        meson_version: '>=1.1.0',
        license: 'ISC',
        default_options: [
          'c_std=c99',
          'prefix=/usr',
          'ns-rule-dir=/usr/share/66/script/ns',
          'enable-dbus=disabled',
          'dbus-system-service-dir=/usr/share/dbus-1/system-services',
          'dbus-session-service-dir=/usr/share/dbus-1/services',
          'dbus-system-name=system_bus_socket',
          'dbus-session-name=dbus',
          'enable-shared=true',
          'with-doc=false',
          'warning_level=2',
          'optimization=2',
          'buildtype=plain',
          'default_library=shared'
        ])

message('See INSTALL_MESON.md for detailed configuration instructions and warnings.')

fs = import('fs')

# Versioning
version_skalibs = '>=2.14.3.0'
version_execline = '>=2.9.6.1'
version_s6 = '>=2.13.1.0'
version_oblibs = '>=0.3.4.0'
version_66 = '>=0.8.0.0'
version_lowdown = '>=0.6.4'

# Compiler setup
cc = meson.get_compiler('c')

# Define dependency checks
# Common include and library paths
default_include_dirs = [get_option('prefix') / get_option('includedir')]
default_lib_dirs = [get_option('prefix') / get_option('libdir')]
user_include_dirs = get_option('with-include-dir')
user_staticlib_dirs = get_option('with-staticlib-dir')
user_dynamiclib_dirs = get_option('with-dynamiclib-dir')

# Combine default and user-provided paths
include_dirs = default_include_dirs + user_include_dirs
lib_dirs = default_lib_dirs + user_staticlib_dirs + user_dynamiclib_dirs

# Dependency: skalibs
skalibs_lib = cc.find_library('skarnet', dirs: lib_dirs, static: get_option('enable-static-deps'), required: false)
skalibs_header = cc.has_header('skalibs.h', args: ['-I', include_dirs], required: false)
if not skalibs_lib.found() # or not skalibs_header
  error('skalibs ' + version_skalibs + ' not found. Ensure libskalibs and skalibs.h are installed, or specify paths with with-include-dir and with-staticlib-dir/with-dynamiclib-dir.')
endif
dep_skalibs = declare_dependency(
  dependencies: skalibs_lib,
  include_directories: include_dirs
)

# Dependency: execline
execline_lib = cc.find_library('execline', dirs: lib_dirs, static: get_option('enable-static-deps'), required: false)
execline_header = cc.has_header('execline.h', args: ['-I', include_dirs], required: false)
if not execline_lib.found() or not execline_header
  error('execline ' + version_execline + ' not found. Ensure libexecline and execline/execline.h are installed, or specify paths with with-include-dir and with-staticlib-dir/with-dynamiclib-dir.')
endif
dep_execline = declare_dependency(
  dependencies: execline_lib,
  include_directories: include_dirs
)

# Dependency: oblibs
oblibs_lib = cc.find_library('oblibs', dirs: lib_dirs, static: get_option('enable-static-deps'), required: false)
oblibs_header = cc.has_header('oblibs.h', args: ['-I', include_dirs], required: false)
if not oblibs_lib.found() or not oblibs_header
  error('oblibs ' + version_oblibs + ' not found. Ensure liboblibs and oblibs.h are installed, or specify paths with with-include-dir and with-staticlib-dir/with-dynamiclib-dir.')
endif
dep_oblibs = declare_dependency(
  dependencies: oblibs_lib,
  include_directories: include_dirs
)

# Dependency: 66
lib66_lib = cc.find_library('66', dirs: lib_dirs, static: get_option('enable-static-deps'), required: false)
lib66_header = cc.has_header('66.h', args: ['-I', include_dirs], required: false)
if not lib66_lib.found() or not oblibs_header
  error('lib66 ' + version_66 + ' not found. Ensure lib66 and include/66/config.h are installed, or specify paths with with-include-dir and with-staticlib-dir/with-dynamiclib-dir.')
endif
dep_lib66 = declare_dependency(
  dependencies: lib66_lib,
  include_directories: include_dirs
)

# Dependency: dbus
dep_dbus = []
if get_option('enable-dbus') != 'disabled'
  # Check for basu or elogind
  if get_option('enable-dbus') == 'basu'
    dep_dbus = dependency('basu', required: false, static: get_option('enable-static-deps'))
    if not dep_dbus.found()
      error('enable-dbus=basu requires basu library, but it was not found.')
    endif
  elif get_option('enable-dbus') == 'elogind'
    dep_dbus = dependency('elogind', required: false, static: get_option('enable-static-deps'))
    if not dep_dbus.found()
      error('enable-dbus=elogind requires elogind library, but it was not found.')
    endif
  endif
endif

# Define global dependencies
skalibs_dep = dep_skalibs
execline_dep = dep_execline
oblibs_dep = dep_oblibs
lib66_dep = dep_lib66
dbus_dep = dep_dbus
doc_dep = dependency('lowdown', version: version_lowdown, required: false, static: get_option('enable-static-deps'))

# Special case for default_options
BINDIR = get_option('prefix') / get_option('bindir')
LIBEXECDIR = get_option('prefix') / get_option('libexecdir')
INCLUDEDIR = get_option('prefix') / get_option('includedir') / '66-tools'
DATADIR = get_option('prefix') / get_option('datadir')

# Final key value
ns_rule_dir = get_option('ns-rule-dir')
dbus_system_service_dir = get_option('dbus-system-service-dir')
dbus_session_service_dir = get_option('dbus-session-service-dir')
dbus_system_name = get_option('dbus-system-name')
dbus_session_name = get_option('dbus-session-name')

# Common compiler flags
base_c_args = [
  '-pipe',
  '-fomit-frame-pointer',
  '-fno-exceptions',
  '-fno-unwind-tables',
  '-fno-asynchronous-unwind-tables',
  '-Werror=implicit-function-declaration',
  '-Werror=implicit-int',
  '-Werror=pointer-sign',
  '-Werror=pointer-arith',
  '-Wno-unused-value',
  '-Wno-parentheses',
  '-ffunction-sections',
  '-fdata-sections',
  '-D_POSIX_C_SOURCE=200809L',
  '-D_XOPEN_SOURCE=700',
  '-D_GNU_SOURCE',
  '-I' + get_option('includedir')
]

# Compiler flags
shared_c_args = base_c_args + ['-fPIC']
static_c_args = get_option('enable-all-pic') ? base_c_args + ['-fPIC'] : base_c_args
exe_c_args = base_c_args

# Linker flags
shared_link_args = [
  '-shared',
  '-Wl,--as-needed',
  '-Wl,--no-undefined',
  '-Wl,-O2',
  '-Wl,--sort-section=alignment',
  '-Wl,--sort-common'
]

if cc.has_link_argument('-Wl,--hash-style=both')
  shared_link_args += ['-Wl,--hash-style=both']
endif

static_link_args = get_option('enable-static-deps') ? ['-static', '-Wl,--gc-sections'] : []
exe_link_args = []
if get_option('enable-static-executable')
  exe_link_args += ['-static']
endif

# Add include paths
foreach inc : get_option('with-include-dir')
  if inc != '' and fs.is_dir(inc) and inc != meson.project_source_root() / 'src/include'
    add_project_arguments('-I' + inc, language: 'c')
  endif
endforeach

# Add library paths
foreach lib : get_option('with-staticlib-dir')
  if lib != '' and fs.is_dir(lib)
    shared_link_args += ['-L' + lib]
    static_link_args += ['-L' + lib]
    exe_link_args += ['-L' + lib]
  endif
endforeach
foreach dynlib : get_option('with-dynamiclib-dir')
  if dynlib != '' and fs.is_dir(dynlib)
    shared_link_args += ['-L' + dynlib]
    exe_link_args += ['-L' + dynlib]
  endif
endforeach

# Verify static libc availability if enable-static-executable is true
if get_option('enable-static-executable')
  static_libc_test = cc.find_library('c', static: true, required: false)
  if not static_libc_test.found()
    error('enable-static-executable requires a static libc, but it was not found on the system')
  endif
  # Test linking a static program
  static_link_test = cc.links('int main() { return 0; }', name: 'static executable test', args: ['-static'], dependencies: [static_libc_test])
  if not static_link_test
    error('enable-static-executable requires a toolchain capable of linking fully static executables')
  endif
endif

# Validate options to avoid conflicts
if get_option('enable-static-executable') and get_option('enable-shared')
  error('Cannot enable both enable-static-executable and enable-shared')
endif
if get_option('enable-static-deps') and get_option('enable-shared')
  error('Cannot enable both enable-static-deps and enable-shared')
endif
if get_option('enable-static-deps') and not get_option('enable-static')
  error('enable-static-deps requires enable-static to build *.a')
endif

# Validate DBus service directories
foreach opt : ['dbus-system-service-dir', 'dbus-session-service-dir']
  dir = get_option(opt)
  if not dir.startswith('/')
    error(dir + ' must be absolute path e.g., /usr/share/dbus-1/system-services.')
  endif
endforeach

# Validate DBus socket names
foreach opt : ['dbus-system-name', 'dbus-session-name']
  name = get_option(opt)
  if name == ''
    error(opt + ' cannot be empty.')
  endif
endforeach

# Include directories
incdir = include_directories(['src/include'])

# Subdirectories
subdir('src/include/66-tools')
subdir('src')
subdir('doc')
