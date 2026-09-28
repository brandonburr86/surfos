# SurfOS
# Top Level Makefile
# (C)2004 Brandon Burr
#######################

BOOTDIR = boot
BLIBCDIR = lib/blibc
TESTDIR = lib/test
SHELLDIR = shell
KERNELDIR = kernel
DRIVERDIR = driver
MMDIR = mm
BINDIR = bin

CP = cp
DD = dd


MAKE=make  #switch to make for linux compile

SUBDIRS =$(BOOTDIR) $(BLIBCDIR) $(MMDIR) $(DRIVERDIR) $(SHELLDIR) $(KERNELDIR) #kernel must be last

all:
	for i in $(SUBDIRS) ; do \
	( cd $$i ; $(MAKE) ) ; \
	done


boot:
	@cd $(BOOTDIR); $(MAKE)

mm:
	@cd $(MMDIR) ; $(MAKE)

blibc:
	@cd $(BLIBCDIR) ; $(MAKE)

shell:
	@cd $(SHELLDIR) ; $(MAKE)

kernel:
	@cd $(KERNELDIR) ; $(MAKE)


copy: all #copy image to floppy drive
	sudo mount /floppy;  $(CP) $(KERNELDIR)/surfos.bin /floppy;  sudo umount /floppy;


bochs: copy
	bochs -qf bochsrc

clean:
	for i in $(SUBDIRS) ; do \
	( cd $$i ; $(MAKE) clean) ; \
	done

#misc stuff added 6/29/04

zip: clean
	zip -r surfos.zip *

backup: zip
	chmod +x sendzip
	./sendzip
