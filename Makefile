# SurfOS top-level Makefile
# (C)2004 Brandon Burr. Rebuilt 2026 for a current GCC / binutils / NASM toolchain:
# one non-recursive Makefile replaces the per-directory ones (see docs/ROADMAP.md, Phase 0).
#
#   make              build $(KERNEL)            (O=2 for an optimised build)
#   make run          boot it in QEMU on this terminal: serial console, Ctrl-A x quits
#   make run-vga      same with a VGA window (needs a display); serial on the terminal
#   make test         boot headless and run the smoke test over the serial console
#   make test-all     smoke test at -O0 and -O2
#   make unittest     host-side unit tests for blibc (formatter, strings)
#   make debug        boot paused with the gdb stub on :1234
#   make iso          bootable ISO via grub-mkrescue (grub-pc-bin, xorriso, mtools)
#   make images       build/images/test.img (MBR + FAT) and initrd.tar (rootfs/), used by run and test
#   make clean

O         ?= 0
BUILD     ?= build/O$(O)
KERNEL     = $(BUILD)/surfos.bin
ISO        = $(BUILD)/surfos.iso
IMAGES     = build/images
DISKIMG    = $(IMAGES)/test.img
INITRD     = $(IMAGES)/initrd.tar
DISK_FILES = README rootfs/motd docs/README.md $(BUILD)/user/bin/hello
ROOTFS_FILES = $(shell find rootfs -type f 2>/dev/null)
# user programs (roadmap P2): static ELF at 0x40000000, libsurf = syscall stubs + blibc
USER_CFLAGS  = $(CFLAGS) -Iuser/include
USER_LIB_SRC = user/libsurf/syscalls.c lib/blibc/vsnprintf.c lib/blibc/string.c lib/blibc/memory.c \
               lib/blibc/stdlib.c lib/blibc/ctype.c
USER_LIB_OBJ = $(USER_LIB_SRC:%.c=$(BUILD)/user/lib/%.o)
USER_CRT0    = $(BUILD)/user/lib/crt0.o
USER_PROGS   = hello crash cat ls count sh
USER_BINS    = $(USER_PROGS:%=$(BUILD)/user/bin/%)


CC         = gcc
LD         = ld
NASM       = nasm
NM         = nm
QEMU       = qemu-system-i386
PYTHON     = python3
GRUB_MKRESCUE = grub-mkrescue

QEMUMEM   ?= 64
QEMUFLAGS ?=
# the test disk on the primary IDE master and the initrd as a Multiboot module (see tools/mkimage.py)
QEMUDISK   = -drive file=$(DISKIMG),format=raw,if=ide,index=0,media=disk -initrd $(INITRD)

# V=1 shows the full commands
Q = $(if $(V),,@)

# Freestanding 32-bit code: no PIE, no stack protector, no CET, no SSE, no unwind tables.
# -fgnu89-inline keeps the 2004 "inline" definitions visible to other files (audit A7).
# The FPU is left enabled on purpose: the shell's speaker tune uses float arithmetic.
CFLAGS     = -m32 -march=i486 -ffreestanding -fno-builtin -nostdlib -nostdinc \
             -fno-pie -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables \
             -fcf-protection=none -mno-sse -mno-sse2 -mno-mmx \
             -fgnu89-inline -fno-strict-aliasing -fno-omit-frame-pointer \
             -Wall -Wno-main -g -O$(O) -Iinclude -MMD -MP
ASFLAGS    = -m32 -ffreestanding -nostdlib -fno-pie -g -Iinclude -MMD -MP
NASMFLAGS  = -felf32 -g -F dwarf
LDFLAGS    = -m elf_i386 -nostdlib -T linker.ld

# boot.o must come first so the Multiboot header lands inside the first 8 KB of the image.
SRC_S      = boot/boot.S
SRC_C      = $(wildcard kernel/*.c) $(wildcard mm/*.c) $(wildcard lib/blibc/*.c) \
             $(wildcard shell/*.c) $(wildcard fs/*.c) $(wildcard net/*.c) $(wildcard driver/*.c) $(wildcard driver/net/*.c) $(wildcard driver/dma/*.c) \
             $(wildcard driver/floppy/*.c) $(wildcard driver/net/3c905b/*.c)
# kernel/setjmp.asm is a dead pre-Multiboot entry stub and is not built.
SRC_ASM    = kernel/traps.asm driver/dma/dma-asm.asm

OBJS       = $(SRC_S:%.S=$(BUILD)/%.o) $(SRC_C:%.c=$(BUILD)/%.o) $(SRC_ASM:%.asm=$(BUILD)/%.o)
DEPS       = $(OBJS:.o=.d) $(USER_LIB_OBJ:.o=.d) $(USER_PROGS:%=$(BUILD)/user/obj/%.d)

all: $(KERNEL)

# The kernel is linked twice: pass 1 with an empty symbol table to learn the addresses,
# pass 2 with the real table (tools/gensyms.py) for backtraces. .ksyms sits after the
# code in linker.ld, so no text address changes between the passes.
STAGE1   = $(BUILD)/surfos.stage1
KSYMS0_C = $(BUILD)/ksyms0.c
KSYMS_C  = $(BUILD)/ksyms.c

$(KSYMS0_C): tools/gensyms.py
	@mkdir -p $(dir $@)
	$(Q)$(PYTHON) tools/gensyms.py < /dev/null > $@

$(STAGE1): $(OBJS) $(BUILD)/ksyms0.o linker.ld
	@echo "LD   $@ (pass 1)"
	$(Q)$(LD) $(LDFLAGS) -o $@ $(OBJS) $(BUILD)/ksyms0.o

$(KSYMS_C): $(STAGE1) tools/gensyms.py
	$(Q)$(NM) -n $(STAGE1) | $(PYTHON) tools/gensyms.py > $@

$(BUILD)/ksyms0.o: $(KSYMS0_C)
	$(Q)$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/ksyms.o: $(KSYMS_C)
	$(Q)$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL): $(OBJS) $(BUILD)/ksyms.o linker.ld
	@echo "LD   $@"
	$(Q)$(LD) $(LDFLAGS) -o $@ $(OBJS) $(BUILD)/ksyms.o
	$(Q)$(NM) -n $@ > $(BUILD)/surfos.sym
	@echo "built $@ ($$(stat -c %s $@) bytes, -O$(O))"

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "CC   $<"
	$(Q)$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	@echo "AS   $<"
	$(Q)$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/%.o: %.asm
	@mkdir -p $(dir $@)
	@echo "NASM $<"
	$(Q)$(NASM) $(NASMFLAGS) -MD $(@:.o=.d) $< -o $@

# -no-reboot makes a triple fault (or the shell's "reboot" command) end the emulator
# instead of looping through the boot log forever.
run: $(KERNEL) images
	$(QEMU) -m $(QEMUMEM) -kernel $(KERNEL) $(QEMUDISK) -nographic -no-reboot $(QEMUFLAGS)

run-vga: $(KERNEL) images
	$(QEMU) -m $(QEMUMEM) -kernel $(KERNEL) $(QEMUDISK) -serial stdio -no-reboot $(QEMUFLAGS)

test: $(KERNEL) images
	$(PYTHON) tools/qemu-run.py --qemu $(QEMU) --kernel $(KERNEL) --mem $(QEMUMEM) --disk $(DISKIMG) --initrd $(INITRD) --test

test-all:
	$(MAKE) O=0 test
	$(MAKE) O=2 test

debug: $(KERNEL) images
	@echo "in another terminal: gdb $(KERNEL) -ex 'target remote :1234'"
	$(QEMU) -m $(QEMUMEM) -kernel $(KERNEL) $(QEMUDISK) -nographic -no-reboot -s -S $(QEMUFLAGS)

iso: $(ISO)

$(ISO): $(KERNEL) boot/grub.cfg $(INITRD)
	rm -rf $(BUILD)/iso
	mkdir -p $(BUILD)/iso/boot/grub
	cp $(KERNEL) $(BUILD)/iso/boot/surfos.bin
	cp $(INITRD) $(BUILD)/iso/boot/initrd.tar
	cp boot/grub.cfg $(BUILD)/iso/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $@ $(BUILD)/iso 2>&1 | grep -v "^xorriso" || true
	@test -s $@ && echo "built $@" || { echo "grub-mkrescue failed (is grub-pc-bin installed?)"; rm -f $@; exit 1; }

run-iso: $(ISO) $(DISKIMG)
	$(QEMU) -m $(QEMUMEM) -cdrom $(ISO) -boot d -drive file=$(DISKIMG),format=raw,if=ide,index=0,media=disk -nographic -no-reboot $(QEMUFLAGS)

# ---- images -------------------------------------------------------------------------------
images: $(DISKIMG) $(INITRD)

$(IMAGES):
	$(Q)mkdir -p $@

$(INITRD): $(ROOTFS_FILES) $(USER_BINS) tools/mkimage.py | $(IMAGES)
	@echo "IMG  $@"
	$(Q)rm -rf $(IMAGES)/initrd && mkdir -p $(IMAGES)/initrd/bin && cp -r rootfs/. $(IMAGES)/initrd/ && cp $(USER_BINS) $(IMAGES)/initrd/bin/
	$(Q)$(PYTHON) tools/mkimage.py initrd $@ $(IMAGES)/initrd

# ---- user programs --------------------------------------------------------------------
user: $(USER_BINS)

$(BUILD)/user/lib/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "UCC  $<"
	$(Q)$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_CRT0): user/libsurf/crt0.S
	@mkdir -p $(dir $@)
	@echo "UAS  $<"
	$(Q)$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/user/obj/%.o: user/bin/%.c
	@mkdir -p $(dir $@)
	@echo "UCC  $<"
	$(Q)$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/user/bin/%: $(BUILD)/user/obj/%.o $(USER_CRT0) $(USER_LIB_OBJ) user/user.ld
	@mkdir -p $(dir $@)
	@echo "ULD  $@"
	$(Q)$(LD) -m elf_i386 -nostdlib -T user/user.ld -o $@ $(USER_CRT0) $< $(USER_LIB_OBJ)

$(DISKIMG): $(DISK_FILES) tools/mkimage.py | $(IMAGES)
	@echo "IMG  $@"
	$(Q)$(PYTHON) tools/mkimage.py disk $@ 16 $(DISK_FILES)

# host-side unit tests for the pure parts of blibc (formatter, strings), see tests/host/
HOSTCC    ?= gcc
# -m32 when the host can link 32-bit programs (libc6-dev-i386), so long is 32 bits like in the kernel
HOST32    := $(shell echo 'int main(void){return 0;}' | $(HOSTCC) -m32 -x c - -o /dev/null 2>/dev/null && echo -m32)
HOSTFLAGS ?= $(HOST32) -O1 -g -Wall
HOSTLIB_SRC = lib/blibc/vsnprintf.c lib/blibc/string.c lib/blibc/memory.c lib/blibc/stdlib.c lib/blibc/ctype.c
HOSTLIB_OBJ = $(HOSTLIB_SRC:lib/blibc/%.c=$(BUILD)/host/%.o)

$(BUILD)/host/%.o: lib/blibc/%.c tests/host/surf_names.h
	@mkdir -p $(dir $@)
	@echo "HOSTCC $<"
	$(Q)$(HOSTCC) $(HOSTFLAGS) -ffreestanding -fno-builtin -nostdinc -Iinclude -include tests/host/surf_names.h -c $< -o $@

$(BUILD)/host/test_blibc: tests/host/test_blibc.c $(HOSTLIB_OBJ)
	@echo "HOSTLD $@"
	$(Q)$(HOSTCC) $(HOSTFLAGS) $^ -o $@

unittest: $(BUILD)/host/test_blibc
	$(BUILD)/host/test_blibc

clean:
	rm -rf build

help:
	@sed -n '2,13p' Makefile

.PHONY: all run run-vga test test-all unittest debug iso run-iso images user clean help
-include $(DEPS)
