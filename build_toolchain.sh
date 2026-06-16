#!/bin/sh

tag=1

if [ "$1" == "commit" ]; then
    tag=0
fi

oblibs_tag="0.3.4.0"

check_tag(){

    if ((tag)); then
        git checkout tags/"${1}"
    fi
}

## oblibs
build_oblibs() {

    git clone https://git.obarun.org/obarun/oblibs
    cd oblibs

    check_tag "${oblibs_tag}"

    meson setup builddir -D prefix=/usr || return 1
    meson compile -C builddir || return 1
    meson install -C builddir || return 1
    cd ..
}

_run() {

    if ! ${1} ; then
        printf "%s" "unable to build ${1#*_}"
        exit 1
    fi
}

## do it
_run build_oblibs
