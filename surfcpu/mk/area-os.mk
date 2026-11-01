# SurfOS on riscv32 (the os track): the machine-mode kernel and its tests.
#
#   make build-kernel     ../build/riscv32/arty_a7/{surfos.elf,surfos.payload.bin,SURFOS.BIN}, and the same
#                         for QEMU's virt machine in ../build/riscv32/qemu_virt/ (../arch/riscv32/kernel.mk)
#   make test-os          the host tests of the port's pure code, then the qemu_virt kernel at -O2 and -O0
#                         in qemu-system-riscv32, driven over its serial console (boards/qemu_virt/test_os.py),
#                         booted as an ELF and through a SurfBoot stand-in (the boot ABI's handoff, FAT over
#                         the SD driver; SURFOS.BIN and a card image where the fw track's tools are present)
#   make test-os-host     only the host tests
#   make test-os-surfemu  the Arty kernel booted by SurfBoot from a mkcard.py card in the fw track's firmware
#                         emulator (where the tree has it; part of test-os)
#   make test-os-persist  files that persist (milestone 8) on the RTL system in surfsim: written, synced and
#                         ejected over the UART, read back after a second boot, checked by fsck.fat and mtools
#                         (about 2.5 minutes; on make check, not in test-os)
#   make test-os-faults   write faults, the card taken out and put back, and a full volume on the RTL system
#                         (surfsim's --sd-fault, --sd-remove, --sd-insert), checked by fsck.fat and mtools
#                         (two runs at once, about 1.5 minutes; on make check, not in test-os)
#   make test-os-kit      the physical test kit (tools/board-test.py, tools/uart.py) proven before there is a
#                         board: fake boards on pseudo-terminals, m3 and m7 on the RTL system through surfsim
#                         --uart-pty, and the edge cases (about a minute; on make check, not in test-os)
#   make test-os-kit-rtl  the kit's m5, m8 and m9 on the RTL system (about 3 minutes; in evidence-os)
#   make evidence-os      test-os, test-os-persist, test-os-faults and the kit, their summaries written to
#                         docs/evidence/os/
#
# The kernel is built from the repository root (one level up); the x86 kernel's Makefile and outputs
# are never touched. QEMU's virt machine is not SurfCPU: test-os says nothing about the FPGA.
#
# SURFOS.BIN is packed by the fw track's tools/surfimg.py and card images are made by its tools/mkcard.py;
# where this tree lacks them (sc/os before integration) those steps say they are skipped. OS_SURFIMG and
# OS_MKCARD name other copies.

OS_SURFIMG ?= tools/surfimg.py
OS_MKCARD  ?= tools/mkcard.py
OS_MAKE     = $(MAKE) --no-print-directory -C .. -f arch/riscv32/kernel.mk SURFIMG=$(abspath $(OS_SURFIMG))
OS_OUT      = $(BUILD)/os
OS_EVIDENCE ?= $(OS_OUT)
OS_HOSTCC  ?= cc
OS_HOST_CFLAGS = -std=gnu11 -O2 -Wall -Wextra -Werror -I../boards/arty_a7/tests/shim -I../boards/arty_a7 \
                 -I../boards/arty_a7/tests
# the Arty board's pure files and their host tests (boards/arty_a7/tests): each test file registers itself
OS_ARTY_PURE  = $(wildcard $(addprefix ../boards/arty_a7/,textscreen.c ps2set2.c kbdset1.c sdproto.c sdcard.c))
OS_ARTY_TESTS = $(wildcard ../boards/arty_a7/tests/*.c)
OS_SEEDS     ?= 1 7 123

# the SD driver against the io track's card model (sim/models/sdcard_model, merged into surfcpu): run where the
# model is in the tree; OS_IOMODEL_DIR names another copy of it
OS_IOMODEL_DIR ?= sim/models
OS_HOSTCXX     ?= c++
OS_IO_C         = ../boards/arty_a7/sdcard.c ../boards/arty_a7/sdproto.c ../boards/arty_a7/tests/iomodel/glue.c

.PHONY: build-kernel-os test-os-host test-os-qemu test-os-iomodel test-os-surfemu test-os-persist test-os-faults \
        test-os-kit test-os-kit-rtl evidence-os
build-kernel: build-kernel-os
test-os: test-os-host test-os-iomodel test-os-qemu test-os-surfemu
check: build-kernel test-os test-os-persist test-os-faults test-os-kit

build-kernel-os:
	$(OS_MAKE) KBOARD=arty_a7
	$(OS_MAKE) KBOARD=qemu_virt

$(OS_OUT)/test_arty: $(OS_ARTY_PURE) $(OS_ARTY_TESTS) $(wildcard ../boards/arty_a7/*.h ../boards/arty_a7/tests/*.h)
	@mkdir -p $(dir $@)
	$(Q)$(OS_HOSTCC) $(OS_HOST_CFLAGS) -o $@ $(OS_ARTY_PURE) $(OS_ARTY_TESTS)

# the Arty board's pure code on the host (several seeds), and the Arty package checked as SurfBoot will
# (the fw track's surfimg.py verify) where the tree has the packer
test-os-host: build-kernel-os $(if $(OS_ARTY_TESTS),$(OS_OUT)/test_arty)
	@mkdir -p $(OS_EVIDENCE)
	$(Q): > $(OS_OUT)/host.txt
ifneq ($(OS_ARTY_TESTS),)
	$(Q)$(OS_OUT)/test_arty $(OS_SEEDS) > $(OS_OUT)/host.txt; s=$$?; cat $(OS_OUT)/host.txt; exit $$s
endif
ifneq ($(wildcard $(OS_SURFIMG)),)
	$(Q)$(PYTHON) $(OS_SURFIMG) verify ../build/riscv32/arty_a7/SURFOS.BIN >> $(OS_OUT)/host.txt; s=$$?; \
	    tail -n 1 $(OS_OUT)/host.txt; exit $$s
else
	$(Q)echo "test-os-host: the Arty package not checked: no $(OS_SURFIMG) (the fw track's packer) in this tree" \
	    | tee -a $(OS_OUT)/host.txt
endif
	$(Q)cp $(OS_OUT)/host.txt $(OS_EVIDENCE)/test-os-host.txt 2>/dev/null || true

test-os-iomodel:
ifneq ($(wildcard $(OS_IOMODEL_DIR)/sdcard_model.cpp),)
	@mkdir -p $(OS_OUT)/iomodel $(OS_EVIDENCE)
	$(Q)for c in $(OS_IO_C); do $(OS_HOSTCC) $(OS_HOST_CFLAGS) -c $$c -o $(OS_OUT)/iomodel/$$(basename $$c .c).o || exit 1; done
	$(Q)$(OS_HOSTCXX) -std=c++17 -O2 -Wall -I$(OS_IOMODEL_DIR) -o $(OS_OUT)/test_sd_iomodel \
	    ../boards/arty_a7/tests/iomodel/test_sd_iomodel.cpp $(OS_IOMODEL_DIR)/sdcard_model.cpp $(OS_OUT)/iomodel/*.o
	$(Q)$(OS_OUT)/test_sd_iomodel $(OS_SEEDS) > $(OS_OUT)/iomodel.txt; s=$$?; cat $(OS_OUT)/iomodel.txt; \
	    cp $(OS_OUT)/iomodel.txt $(OS_EVIDENCE)/test-os-iomodel.txt; exit $$s
else
	@echo "test-os-iomodel: no $(OS_IOMODEL_DIR)/sdcard_model.cpp (the io track's card model) here: skipped"
endif

test-os-qemu:
	@mkdir -p $(OS_OUT) $(OS_EVIDENCE)
	$(OS_MAKE) KBOARD=qemu_virt
	$(OS_MAKE) KBOARD=qemu_virt O=0
	$(Q)cd .. && $(PYTHON) boards/qemu_virt/test_os.py --kernel build/riscv32/qemu_virt/surfos.elf \
	    --stub build/riscv32/qemu_virt/surfboot_stub.elf --package build/riscv32/qemu_virt/SURFOS.BIN \
	    --surfimg $(abspath $(OS_SURFIMG)) --mkcard $(abspath $(OS_MKCARD)) \
	    --evidence surfcpu/$(OS_EVIDENCE)/test-os-qemu-O2.txt --transcript surfcpu/$(OS_OUT)/transcript-O2.txt
	$(Q)cd .. && $(PYTHON) boards/qemu_virt/test_os.py --kernel build/riscv32/qemu_virt-O0/surfos.elf \
	    --stub build/riscv32/qemu_virt-O0/surfboot_stub.elf --package build/riscv32/qemu_virt-O0/SURFOS.BIN \
	    --surfimg $(abspath $(OS_SURFIMG)) --mkcard $(abspath $(OS_MKCARD)) \
	    --evidence surfcpu/$(OS_EVIDENCE)/test-os-qemu-O0.txt --transcript surfcpu/$(OS_OUT)/transcript-O0.txt

# The Arty kernel package booted by SurfBoot from a mkcard.py card in the fw track's firmware emulator, an
# instruction-level model of the SoC (firmware/tests/surfemu.c): the handoff, the console on surf_uart and surf_video,
# the card over surf_spi with FAT and /USERS/DOCUMENTS, tasks and the self test (boards/arty_a7/tests/surfemu_test.py).
# A test aid, not the RTL and not the board. Skipped where the tree lacks the emulator, the packer or the card tool.
ifneq ($(and $(wildcard firmware/tests/surfemu.c),$(wildcard $(OS_SURFIMG)),$(wildcard $(OS_MKCARD))),)
test-os-surfemu: build-kernel-os $(BUILD)/fw/surfboot.mem $(BUILD)/fw/host/surfemu
	@mkdir -p $(OS_OUT) $(OS_EVIDENCE)
	$(Q)$(PYTHON) ../boards/arty_a7/tests/surfemu_test.py --surfemu $(BUILD)/fw/host/surfemu \
	    --rom $(BUILD)/fw/surfboot.mem --mkcard $(OS_MKCARD) --package ../build/riscv32/arty_a7/SURFOS.BIN \
	    --evidence $(OS_EVIDENCE)/test-os-surfemu.txt --transcript $(OS_OUT)/transcript-surfemu.txt
else
test-os-surfemu:
	@echo "test-os-surfemu: skipped: this tree has no fw firmware emulator (firmware/tests/surfemu.c), packer or card tool"
endif

# Files that persist (milestone 8) on the RTL system (sim/soc/tools/system_persist_test.py): build/sim/surfsim,
# the soc track's whole SoC with the io track's card model, boots the Arty kernel from a mkcard.py card with
# sd=rw; the shell writes (mkdir, write, a 160 KiB cp, write over, append, mv, rm), syncs and ejects; surfsim's
# --sd-flush keeps what the card got; a second boot reads it all back by name, size and CRC; fsck.fat and
# mtools check the image. Two runs one after the other.
OS_PERSIST_READY := $(and $(wildcard sim/soc/surfsim/surfsim.cpp),$(wildcard sim/soc/tools/system_boot_test.py),\
                      $(wildcard tools/mkcard.py),$(wildcard tools/surfimg.py))
ifneq ($(OS_PERSIST_READY),)
test-os-persist: build-kernel-os $(BUILD)/fw/surfboot.mem $(BUILD)/sim/surfsim
	@mkdir -p $(OS_OUT) $(OS_EVIDENCE)
	$(Q)$(PYTHON) sim/soc/tools/system_persist_test.py --surfsim $(BUILD)/sim/surfsim --rom $(BUILD)/fw/surfboot.mem \
	    --mkcard tools/mkcard.py --package ../build/riscv32/arty_a7/SURFOS.BIN --work $(OS_OUT)/system_persist \
	    --evidence $(OS_EVIDENCE)/test-os-persist.txt
else
test-os-persist:
	@echo "test-os-persist: skipped: this tree has no surfsim, system_boot_test.py, mkcard.py or surfimg.py"
endif

# Write faults, the card taken out and put back, and a full volume on the RTL system
# (sim/soc/tools/system_fault_test.py): two surfsim runs at once with surfsim's card actions
# (--sd-fault, --sd-remove, --sd-insert), each on a mkcard.py card with sd=rw; fsck.fat and mtools
# check the cards afterwards.
ifneq ($(OS_PERSIST_READY),)
test-os-faults: build-kernel-os $(BUILD)/fw/surfboot.mem $(BUILD)/sim/surfsim
	@mkdir -p $(OS_OUT) $(OS_EVIDENCE)
	$(Q)$(PYTHON) sim/soc/tools/system_fault_test.py --surfsim $(BUILD)/sim/surfsim --rom $(BUILD)/fw/surfboot.mem \
	    --mkcard tools/mkcard.py --package ../build/riscv32/arty_a7/SURFOS.BIN --work $(OS_OUT)/system_faults \
	    --evidence $(OS_EVIDENCE)/test-os-faults.txt
else
test-os-faults:
	@echo "test-os-faults: skipped: this tree has no surfsim, system_boot_test.py, mkcard.py or surfimg.py"
endif

# The physical test kit (docs/PHYSICAL-TESTS.md) run as the owner will run it, the owner's steps done by a
# stand-in (sim/soc/tools/kit_test.py): against fake boards on pseudo-terminals (m1, uart.py, a port that
# goes away, garbage, silence, a wrong manifest, a lost second) and against the RTL system through surfsim
# --uart-pty (a power cycle is a new surfsim run on the card image the last one flushed). test-os-kit: the
# fake boards, m3, m7 and two RTL edge cases at once; test-os-kit-rtl: m5, m8 and m9 with a one-minute soak.
# OS_KIT_FULL=DIR also keeps each RTL run's whole evidence file, transcript included.
OS_KIT = $(PYTHON) sim/soc/tools/kit_test.py --surfsim $(BUILD)/sim/surfsim --fw $(BUILD)/fw \
         --kernel ../build/riscv32/arty_a7/SURFOS.BIN --mkcard $(OS_MKCARD)
OS_KIT_DEPS = build-kernel-os $(BUILD)/fw/surfboot.mem $(BUILD)/fw/bramdiag.mem $(BUILD)/fw/DDRDIAG.BIN \
              $(BUILD)/sim/surfsim
ifneq ($(OS_PERSIST_READY),)
test-os-kit: $(OS_KIT_DEPS)
	@mkdir -p $(OS_OUT) $(OS_EVIDENCE)
	$(Q)$(OS_KIT) --set check --work $(OS_OUT)/kit --evidence $(OS_EVIDENCE)/test-os-kit.txt \
	    $(if $(OS_KIT_FULL),--evidence-full $(OS_KIT_FULL))

test-os-kit-rtl: $(OS_KIT_DEPS)
	@mkdir -p $(OS_OUT) $(OS_EVIDENCE)
	$(Q)$(OS_KIT) --set rtl --work $(OS_OUT)/kit-rtl --evidence $(OS_EVIDENCE)/test-os-kit-rtl.txt \
	    $(if $(OS_KIT_FULL),--evidence-full $(OS_KIT_FULL))
else
test-os-kit test-os-kit-rtl:
	@echo "$@: skipped: this tree has no surfsim, system_boot_test.py, mkcard.py or surfimg.py"
endif

OS_ELFS = ../build/riscv32/arty_a7/surfos.elf ../build/riscv32/qemu_virt/surfos.elf ../build/riscv32/qemu_virt-O0/surfos.elf
OS_EVIDENCE_DIR ?= docs/evidence/os

evidence-os:
	$(MAKE) --no-print-directory test-os test-os-persist test-os-faults test-os-kit test-os-kit-rtl \
	    OS_EVIDENCE=$(OS_EVIDENCE_DIR) OS_KIT_FULL=$(OS_EVIDENCE_DIR)/kit
	$(Q)( echo "# make -C surfcpu build-kernel: sizes, the ISA audit, the packages ($$(date -u +%Y-%m-%d), build $(SURFCPU_BUILD_ID))"; \
	  $(RV_SIZE) $(OS_ELFS); \
	  for e in $(OS_ELFS); do $(PYTHON) ../arch/riscv32/isacheck.py --objdump $(RV_OBJDUMP) $$e; done; \
	  for t in $(OS_SURFIMG) $(OS_MKCARD); do if [ -f $$t ]; then echo "fw tool $$t: sha256 $$(sha256sum < $$t | cut -c1-64)"; fi; done; \
	  if [ -f $(OS_SURFIMG) ]; then $(PYTHON) $(OS_SURFIMG) info ../build/riscv32/arty_a7/SURFOS.BIN; \
	    for p in $(OS_ELFS:surfos.elf=SURFOS.BIN); do $(PYTHON) $(OS_SURFIMG) verify $$p; done; \
	  else echo "SURFOS.BIN not built: no $(OS_SURFIMG) (the fw track's packer) in this tree"; fi ) \
	  > $(OS_EVIDENCE_DIR)/build.txt
	@echo "evidence-os: $(OS_EVIDENCE_DIR)/{build,test-os-host,test-os-iomodel,test-os-qemu-O2,test-os-qemu-O0,test-os-surfemu,test-os-persist,test-os-faults,test-os-kit,test-os-kit-rtl}.txt, kit/"
