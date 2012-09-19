CFLAGS=-O2 -DDEBUG=0 -ggdb -Wall -D_FILE_OFFSET_BITS=64 -DHAVE_SETXATTR -I/usr/include/fuse
LDFLAGS=-pthread -lfuse -lrt -ldl
CC=gcc

muse: muse.o
	$(CC) $(LDFLAGS) $< -o $@
clean:
	rm -f *.o muse
