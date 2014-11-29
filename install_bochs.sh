#!/bin/bash
set -euo pipefail

# El proposito de este script es instalar bochs en la carpeta actual, con
# soporte para SMP y demas chiches. Lo hace automaticamente para evitar
# problemas de versiones.
#
# PREREQUISITO: libgtk2.0-dev

if [[ -f ./bochs/bin/bochs ]]; then
	echo "Bochs ya esta instalado"
	exit 0
fi

install() {
    DIR=$(pwd)
    pushd $(mktemp -d)
    URL="http://downloads.sourceforge.net/project/bochs/bochs/2.6.2/bochs-2.6.2.tar.gz"
    #Prerequisito para bochs, tener gtk
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
