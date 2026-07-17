#!/bin/bash
set -euo pipefail

# This script installs Bochs into the current directory, with SMP support and
# the other goodies enabled. It builds it automatically to avoid version
# mismatches.
#
# PREREQUISITE: libgtk2.0-dev

if [[ -f ./bochs/bin/bochs ]]; then
	echo "Bochs is already installed"
	exit 0
fi

install() {
    DIR=$(pwd)
    pushd $(mktemp -d)
    URL="http://downloads.sourceforge.net/project/bochs/bochs/2.6.2/bochs-2.6.2.tar.gz"
    # Bochs needs GTK
    wget -O bochs.tar.gz $URL
    mkdir $DIR/bochs
    tar zxvf bochs.tar.gz
    cd bochs-2.6.2
    export LDFLAGS=-lpthread
    ./configure --enable-smp --enable-debugger --enable-disasm\
        --enable-readline --enable-cpu-level=6\
        --enable-all-optimizations --prefix="$DIR/bochs"\
        && make -j && make install
    cd ../..
    popd
}

install
