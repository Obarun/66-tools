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
