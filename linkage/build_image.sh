#!/bin/bash
set -euo pipefail

#Construir la imagen usando el directorio testdir para los archivos
cd "$(dirname "$0")"
#Paso 1: Crear el .img (dd)
../bochs/bin/bximage -hd -mode=flat -size=16 -q hdd.img
#Paso 2: Armar el sistema de archivos
mkfs -V -t minix hdd.img
#Paso 3: Montarlo
sudo losetup /dev/loop0 hdd.img
sleep 1
sudo mount /dev/loop0 /mnt
#Paso 4: Armar los directorios
sudo cp -r docs /mnt
make -C tasks/
sudo mkdir /mnt/tasks
sudo cp tasks/*.run /mnt/tasks
sudo mkdir /mnt/dev
sudo mknod /mnt/dev/tty c 0 0
#Paso 5: Desmontarlo a imagen
sudo umount /dev/loop0
sleep 1
sudo losetup -d /dev/loop0
#Paso 6: Ponerlo donde va
mv hdd.img ../hdd.img
