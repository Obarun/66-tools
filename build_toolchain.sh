#!/bin/sh

skalibs_tag="v2.9.1.0"
execline_tag="v2.5.3.0"
s6_tag="v2.9.0.1"
s6_rc_tag="v0.5.1.1"
oblibs_tag="v0.0.5.0"

## skalibs
build_skalibs() {

	git clone https://github.com/skarnet/skalibs
	cd skalibs
	git checkout tags/"${skalibs_tag}"
	./configure \
		--prefix=/usr \
		--with-default-path=/usr/bin \
		--enable-shared
	
	make install || return 1
	cd ..
}

## execline
build_execline() {

	git clone https://github.com/skarnet/execline
	cd execline
	git checkout tags/"${execline_tag}"
	./configure \
		--prefix=/usr \
		--libexecdir=/usr/libexec \
		--bindir=/usr/bin \
		--sbindir=/usr/bin \
		--shebangdir=/usr/bin \
		--enable-shared
	
	make install || return 1
	cd ..
}

## s6
build_s6() {

	git clone https://github.com/skarnet/s6
	cd s6
	git checkout tags/"${s6_tag}"
	./configure \
		--prefix=/usr \
		--bindir=/usr/bin \
		--sbindir=/usr/bin \
		--enable-shared
	
	make install || return 1
	cd ..
}

## s6-rc
build_s6_rc() {
	git clone https://github.com/skarnet/s6-rc
	cd s6-rc
	git checkout tags/"${s6_rc_tag}"
	./configure \
		--prefix=/usr \
		--bindir=/usr/bin \
		--sbindir=/usr/bin \
		--enable-shared
	
	make install || return 1
	cd ..
}

## oblibs
build_oblibs() {
	
	git clone https://framagit.org/obarun/oblibs
	cd oblibs
	./configure \
		--enable-shared
	
	make install || return 1
	cd ..
}

_run() {

	if ! ${1} ; then
		printf "%s" "unable to build ${1#*_}"
		exit 1
	fi
}

## do it
_run build_skalibs
_run build_execline
#_run build_s6
#_run build_s6_rc
_run build_oblibs
