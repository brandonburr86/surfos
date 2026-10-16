# SurfOS for riscv32 (SurfCPU), machine mode: the kernel build (10/15/26).
#
# Run from the repository root, inside the SurfCPU toolchain container
# (surfcpu/devenv/run.sh, or `make -C surfcpu build-kernel`, which calls this):
#
#   make -f arch/riscv32/kernel.mk KBOARD=arty_a7      build/riscv32/arty_a7/{surfos.elf,surfos.payload.bin,SURFOS.BIN}
#   make -f arch/riscv32/kernel.mk KBOARD=qemu_virt    build/riscv32/qemu_virt/... (QEMU's virt machine, for the tests)
#   make -f arch/riscv32/kernel.mk KBOARD=qemu_virt O=0   an unoptimised build, in build/riscv32/qemu_virt-O0/
#
# SURFOS.BIN is packed by the fw track's surfcpu/tools/surfimg.py (SURFIMG names another copy).
# Where the tree has no surfimg.py, surfos.elf and surfos.payload.bin are built and the package
# is skipped with a message saying so: there is no other packer.
#
# KBOARD (not BOARD: the SurfCPU makefiles use BOARD for the FPGA part) is arty_a7 (both
# Arty parts run the same SoC map and image) or qemu_virt. The x86 kernel's Makefile never
# reaches arch/riscv32, include/arch/riscv32 or boards/, and nothing here writes outside
# build/riscv32/. The shared kernel files are compiled from where they are, with
# include/arch/riscv32 ahead of include: its headers take the place of the x86 ones of the
# same name (irq.h, trap.h, interrupt.h, cpu.h, asm/io.h, mm/memory.h).

KBOARD     ?= arty_a7
O          ?= 2
ifeq ($(O),2)
OUT        ?= build/riscv32/$(KBOARD)
else
OUT        ?= build/riscv32/$(KBOARD)-O$(O)
endif
OBJ         = $(OUT)/obj

RV_PREFIX  ?= riscv64-unknown-elf-
CC          = $(RV_PREFIX)gcc
NM          = $(RV_PREFIX)nm
OBJCOPY     = $(RV_PREFIX)objcopy
OBJDUMP     = $(RV_PREFIX)objdump
PYTHON     ?= python3
Q           = $(if $(V),,@)

ifeq ($(filter $(KBOARD),arty_a7 qemu_virt),)
$(error KBOARD must be arty_a7 or qemu_virt, not '$(KBOARD)')
endif

# Build identity: surfcpu/devenv/run.sh passes the commit in from the Mac.
SURFCPU_BUILD_ID ?= unknown
BUILD_ID_HEX     := $(shell printf '%.8s' '$(SURFCPU_BUILD_ID)' | grep -E '^[0-9a-f]{8}$$' || echo 00000000)
BUILD_TIME       := $(or $(SOURCE_DATE_EPOCH),$(shell date -u +%s))

# rv32im_zicsr_zifencei / ilp32 (surfcpu/docs/ISA.md). Freestanding, as the x86 kernel:
# its own libc headers (-nostdinc), frame pointers for backtraces, gnu89 inline semantics.
# -fsigned-char: the shared code was written where char is signed (x86); RISC-V's psABI
# says unsigned, which matters only at interfaces with foreign code (there is none: libgcc
# has no char interfaces). -mstrict-align: SurfCPU traps misaligned loads and stores, so the
# compiler must never make one itself.
ARCHFLAGS   = -march=rv32im_zicsr_zifencei -mabi=ilp32 -mcmodel=medlow
CFLAGS      = $(ARCHFLAGS) -ffreestanding -fno-builtin -nostdlib -nostdinc -fno-pic -fno-pie \
              -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
              -fgnu89-inline -fno-strict-aliasing -fno-omit-frame-pointer -fsigned-char -mstrict-align \
              -fno-common -fno-tree-loop-distribute-patterns \
              -Wall -Wno-main -g -O$(O) \
              -DSURFOS_RISCV32 -DSURFOS_BOARD_$(KBOARD) -DSURFOS_BUILD_TIME=$(BUILD_TIME)UL \
              -Iinclude/arch/riscv32 -Iinclude -Isurfcpu/firmware/include -Iboards/$(KBOARD) \
              -MMD -MP
ASFLAGS     = $(ARCHFLAGS) -g -D__ASSEMBLER__ -Iinclude/arch/riscv32 -Iinclude -MMD -MP
LDFLAGS     = $(ARCHFLAGS) -nostdlib -nostartfiles -static -T arch/riscv32/linker.ld \
              -Lboards/$(KBOARD) -Lsurfcpu/firmware/ld -Wl,--no-warn-rwx-segments
LIBGCC      = -lgcc

# The shared kernel files the machine-mode port builds: the scheduler and its waits, locks and
# timers, the consoles, ttys and kernel log, the heap, the C library, the VFS, block cache, FAT,
# devfs and tarfs, the block layer and partition tables, and the shell's file system commands
# and prompt. Each compiles unchanged except kernel/task.c and lib/blibc/vsnprintf.c, whose
# few x86 lines have riscv32 alternatives under #ifdef SURFOS_RISCV32.
# lib/blibc/memory.c is not built: arch/riscv32/memory.c has the same functions moving words
# (10/16/26: SurfCPU fetches every instruction from DDR).
SHARED_C    = kernel/task.c kernel/wait.c kernel/sync.c kernel/ktimer.c kernel/klog.c \
              kernel/console.c kernel/tty.c \
              mm/kalloc.c \
              lib/blibc/chars.c lib/blibc/ctype.c lib/blibc/printf.c \
              lib/blibc/stdlib.c lib/blibc/string.c lib/blibc/strings.c lib/blibc/vsnprintf.c \
              fs/vfs.c fs/bcache.c fs/fat.c fs/fat_mkfs.c fs/devfs.c fs/poll.c fs/tarfs.c \
              driver/bdev.c driver/part.c driver/parttab.c driver/cp437.c \
              shell/prompt.c shell/fscmd.c shell/fdisk.c
ARCH_C      = $(wildcard arch/riscv32/*.c)
ARCH_S      = $(wildcard arch/riscv32/*.S)
BOARD_C     = $(wildcard boards/$(KBOARD)/*.c)
ifeq ($(KBOARD),qemu_virt)
# the Arty's SD protocol driver, its block device and card watch, and the simulated card of its
# host test, for the word sdsim (boards/qemu_virt/sdsim.c): plain C, no hardware
BOARD_C    += $(if $(wildcard boards/qemu_virt/sdsim.c),boards/arty_a7/sdcard.c boards/arty_a7/sdproto.c \
                   boards/arty_a7/sdblk.c boards/arty_a7/tests/sdmodel.c)
endif
BOARD_S     = $(wildcard boards/$(KBOARD)/*.S)

OBJS        = $(ARCH_S:%.S=$(OBJ)/%.o) $(BOARD_S:%.S=$(OBJ)/%.o) \
              $(ARCH_C:%.c=$(OBJ)/%.o) $(BOARD_C:%.c=$(OBJ)/%.o) $(SHARED_C:%.c=$(OBJ)/%.o)
DEPS        = $(OBJS:.o=.d)

ELF         = $(OUT)/surfos.elf
STAGE1      = $(OUT)/surfos.stage1
PAYLOAD     = $(OUT)/surfos.payload.bin
PACKAGE     = $(OUT)/SURFOS.BIN
SYMS        = $(OUT)/surfos.sym
ISACHECK    = arch/riscv32/isacheck.py
SURFIMG    ?= surfcpu/tools/surfimg.py

# SURFOS.BIN records the commit it was built from (its build ID). The stamp is rewritten only when
# the ID changes, so a new commit repacks the kernel even when no source file changed.
BUILD_ID_STAMP = $(OUT)/build-id.txt
$(shell mkdir -p $(OUT); [ "`cat $(BUILD_ID_STAMP) 2>/dev/null`" = "$(BUILD_ID_HEX)" ] || echo "$(BUILD_ID_HEX)" > $(BUILD_ID_STAMP))

.PHONY: all clean no-package
.DELETE_ON_ERROR:
ifneq ($(wildcard $(SURFIMG)),)
all: $(PACKAGE)
else
all: $(PAYLOAD) no-package
endif

$(OBJ)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "RVCC $<"
	$(Q)$(CC) $(CFLAGS) -c $< -o $@

$(OBJ)/%.o: %.S
	@mkdir -p $(dir $@)
	@echo "RVAS $<"
	$(Q)$(CC) $(ASFLAGS) -c $< -o $@

# Two passes, as the x86 kernel: an empty symbol table to learn the addresses, then the
# real one (tools/gensyms.py). The table's room is fixed (linker.ld), so the second link
# must put every function where the first did: checked below.
$(OBJ)/ksyms0.c: tools/gensyms.py
	@mkdir -p $(dir $@)
	$(Q)$(PYTHON) tools/gensyms.py < /dev/null > $@

$(STAGE1): $(OBJS) $(OBJ)/ksyms0.o arch/riscv32/linker.ld boards/$(KBOARD)/board.ld
	@echo "RVLD $@ (pass 1)"
	$(Q)$(CC) $(LDFLAGS) -o $@ $(OBJS) $(OBJ)/ksyms0.o $(LIBGCC)

$(OBJ)/ksyms.c: $(STAGE1) tools/gensyms.py
	$(Q)$(NM) -n $(STAGE1) | $(PYTHON) tools/gensyms.py > $@

$(OBJ)/ksyms0.o $(OBJ)/ksyms.o: $(OBJ)/%.o: $(OBJ)/%.c
	$(Q)$(CC) $(CFLAGS) -c $< -o $@

$(ELF): $(OBJS) $(OBJ)/ksyms.o arch/riscv32/linker.ld boards/$(KBOARD)/board.ld
	@echo "RVLD $@"
	$(Q)$(CC) $(LDFLAGS) -Wl,-Map,$(OUT)/surfos.map -o $@ $(OBJS) $(OBJ)/ksyms.o $(LIBGCC)
	$(Q)$(NM) -n $@ > $(SYMS)
	$(Q)$(NM) -n $(STAGE1) | grep -i ' t ' > $(OBJ)/text1.txt
	$(Q)grep -i ' t ' $(SYMS) > $(OBJ)/text2.txt
	$(Q)cmp -s $(OBJ)/text1.txt $(OBJ)/text2.txt || { echo "kernel.mk: the second link moved code (backtraces would lie)"; \
	    diff $(OBJ)/text1.txt $(OBJ)/text2.txt | head; exit 1; }
	$(Q)$(PYTHON) $(ISACHECK) --objdump $(OBJDUMP) $@

$(PAYLOAD): $(ELF)
	$(Q)$(OBJCOPY) -O binary $< $@

# The fw track's packer (BOOT-ABI.md section 2): it puts the ELF through tools/isa-audit.py and
# refuses to write a package SurfBoot would refuse. The payload it packs must be the same bytes as
# objcopy's surfos.payload.bin.
$(PACKAGE): $(ELF) $(PAYLOAD) $(SURFIMG) $(BUILD_ID_STAMP)
	$(Q)$(PYTHON) $(SURFIMG) pack --elf $(ELF) -o $@ --payload-out $(OUT)/surfimg.payload.bin \
	    --name SurfOS --version 1.0.0 --build-id $(BUILD_ID_HEX)
	$(Q)cmp -s $(OUT)/surfimg.payload.bin $(PAYLOAD) || { echo "kernel.mk: surfimg.py packed other bytes than $(PAYLOAD)"; \
	    rm -f $@; exit 1; }
	@echo "built $@ ($(KBOARD), -O$(O))"

no-package: $(PAYLOAD)
	@rm -f $(PACKAGE)
	@echo "kernel.mk: SURFOS.BIN skipped: no $(SURFIMG) (the fw track's packer) in this tree; built $(ELF) and $(PAYLOAD)"

# QEMU virt only: a stand-in for SurfBoot's last step (copy the payload of SURFOS.BIN, hand over
# with a0/a1/a2 as BOOT-ABI.md says), so the tests can boot the package and not only the ELF
ifeq ($(KBOARD),qemu_virt)
STUB        = $(OUT)/surfboot_stub.elf
all: $(STUB)
$(STUB): boards/qemu_virt/stub/surfboot_stub.S
	@mkdir -p $(dir $@)
	$(Q)$(CC) $(ARCHFLAGS) -nostdlib -nostartfiles -static -Wl,-Ttext=0x8C100000 -Wl,-e,_start \
	    -Wl,--no-warn-rwx-segments -o $@ $<
endif

clean:
	rm -rf $(OUT)

-include $(DEPS)
