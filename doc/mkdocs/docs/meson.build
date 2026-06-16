foreach page : mkdocs_pages
  doc_targets += custom_target(
    page,
    input: meson.project_source_root() / 'doc' / (page + '.md'),
    output: page + '.md',
    command: [
      'sh', build_mkdocs, '@INPUT@', '@OUTPUT@',
    ] + mkdocs_sed_args,
    build_by_default: true,
  )
endforeach

# Copy doc/assets/ (logo, favicon, custom css, version-selector js)
# into builddir/doc/mkdocs/docs/assets/ so `mkdocs build` includes them
# in the site output.
doc_targets += custom_target(
  'mkdocs-assets',
  input: [
    meson.project_source_root() / 'doc' / 'assets' / 'logo.svg',
    meson.project_source_root() / 'doc' / 'assets' / 'favicon.ico',
    meson.project_source_root() / 'doc' / 'assets' / 'extra.css',
    meson.project_source_root() / 'doc' / 'assets' / 'version-selector.js',
  ],
  output: 'assets',
  command: [
    'sh',
    meson.project_source_root() / 'doc' / 'tools' / 'copy-assets.sh',
    meson.project_source_root() / 'doc' / 'assets',
    meson.current_build_dir() / 'assets',
  ],
  build_by_default: true,
)
