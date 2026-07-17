CC=gcc
ASM=nasm
CFLAGS=-m32 -std=c99 -mno-mmx -mno-sse -mno-sse2 -mno-3dnow -nostdlib -fno-builtin -nostartfiles -nodefaultlibs -fno-stack-protector -Wall -O0
ASMFLAGS=-i../../include/ -f elf32
OBJ=syscall_wrappers.o stdio.o utils.o parser.o errors.o
TASKS=cp.run ls.run cat.run shell.run echo.run date.run

%.o: %.asm
	$(ASM) $(ASMFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $^

%.run: %.o $(OBJ)
	ld -emain -melf_i386 -o $@ $^

all: $(OBJ) $(TASKS)

clean:
	rm -rf *.o $(TASKS)

.PHONY: clean
