CC=c99
CFLAGS=-Wall -ggdb -O2
SOURCES=$(wildcard *.c)
OBJS=$(patsubst %.c,%.o,$(SOURCES)) 

shell: $(OBJS)
	$(CC) -o $@ $^

clean:
	rm -rf $(OBJS) shell
