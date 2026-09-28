# SurfOS: memory manager
# Makefile
# (C)2004 Brandon Burr
#######################
# Rebuilt 9/28/2026 - not in the printout, see REBUILD-NOTES.md

OBJS = kalloc.o memory.o paging.o palloc.o
LIBRARY = surfmm.o

RM = /bin/rm -f
CP = /bin/cp

CFLAGS = -fwritable-strings -ffreestanding -fno-builtin -nostdlib -nostdinc -I../include -Wall -Wno-main #-O2


.c.o: $(CC) $(CFLAGS) -c $< -o $@
.S.o: $(CC) $(CFLAGS) -traditional -c $< -o $@

all: $(OBJS)
	$(LD) -r -o $(LIBRARY) $(OBJS)

clean:
	$(RM) *.o
