# SurfOS Bootloader Makefile
# (C)2004 Brandon Burr
############################

FLAGS = -fwritable-strings -ffreestanding -fno-builtin -nostdlib -nostdinc -I../include -Wall -Wno-main

.S.o: $(CC) $(CFLAGS) -traditional -c $< -o $@

all: boot.o

clean:
	rm -f *.o
