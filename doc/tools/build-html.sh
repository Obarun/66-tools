#!/bin/sh
# Build a single HTML page from a markdown source.
#
# Args:
#   $1  page name (basename without .md, e.g. "66-version")
#   $2  header template path (doc/html-header.md.in)
#   $3  input .md file (without frontmatter)
#   $4  output .html file
#   $5+ sed substitution args for %%placeholder%% expansion
#
# The header template is prepended to the page content (with @PAGE@ substituted),
# then the whole stream is piped through lowdown -s to produce standalone HTML.

set -eu

page="$1"
header="$2"
input="$3"
output="$4"
shift 4

{
    sed "s|@PAGE@|${page}|g" "$header"
    sed "$@" "$input"
} | lowdown -s -o "$output"
