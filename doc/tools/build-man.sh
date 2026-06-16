#!/bin/sh
# Build a single man page from a markdown source.
#
# Args:
#   $1  page name (basename without .md, e.g. "66-version")
#   $2  section number (1, 5, or 8)
#   $3  project version (used in .TH "source" field)
#   $4  input .md file (without frontmatter)
#   $5  output .N file
#   $6+ sed substitution args for %%placeholder%% expansion
#
# Lowdown emits a default ".TH \"\" \"7\" \"\"" line at line 2. We overwrite it
# with a properly formed .TH including page, section, date, source and manual.

set -eu

page="$1"
section="$2"
version="$3"
input="$4"
output="$5"
shift 5

sed "$@" "$input" | lowdown -s -Tman -o "$output"

date=$(date -u +%Y-%m-%d)
sed -i "2s|.*|.TH ${page} ${section} \"${date}\" \"66-tools ${version}\" \"66-tools Suite\"|" "$output"
