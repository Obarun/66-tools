#!/bin/sh
# Copy doc/assets/* into the mkdocs build dir for inclusion in the site.
# Args:
#   $1  source assets dir (doc/assets/)
#   $2  destination dir (will be created if missing)

set -eu

src="$1"
dst="$2"

mkdir -p "$dst"
cp -r "$src"/. "$dst/"
