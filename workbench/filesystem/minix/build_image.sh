#!/bin/bash

#Construir la imagen usando el directorio testdir para los archivos

#Paso 1: Crear el .img (dd)
bximage -hd -mode=flat -size=16 -q hdd.img
#Paso 2: Armar el sistema de archivos
mkfs -V -t minix hdd.img > disk_data.txt
#Paso 3: Montarlo
sudo losetup /dev/loop0 hdd.img
sleep 1
sudo mount /dev/loop0 /mnt
#Paso 4: Copiar el directorio y crear los inodos especiales
sudo cp -r testimg/* /mnt
sudo mkdir /mnt/dev
sudo mknod /mnt/dev/tty c 0xDE 0xAD
#Paso 5: Desmontarlo a imagen
sudo umount /dev/loop0
sleep 1
sudo losetup -d /dev/loop0
