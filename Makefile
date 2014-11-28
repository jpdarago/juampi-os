CC=gcc
ASM=nasm
LINKER=ld
LINK.o=ld
#Considerar retirar el -Os si empieza a andar mal
CFLAGS=-m32 -O2 -std=c99 -Wall -nostdlib -fno-builtin -nostartfiles -nodefaultlibs -fno-stack-protector -I./include

NASMFLAGS=-i./include/ -felf 
LINKSCRIPT=linkage/linker.ld
LINKERFLAGS=-melf_i386 -T $(LINKSCRIPT)

.PHONY=all clean

CSOURCES=$(wildcard src/**/*.c src/*.c)
ASMSOURCES=$(wildcard src/**/*.asm src/*.asm)
ASMOBJS=$(patsubst %.asm,%.o,$(ASMSOURCES))
COBJS=$(patsubst %.c,%.o,$(CSOURCES))

TASKSSRC=$(wildcard linkage/inittasks/*.asm linkage/inittasks/*.c)
TASKS=$(patsubst %.{c,h},%.o,$(TASKSSRC))

%.o: %.c
	@echo "===COMPILANDO $^==="
	$(CC) $(CFLAGS) -c -o $@ $^

%.o: %.asm
	@echo "===COMPILANDO $^==="
	$(ASM) $(NASMFLAGS) -o $@ $^

kernel.bin: $(COBJS) $(ASMOBJS)
	@echo "===LINKEANDO EL KERNEL==="
	$(LINKER) $(LINKERFLAGS) -o $@ $^

hdd.img: linkage/tasks linkage/docs
	@echo "==GENERANDO IMAGEN DE DISCO DURO==="
	cd linkage && ./build_image.sh

init: linkage/bootstrap
	cd linkage/bootstrap && make

all: init hdd.img kernel.bin 
	@echo "===METIENDO EL KERNEL EN EL FLOPPY==="
	cp floppy_raw.img floppy.img
	e2cp kernel.bin floppy.img:/
	e2cp linkage/menu.lst floppy.img:/boot/grub/menu.lst
	e2cp linkage/bootstrap/init floppy.img:/init

BOCHSDIR?=/home/jpdarago/bochs/bin
BOCHSCONF?=run/bochsrc.txt
run: all
	@echo "==CORRIENDO SIMULACION=="
	$(BOCHSDIR)/bochs -q -f $(BOCHSCONF)

clean:
	cd linkage/bootstrap && make clean
	cd linkage/tasks && make clean
	rm -rf $(ASMOBJS) $(COBJS) kernel.bin hdd.img
