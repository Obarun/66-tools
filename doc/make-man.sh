#!/bin/sh

man1='66-clock 66-getenv 66-gnwenv 66-olexec 66-which 66-writenv 66-yeller execl-cmdline execl-subuidgid execl-toc 66-ns'

for i in 1 ;do
    if [ ! -d doc/man/man"${i}" ]; then
       mkdir -p -m 0755 doc/man/man"${i}"
    fi
done

for i in ${man1}; do
    lowdown -s -Tman doc/"${i}".md -o doc/man/man1/"${i}".1
    var=$(head -n1 < doc/man/man1/"${i}".1)
    var=$(printf '%s' "$var" | tr '7' '1')
    var="${var} \"\" \"General Commands Manual\""
    sed -i "s!^.TH.*!${var}!" doc/man/man1/"${i}".1
    sed -i '2,7d' doc/man/man1/"${i}".1
done

man_5(){
    for i in ${man5}; do
        lowdown -s -Tman doc/"${i}".md -o doc/man/man5/"${i}".5
        var=$(head -n1 < doc/man/man5/"${i}".5)
        var=$(printf '%s' "$var" | tr '7' '5')
        var="${var} \"\" \"File Formats Manual\""
        sed -i "s!^.TH.*!${var}!" doc/man/man5/"${i}".5
        sed -i '2,7d' doc/man/man5/"${i}".5
    done
}

man_8(){
    for i in ${man8}; do
        lowdown -s -Tman doc/"${i}".md -o doc/man/man8/"${i}".8
        var=$(head -n1 < doc/man/man8/"${i}".8)
        var=$(printf '%s' "$var" | tr '7' '8')
        var="${var} \"\" \"System Administration\""
        sed -i "s!^.TH.*!${var}!" doc/man/man8/"${i}".8
        sed -i '2,7d' doc/man/man8/"${i}".8
    done
}
