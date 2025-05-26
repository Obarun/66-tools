fs = import('fs')

ss_version = meson.project_version()

if not get_option('with-doc')
  subdir_done()
endif

# Find lowdown program (required only if doc option is enabled)
lowdown = find_program('lowdown', required: get_option('with-doc'))

# Generate HTML files
html_result = run_command('sh', 'make-html-meson.sh', ss_version, check: false)
if html_result.returncode() != 0
  warning('make-html.sh failed: ' + html_result.stderr())
endif

# Generate man pages
man_result = run_command('sh', 'make-man-meson.sh', check: false)
if man_result.returncode() != 0
  warning('make-man.sh failed: ' + man_result.stderr())
endif

# HTML
html_path = join_paths('doc', ss_version, 'html')
html_list = run_command('sh', '-c', 'find "$path" -maxdepth 1 -type f', env: ['path=' + html_path], check: false)
if html_list.returncode() != 0
  error('Failed to run find command for files in: ' + html_path + html_list.stderr())
endif

# Process all documentation files
foreach doc_html : html_list.stdout().strip().split('\n')
  if not fs.is_file(doc_html)
    warning('Documentation file ' + doc_html + ' not found')
    continue
  endif

  doc_output = doc_html.split('/').get(-1)

  # Determine installation directory
  install_dir = join_paths(get_option('datadir'), 'doc', '66-tools', ss_version, 'html')

  # Perform sed replacements
  configure_file(
    input: doc_html,
    output: doc_output,
    command: [
      'sed',
      '-e', 's|%%dbus_system_service%%|' + dbus_system_service_dir + '|g',
      '-e', 's|%%dbus_session_service%%|' + dbus_session_service_dir + '|g',
      '-e', 's|%%dbus_system_name%%|' + dbus_system_name + '|g',
      '-e', 's|%%dbus_session_name%%|' + dbus_session_name + '|g',
      '-e', 's|%%ns_rule%%|' + ns_rule_dir + '|g',
      '-e', 's|%%datarootdir%%|' + DATADIR + '|g',
      '@INPUT@'
    ],
    capture: true,
    install: true,
    install_dir: install_dir,
    install_mode: 'rw-r--r--',
    install_tag: 'html'
  )
endforeach

# MAN1
man1_path = join_paths('doc', 'man', 'man1')
man1_list = run_command('sh', '-c', 'find "$path" -maxdepth 1 -type f', env: ['path=' + man1_path], check: false)
if man1_list.returncode() != 0
  error('Failed to run find command for files in: ' + man1_path + man1_list.stderr())
endif

foreach doc_man1 : man1_list.stdout().strip().split('\n')
  if not fs.is_file(doc_man1)
    warning('Documentation file ' + doc_man1 + ' not found')
    continue
  endif

  doc_output = doc_man1.split('/').get(-1)

  install_dir = join_paths(get_option('datadir'), 'man', 'man1')

  configure_file(
    input: doc_man1,
    output: doc_output,
    command: [
      'sed',
      '-e', 's|%%dbus_system_service%%|' + dbus_system_service_dir + '|g',
      '-e', 's|%%dbus_session_service%%|' + dbus_session_service_dir + '|g',
      '-e', 's|%%dbus_system_name%%|' + dbus_system_name + '|g',
      '-e', 's|%%dbus_session_name%%|' + dbus_session_name + '|g',
      '-e', 's|%%ns_rule%%|' + ns_rule_dir + '|g',
      '-e', 's|%%datarootdir%%|' + DATADIR + '|g',
      '@INPUT@'
    ],
    capture: true,
    install: true,
    install_dir: install_dir,
    install_mode: 'rw-r--r--',
    install_tag: 'man1'
  )
endforeach
