#!/bin/sh

html='66-clock 66-getenv 66-gnwenv 66-olexec 66-which 66-writenv 66-yeller execl-cmdline execl-subuidgid execl-toc 66-ns index'

version=${1}

if [ ! -d doc/${version}/html ]; then
    mkdir -p -m 0755 doc/${version}/html || exit 1
fi

for i in ${html};do
     lowdown -s doc/${i}.md -o doc/${version}/html/${i}.html || exit 1
done

exit 0
