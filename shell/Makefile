# SurfOS: basic shell (ring0)
# Makefile
# (C)2004 Brandon Burr
#######################

OBJS = main.o parport.o
LIBRARY = shell.o

RM = /bin/rm -f
CP = /bin/cp

CFLAGS = -fwritable-strings -ffreestanding -fno-builtin -nostdlib -nostdinc -I../include -Wall -Wno-main -O2


.c.o: $(CC) $(CFLAGS) -c $< -o $@
.S.o: $(CC) $(CFLAGS) -traditional -c $< -o $@

all: $(OBJS)
	$(LD) -r -o $(LIBRARY) $(OBJS)

clean:
	$(RM) *.o
