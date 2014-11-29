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

Para instalar las dependencias, ejecutar

    sudo apt-get install nasm e2tools
    ./install-bochs.sh

Instalar
--------

Es necesario tener sudo para poder armar la imagen de disco Minix. Esto
deberia desaparecer en un futuro lo mas cercano.

PRECAUCION: LA MANERA EN LA QUE SE HACE LA IMAGEN DE DISCO ES MONTANDO EN
/dev/loop0 Y MONTANDO EN MOUNT. REVISAR build/build_image.sh ANTES DE CORRERLO.

Para compilar y correr, ejecutar:

    make && make run

Para otros comandos mirar el Makefile

TODOs
------

* 64 bits.
* Multicore.
* Implementar logica para SIGSEGV.
* Mejor shell (pipes, mas comandos, etc.).
* Portear una libc.
* Multiples procesos puedan acceder al filesystem.
* Background jobs.
* Signals para procesos (groups).
* Permisos en el filesystem.
* Kernel preemteable.
* _Swapping_ a disco.
* I/O de disco por DMA.
* Optimizar los algoritmos.
* Driver VGA o VESA.

Las dos primeras se implementaron para el proyecto DeliriOS (<http://github.com/Izikiel/intel_multicore>).
Veanlo porque esta bueno.

Agradecimientos
---------------

* OSDEV Wiki: <http://osdev.org>
* James Molloy kernel development tutorials: <http://jamesmolloy.co.uk/tutorial_html>
