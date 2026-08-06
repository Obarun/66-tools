#!/bin/sh

tag=1

if [ "$1" == "commit" ]; then
    tag=0
fi

oblibs_tag="0.4.0.0"
ss_tag="0.9.0.0"

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

    meson setup builddir || return 1
    meson compile -C builddir || return 1
    meson install -C builddir || return 1
    cd ..
}

## 66
build_66() {

    git clone https://git.obarun.org/obarun/66
    cd 66

    check_tag "${ss_tag}"

    meson setup builddir || return 1
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
_run build_66
