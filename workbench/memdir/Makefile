CC=c99
CFLAGS=-Wall -ggdb -pedantic -DTEST_FLAG
LDLIBS=-lcheck
BITSET_OBJECTS= bitset_test.o bitset.o bitset_search.o

.PHONY: clean

%.o: %.asm
	nasm -felf -g -F dwarf -o $@ $^

bitset_test: $(BITSET_OBJECTS)

bitset_search_test: bitset_search_test.o bitset.o bitset_search.o
	$(CC) -ggdb -o $@ $^

clean:
	rm -rf *.o bitset_test bitset_search_test
