# SurfOS: blibc
# Makefile
# (C)2004 Brandon Burr
#######################

OBJS = ctype.o chars.o strings.o printf.o memory.o string.o time.o
LIBRARY = blibc.o

CFLAGS = -fwritable-strings -ffreestanding -fno-builtin -nostdlib -nostdinc -I../../include -Wall -Wno-main -O2

RM = /bin/rm -f

all: $(OBJS)
	$(LD) -r -o $(LIBRARY) $(OBJS)

clean:
	$(RM) *.o

.c.o: $(CC) $(CFLAGS) -c $< -o $@
