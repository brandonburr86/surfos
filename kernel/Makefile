# SurfOS Kernel Makefile
# (C)2004 Brandon Burr
#

NASM = nasm
CC = gcc
INCLUDE = ../include
DD = dd

OBJS = ../boot/boot.o main.o assem.o gdt.o timer.o interrupt.o task.o turf.o panic.o console.o \
		sys.o keyboard.o ../lib/blibc/blibc.o ../shell/shell.o ../mm/surfmm.o  \
		../driver/driver.o

KERNIMAGE = surfkernel.bin

CFLAGS = -fwritable-strings -ffreestanding -fno-builtin -nostdlib -nostdinc -I../include -Wall -Wno-main #-O2

.c.o: $(CC) $(CFLAGS) -c $< -o $@
.S.o: $(CC) $(CFLAGS) -traditional -c $< -o $@

all: $(OBJS)
		ld -oformat=elf -e start -Ttext 0x100000 -o surfos.bin ${OBJS}

clean:
	rm -f *.o
	rm -f *.bin

assem.o:
	${NASM} -felf -o assem.o assem.asm

setjmp.o:
	${NASM} -felf -o setjmp.o setjmp.asm

bin: $(OBJS)
	#ld -oformat=pei-i386 -emain -Ttext 0x7400 -omain2.bin ${OBJS}  #0x7400
	#objcopy --input-target=pei-i386 --output-target=binary main2.bin ../bin/surfos.bin #ckern.bin
	#cat asmkern.bin ckern.bin > $(KERNIMAGE)
	#ld -oformat=elf -emain  -o../bin/surfos.bin ${OBJS} #-Ttext 0x7400
