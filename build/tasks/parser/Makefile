CC=c99
CFLAGS=-Wall -ggdb -pedantic -nostdlib -nodefaultlibs -fno-builtin
LDFLAGS=-lcheck
CSOURCES=$(wildcard *.c)
ASMSOURCES=$(wildcard *.asm)
ASMOBJS=$(patsubst %.asm,%.o,$(ASMSOURCES))
COBJS=$(patsubst %.c,%.o,$(CSOURCES))
P=parser_tester

.DEFAULT_GOAL := $(P)

%.o: %.asm
	nasm -felf -g -F dwarf -o $@ $^

parser_tester.o: parser_tester.c
	$(CC) -ggdb -Wall -pedantic -c -o $@ $^

$(P): $(COBJS) $(ASMOBJS)
	$(CC) $(FLAGS) -o $@ $^ $(LDFLAGS)

.PHONY: clean
clean:
	rm -rf $(P) *.o
