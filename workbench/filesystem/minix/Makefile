P=filesystem
CC=c99
CFLAGS= -DTEST_FLAG -Wall -ggdb --pedantic -I.
LDLIBS= -lcheck
OBJECTS= fs_minix_test.o fs_minix.o bitset.o buffer_cache.o bitset_search.o exception.o list.o hdd.o rw_sem.o sem.o

$(P): hdd.img $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

hdd.img:
	./build_image.sh

%.o: %.asm
	nasm -f elf -o $@ $^

.PHONY: clean tests
clean:
	rm -rf hdd.img filesystem *.o

tests:
	./filesystem
