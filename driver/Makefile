# SurfOS
# Driver Makefile
# (C)2004 Brandon Burr
#######################
# Rebuilt 9/28/2026 - not in the printout, see REBUILD-NOTES.md

SUBDIRS = dma floppy net

OBJS = drivers.o parport.o pci.o
LIBS = dma/dma-drv.o floppy/floppy-drv.o net/netdriver.o

LIBRARY = driver.o

RM = /bin/rm -f
CP = /bin/cp

CFLAGS = -fwritable-strings -ffreestanding -fno-builtin -nostdlib -nostdinc -I../include -Wall -Wno-main -O2


.c.o: $(CC) $(CFLAGS) -c $< -o $@
.S.o: $(CC) $(CFLAGS) -traditional -c $< -o $@


all: $(OBJS)
	for i in $(SUBDIRS) ; do \
	( cd $$i ; $(MAKE)) ; \
	done
	$(LD) -r -o $(LIBRARY) $(OBJS) $(LIBS)

clean:
	$(RM) *.o
	for i in $(SUBDIRS) ; do \
	( cd $$i ; $(MAKE) clean) ; \
	done
