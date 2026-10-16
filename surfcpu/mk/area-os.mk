# SurfOS on riscv32 (the os track): the machine-mode kernel and its tests.
#
#   make build-kernel     ../build/riscv32/arty_a7/{surfos.elf,surfos.payload.bin,SURFOS.BIN}, and the same
#                         for QEMU's virt machine in ../build/riscv32/qemu_virt/ (../arch/riscv32/kernel.mk)
#   make test-os          the host tests of the port's pure code, then the qemu_virt kernel at -O2 and -O0
#                         in qemu-system-riscv32, driven over its serial console (boards/qemu_virt/test_os.py),
#                         booted as an ELF and as the SURFOS.BIN package through a SurfBoot stand-in
#   make test-os-host     only the host tests
#   make evidence-os      test-os, with its summaries written to docs/evidence/os/
#
# The kernel is built from the repository root (one level up); the x86 kernel's Makefile and outputs
# are never touched. QEMU's virt machine is not SurfCPU: test-os says nothing about the FPGA.

OS_MAKE     = $(MAKE) --no-print-directory -C .. -f arch/riscv32/kernel.mk
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

.PHONY: build-kernel-os test-os-host test-os-qemu test-os-iomodel evidence-os
build-kernel: build-kernel-os
test-os: test-os-host test-os-iomodel test-os-qemu
check: build-kernel test-os

build-kernel-os:
	$(OS_MAKE) KBOARD=arty_a7
	$(OS_MAKE) KBOARD=qemu_virt

$(OS_OUT)/test_arty: $(OS_ARTY_PURE) $(OS_ARTY_TESTS) $(wildcard ../boards/arty_a7/*.h ../boards/arty_a7/tests/*.h)
	@mkdir -p $(dir $@)
	$(Q)$(OS_HOSTCC) $(OS_HOST_CFLAGS) -o $@ $(OS_ARTY_PURE) $(OS_ARTY_TESTS)

# the Arty board's pure code on the host (several seeds), the temporary packer's CRC vectors and
# corruption checks (until tools/surfimg.py merges), and the Arty package checked as SurfBoot will
test-os-host: build-kernel-os $(if $(OS_ARTY_TESTS),$(OS_OUT)/test_arty)
	@mkdir -p $(OS_EVIDENCE)
	$(Q): > $(OS_OUT)/host.txt
ifneq ($(OS_ARTY_TESTS),)
	$(Q)$(OS_OUT)/test_arty $(OS_SEEDS) > $(OS_OUT)/host.txt; s=$$?; cat $(OS_OUT)/host.txt; exit $$s
endif
	$(Q)$(PYTHON) ../arch/riscv32/surfimg_tmp.py selftest >> $(OS_OUT)/host.txt
	$(Q)$(PYTHON) ../arch/riscv32/surfimg_tmp.py verify ../build/riscv32/arty_a7/SURFOS.BIN >> $(OS_OUT)/host.txt
	$(Q)tail -n 2 $(OS_OUT)/host.txt
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
	    --package build/riscv32/qemu_virt/SURFOS.BIN --stub build/riscv32/qemu_virt/surfboot_stub.elf \
	    --evidence surfcpu/$(OS_EVIDENCE)/test-os-qemu-O2.txt --transcript surfcpu/$(OS_OUT)/transcript-O2.txt
	$(Q)cd .. && $(PYTHON) boards/qemu_virt/test_os.py --kernel build/riscv32/qemu_virt-O0/surfos.elf \
	    --package build/riscv32/qemu_virt-O0/SURFOS.BIN --stub build/riscv32/qemu_virt-O0/surfboot_stub.elf \
	    --evidence surfcpu/$(OS_EVIDENCE)/test-os-qemu-O0.txt --transcript surfcpu/$(OS_OUT)/transcript-O0.txt

OS_ELFS = ../build/riscv32/arty_a7/surfos.elf ../build/riscv32/qemu_virt/surfos.elf ../build/riscv32/qemu_virt-O0/surfos.elf

evidence-os:
	$(MAKE) --no-print-directory test-os OS_EVIDENCE=docs/evidence/os
	$(Q)( echo "# make -C surfcpu build-kernel: sizes, the ISA audit, the packages ($$(date -u +%Y-%m-%d), build $(SURFCPU_BUILD_ID))"; \
	  $(RV_SIZE) $(OS_ELFS); \
	  for e in $(OS_ELFS); do $(PYTHON) ../arch/riscv32/isacheck.py --objdump $(RV_OBJDUMP) $$e; done; \
	  for p in $(OS_ELFS:surfos.elf=SURFOS.BIN); do $(PYTHON) ../arch/riscv32/surfimg_tmp.py verify $$p; done ) \
	  > docs/evidence/os/build.txt
	@echo "evidence-os: docs/evidence/os/{build,test-os-host,test-os-qemu-O2,test-os-qemu-O0}.txt"
