dir = meson.current_source_dir()
sources = run_command(
    'sh', '-c',
    'find "@0@" -maxdepth 1 -type f -name "*.rule"'.format(dir),
    check: false
)

message(dir)
install_data(sources: sources.stdout().split(), install_dir: ns_rule_dir)


