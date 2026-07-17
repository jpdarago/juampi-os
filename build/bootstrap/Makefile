CC=gcc
ASM=nasm
CFLAGS=-m32 -std=c99 -mno-mmx -mno-sse -mno-sse2 -mno-3dnow -nostdlib -fno-builtin -nostartfiles -nodefaultlibs -fno-stack-protector -Wall -O0
ASMFLAGS=-i../../include/ -f elf32
OBJ=syscall_wrappers.o

%.o: %.asm
	$(ASM) $(ASMFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $^

init: init.o $(OBJ)
	ld -emain -melf_i386 -o $@ $^

all: $(OBJ)

clean:
	rm -rf *.o init

.PHONY: clean
