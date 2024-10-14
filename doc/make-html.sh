#!/bin/sh -e

html='66-clock 66-dbus-launch 66-getenv 66-gnwenv 66-olexec 66-which 66-writenv 66-yeller execl-cmdline execl-subuidgid execl-toc 66-ns index upgrade'

version=${1}

mkdir -p doc/${version}/html

for i in ${html};do
     lowdown -s doc/${i}.md -o doc/${version}/html/${i}.html
done
