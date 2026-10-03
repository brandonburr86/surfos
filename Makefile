# SurfOS top-level Makefile
# (C)2004 Brandon Burr. Rebuilt 2026 for a current GCC / binutils / NASM toolchain:
# one non-recursive Makefile replaces the per-directory ones (see docs/ROADMAP.md, Phase 0).
#
#   make              build $(KERNEL)            (O=2 for an optimised build)
#   make run          boot it in QEMU on this terminal: serial console, Ctrl-A x quits
#   make run-vga      same with a VGA window (needs a display); serial on the terminal
#   make test         boot headless and run the smoke test over the serial console
#   make test-all     smoke test at -O0 and -O2
#   make debug        boot paused with the gdb stub on :1234
#   make iso          bootable ISO via grub-mkrescue (grub-pc-bin, xorriso, mtools)
#   make clean

O         ?= 0
BUILD     ?= build/O$(O)
KERNEL     = $(BUILD)/surfos.bin
ISO        = $(BUILD)/surfos.iso

CC         = gcc
LD         = ld
NASM       = nasm
NM         = nm
QEMU       = qemu-system-i386
PYTHON     = python3
GRUB_MKRESCUE = grub-mkrescue

QEMUMEM   ?= 64
QEMUFLAGS ?=

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
             $(wildcard shell/*.c) $(wildcard driver/*.c) $(wildcard driver/dma/*.c) \
             $(wildcard driver/floppy/*.c) $(wildcard driver/net/3c905b/*.c)
# kernel/setjmp.asm is a dead pre-Multiboot entry stub and is not built.
SRC_ASM    = kernel/assem.asm driver/dma/dma-asm.asm

OBJS       = $(SRC_S:%.S=$(BUILD)/%.o) $(SRC_C:%.c=$(BUILD)/%.o) $(SRC_ASM:%.asm=$(BUILD)/%.o)
DEPS       = $(OBJS:.o=.d)

all: $(KERNEL)

$(KERNEL): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)
	$(NM) -n $@ > $(BUILD)/surfos.sym
	@echo "built $@ ($$(stat -c %s $@) bytes, -O$(O))"

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(NASM) $(NASMFLAGS) -MD $(@:.o=.d) $< -o $@

# -no-reboot makes a triple fault (or the shell's "reboot" command) end the emulator
# instead of looping through the boot log forever.
run: $(KERNEL)
	$(QEMU) -m $(QEMUMEM) -kernel $(KERNEL) -nographic -no-reboot $(QEMUFLAGS)

run-vga: $(KERNEL)
	$(QEMU) -m $(QEMUMEM) -kernel $(KERNEL) -serial stdio -no-reboot $(QEMUFLAGS)

test: $(KERNEL)
	$(PYTHON) tools/qemu-run.py --qemu $(QEMU) --kernel $(KERNEL) --mem $(QEMUMEM) --test

test-all:
	$(MAKE) O=0 test
	$(MAKE) O=2 test

debug: $(KERNEL)
	@echo "in another terminal: gdb $(KERNEL) -ex 'target remote :1234'"
	$(QEMU) -m $(QEMUMEM) -kernel $(KERNEL) -nographic -no-reboot -s -S $(QEMUFLAGS)

iso: $(ISO)

$(ISO): $(KERNEL) boot/grub.cfg
	rm -rf $(BUILD)/iso
	mkdir -p $(BUILD)/iso/boot/grub
	cp $(KERNEL) $(BUILD)/iso/boot/surfos.bin
	cp boot/grub.cfg $(BUILD)/iso/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $@ $(BUILD)/iso 2>&1 | grep -v "^xorriso" || true
	@test -s $@ && echo "built $@" || { echo "grub-mkrescue failed (is grub-pc-bin installed?)"; rm -f $@; exit 1; }

run-iso: $(ISO)
	$(QEMU) -m $(QEMUMEM) -cdrom $(ISO) -boot d -nographic -no-reboot $(QEMUFLAGS)

clean:
	rm -rf build

help:
	@sed -n '2,13p' Makefile

.PHONY: all run run-vga test test-all debug iso run-iso clean help
-include $(DEPS)
