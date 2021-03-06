CC=gcc
ASM=nasm
LINKER=ld
LINK.o=ld

CFLAGS=-m32 -O2 -std=c99 -Werror -Wall -Wextra\
	-Wno-unused-parameter -Wno-override-init\
	-Wunreachable-code -Wshadow -Wcast-qual \
	-Wformat=2 -Wwrite-strings -Wstrict-prototypes\
	-Wredundant-decls -Wnested-externs -Wmissing-include-dirs\
	-Wjump-misses-init -Wlogical-op\
	-nostdlib -fno-builtin -nostartfiles\
	-nodefaultlibs -fno-stack-protector -I./include

NASMFLAGS=-i./include/ -felf32
LINKSCRIPT=build/linker.ld
LINKERFLAGS=-melf_i386 -T $(LINKSCRIPT)

.PHONY=all clean

CSOURCES=$(wildcard src/**/*.c src/*.c)
CINCLUDES=$(wildcard include/*.h)
ASMSOURCES=$(wildcard src/**/*.asm src/*.asm)
ASMOBJS=$(patsubst %.asm,%.o,$(ASMSOURCES))
COBJS=$(patsubst %.c,%.o,$(CSOURCES))

TASKSSRC=$(wildcard build/inittasks/*.asm build/inittasks/*.c)
TASKS=$(patsubst %.{c,h},%.o,$(TASKSSRC))

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $^

%.o: %.asm
	$(ASM) $(NASMFLAGS) -o $@ $^

kernel.bin: $(COBJS) $(ASMOBJS)
	$(LINKER) $(LINKERFLAGS) -o $@ $^

hdd.img: build/tasks build/docs
	cd build && ./build_image.sh

init: build/bootstrap
	cd build/bootstrap && make

all: init hdd.img kernel.bin
	cp floppy_raw.img floppy.img
	e2cp kernel.bin floppy.img:/
	e2cp build/menu.lst floppy.img:/boot/grub/menu.lst
	e2cp build/bootstrap/init floppy.img:/init

BOCHSDIR?=./bochs/bin
BOCHSCONF?=run/bochsrc.txt
run: all
	$(BOCHSDIR)/bochs -q -f $(BOCHSCONF)

clean:
	cd build/bootstrap && make clean
	cd build/tasks && make clean
	rm -rf $(ASMOBJS) $(COBJS) kernel.bin hdd.img floppy.img

format:
	uncrustify -c uncrustify.cfg --replace --no-backup $(CSOURCES) $(CINCLUDES)
