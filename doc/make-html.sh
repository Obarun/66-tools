#!/bin/sh

html='66-clock 66-getenv 66-gnwenv 66-olexec 66-which 66-writenv execl-cmdline execl-envfile execl-subuidgid execl-toc index'

if [ ! -d doc/html ]; then
    mkdir -p -m 0755 doc/html
fi

for i in ${html};do
     lowdown -s doc/${i}.md -o doc/html/${i}.html
done
