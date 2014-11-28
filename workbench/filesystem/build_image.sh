#!/bin/bash

#Construir la imagen usando el directorio testdir para los archivos

#Paso 1: Crear el .img (dd)
dd if=/dev/zero of=hdd.img bs=512 count=32K
#Paso 2: Armar el sistema de archivos (mkdosfs -F 16)
mkfs -t msdos -F 16 -f 2 -h 1 -n TESTDRV -r 16 -R 1 -s 1 hdd.img
#Paso 3: Montarlo
sudo losetup /dev/loop0 hdd.img
sudo mount /dev/loop0 /mnt
#Paso 4: Copiar el directorio
sudo cp -r testimg/* /mnt
#Paso 4.5: Setear permisos
sudo fatattr +r /mnt/dir1/archivoBloqueado.txt
#Paso 5: Desmontarlo a imagen
sudo umount /dev/loop0
#sleep 1
sudo losetup -d /dev/loop0
