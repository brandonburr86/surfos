# SurfOS
# Net Driver Makefile
# (C)2004 Brandon Burr
#######################

SUBDIRS = 3c905b

OBJS = 3c905b/3c905b-drv.o

LIBRARY = netdriver.o

RM = /bin/rm -f
CP = /bin/cp

MAKE=gmake

CFLAGS = -fwritable-strings -ffreestanding -fno-builtin -nostdlib -nostdinc -I../../include -Wall -Wno-main -O2


.c.o: $(CC) $(CFLAGS) -c $< -o $@
.S.o: $(CC) $(CFLAGS) -traditional -c $< -o $@


all:
	for i in $(SUBDIRS) ; do \
	( cd $$i ; $(MAKE)) ; \
	done
	$(LD) -r -o $(LIBRARY) $(OBJS)

clean:
	rm -f *.o
	for i in $(SUBDIRS) ; do \
	( cd $$i ; $(MAKE) clean) ; \
	done
