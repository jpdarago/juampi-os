CC=c99
CFLAGS=-Wall -ggdb -pedantic -O1 -DTEST_FLAG
LDLIBS=-lcheck
OBJECTS= utils_test.o utils.o

.PHONY: clean

%.o: %.asm
	nasm -felf -g -F dwarf -o $@ $^

utils_tests: $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

clean:
	rm -rf *.o utils_tests
