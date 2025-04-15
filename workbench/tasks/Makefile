CC=gcc
CFLAGS=-Wall
LINK.o=ld
LDLIBS=
LDFLAGS=

%.o: %.asm
	nasm -felf32 -g -F dwarf -o $@ $^

%.o: %.c
	c99 -c -o $@ $^
	
even_simpler_lib: even_simpler_lib.o math.o
	ld -o $@ $^

tarea_inicial: tarea_inicial.o
	ld -o $@ $^

.PHONY: clean
clean:
	rm -rf *.o simpler even_simpler even_simpler_lib tss tarea_inicial
