# Set CROSS to a cross-compiler prefix (e.g. i686-elf-) to build without the
# host gcc-multilib; the top-level Makefile passes it down.
CROSS ?=
CC=$(CROSS)gcc
ASM=nasm
LD=$(CROSS)ld
CFLAGS=-std=c99 -mno-mmx -mno-sse -mno-sse2 -mno-3dnow -nostdlib -fno-builtin -nostartfiles -nodefaultlibs -fno-stack-protector -Wall -O0
ifeq ($(strip $(CROSS)),)
CFLAGS += -m32
endif
ASMFLAGS=-i../../include/ -f elf32
OBJ=syscall_wrappers.o stdio.o utils.o parser.o errors.o
TASKS=cp.run ls.run cat.run shell.run echo.run date.run

%.o: %.asm
	$(ASM) $(ASMFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $^

%.run: %.o $(OBJ)
	$(LD) -emain -melf_i386 -o $@ $^

all: $(OBJ) $(TASKS)

clean:
	rm -rf *.o $(TASKS)

.PHONY: clean
