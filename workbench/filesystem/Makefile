P=filesystem
CC=c99
CFLAGS= -DTEST_FLAG -Wall -ggdb --pedantic
LDLIBS= -lcheck
OBJECTS= fs.o list.o buffer.o hdd.o memory.o exception.o

$(P):	$(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
	./$(P)

list_test: list_test.o list.o memory.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
	./list_test

.PHONY: clean
clean:
	rm -rf fs *.o
