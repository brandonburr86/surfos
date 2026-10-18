# SurfOS top-level Makefile
# (C)2004 Brandon Burr. Rebuilt 2026 for a current GCC / binutils / NASM toolchain:
# one non-recursive Makefile replaces the per-directory ones (see docs/ROADMAP.md, Phase 0).
#
#   make              build $(KERNEL)            (O=2 for an optimised build)
#   make run          boot it in QEMU on this terminal: serial console, Ctrl-A x quits
#   make run-vga      same with a VGA window (needs a display); serial on the terminal
#   make test         boot headless and run the smoke test over the serial console
#   make test-all     smoke test at -O0 and -O2
#   make test-tcp     TCP against hand-made segments on a raw link (tools/qemu-tcp.py; the smoke test runs it too)
#   make unittest     host-side unit tests for blibc (formatter, strings) and pure kernel code (mkfs, the block cache and FAT too)
#   make debug        boot paused with the gdb stub on :1234
#   make iso          bootable ISO via grub-mkrescue (grub-pc-bin, xorriso, mtools)
#   make images       build/images/test.img (MBR + FAT) and initrd.tar (rootfs/), used by run and test
#   make usb-image    build/usb/surfos-ideapad-15iil05.img: GPT + FAT32 + GRUB for UEFI (docs/USB-BOOT.md)
#   make test-usb     boot that exact image under QEMU + OVMF from an emulated USB stick and test it
#   make test-devloop the development loop's host side: tools/devloop.py, the data stick image, the build id
#   make clean

O         ?= 0
BUILD     ?= build/O$(O)
KERNEL     = $(BUILD)/surfos.bin
ISO        = $(BUILD)/surfos.iso
IMAGES     = build/images
DISKIMG    = $(IMAGES)/test.img
INITRD     = $(IMAGES)/initrd.tar
FOREIGNIMG = $(IMAGES)/foreign-fat32.img
DISK_FILES = README rootfs/motd docs/README.md $(BUILD)/user/bin/hello
ROOTFS_FILES = $(shell find rootfs -type f 2>/dev/null)
# user programs (roadmap P2): static ELF at 0x40000000 linked with libsurf.a, the user C library
# (10/5/26). Programs see only user/include, never the kernel's headers; the library also finds
# the system call numbers in include/surfos/syscall.h (-idirafter). Sections per function and
# --gc-sections keep each program to what it uses.
USER_CFLAGS  = $(filter-out -Iinclude,$(CFLAGS)) -Iuser/include -ffunction-sections -fdata-sections \
               -fno-tree-loop-distribute-patterns
USER_LIB_CFLAGS = $(USER_CFLAGS) -idirafter include
USER_LIB_SRC = $(wildcard user/libsurf/*.c)
USER_LIB_OBJ = $(USER_LIB_SRC:%.c=$(BUILD)/user/lib/%.o)
USER_LIB     = $(BUILD)/user/lib/libsurf.a
USER_CRT0    = $(BUILD)/user/lib/crt0.o
USER_PROGS   = hello crash cat ls count sh echo cp mv rm mkdir rmdir touch df wc head hexdump fstest edit pagetest surfsh kcmdtest nettest \
               telnetd
USER_BINS    = $(USER_PROGS:%=$(BUILD)/user/bin/%)
USER_PROGS  += ptytest       # 10/6/26 (the telnet server): pseudo-terminals, poll, procinfo, detach, /dev/kmsg
USER_PROGS  += files         # text file browser, independent of the web browser
USER_PROGS  += undertow      # 10/11/26: the web browser, a program of several files in user/undertow/
USER_PROGS  += rx            # 10/9/26 (the development loop): one file from the network, for tools/devloop.py
# surfsh, the SurfOS shell (10/6/26): a program of several files in user/surfsh/
SURFSH_SRC   = $(wildcard user/surfsh/*.c)
SURFSH_OBJ   = $(SURFSH_SRC:user/surfsh/%.c=$(BUILD)/user/surfsh/%.o)
# telnetd, the telnet server (10/6/26): user/telnetd/, its protocol module host-tested too
TELNETD_SRC  = $(wildcard user/telnetd/*.c)
TELNETD_OBJ  = $(TELNETD_SRC:user/telnetd/%.c=$(BUILD)/user/telnetd/%.o)
# Undertow, the web browser (10/10/26): user/undertow/, each module host-tested too (tests/host/test_undertow_*.c)
UNDERTOW_SRC = $(wildcard user/undertow/*.c)
UNDERTOW_OBJ = $(UNDERTOW_SRC:user/undertow/%.c=$(BUILD)/user/undertow/%.o)


CC         = gcc
LD         = ld
AR         = ar
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

# boot.o must come first so the Multiboot headers land inside the first 8 KB of the image.
SRC_S      = boot/boot.S
SRC_C      = $(wildcard kernel/*.c) $(wildcard mm/*.c) $(wildcard lib/blibc/*.c) \
             $(wildcard shell/*.c) $(wildcard fs/*.c) $(wildcard net/*.c) $(wildcard driver/*.c) $(wildcard driver/net/*.c) $(wildcard driver/dma/*.c) \
             $(wildcard driver/floppy/*.c) $(wildcard driver/net/3c905b/*.c) $(wildcard driver/usb/*.c) \
             $(wildcard driver/wifi/*.c) $(wildcard net/wlan/*.c) $(wildcard lib/crypto/*.c)
# kernel/setjmp.asm is a dead pre-Multiboot entry stub and is not built.
SRC_ASM    = kernel/traps.asm driver/dma/dma-asm.asm

OBJS       = $(SRC_S:%.S=$(BUILD)/%.o) $(SRC_C:%.c=$(BUILD)/%.o) $(SRC_ASM:%.asm=$(BUILD)/%.o)
DEPS       = $(OBJS:.o=.d) $(USER_LIB_OBJ:.o=.d) $(USER_PROGS:%=$(BUILD)/user/obj/%.d) $(SURFSH_OBJ:.o=.d) $(TELNETD_OBJ:.o=.d) \
             $(UNDERTOW_OBJ:.o=.d)

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

# the smoke test also gets a blank 64 MB module, the scratch ramdisk rd1 that fdisk and mkfs may write
TESTMEM ?= 256
test: $(KERNEL) images
	$(PYTHON) tools/qemu-run.py --qemu $(QEMU) --kernel $(KERNEL) --mem $(TESTMEM) --disk $(DISKIMG) --initrd $(INITRD) --scratch 64 --test

test-all:
	$(MAKE) O=0 test
	$(MAKE) O=2 test

# 10/6/26: the guest's NIC on a raw link to tools/rawnet.py, which makes TCP segments by hand
test-tcp: $(KERNEL) images
	$(PYTHON) tools/qemu-tcp.py --qemu $(QEMU) --kernel $(KERNEL) --initrd $(INITRD) --test

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
images: $(DISKIMG) $(INITRD) $(FOREIGNIMG)

# a FAT32 stick as another computer leaves it (mkfs.fat + mtools), for the USB storage tests
$(FOREIGNIMG): tools/mkimage.py | $(IMAGES)
	@echo "IMG  $@"
	$(Q)$(PYTHON) tools/mkimage.py foreign $@

$(IMAGES):
	$(Q)mkdir -p $@

# 10/9/26 (the development loop): the initrd's etc/build-id names the build, the source revision (-dirty with
# uncommitted changes: SURFOS_REV and SURFOS_DIRTY, below) and the UTC time it was made; tools/devloop.py checks it
# after a reboot. Both initrds are made again at every build (FORCE), so the time is always that build's.
BUILD_REV  = $(SURFOS_REV)$(if $(filter yes,$(SURFOS_DIRTY)),-dirty)
FORCE:

# 10/10/26 (Wi-Fi): Intel's firmware for the laptop's card, with its licence, under /lib/firmware/ of the USB
# image's initrd when tools/fetch-firmware.py has put it in firmware/ (never committed); without it the initrd is
# as before. The QEMU initrd (build/images/) has no Intel card to load it into and stays as it was.
define initrd_firmware
if ls firmware/*.ucode > /dev/null 2>&1; then mkdir -p $(1)/lib/firmware && cp firmware/*.ucode firmware/LICENCE.* $(1)/lib/firmware/; fi
endef

# 10/11/26 (Undertow): the licence of Mbed TLS, whose code /bin/undertow carries, in /licenses/ of both initrds
# (Apache-2.0 asks for it with every copy of the program). The fetch put it in third_party/ before undertow was built.
define initrd_licenses
mkdir -p $(1)/licenses && cp third_party/mbedtls/LICENSE $(1)/licenses/mbedtls.txt
endef

$(INITRD): $(ROOTFS_FILES) $(USER_BINS) tools/mkimage.py FORCE | $(IMAGES)
	@echo "IMG  $@"
	$(Q)rm -rf $(IMAGES)/initrd && mkdir -p $(IMAGES)/initrd/bin && cp -r rootfs/. $(IMAGES)/initrd/ && cp $(USER_BINS) $(IMAGES)/initrd/bin/
	$(Q)$(call initrd_licenses,$(IMAGES)/initrd)
	$(Q)$(PYTHON) tools/mkimage.py initrd $@ $(IMAGES)/initrd --build-id $(BUILD_REV)

# ---- user programs --------------------------------------------------------------------
user: $(USER_BINS)

$(BUILD)/user/lib/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "UCC  $<"
	$(Q)$(CC) $(USER_LIB_CFLAGS) -c $< -o $@

$(USER_LIB): $(USER_LIB_OBJ)
	@echo "AR   $@"
	$(Q)rm -f $@ && $(AR) rcs $@ $(USER_LIB_OBJ)

$(USER_CRT0): user/libsurf/crt0.S
	@mkdir -p $(dir $@)
	@echo "UAS  $<"
	$(Q)$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/user/obj/%.o: user/bin/%.c
	@mkdir -p $(dir $@)
	@echo "UCC  $<"
	$(Q)$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/user/bin/%: $(BUILD)/user/obj/%.o $(USER_CRT0) $(USER_LIB) user/user.ld
	@mkdir -p $(dir $@)
	@echo "ULD  $@"
	$(Q)$(LD) -m elf_i386 -nostdlib --gc-sections -T user/user.ld -o $@ $(USER_CRT0) $< $(USER_LIB)

# keep the program objects (make would delete them as intermediates and rebuild them every time)
.SECONDARY: $(USER_PROGS:%=$(BUILD)/user/obj/%.o)

# surfsh is linked from its own objects: an explicit rule, which make prefers to the one-file pattern above
$(BUILD)/user/surfsh/%.o: user/surfsh/%.c
	@mkdir -p $(dir $@)
	@echo "UCC  $<"
	$(Q)$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/user/bin/surfsh: $(SURFSH_OBJ) $(USER_CRT0) $(USER_LIB) user/user.ld
	@mkdir -p $(dir $@)
	@echo "ULD  $@"
	$(Q)$(LD) -m elf_i386 -nostdlib --gc-sections -T user/user.ld -o $@ $(USER_CRT0) $(SURFSH_OBJ) $(USER_LIB)

# telnetd likewise
$(BUILD)/user/telnetd/%.o: user/telnetd/%.c
	@mkdir -p $(dir $@)
	@echo "UCC  $<"
	$(Q)$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/user/bin/telnetd: $(TELNETD_OBJ) $(USER_CRT0) $(USER_LIB) user/user.ld
	@mkdir -p $(dir $@)
	@echo "ULD  $@"
	$(Q)$(LD) -m elf_i386 -nostdlib --gc-sections -T user/user.ld -o $@ $(USER_CRT0) $(TELNETD_OBJ) $(USER_LIB)

# Undertow likewise (10/11/26)
$(BUILD)/user/undertow/%.o: user/undertow/%.c
	@mkdir -p $(dir $@)
	@echo "UCC  $<"
	$(Q)$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/user/bin/undertow: $(UNDERTOW_OBJ) $(USER_CRT0) $(USER_LIB) user/user.ld
	@mkdir -p $(dir $@)
	@echo "ULD  $@"
	$(Q)$(LD) -m elf_i386 -nostdlib --gc-sections -T user/user.ld -o $@ $(USER_CRT0) $(UNDERTOW_OBJ) $(USER_LIB)

$(DISKIMG): $(DISK_FILES) tools/mkimage.py | $(IMAGES)
	@echo "IMG  $@"
	$(Q)$(PYTHON) tools/mkimage.py disk $@ 16 $(DISK_FILES)

# ---- USB boot image for UEFI PCs (docs/USB-BOOT.md) ---------------------------------------
# The image carries this level's kernel and an initrd of this level's user programs; the shared
# build/images/initrd.tar is whichever level was built last, so it is not used here.
USB_DIR      = build/usb
USB_IMAGE    = $(USB_DIR)/surfos-ideapad-15iil05.img
USB_INITRD   = $(BUILD)/initrd.tar
USB_TESTDIR  = build/usb-test/O$(O)
GRUB_FILE    = grub-file
QEMU64       = qemu-system-x86_64
# devenv/build.sh passes these in: the Docker copy of the tree has no .git
SURFOS_REV   ?= $(shell git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
SURFOS_DIRTY ?= $(shell git rev-parse --git-dir >/dev/null 2>&1 && { test -z "$$(git status --porcelain 2>/dev/null)" && echo no || echo yes; } || echo unknown)
OVMF_ARGS    = $(if $(OVMF_CODE),--ovmf-code $(OVMF_CODE)) $(if $(OVMF_VARS),--ovmf-vars $(OVMF_VARS))

$(USB_INITRD): $(ROOTFS_FILES) $(USER_BINS) tools/mkimage.py FORCE
	@echo "IMG  $@"
	$(Q)rm -rf $(BUILD)/initrd && mkdir -p $(BUILD)/initrd/bin && cp -r rootfs/. $(BUILD)/initrd/ && cp $(USER_BINS) $(BUILD)/initrd/bin/
	$(Q)$(call initrd_firmware,$(BUILD)/initrd)
	$(Q)$(call initrd_licenses,$(BUILD)/initrd)
	$(Q)$(PYTHON) tools/mkimage.py initrd $@ $(BUILD)/initrd --build-id $(BUILD_REV)

usb-image: $(KERNEL) $(USB_INITRD) boot/grub-usb.cfg boot/grub-usb-bootstrap.cfg tools/mkusb.py
	@mkdir -p $(USB_DIR)
	$(Q)$(GRUB_FILE) --is-x86-multiboot $(KERNEL) && $(GRUB_FILE) --is-x86-multiboot2 $(KERNEL) \
		|| { echo "$(KERNEL): grub-file does not find both Multiboot headers"; exit 1; }
	$(PYTHON) tools/mkusb.py --kernel $(KERNEL) --initrd $(USB_INITRD) --output $(USB_IMAGE) \
		--optimization O$(O) --source-rev $(SURFOS_REV) --source-dirty $(SURFOS_DIRTY)

# boots the image file itself (no -kernel): OVMF with fresh variables, xHCI + usb-storage
test-usb: usb-image
	$(PYTHON) tools/qemu-usb.py --qemu $(QEMU64) --image $(USB_IMAGE) --output-dir $(USB_TESTDIR) $(OVMF_ARGS) --test

test-mkusb:
	$(PYTHON) -m unittest discover -s tests -p test_mkusb.py -v

# 10/9/26 (the development loop): tools/devloop.py's own logic, the data stick image (tools/mkimage.py) and the build
# id; nothing connects anywhere (tools/qemu-usb.py's devloop scenario drives devloop.py against QEMU)
test-devloop:
	$(PYTHON) -m unittest discover -s tests -p test_devloop.py -v
.PHONY: test-devloop

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

# kernel code with no hardware access, tested the same way: the Multiboot 2 parser, the free-frame
# arithmetic, the framebuffer renderer and the USB descriptor/NCM parser. The tests see SurfOS
# headers, as the kernel does.
HOSTKERN_FLAGS = -ffreestanding -fno-builtin -nostdinc -Iinclude -include tests/host/surf_names.h -MMD -MP
$(BUILD)/host/kernel/%.o: %.c tests/host/surf_names.h
	@mkdir -p $(dir $@)
	@echo "HOSTCC $<"
	$(Q)$(HOSTCC) $(HOSTFLAGS) $(HOSTKERN_FLAGS) -c $< -o $@

$(BUILD)/host/test_bootinfo: tests/host/test_bootinfo.c $(BUILD)/host/kernel/kernel/multiboot2.o $(BUILD)/host/kernel/mm/memrange.o $(HOSTLIB_OBJ)
	@echo "HOSTLD $@"
	$(Q)$(HOSTCC) $(HOSTFLAGS) $(HOSTKERN_FLAGS) $^ -o $@

$(BUILD)/host/test_framebuffer: tests/host/test_framebuffer.c $(BUILD)/host/kernel/driver/fbcore.o $(BUILD)/host/kernel/driver/font8x16.o $(HOSTLIB_OBJ)
	@echo "HOSTLD $@"
	$(Q)$(HOSTCC) $(HOSTFLAGS) $(HOSTKERN_FLAGS) $^ -o $@

$(BUILD)/host/test_usb: tests/host/test_usb.c $(BUILD)/host/kernel/driver/usb/usbdesc.o $(HOSTLIB_OBJ)
	@echo "HOSTLD $@"
	$(Q)$(HOSTCC) $(HOSTFLAGS) $(HOSTKERN_FLAGS) $^ -o $@

$(BUILD)/host/test_part: tests/host/test_part.c $(BUILD)/host/kernel/driver/parttab.o $(HOSTLIB_OBJ)
	@echo "HOSTLD $@"
	$(Q)$(HOSTCC) $(HOSTFLAGS) $(HOSTKERN_FLAGS) $^ -o $@

# 10/10/26: Intel's Wi-Fi firmware files (driver/wifi/iwl_fw.c), against the real ones in firmware/ when fetched
$(BUILD)/host/test_iwlfw: tests/host/test_iwlfw.c $(BUILD)/host/kernel/driver/wifi/iwl_fw.o $(HOSTLIB_OBJ)
	@echo "HOSTLD $@"
	$(Q)$(HOSTCC) $(HOSTFLAGS) $(HOSTKERN_FLAGS) $^ -o $@

# 10/7/26: lspci -v's and wifiinfo's reading of PCI configuration space
$(BUILD)/host/test_pciinfo: tests/host/test_pciinfo.c $(BUILD)/host/kernel/driver/pciinfo.o $(HOSTLIB_OBJ)
	@echo "HOSTLD $@"
	$(Q)$(HOSTCC) $(HOSTFLAGS) $(HOSTKERN_FLAGS) $^ -o $@

# the SurfOS prompt (10/6/26), the same file in the kernel's shell and in libsurf for surfsh
$(BUILD)/host/test_prompt: tests/host/test_prompt.c $(BUILD)/host/kernel/shell/prompt.o $(HOSTLIB_OBJ)
	@echo "HOSTLD $@"
	$(Q)$(HOSTCC) $(HOSTFLAGS) $(HOSTKERN_FLAGS) $^ -o $@

# the console's character set (10/6/26): code page 437, the UTF-8 decoder, the font in the same order
$(BUILD)/host/test_cp437: tests/host/test_cp437.c $(BUILD)/host/kernel/driver/cp437.o $(BUILD)/host/kernel/driver/font8x16.o \
                          $(HOSTLIB_OBJ)
	@echo "HOSTLD $@"
	$(Q)$(HOSTCC) $(HOSTFLAGS) $(HOSTKERN_FLAGS) $^ -o $@

# the console and its pager (10/6/26): the real console.c, tty.c and cp437.c on the host, with pager_bridge.c for
# the rest of the kernel and pager_shim/ for port I/O and the interrupt flag; the test runs every task as a thread
PAGER_FLAGS = -Itests/host/pager_shim $(HOSTKERN_FLAGS) -fgnu89-inline
PAGER_OBJ   = $(BUILD)/host/pager/kernel/console.o $(BUILD)/host/pager/kernel/tty.o $(BUILD)/host/pager/driver/cp437.o \
              $(BUILD)/host/pager/tests/host/pager_bridge.o
$(BUILD)/host/pager/%.o: %.c tests/host/surf_names.h
	@mkdir -p $(dir $@)
	@echo "HOSTCC $< (pager test)"
	$(Q)$(HOSTCC) $(HOSTFLAGS) $(PAGER_FLAGS) -c $< -o $@

$(BUILD)/host/test_pager: tests/host/test_pager.c $(PAGER_OBJ) $(HOSTLIB_OBJ)
	@echo "HOSTLD $@"
	$(Q)$(HOSTCC) $(HOSTFLAGS) -pthread $^ -o $@

# 10/16/26 (audit B5-B11): the block cache and FAT together, fs/bcache.c, fs/fat.c and fs/fat_mkfs.c as they are, over
# RAM disks that refuse writes on request, with a check of the image after every write the disk stores
# (tests/host/test_fatfs.c); tests/host/pager_shim stands in for the interrupt flag
FATFS_FLAGS = -Itests/host/pager_shim $(HOSTKERN_FLAGS) -fgnu89-inline
FATFS_OBJ   = $(BUILD)/host/fatfs/fs/bcache.o $(BUILD)/host/fatfs/fs/fat.o $(BUILD)/host/fatfs/fs/fat_mkfs.o
$(BUILD)/host/fatfs/%.o: %.c tests/host/surf_names.h
	@mkdir -p $(dir $@)
	@echo "HOSTCC $< (fatfs test)"
	$(Q)$(HOSTCC) $(HOSTFLAGS) $(FATFS_FLAGS) -c $< -o $@

$(BUILD)/host/test_fatfs: tests/host/test_fatfs.c $(FATFS_OBJ) $(HOSTLIB_OBJ)
	@echo "HOSTLD $@"
	$(Q)$(HOSTCC) $(HOSTFLAGS) $(FATFS_FLAGS) $^ -o $@

# 10/16/26 (round fs-fix-2, audit B12-B19): the randomized harness of the review of B5-B11, kept as a test: random
# FAT operations over a disk that refuses writes, and reads, at random, with test_fatfs.c's oracle and checks of the
# image (tests/host/test_fatfuzz.c: a fixed set of seeds, about 10 s in the devenv)
$(BUILD)/host/test_fatfuzz: tests/host/test_fatfuzz.c tests/host/test_fatfs.c $(FATFS_OBJ) $(HOSTLIB_OBJ)
	@echo "HOSTLD $@"
	$(Q)$(HOSTCC) $(HOSTFLAGS) $(FATFS_FLAGS) tests/host/test_fatfuzz.c $(FATFS_OBJ) $(HOSTLIB_OBJ) -o $@

# and its threaded one: the cache and FAT run by host threads at once with the kernel's waits made real
# (tests/host/test_fatconc.c; its host side fatconc_host.c is built with the host's headers, fatconc_shim/ stands in
# for the interrupt lock): four settings, 3 s each (10/16/26, round fs-fix-3: the fourth is the review of B12-B19's,
# six writers on a small volume, which found trim_chain()'s window, B25); 10/17/26 (round fs-fix-7, audit B46): and
# `umount`, an umount interrupted while another thread's write is in flight (about 1 s)
FATCONC_FLAGS = -Itests/host/fatconc_shim $(HOSTKERN_FLAGS) -fgnu89-inline
# 10/16/26 (round fs-fix-3): in the cache and FAT, curTask stands for the calling thread's own task (fatconc_task(),
# tests/host/test_fatconc.c): a block one thread's operation sets aside is the other threads' syncs' debt (audit B26)
FATCONC_FSFLAGS = $(FATCONC_FLAGS) '-DcurTask=(*fatconc_task())'
FATCONC_OBJ   = $(BUILD)/host/fatconc/fs/bcache.o $(BUILD)/host/fatconc/fs/fat.o $(BUILD)/host/fatconc/fs/fat_mkfs.o
$(BUILD)/host/fatconc/fs/%.o: fs/%.c tests/host/surf_names.h
	@mkdir -p $(dir $@)
	@echo "HOSTCC $< (fatconc test)"
	$(Q)$(HOSTCC) $(HOSTFLAGS) $(FATCONC_FSFLAGS) -c $< -o $@
$(BUILD)/host/fatconc/fatconc_host.o: tests/host/fatconc_host.c
	@mkdir -p $(dir $@)
	@echo "HOSTCC $< (fatconc test)"
	$(Q)$(HOSTCC) $(HOSTFLAGS) -c $< -o $@
$(BUILD)/host/test_fatconc: tests/host/test_fatconc.c $(FATCONC_OBJ) $(BUILD)/host/fatconc/fatconc_host.o $(HOSTLIB_OBJ)
	@echo "HOSTLD $@"
	$(Q)$(HOSTCC) $(HOSTFLAGS) $(FATCONC_FLAGS) -pthread $^ -o $@

# mkfs writes image files (a temporary directory, sparse, removed afterwards); fsck.fat and mtools check them.
# fs/vfs.h reaches console.h's 2004 inline declarations, so these two use the kernel's inline rules.
$(BUILD)/host/test_mkfs $(BUILD)/host/kernel/fs/fat_mkfs.o: HOSTKERN_FLAGS += -fgnu89-inline
$(BUILD)/host/test_mkfs: tests/host/test_mkfs.c $(BUILD)/host/kernel/fs/fat_mkfs.o $(HOSTLIB_OBJ)
	@echo "HOSTLD $@"
	$(Q)$(HOSTCC) $(HOSTFLAGS) $(HOSTKERN_FLAGS) $^ -o $@

# the user C library (10/5/26): its portable parts against the host libc, every symbol renamed
HOSTUSER_SRC = user/libsurf/printf.c user/libsurf/string.c user/libsurf/stdlib.c user/libsurf/ctype.c \
               user/libsurf/time.c user/libsurf/malloc.c
HOSTUSER_OBJ = $(HOSTUSER_SRC:user/libsurf/%.c=$(BUILD)/host/libsurf/%.o)
$(BUILD)/host/libsurf/%.o: user/libsurf/%.c tests/host/libsurf_names.h
	@mkdir -p $(dir $@)
	@echo "HOSTCC $<"
	$(Q)$(HOSTCC) $(HOSTFLAGS) -ffreestanding -fno-builtin -nostdinc -Iuser/include -include tests/host/libsurf_names.h -MMD -MP -c $< -o $@

$(BUILD)/host/test_libc: tests/host/test_libc.c $(HOSTUSER_OBJ)
	@echo "HOSTLD $@"
	$(Q)$(HOSTCC) $(HOSTFLAGS) $^ -o $@

# telnetd's telnet protocol (10/6/26): user/telnetd/telnet.c makes no calls, so it builds as it is
$(BUILD)/host/test_telnet: tests/host/test_telnet.c user/telnetd/telnet.c user/telnetd/telnet.h
	@mkdir -p $(dir $@)
	@echo "HOSTLD $@"
	$(Q)$(HOSTCC) $(HOSTFLAGS) -Iuser/telnetd tests/host/test_telnet.c user/telnetd/telnet.c -o $@

# 10/6/26: telnetd's server loop, user/telnetd/server.c as it is, against a simulated kernel (tests/host/telnetd/surf.h)
$(BUILD)/host/test_telnetd: tests/host/test_telnetd.c tests/host/telnetd/surf.h user/telnetd/server.c user/telnetd/telnetd.h \
                            user/telnetd/telnet.c user/telnetd/telnet.h
	@mkdir -p $(dir $@)
	@echo "HOSTLD $@"
	$(Q)$(HOSTCC) $(HOSTFLAGS) -Itests/host/telnetd -Iuser/telnetd tests/host/test_telnetd.c user/telnetd/telnet.c -o $@

unittest: $(BUILD)/host/test_blibc $(BUILD)/host/test_bootinfo $(BUILD)/host/test_framebuffer $(BUILD)/host/test_usb \
          $(BUILD)/host/test_part $(BUILD)/host/test_libc $(BUILD)/host/test_mkfs $(BUILD)/host/test_prompt \
          $(BUILD)/host/test_cp437 $(BUILD)/host/test_pager $(BUILD)/host/test_telnet $(BUILD)/host/test_telnetd \
          $(BUILD)/host/test_pciinfo $(BUILD)/host/test_iwlfw $(BUILD)/host/test_fatfs $(BUILD)/host/test_fatfuzz \
          $(BUILD)/host/test_fatconc
	$(BUILD)/host/test_blibc
	$(BUILD)/host/test_bootinfo
	$(BUILD)/host/test_framebuffer
	$(BUILD)/host/test_usb
	$(BUILD)/host/test_part
	$(PYTHON) tests/test_part_tools.py $(BUILD)/host/test_part
	$(BUILD)/host/test_libc
	$(BUILD)/host/test_mkfs
	$(BUILD)/host/test_fatfs
	$(BUILD)/host/test_fatfuzz
	$(BUILD)/host/test_fatconc 11 3 3 1 30 0
	$(BUILD)/host/test_fatconc 12 3 4 1 0 1
	$(BUILD)/host/test_fatconc 21 3 3 1 100 1 1 1 1 16384 0
	$(BUILD)/host/test_fatconc 33 3 6 1 0 1 0 0 1 8192 0
	$(BUILD)/host/test_fatconc umount
	$(BUILD)/host/test_prompt
	$(BUILD)/host/test_cp437
	$(BUILD)/host/test_pager
	$(BUILD)/host/test_telnet
	$(BUILD)/host/test_telnetd
	$(BUILD)/host/test_pciinfo
	$(BUILD)/host/test_iwlfw
	$(PYTHON) -m unittest discover -s tests -p test_devloop.py
	$(PYTHON) -m unittest discover -s tests -p test_source_sync.py
	$(PYTHON) -m unittest discover -s tests -p test_devenv.py

# ---- Wi-Fi, track C (10/9/26): lib/crypto/ and the WPA2 supplicant, on the host ---------------
# The kernel builds lib/crypto/ and net/wlan/ through SRC_C; here they are built against the SurfOS
# headers like the other kernel objects. random.c's and wpa.c's headers reach console.h's 2004 inline
# declarations, hence -fgnu89-inline. `make unittest` runs these first.
CRYPTO_HOST_OBJ = $(patsubst %.c,$(BUILD)/host/kernel/%.o,$(wildcard lib/crypto/*.c))
$(CRYPTO_HOST_OBJ) $(BUILD)/host/test_crypto: HOSTKERN_FLAGS += -fgnu89-inline
# Only the host random object can replace individual hardware attempts.
$(BUILD)/host/kernel/lib/crypto/random.o: HOSTKERN_FLAGS += -DCRYPTO_RANDOM_TEST
$(BUILD)/host/test_crypto: HOSTKERN_FLAGS += -ffunction-sections -Wl,--gc-sections
$(BUILD)/host/test_crypto: tests/host/test_crypto.c $(CRYPTO_HOST_OBJ) $(HOSTLIB_OBJ)
	@echo "HOSTLD $@"
	$(Q)$(HOSTCC) $(HOSTFLAGS) $(HOSTKERN_FLAGS) $^ -o $@

# the supplicant (net/wlan/wpa.c) against a simulated access point and a captured handshake; the test
# has its own crypto_random() (scripted nonces), so random.o stays out
WPA_HOST_OBJ = $(BUILD)/host/kernel/net/wlan/wpa.o
$(WPA_HOST_OBJ) $(BUILD)/host/test_wpa: HOSTKERN_FLAGS += -fgnu89-inline
$(BUILD)/host/test_wpa: tests/host/test_wpa.c $(WPA_HOST_OBJ) $(filter-out %/random.o,$(CRYPTO_HOST_OBJ)) $(HOSTLIB_OBJ)
	@echo "HOSTLD $@"
	$(Q)$(HOSTCC) $(HOSTFLAGS) $(HOSTKERN_FLAGS) $^ -o $@

test-wifi-c: $(BUILD)/host/test_crypto $(BUILD)/host/test_wpa
	$(BUILD)/host/test_crypto
	$(BUILD)/host/test_wpa

unittest: test-wifi-c
.PHONY: test-wifi-c
-include $(wildcard $(BUILD)/host/kernel/lib/crypto/*.d $(BUILD)/host/kernel/net/wlan/wpa.d)

# 10/9/26 (Wi-Fi, track S): the station layer's pure parts on the host: the 802.11 frames (net/wlan/ieee80211.c).
# net/wlan.h reaches console.h's 2004 inline declarations, as fs/vfs.h does for test_mkfs.
WLAN_HOSTTESTS = $(BUILD)/host/test_ieee80211
$(WLAN_HOSTTESTS) $(BUILD)/host/kernel/net/wlan/ieee80211.o: HOSTKERN_FLAGS += -fgnu89-inline
$(BUILD)/host/test_ieee80211: tests/host/test_ieee80211.c $(BUILD)/host/kernel/net/wlan/ieee80211.o $(HOSTLIB_OBJ)
	@echo "HOSTLD $@"
	$(Q)$(HOSTCC) $(HOSTFLAGS) $(HOSTKERN_FLAGS) $^ -o $@
unittest: unittest-wlan
unittest-wlan: $(WLAN_HOSTTESTS)
	$(BUILD)/host/test_ieee80211
-include $(wildcard $(BUILD)/host/kernel/net/wlan/*.d)
.PHONY: unittest-wlan

clean:
	rm -rf build

help:
	@sed -n '2,18p' Makefile

.PHONY: all run run-vga test test-all test-tcp unittest debug iso run-iso images user clean help usb-image test-usb test-mkusb
-include $(DEPS) $(wildcard $(BUILD)/host/*.d $(BUILD)/host/kernel/*/*.d $(BUILD)/host/libsurf/*.d $(BUILD)/host/pager/*/*.d \
                           $(BUILD)/host/pager/tests/host/*.d $(BUILD)/host/fatfs/fs/*.d $(BUILD)/host/fatconc/fs/*.d)

# Undertow (10/10/26): bounded DNS packet decoding, constructed packets on the host.
$(BUILD)/host/test_dns: tests/host/test_dns.c $(BUILD)/host/kernel/net/dns_wire.o $(HOSTLIB_OBJ)
	@echo "HOSTLD $@"
	$(Q)$(HOSTCC) $(HOSTFLAGS) $(HOSTKERN_FLAGS) -fgnu89-inline -ffunction-sections -Wl,--gc-sections tests/host/test_dns.c $(filter %.o,$^) -o $@

$(BUILD)/host/test_dns: net/dns.c kernel/websvc.c

test-dns: $(BUILD)/host/test_dns
	$(BUILD)/host/test_dns
unittest: test-dns
.PHONY: test-dns

# Strict entropy checks retain QEMU's default CPU for the unsupported baseline;
# tools/qemu-run.py selects RDRAND explicitly only in the positive child guest.
test-entropy: $(KERNEL) $(INITRD)
	$(PYTHON) tools/qemu-run.py --qemu $(QEMU) --kernel $(KERNEL) --initrd $(INITRD) --entropy-test
.PHONY: test-entropy

# Undertow's host tests (10/10/26). Its modules are built with the host's C library, -m32 as
# HOSTFLAGS has it; tests/host/undertow/surf.h stands in for SurfOS's own calls, which each test
# defines, and tests/host/undertow/alloc.c for malloc and its friends, so that a test can count
# what a module holds and make any allocation fail. Every test also builds the real program,
# so the modules compile against SurfOS's own headers (user/include) as well.
UT_HOSTFLAGS = $(HOSTFLAGS) -Itests/host/undertow -Iuser/undertow -MMD -MP
UT_ALLOC     = -Dmalloc=ut_test_malloc -Dcalloc=ut_test_calloc -Drealloc=ut_test_realloc -Dfree=ut_test_free \
               -Dstrdup=ut_test_strdup -Dstrndup=ut_test_strndup
$(BUILD)/host/undertow/%.o: user/undertow/%.c
	@mkdir -p $(dir $@)
	@echo "HOSTCC $<"
	$(Q)$(HOSTCC) $(UT_HOSTFLAGS) $(UT_ALLOC) -c $< -o $@

$(BUILD)/host/undertow/alloc.o: tests/host/undertow/alloc.c
	@mkdir -p $(dir $@)
	@echo "HOSTCC $<"
	$(Q)$(HOSTCC) $(UT_HOSTFLAGS) -c $< -o $@
-include $(wildcard $(BUILD)/host/undertow/*.d)

# url.c (task 3)
$(BUILD)/host/test_undertow_url: tests/host/test_undertow_url.c $(BUILD)/host/undertow/url.o
	@mkdir -p $(dir $@)
	@echo "HOSTLD $@"
	$(Q)$(HOSTCC) $(HOSTFLAGS) -Iuser/undertow tests/host/test_undertow_url.c $(BUILD)/host/undertow/url.o -o $@

test-undertow-url: $(BUILD)/host/test_undertow_url $(BUILD)/user/bin/undertow
	$(BUILD)/host/test_undertow_url
unittest: test-undertow-url
.PHONY: test-undertow-url

# util.c and config.c (the shared helpers, the settings and the clock)
$(BUILD)/host/test_undertow_util: tests/host/test_undertow_util.c $(BUILD)/host/undertow/util.o \
                                  $(BUILD)/host/undertow/config.o $(BUILD)/host/undertow/alloc.o
	@mkdir -p $(dir $@)
	@echo "HOSTLD $@"
	$(Q)$(HOSTCC) $(UT_HOSTFLAGS) $^ -o $@

test-undertow-util: $(BUILD)/host/test_undertow_util $(BUILD)/user/bin/undertow
	$(BUILD)/host/test_undertow_util
unittest: test-undertow-util
.PHONY: test-undertow-util

# -- http.c (track H) --
# http.c against scripted servers: the test provides the transport's calls itself, so neither
# transport.o nor TLS is linked; url.c resolves the redirects as it does in the program.
$(BUILD)/host/test_undertow_http: tests/host/test_undertow_http.c $(BUILD)/host/undertow/http.o \
                                  $(BUILD)/host/undertow/url.o $(BUILD)/host/undertow/util.o \
                                  $(BUILD)/host/undertow/alloc.o
	@mkdir -p $(dir $@)
	@echo "HOSTLD $@"
	$(Q)$(HOSTCC) $(UT_HOSTFLAGS) $^ -o $@

test-undertow-http: $(BUILD)/host/test_undertow_http $(BUILD)/user/bin/undertow
	$(BUILD)/host/test_undertow_http
unittest: test-undertow-http
.PHONY: test-undertow-http

# -- html.c (track R) --
# html.c with url.c and util.c: the fixtures in tests/fixtures/undertow/ (NAME.html, or NAME.body for other text,
# in; NAME.txt and NAME.doc what must come out) and pages the test makes. The test brings its own allocator, which counts bytes so that a
# document's bytes field is checked, so alloc.o is not linked.
$(BUILD)/host/test_undertow_html: tests/host/test_undertow_html.c $(BUILD)/host/undertow/html.o \
                                  $(BUILD)/host/undertow/url.o $(BUILD)/host/undertow/util.o
	@mkdir -p $(dir $@)
	@echo "HOSTLD $@"
	$(Q)$(HOSTCC) $(UT_HOSTFLAGS) $^ -o $@

test-undertow-html: $(BUILD)/host/test_undertow_html $(BUILD)/user/bin/undertow
	$(BUILD)/host/test_undertow_html tests/fixtures/undertow
unittest: test-undertow-html
.PHONY: test-undertow-html

# The same test with UndefinedBehaviorSanitizer at SurfOS's ABI (-m32, where a long is 32 bits), when the host
# compiler has it for that ABI (gcc with lib32ubsan): arithmetic that overflows only in 32 bits stops the test.
UT_UBSAN      := $(shell echo 'int main(void){return 0;}' | $(HOSTCC) $(HOST32) -fsanitize=undefined -x c - -o /dev/null 2>/dev/null && echo yes)
UT_UBSANFLAGS  = $(HOSTFLAGS) -fsanitize=undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer \
                 -Itests/host/undertow -Iuser/undertow
$(BUILD)/host/undertow-ubsan/%.o: user/undertow/%.c
	@mkdir -p $(dir $@)
	@echo "HOSTCC $< (UBSan)"
	$(Q)$(HOSTCC) $(UT_UBSANFLAGS) -MMD -MP $(UT_ALLOC) -c $< -o $@

$(BUILD)/host/test_undertow_html_ubsan: tests/host/test_undertow_html.c $(BUILD)/host/undertow-ubsan/html.o \
                                        $(BUILD)/host/undertow-ubsan/url.o $(BUILD)/host/undertow-ubsan/util.o
	@echo "HOSTLD $@"
	$(Q)$(HOSTCC) $(UT_UBSANFLAGS) $^ -o $@

test-undertow-html-ubsan: $(if $(UT_UBSAN),$(BUILD)/host/test_undertow_html_ubsan)
	$(if $(UT_UBSAN),$(BUILD)/host/test_undertow_html_ubsan tests/fixtures/undertow,@echo "test-undertow-html-ubsan: skipped: $(HOSTCC) has no UndefinedBehaviorSanitizer for $(or $(HOST32),this ABI)")
unittest: test-undertow-html-ubsan
.PHONY: test-undertow-html-ubsan
-include $(wildcard $(BUILD)/host/undertow-ubsan/*.d)

# -- session.c, view.c, main.c (track S) --
# session.c with the test's own fake ut_fetch() and ut_render() (never http.o or html.o)
$(BUILD)/host/test_undertow_session: tests/host/test_undertow_session.c $(BUILD)/host/undertow/session.o \
                                     $(BUILD)/host/undertow/url.o $(BUILD)/host/undertow/util.o \
                                     $(BUILD)/host/undertow/config.o $(BUILD)/host/undertow/alloc.o
	@mkdir -p $(dir $@)
	@echo "HOSTLD $@"
	$(Q)$(HOSTCC) $(UT_HOSTFLAGS) $^ -o $@

test-undertow-session: $(BUILD)/host/test_undertow_session $(BUILD)/user/bin/undertow
	$(BUILD)/host/test_undertow_session
unittest: test-undertow-session
.PHONY: test-undertow-session

# view.c and edit.c with the test's own terminal (views written into memory, keys from a script), and the
# console's real code page 437 (driver/cp437.c, which libsurf builds as user/libsurf/cp437.c); view.c finds
# its surfos/cp437.h in user/include after the host's own headers
$(BUILD)/host/undertow/view.o: UT_HOSTFLAGS += -idirafter user/include
$(BUILD)/host/test_undertow_view: tests/host/test_undertow_view.c $(BUILD)/host/undertow/view.o \
                                  $(BUILD)/host/undertow/edit.o $(BUILD)/host/undertow/util.o $(BUILD)/host/undertow/alloc.o \
                                  $(BUILD)/host/kernel/driver/cp437.o
	@mkdir -p $(dir $@)
	@echo "HOSTLD $@"
	$(Q)$(HOSTCC) $(UT_HOSTFLAGS) $^ -o $@

test-undertow-view: $(BUILD)/host/test_undertow_view $(BUILD)/user/bin/undertow
	$(BUILD)/host/test_undertow_view
unittest: test-undertow-view
.PHONY: test-undertow-view

# command.c: what a line typed at the prompt asks for (main.c carries it out)
$(BUILD)/host/test_undertow_command: tests/host/test_undertow_command.c $(BUILD)/host/undertow/command.o
	@mkdir -p $(dir $@)
	@echo "HOSTLD $@"
	$(Q)$(HOSTCC) $(UT_HOSTFLAGS) $^ -o $@

test-undertow-command: $(BUILD)/host/test_undertow_command $(BUILD)/user/bin/undertow
	$(BUILD)/host/test_undertow_command
unittest: test-undertow-command
.PHONY: test-undertow-command

# -- transport.c and Mbed TLS (track T) --
# Undertow's HTTPS (10/11/26): Mbed TLS 3.6.7, pinned by SHA-256 in third_party/undertow-lock.json and never
# committed (third_party/README.md). tools/fetch-webdeps.py downloads the release archive (or takes the cached,
# checked copy), extracts include/, library/ and LICENSE into third_party/mbedtls/ and writes the stamp below;
# devenv/build.sh runs it on the Mac before the tree is copied into Docker, and this rule runs it wherever the stamp
# is missing or older than the lock (a clean Linux checkout, CI), so one make builds everything. The sources are
# listed, not found with $(wildcard) (nothing is there before the fetch): the files tls_config.h compiles to code;
# one it newly needs shows up as an undefined symbol at the link.
MBEDTLS_DIR    = third_party/mbedtls
MBEDTLS_STAMP  = $(MBEDTLS_DIR)/UNDERTOW-LOCK
MBEDTLS_SRC    = aes.c asn1parse.c asn1write.c base64.c bignum.c bignum_core.c chacha20.c chachapoly.c cipher.c \
                 cipher_wrap.c constant_time.c ctr_drbg.c ecdh.c ecdsa.c ecp.c ecp_curves.c entropy.c gcm.c hkdf.c md.c \
                 mps_reader.c oid.c pem.c pk.c pk_ecc.c pk_wrap.c pkparse.c platform.c platform_util.c poly1305.c \
                 psa_crypto.c psa_crypto_aead.c psa_crypto_cipher.c psa_crypto_client.c \
                 psa_crypto_driver_wrappers_no_static.c psa_crypto_ecp.c psa_crypto_hash.c psa_crypto_mac.c \
                 psa_crypto_random.c psa_crypto_rsa.c psa_crypto_slot_management.c psa_util.c rsa.c rsa_alt_helpers.c \
                 sha1.c sha256.c sha512.c ssl_ciphersuites.c ssl_client.c ssl_msg.c ssl_tls.c ssl_tls12_client.c \
                 ssl_tls13_client.c ssl_tls13_generic.c ssl_tls13_keys.c x509.c x509_crt.c
MBEDTLS_CONF   = -DMBEDTLS_CONFIG_FILE='"tls_config.h"' -I$(MBEDTLS_DIR)/include -Iuser/undertow
MBEDTLS_OBJ    = $(MBEDTLS_SRC:%.c=$(BUILD)/user/mbedtls/%.o)
MBEDTLS_LIB    = $(BUILD)/user/mbedtls/libmbedtls.a
# The library is built with -O2 at every level (debugging Mbed TLS is not what -O0 is for, and slow crypto makes
# handshakes under QEMU slower still) and without debug information (it would add megabytes to the initrd); the
# target's other flags stay. -fbuiltin undoes -ffreestanding's -fno-builtin here: Mbed TLS reads unaligned words
# through memcpy() and relies on the compiler to make that one instruction (library/alignment.h).
MBEDTLS_CFLAGS = $(filter-out -O% -g,$(USER_CFLAGS)) -O2 -g0 -fbuiltin -Werror=implicit-function-declaration $(MBEDTLS_CONF)
# the cross-compiler's own 32-bit helpers (64-bit division), never the host's C library
UT_LIBGCC     := $(shell $(CC) -m32 -print-libgcc-file-name 2>/dev/null)

$(MBEDTLS_STAMP): third_party/undertow-lock.json
	$(PYTHON) tools/fetch-webdeps.py

# A source exists once the stamp does; this rule only makes make fetch first when it does not. The rules name their
# files (static patterns), so that make never chains its built-in rules through them.
MBEDTLS_SOURCES = $(addprefix $(MBEDTLS_DIR)/library/,$(MBEDTLS_SRC) ssl_tls12_server.c ssl_tls13_server.c)
$(MBEDTLS_SOURCES): | $(MBEDTLS_STAMP)
	@test -f $@ || { echo "$@ is not in the extracted Mbed TLS: run tools/fetch-webdeps.py --check"; exit 1; }

$(filter-out $(BUILD)/user/mbedtls/ssl_tls13_keys.o,$(MBEDTLS_OBJ)): $(BUILD)/user/mbedtls/%.o: $(MBEDTLS_DIR)/library/%.c $(MBEDTLS_STAMP) user/undertow/tls_config.h \
                                           user/undertow/tls_platform.h
	@mkdir -p $(dir $@)
	@echo "UCC  $<"
	$(Q)$(CC) $(MBEDTLS_CFLAGS) -c $< -o $@

$(MBEDTLS_LIB): $(MBEDTLS_OBJ)
	@echo "AR   $@"
	$(Q)rm -f $@ && $(AR) rcs $@ $(MBEDTLS_OBJ)

# transport.c and tls_platform.c see Mbed TLS's headers; the program links the library and libgcc after its
# own objects (the link rule above takes them from UNDERTOW_OBJ) and before libsurf, which both use
$(BUILD)/user/undertow/transport.o $(BUILD)/user/undertow/tls_platform.o: USER_CFLAGS += $(MBEDTLS_CONF)
$(BUILD)/user/undertow/transport.o $(BUILD)/user/undertow/tls_platform.o: $(MBEDTLS_STAMP)
UNDERTOW_OBJ  += $(MBEDTLS_LIB) $(UT_LIBGCC)
$(BUILD)/user/bin/undertow: $(MBEDTLS_LIB)

# Every initrd checks the program first: a 32-bit i386 ELF with no undefined symbol (a weak reference would be 0 at
# run time). Until http.c calls the transport, --gc-sections leaves TLS out of undertow, so the same objects are also
# linked with the transport's entry points as roots (-u): undertow-tls.elf, never installed, is undertow with all of
# TLS in it, and gets the same checks. The file left says how much of each is Mbed TLS: the sizes of the symbols the
# library defines, out of all of them.
UNDERTOW_LINKCHECK = $(BUILD)/user/undertow/link-check.txt
UNDERTOW_TLS_ELF   = $(BUILD)/user/undertow/undertow-tls.elf
UT_TRANSPORT_ROOTS = $(addprefix -u ,ut_transport_open ut_transport_read ut_transport_write ut_transport_close \
                       ut_transport_error ut_transport_verified ut_transport_tls ut_transport_cleanup ut_transport_status)
$(UNDERTOW_TLS_ELF): $(BUILD)/user/bin/undertow
	@echo "ULD  $@ (link check)"
	$(Q)$(LD) -m elf_i386 -nostdlib --gc-sections -T user/user.ld $(UT_TRANSPORT_ROOTS) -o $@ $(USER_CRT0) \
		$(UNDERTOW_OBJ) $(USER_LIB)

define ut_link_check
u="$$($(NM) -u $(1))"; if [ -n "$$u" ]; then echo "$$u"; echo "$(1): undefined symbols"; exit 1; fi; \
readelf -h $(1) | grep -q 'Class:.*ELF32' && readelf -h $(1) | grep -q 'Machine:.*Intel 80386' \
	|| { echo "$(1): not a 32-bit i386 ELF"; exit 1; }; \
$(NM) -S -t d $(1) | awk 'FNR == NR { lib[$$1] = 1; next } NF == 4 { t += $$2; if($$4 in lib) m += $$2 } \
	END { printf "%s: %d bytes in symbols, %d of them Mbed TLS\n", "$(1)", t, m }' $@.lib - >> $@
endef

$(UNDERTOW_LINKCHECK): $(BUILD)/user/bin/undertow $(UNDERTOW_TLS_ELF) $(MBEDTLS_LIB)
	@$(NM) $(MBEDTLS_LIB) | awk 'NF == 3 { print $$3 }' > $@.lib
	@rm -f $@
	@$(call ut_link_check,$(BUILD)/user/bin/undertow)
	@$(call ut_link_check,$(UNDERTOW_TLS_ELF))
	@rm -f $@.lib
	@cat $@
$(INITRD) $(USB_INITRD): $(UNDERTOW_LINKCHECK)
-include $(MBEDTLS_OBJ:.o=.d)

# The host tests: the same configuration plus the server side for the test's peer (tests/host/undertow/
# tls_host_config.h), built apart from the target's and never linked into it
MBEDTLS_HOST_OBJ   = $(MBEDTLS_SRC:%.c=$(BUILD)/host/mbedtls/%.o) $(BUILD)/host/mbedtls/ssl_tls12_server.o \
                     $(BUILD)/host/mbedtls/ssl_tls13_server.o
MBEDTLS_HOST_LIB   = $(BUILD)/host/mbedtls/libmbedtls-host.a
MBEDTLS_HOST_CONF  = $(MBEDTLS_CONF) -DMBEDTLS_USER_CONFIG_FILE='"tls_host_config.h"' -Itests/host/undertow
$(filter-out $(BUILD)/host/mbedtls/ssl_tls13_keys.o,$(MBEDTLS_HOST_OBJ)): $(BUILD)/host/mbedtls/%.o: $(MBEDTLS_DIR)/library/%.c $(MBEDTLS_STAMP) user/undertow/tls_config.h \
                                                user/undertow/tls_platform.h tests/host/undertow/tls_host_config.h
	@mkdir -p $(dir $@)
	@echo "HOSTCC $<"
	$(Q)$(HOSTCC) $(HOSTFLAGS) -MMD -MP $(MBEDTLS_HOST_CONF) -c $< -o $@

$(MBEDTLS_HOST_LIB): $(MBEDTLS_HOST_OBJ)
	@echo "AR   $@"
	$(Q)rm -f $@ && $(AR) rcs $@ $(MBEDTLS_HOST_OBJ)
-include $(MBEDTLS_HOST_OBJ:.o=.d)

# transport.c and tls_platform.c on the host: their socket calls go to the test's simulated network (UT_HOST_NET,
# tests/host/undertow/surf.h)
$(BUILD)/host/undertow/transport.o $(BUILD)/host/undertow/tls_platform.o: UT_HOSTFLAGS += $(MBEDTLS_HOST_CONF) -DUT_HOST_NET
$(BUILD)/host/undertow/transport.o $(BUILD)/host/undertow/tls_platform.o: $(MBEDTLS_STAMP) tests/host/undertow/tls_host_config.h

UT_TRANSPORT_OBJ = $(BUILD)/host/undertow/transport.o $(BUILD)/host/undertow/tls_platform.o \
                   $(BUILD)/host/undertow/util.o $(BUILD)/host/undertow/config.o $(BUILD)/host/undertow/url.o \
                   $(BUILD)/host/undertow/alloc.o
$(BUILD)/host/test_undertow_transport: tests/host/test_undertow_transport.c $(UT_TRANSPORT_OBJ) $(MBEDTLS_HOST_LIB)
	@mkdir -p $(dir $@)
	@echo "HOSTLD $@"
	$(Q)$(HOSTCC) $(UT_HOSTFLAGS) $(MBEDTLS_HOST_CONF) -I$(MBEDTLS_DIR)/library $(filter %.c %.o %.a,$^) -o $@

# the certificates are the test's own (tests/host/undertow/certs/, made by make-certs.sh there)
test-undertow-transport: $(BUILD)/host/test_undertow_transport $(BUILD)/user/bin/undertow $(UNDERTOW_LINKCHECK)
	$(BUILD)/host/test_undertow_transport tests/host/undertow/certs
unittest: test-undertow-transport
.PHONY: test-undertow-transport

# tools/fetch-webdeps.py against local archives made by the test (nothing is downloaded), then the real thing
# checked: every extracted Mbed TLS file as it came from the archive, and rootfs/etc/ssl/cert.pem as pinned
test-webdeps: $(MBEDTLS_STAMP)
	$(PYTHON) -m unittest discover -s tests -p test_webdeps.py
	$(PYTHON) tools/fetch-webdeps.py --check
unittest: test-webdeps
.PHONY: test-webdeps

# -- the browser in QEMU (track F) --
# 10/11/26: the web fixtures (tools/web-fixtures.py: HTTP and HTTPS servers on 127.0.0.1 with a test CA made for each
# run, which needs an OpenSSL 3 openssl command) and the end-to-end harness's own logic (tools/qemu-web.py: the test
# initrd, a copy of the normal one that is never written, and its scenarios) on the host; then the browser in QEMU
# against those fixtures, with its transcripts in build/web-test/O<level>/. The images make and make usb-image build
# never hold the fixture CA: the harness makes its own initrd in a temporary directory.
WEB_TESTDIR = build/web-test/O$(O)
test-web-host:
	$(PYTHON) -m unittest discover -s tests -p test_web_fixtures.py
	$(PYTHON) -m unittest discover -s tests -p test_qemu_web.py
unittest: test-web-host

test-web: $(KERNEL) images
	$(PYTHON) tools/qemu-web.py --qemu $(QEMU) --kernel $(KERNEL) --initrd $(INITRD) --output-dir $(WEB_TESTDIR) --test
.PHONY: test-web test-web-host

# Standalone file-browser integration checks; uses temporary QEMU disk copies.
test-files: $(KERNEL) images
	$(PYTHON) tools/qemu-files.py --qemu $(QEMU) --kernel $(KERNEL) --disk $(DISKIMG) --initrd $(INITRD)
.PHONY: test-files
test: test-files

# Native GUI input and application behavior (10/10/26).
$(BUILD)/host/pointer.o: driver/pointer.c include/sys/pointer.h
	@mkdir -p $(dir $@)
	$(Q)$(HOSTCC) $(HOSTFLAGS) -Itests/host/gui_stubs $(HOSTKERN_FLAGS) -c $< -o $@
$(BUILD)/host/test_pointer: tests/host/test_pointer.c $(BUILD)/host/pointer.o $(HOSTLIB_OBJ)
	$(Q)$(HOSTCC) $(HOSTFLAGS) $(HOSTKERN_FLAGS) $^ -o $@
unittest: unittest-gui
unittest-gui: $(BUILD)/host/test_pointer
.PHONY: unittest-gui
$(BUILD)/host/test_gui: tests/host/test_gui.c $(BUILD)/host/kernel/shell/guicore.o $(BUILD)/host/kernel/driver/fbcore.o $(BUILD)/host/kernel/driver/font8x16.o $(HOSTLIB_OBJ)
	$(Q)$(HOSTCC) $(HOSTFLAGS) $(HOSTKERN_FLAGS) $^ -o $@
unittest-gui: $(BUILD)/host/test_gui
	$(BUILD)/host/test_pointer
	$(BUILD)/host/test_gui

$(BUILD)/host/test_ps2mouse: tests/host/test_ps2mouse.c $(BUILD)/host/kernel/driver/ps2mouse.o
	$(Q)$(HOSTCC) $(HOSTFLAGS) $(HOSTKERN_FLAGS) $^ -o $@
$(BUILD)/host/test_usbmouse: tests/host/test_usbmouse.c driver/usb/mouse.c $(BUILD)/host/kernel/driver/usb/mousedesc.o $(HOSTLIB_OBJ)
	$(Q)$(HOSTCC) $(HOSTFLAGS) $(HOSTKERN_FLAGS) tests/host/test_usbmouse.c $(BUILD)/host/kernel/driver/usb/mousedesc.o $(HOSTLIB_OBJ) -o $@
unittest-mouse: $(BUILD)/host/test_ps2mouse $(BUILD)/host/test_usbmouse
	$(BUILD)/host/test_ps2mouse
	$(BUILD)/host/test_usbmouse
unittest: unittest-mouse
.PHONY: unittest-mouse

# Read-only firmware AML export, including runtime mapping ownership.
$(BUILD)/host/test_acpidump: tests/host/test_acpidump.c $(BUILD)/host/kernel/kernel/acpi.o $(BUILD)/host/kernel/shell/acpidump.o $(HOSTLIB_OBJ)
	$(Q)$(HOSTCC) $(HOSTFLAGS) $(HOSTKERN_FLAGS) $^ -o $@
$(BUILD)/host/test_acpidump $(BUILD)/host/kernel/kernel/acpi.o: HOSTKERN_FLAGS += -fgnu89-inline
unittest-acpi: $(BUILD)/host/test_acpidump
	$(BUILD)/host/test_acpidump
	$(PYTHON) -m unittest discover -s tests -p test_acpi_fetch.py
unittest: unittest-acpi
.PHONY: unittest-acpi

# Bounded DesignWare I2C PIO over a simulated finite-FIFO controller.
$(BUILD)/host/test_i2cdw: tests/host/test_i2cdw.c $(BUILD)/host/kernel/driver/i2cdw.o
	$(Q)$(HOSTCC) $(HOSTFLAGS) $(HOSTKERN_FLAGS) $^ -o $@
unittest-i2c: $(BUILD)/host/test_i2cdw
	$(BUILD)/host/test_i2cdw
unittest: unittest-i2c
.PHONY: unittest-i2c

# Bounded firmware fields and HID reports used by the native touchpad binding.
$(BUILD)/host/test_amlfield: tests/host/test_amlfield.c $(BUILD)/host/kernel/driver/amlfield.o $(HOSTLIB_OBJ)
	$(Q)$(HOSTCC) $(HOSTFLAGS) $(HOSTKERN_FLAGS) $^ -o $@
$(BUILD)/host/test_i2chid: tests/host/test_i2chid.c $(BUILD)/host/kernel/driver/i2chid.o
	$(Q)$(HOSTCC) $(HOSTFLAGS) $(HOSTKERN_FLAGS) $^ -o $@
$(BUILD)/host/test_hidpointer: tests/host/test_hidpointer.c $(BUILD)/host/kernel/driver/hidpointer.o $(HOSTLIB_OBJ)
	$(Q)$(HOSTCC) $(HOSTFLAGS) $(HOSTKERN_FLAGS) $^ -o $@
TOUCHPAD_TEST_OBJ = $(BUILD)/host/kernel/driver/amlfield.o $(BUILD)/host/kernel/driver/i2chid.o \
	$(BUILD)/host/kernel/driver/hidpointer.o $(HOSTLIB_OBJ)
$(BUILD)/host/test_touchpad: tests/host/test_touchpad.c driver/touchpad.c $(TOUCHPAD_TEST_OBJ)
	$(Q)$(HOSTCC) $(HOSTFLAGS) $(HOSTKERN_FLAGS) -fgnu89-inline -ffunction-sections -fdata-sections \
		tests/host/test_touchpad.c $(TOUCHPAD_TEST_OBJ) -Wl,--gc-sections -o $@
unittest-touchpad: $(BUILD)/host/test_amlfield $(BUILD)/host/test_i2chid $(BUILD)/host/test_hidpointer $(BUILD)/host/test_touchpad
	$(BUILD)/host/test_amlfield
	$(BUILD)/host/test_i2chid
	$(BUILD)/host/test_hidpointer
	$(BUILD)/host/test_touchpad
unittest: unittest-touchpad
.PHONY: unittest-touchpad

# A verified build-only patch fixes partial TLS 1.3 transform cleanup in 3.6.7.
# Both target and host use this same generated file; the vendor extraction stays pristine.
MBEDTLS_PATCHED = $(BUILD)/mbedtls-patched/ssl_tls13_keys.c
MBEDTLS_PATCH = third_party/patches/mbedtls-3.6.7-tls13-cleanup.json
$(MBEDTLS_PATCHED): $(MBEDTLS_DIR)/library/ssl_tls13_keys.c $(MBEDTLS_STAMP) $(MBEDTLS_PATCH) tools/patch-webdeps.py
	$(PYTHON) tools/patch-webdeps.py --source $< --patch $(MBEDTLS_PATCH) --output $@

$(BUILD)/user/mbedtls/ssl_tls13_keys.o: $(MBEDTLS_PATCHED) user/undertow/tls_config.h user/undertow/tls_platform.h
	@mkdir -p $(dir $@)
	$(Q)$(CC) $(MBEDTLS_CFLAGS) -I$(MBEDTLS_DIR)/library -c $< -o $@

$(BUILD)/host/mbedtls/ssl_tls13_keys.o: $(MBEDTLS_PATCHED) user/undertow/tls_config.h user/undertow/tls_platform.h tests/host/undertow/tls_host_config.h
	@mkdir -p $(dir $@)
	$(Q)$(HOSTCC) $(HOSTFLAGS) -MMD -MP $(MBEDTLS_HOST_CONF) -I$(MBEDTLS_DIR)/library -c $< -o $@

test-webpatch:
	$(PYTHON) -m unittest discover -s tests -p test_webpatch.py
unittest: test-webpatch
.PHONY: test-webpatch
