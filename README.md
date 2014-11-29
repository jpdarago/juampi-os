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

TODOs
------

* 64 bits.
* Multicore.
* Implementar logica para SIGSEGV.
* Pipes.
* Mejor shell.
* Portear una libc.
* Multiples procesos puedan acceder al filesystem.
* Background jobs.
* Signals para procesos (groups).
* Permisos en el filesystem.

Las dos primeras se implementaron para el proyecto DeliriOS (<github.com/Izikiel/intel_multicore>).
Veanlo porque esta bueno.

Agradecimientos
---------------

* OSDEV Wiki: <osdev.org>
* James Molloy kernel development tutorials: <http://jamesmolloy.co.uk/tutorial_html>
