if not get_option('with-doc')
  subdir_done()
endif

ss_version = meson.project_version()
lowdown = find_program('lowdown', required: true)

build_html = files('tools/build-html.sh')
build_man = files('tools/build-man.sh')
build_mkdocs = files('tools/build-mkdocs.sh')
html_header = files('html-header.md.in')

# Placeholder substitutions shared by all formats.
sed_args = [
  '-e', 's|%%dbus_system_service%%|@0@|g'.format(dbus_system_service_dir),
  '-e', 's|%%dbus_session_service%%|@0@|g'.format(dbus_session_service_dir),
  '-e', 's|%%dbus_system_name%%|@0@|g'.format(dbus_system_name),
  '-e', 's|%%dbus_session_name%%|@0@|g'.format(dbus_session_name),
  '-e', 's|%%ns_rule%%|@0@|g'.format(ns_rule_dir),
  '-e', 's|%%datarootdir%%|@0@|g'.format(DATADIR),
]

# Pages — HTML output (also used as the mkdocs page set).
html_pages = [
  '66-clock', '66-dbus-launch', '66-getenv', '66-gnwenv', '66-ns',
  '66-olexec', '66-which', '66-writenv', '66-yeller', 'execl-cmdline',
  'execl-subuidgid', 'execl-toc', 'index', 'upgrade', 'prototype',
]

# Pages — man section 1.
man1_pages = [
  '66-clock', '66-dbus-launch', '66-getenv', '66-gnwenv', '66-ns',
  '66-olexec', '66-which', '66-writenv', '66-yeller', 'execl-cmdline',
  'execl-subuidgid', 'execl-toc',
]

# Mkdocs uses the same set as HTML.
mkdocs_pages = html_pages

# Mkdocs needs all placeholder substitutions PLUS rewriting internal links
# from (NAME.html) to (NAME.md). Build the extended sed args once.
mkdocs_sed_args = sed_args
foreach name : html_pages
  mkdocs_sed_args += ['-e', 's|(@0@.html|(@0@.md|g'.format(name)]
endforeach

# Collected to build a single `doc` alias target. Use
# `meson compile -C build doc` to build only the documentation (HTML,
# man pages, mkdocs sources) without recompiling the C code — useful
# for the CI docbuild job.
doc_targets = []

html_install_dir = get_option('datadir') / 'doc' / '66-tools' / ss_version / 'html'

foreach page : html_pages
  doc_targets += custom_target(
    page + '.html',
    input: page + '.md',
    output: page + '.html',
    command: [
      'sh', build_html, page, html_header, '@INPUT@', '@OUTPUT@',
    ] + sed_args,
    install: true,
    install_dir: html_install_dir,
    install_tag: 'html',
    build_by_default: true,
  )
endforeach

man1_install_dir = get_option('mandir') / 'man1'

foreach page : man1_pages
  doc_targets += custom_target(
    page + '.1',
    input: page + '.md',
    output: page + '.1',
    command: [
      'sh', build_man, page, '1', ss_version, '@INPUT@', '@OUTPUT@',
    ] + sed_args,
    install: true,
    install_dir: man1_install_dir,
    install_tag: 'man1',
    build_by_default: true,
  )
endforeach

subdir('mkdocs')

alias_target('doc', doc_targets)
