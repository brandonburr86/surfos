# SurfOS
# 3c905b network driver makefile
# (C)2004 Brandon Burr
#######################


OBJS = 3c905b.o wnet.o
LIBRARY = 3c905b-drv.o

RM = /bin/rm -f
CP = /bin/cp

MAKE=gmake

CFLAGS = -fwritable-strings -ffreestanding -fno-builtin -nostdlib -nostdinc -I../../../include -Wall -Wno-main -O2


.c.o: $(CC) $(CFLAGS) -c $< -o $@
.S.o: $(CC) $(CFLAGS) -traditional -c $< -o $@


all: $(OBJS)
	$(LD) -r -o $(LIBRARY) $(OBJS)

clean:
	rm -f *.o
