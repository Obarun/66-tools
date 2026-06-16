#!/bin/sh
# Build a single mkdocs source file from a markdown source.
#
# Args:
#   $1  input .md file (without frontmatter)
#   $2  output .md file
#   $3+ sed substitution args:
#         - 21 %%placeholder%% expansions
#         - 44 (NAME.html) -> (NAME.md) rewrites for internal links

set -eu

input="$1"
output="$2"
shift 2

sed "$@" "$input" > "$output"
