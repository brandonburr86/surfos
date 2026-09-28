# SurfOS: DMA driver
# Makefile
# (C)2004 Brandon Burr
#######################

OBJS = dma.o dma-asm.o dma-mm.o
LIBRARY = dma-drv.o

RM = /bin/rm -f
CP = /bin/cp

CFLAGS = -fwritable-strings -ffreestanding -fno-builtin -nostdlib -nostdinc -I../../include -Wall -Wno-main -O2


.c.o: $(CC) $(CFLAGS) -c $< -o $@
.S.o: $(CC) $(CFLAGS) -traditional -c $< -o $@



all: $(OBJS)
	$(LD) -r -o $(LIBRARY) $(OBJS)

clean:
	$(RM) *.o

dma-asm.o:
	nasm -felf -o dma-asm.o dma-asm.asm
