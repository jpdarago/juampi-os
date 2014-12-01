juampi-os
=========

Mi kernel con el TP final de Organización del Computador II (UBA - FCEyN).

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

    sudo apt-get install nasm e2tools libgtk2.0-dev
    ./install-bochs.sh

Instalar
--------

Es necesario tener sudo para poder armar la imagen de disco Minix. Esto
deberia desaparecer en un futuro lo mas cercano.

PRECAUCION: LA MANERA EN LA QUE SE HACE LA IMAGEN DE DISCO ES MONTANDO EN
/dev/loop0 Y MONTANDO EN /mnt. REVISAR build/build_image.sh ANTES DE CORRER
EL COMANDO make run PARA NO ROMPER NADA.

Para compilar y correr, ejecutar:

    make && make run

Para otros comandos mirar el Makefile

Documentación
-------------

El informe del trabajo práctico se encuentra en la carpeta informe/.
Para generar el PDF ejecutar desde la carpeta:

    make

TODOs
------

* 64 bits.
* Multicore.
* Implementar logica para SIGSEGV.
* Mejor shell (pipes, _output redirection_, mas comandos, etc.).
* Portear una libc.
* Multiples procesos puedan acceder al filesystem.
* _Background jobs_.
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

* Ver agradecimientos en el informe.
* OSDEV Wiki: <http://osdev.org>
* James Molloy kernel development tutorials: <http://jamesmolloy.co.uk/tutorial_html>
