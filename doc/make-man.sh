#!/bin/sh -e

man1='66-clock 66-getenv 66-gnwenv 66-olexec 66-which 66-writenv 66-yeller execl-cmdline execl-subuidgid execl-toc 66-ns'

mkdir -p doc/man/man1

for f in ${man1}; do
    lowdown -s -Tman doc/"${f}".md -o doc/man/man1/"${f}".1
    var=$(sed -n '/^.TH/p' doc/man/man1/"${f}".1 | tr '7' '1')
    sed -i "s!^.TH.*!${var}!" doc/man/man1/"${f}".1
    sed -i '4,8d' doc/man/man1/"${f}".1
done
