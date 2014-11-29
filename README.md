juampi-os
=========

Mi kernel con el TP final de Orga 2

Features
--------

* 32 bits.
* Multitarea con scheduler.
* Disco duro ATA por PIO.
* VFS y Filesystem de Minix.
* Loader de ELF32
* Mini libc
* Asignador de memoria
* _copy on write_

Dependencias
------------

Ejecutar:

    sudo apt-get install coreutils nasm e2tools
    ./install-bochs.sh

Instalar
--------

Despues de instalar las dependencias, necesitas sudo para poder montar el disco.
Ejecutar:

    make && make run
