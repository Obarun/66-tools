fs = import('fs')

exe_configs = {
  '66-clock': {
    'sources': ['66-clock.c'],
    'deps': [oblibs_dep]
  },
  '66-getenv': {
    'sources': ['66-getenv.c'],
    'deps': [oblibs_dep]
  },
  '66-gnwenv': {
    'sources': ['66-gnwenv.c'],
    'deps': [oblibs_dep]
  },
  '66-ns': {
    'sources': [
      'ns_cli.c', 'ns_dir.c', 'ns_elements.c', 'ns_mount.c', 'ns_net.c',
      'ns_parse.c', 'ns_run.c', 'ns_session.c', 'ns_supervise.c', 'ns_userns.c',
    ],
    'deps': [oblibs_dep, lib66_dep]
  },
  '66-olexec': {
    'sources': ['66-olexec.c'],
    'deps': [oblibs_dep]
  },
  '66-which': {
    'sources': ['66-which.c'],
    'deps': [oblibs_dep]
  },
  '66-writenv': {
    'sources': ['66-writenv.c'],
    'deps': [oblibs_dep]
  },
  '66-yeller': {
    'sources': ['66-yeller.c'],
    'deps': [oblibs_dep]
  },
  'execl-cmdline': {
    'sources': ['execl-cmdline.c'],
    'deps': [oblibs_dep]
  },
  'execl-subuidgid': {
    'sources': ['execl-subuidgid.c'],
    'deps': [oblibs_dep]
  },
  'execl-toc': {
    'sources': ['execl-toc.c'],
    'deps': [oblibs_dep]
  }
}

# Special handling for 66-dbus-launch
if get_option('enable-dbus') != 'disabled'
  # Define temporary lib66dbusbroker (not installed)
  dbus_launch_dir = meson.current_source_dir() / '66-dbus-launch'
  find_dbus_sources_cmd = run_command(
    'sh', '-c',
    'find "@0@" -maxdepth 1 -type f -name "*.c" ! -name "66-dbus-launch.c"'.format(dbus_launch_dir),
    check: false
  )
  if find_dbus_sources_cmd.returncode() != 0
    error('Failed to find source files in ' + dbus_launch_dir + ': ' + find_dbus_sources_cmd.stderr())
  endif

  lib66dbusbroker_sources = []
  foreach file : find_dbus_sources_cmd.stdout().strip().split('\n')
    if file != '' and fs.is_file(file)
      lib66dbusbroker_sources += files(file)
    endif
  endforeach

  if lib66dbusbroker_sources.length() == 0
    error('No source files found for lib66dbusbroker in ' + dbus_launch_dir)
  endif

  lib66dbusbroker = static_library(
    '66dbusbroker',
    lib66dbusbroker_sources,
    include_directories: [incdir, include_directories('66-dbus-launch')],
    dependencies: [lib66_dep, oblibs_dep],
    c_args: static_c_args,
    link_args: static_link_args,
    install: false
  )

  lib66dbusbroker_dep = declare_dependency(
    link_with: lib66dbusbroker,
    include_directories: [incdir, include_directories('66-dbus-launch')],
    dependencies: [lib66_dep, oblibs_dep]
  )

  # Add 66-dbus-launch to exe_configs
  exe_configs += {
    '66-dbus-launch': {
      'sources': ['66-dbus-launch.c'],
      'deps': [lib66dbusbroker_dep, lib66_dep, oblibs_dep, dbus_dep]
    }
  }
endif

# Special handling for 66-userd
# The directory holds three artifacts sharing the same model: the daemon
# (66-userd), the query CLI (66-userctl) and the PAM module (pam_userd.so).
# The shared code is gathered in a temporary static library (not installed);
# the PAM module compiles the two units it needs itself, since it must be
# position independent and never links the whole model.
userd_dir = meson.current_source_dir() / '66-userd'

lib66userd = static_library(
  '66userd',
  files(
    userd_dir / 'common.c',
    userd_dir / 'driver.c',
    userd_dir / 'leader.c',
    userd_dir / 'power.c',
    userd_dir / 'runtimedir.c',
    userd_dir / 'session.c',
    userd_dir / 'state.c',
    userd_dir / 'user.c',
    userd_dir / 'userenv.c',
  ),
  include_directories: [incdir, include_directories('66-userd')],
  dependencies: [lib66_dep, oblibs_dep],
  c_args: static_c_args,
  link_args: static_link_args,
  install: false
)

lib66userd_dep = declare_dependency(
  link_with: lib66userd,
  include_directories: [incdir, include_directories('66-userd')],
  dependencies: [lib66_dep, oblibs_dep]
)

# Add 66-userd and 66-userctl to exe_configs
exe_configs += {
  '66-userd': {
    'sources': ['66-userd.c'],
    'deps': [lib66userd_dep, lib66_dep, oblibs_dep]
  },
  '66-userctl': {
    'sources': ['66-userctl.c'],
    'deps': [lib66userd_dep, lib66_dep, oblibs_dep]
  }
}

# pam_userd.so is dlopen'd by libpam: it is neither an executable nor a
# library of this project, so it stays out of the executable loop below.
# It lands in libpam's compiled SECUREDIR, fixed by libpam and NOT relative
# to this project prefix, hence the absolute userd-pam-dir option.
# Every symbol must resolve at load time: fail the build (not silently at
# dlopen) if a source providing one is missing.
if get_option('enable-static-deps')
  warning('enable-static-deps: pam_userd.so is a shared module, so the static libraries it links must have been built as position independent code, otherwise the link of the module fails.')
endif

pam_link_args = ['-Wl,--no-undefined']
foreach lib : get_option('with-staticlib-dir') + get_option('with-dynamiclib-dir')
  if lib != '' and fs.is_dir(lib)
    pam_link_args += ['-L' + lib]
  endif
endforeach

shared_module(
  'pam_userd',
  files(
    userd_dir / 'pam_userd.c',
    userd_dir / 'common.c',
    userd_dir / 'user.c',
  ),
  name_prefix: '',
  include_directories: [incdir, include_directories('66-userd')],
  dependencies: [oblibs_dep, pam_dep],
  c_args: shared_c_args,
  link_args: pam_link_args,
  install: true,
  install_dir: get_option('userd-pam-dir')
)

bin_dir = meson.current_source_dir()
find_dirs_cmd = run_command('sh', '-c', 'find "@0@" -maxdepth 1 -type d ! -path "@0@"'.format(bin_dir), check: false)
if find_dirs_cmd.returncode() != 0
  error('Failed to run find command for subdirectories: ' + find_dirs_cmd.stderr())
endif

exe_dirs = []
foreach dir : find_dirs_cmd.stdout().strip().split('\n')
  if dir != '' and fs.is_dir(dir)
    exe_dirs += fs.name(dir)
  endif
endforeach

if exe_dirs.length() == 0
  warning('No subdirectories found in src/')
endif

foreach dir : exe_dirs
  exe_dir = bin_dir / dir
  foreach exe, config : exe_configs
    exe_sources = []
    foreach src : config['sources']
      src_path = exe_dir / src
      if fs.is_file(src_path)
        exe_sources += files(src_path)
      endif
    endforeach

    if exe_sources.length() == 0
      continue  # Skip if no source files found in this directory
    endif

    install_dir = get_option('bindir')

    link_args = get_option('enable-static-executable') ? static_link_args : exe_link_args

    executable(exe,
      sources: exe_sources,
      include_directories: incdir,
      dependencies: config['deps'],
      install: true,
      install_dir: install_dir,
      c_args: exe_c_args,
      link_args: link_args,
      pie: get_option('enable-pie')
    )

    # Optional test
    if get_option('test')
      test_dir = exe_dir / 'test'
      if fs.is_dir(test_dir)
        find_test_files_cmd = run_command('sh', '-c', 'find "@0@" -type f -name "*.c"'.format(test_dir), check: false)
        if find_test_files_cmd.returncode() != 0
          error('Failed to run find command for test files in ' + test_dir + ': ' + find_test_files_cmd.stderr())
        endif

        test_files = []
        foreach file : find_test_files_cmd.stdout().strip().split('\n')
          if file != '' and fs.is_file(file)
            test_files += files(file)
          endif
        endforeach

        if test_files.length() > 0
          test_deps = config['deps']
          test_incdirs = incdir
          # For 66-dbus-launch tests, ensure lib66dbusbroker_dep is included
          if exe == '66-dbus-launch' and get_option('enable-dbus') != 'disabled'
            test_deps = [lib66dbusbroker_dep, lib66_dep, oblibs_dep, dbus_dep]
            test_incdirs = [include_directories('66-dbus-launch'), incdir]
          endif
          test_exe = executable('test_@0@'.format(exe),
            sources: test_files,
            include_directories: test_incdirs,
            dependencies: test_deps,
            build_by_default: false,
            c_args: exe_c_args,
            link_args: link_args,
            pie: get_option('enable-pie')
          )
          test('@0@_test'.format(exe), test_exe)
        endif
      endif
    endif
  endforeach
endforeach

