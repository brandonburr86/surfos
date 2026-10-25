# SurfGPU in SurfOS (milestone G3, docs/GPU.md section 15): the GPU driver of the Arty kernel
# (boards/arty_a7/gpu.c, gpuhw.c) and the gfx command (boards/arty_a7/gfx.c).
#
#   make test-gpuos-host   the driver and gfx built for the host against the C model of the engine
#                          (sim/models/gpu_model.c) through a stand-in for the hardware
#                          (boards/arty_a7/tests/gpu/: test_gpu.c, gpustub.c), seeds 1 7 123; on test-os
#   make test-gpuos        SurfOS booted by SurfBoot from a card on the RTL SoC with SurfGPU
#                          (build/sim/surfsim_gpu): gfx commands, every gfx crc against the C model
#                          (gfxscene) and the frames captured at the VGA pins, the overlay, gfx demo 1,
#                          the bench; and on the SoC without one (build/sim/surfsim) gfx says there is
#                          none (boards/arty_a7/tests/gpuos_test.py; two runs side by side, under three
#                          minutes); on check
#   make gfxscene          build/gpuos/gfxscene: gfx command lines replayed in the C model, the
#                          FRAME_CRCs SurfOS must print for them
#   make evidence-gpuos    both, their summaries written to docs/evidence/gpu-g3/
#
# GPUOS_SEEDS chooses the host test's seeds. The host build sees the kernel's headers only after
# the system's (-idirafter), so <stdio.h> is the host's; gpu.c and gfx.c print through gt_printf
# (the stand-in keeps what they say).

GPUOS_OUT       = $(BUILD)/gpuos
GPUOS_EVIDENCE ?= $(GPUOS_OUT)
GPUOS_TDIR      = ../boards/arty_a7/tests/gpu
GPUOS_CC       ?= cc
GPUOS_INC       = -I../boards/arty_a7/tests/shim -I../boards/arty_a7 -I../boards/arty_a7/tests -I$(GPUOS_TDIR) \
                  -Isim/models -Ifirmware/include -idirafter ../include
GPUOS_CFLAGS    = -std=gnu11 -O2 -g -Wall -Wextra -Werror -U_FORTIFY_SOURCE $(GPUOS_INC)
GPUOS_SEEDS    ?= 1 7 123
GPUOS_HDRS      = ../boards/arty_a7/gpu.h $(wildcard $(GPUOS_TDIR)/*.h ../boards/arty_a7/tests/shim/surfos/*.h) \
                  ../boards/arty_a7/tests/harness.h sim/models/gpu_model.h firmware/include/surfcpu/gpu.h \
                  firmware/include/surfcpu/soc.h ../shell/shell.h
GPUOS_OBJ       = $(GPUOS_OUT)/obj
# the driver and the command as the kernel has them, the font and character set from where they are
GPUOS_LINK      = $(GPUOS_OBJ)/gpu.o $(GPUOS_OBJ)/gfx.o $(GPUOS_OBJ)/gpustub.o $(GPUOS_OBJ)/gpu_model.o \
                  $(GPUOS_OBJ)/font8x16.o $(GPUOS_OBJ)/cp437.o

.PHONY: test-gpuos-host test-gpuos gfxscene evidence-gpuos
test-os: test-gpuos-host
check: test-gpuos-host test-gpuos

$(GPUOS_OBJ)/gpu.o: ../boards/arty_a7/gpu.c $(GPUOS_HDRS)
	@mkdir -p $(dir $@)
	$(Q)$(GPUOS_CC) $(GPUOS_CFLAGS) -Dprintf=gt_printf -c $< -o $@

$(GPUOS_OBJ)/gfx.o: ../boards/arty_a7/gfx.c $(GPUOS_HDRS)
	@mkdir -p $(dir $@)
	$(Q)$(GPUOS_CC) $(GPUOS_CFLAGS) -Dprintf=gt_printf -c $< -o $@

$(GPUOS_OBJ)/gpustub.o: $(GPUOS_TDIR)/gpustub.c $(GPUOS_HDRS)
	@mkdir -p $(dir $@)
	$(Q)$(GPUOS_CC) $(GPUOS_CFLAGS) -c $< -o $@

$(GPUOS_OBJ)/gpu_model.o: sim/models/gpu_model.c sim/models/gpu_model.h firmware/include/surfcpu/gpu.h
	@mkdir -p $(dir $@)
	$(Q)$(GPUOS_CC) -std=c99 -O2 -g -Wall -Wextra -Werror -Isim/models -Ifirmware/include -c $< -o $@

# shared kernel files, compiled unchanged (warnings are the x86 build's business, not -Werror here)
$(GPUOS_OBJ)/font8x16.o: ../driver/font8x16.c
	@mkdir -p $(dir $@)
	$(Q)$(GPUOS_CC) -std=gnu11 -O2 -g -w $(GPUOS_INC) -c $< -o $@

$(GPUOS_OBJ)/cp437.o: ../driver/cp437.c
	@mkdir -p $(dir $@)
	$(Q)$(GPUOS_CC) -std=gnu11 -O2 -g -w $(GPUOS_INC) -c $< -o $@

$(GPUOS_OUT)/test_gpu: $(GPUOS_TDIR)/test_gpu.c ../boards/arty_a7/tests/harness.c $(GPUOS_LINK) $(GPUOS_HDRS)
	$(Q)$(GPUOS_CC) $(GPUOS_CFLAGS) -o $@ $(GPUOS_TDIR)/test_gpu.c ../boards/arty_a7/tests/harness.c $(GPUOS_LINK)

$(GPUOS_OUT)/gfxscene: $(GPUOS_TDIR)/gfxscene.c $(GPUOS_LINK) $(GPUOS_HDRS)
	$(Q)$(GPUOS_CC) $(GPUOS_CFLAGS) -o $@ $(GPUOS_TDIR)/gfxscene.c $(GPUOS_LINK)

gfxscene: $(GPUOS_OUT)/gfxscene

test-gpuos-host: $(GPUOS_OUT)/test_gpu
	@mkdir -p $(GPUOS_EVIDENCE)
	$(Q)$(GPUOS_OUT)/test_gpu $(GPUOS_SEEDS) > $(GPUOS_OUT)/host.txt; s=$$?; cat $(GPUOS_OUT)/host.txt; \
	  cp $(GPUOS_OUT)/host.txt $(GPUOS_EVIDENCE)/test-gpuos-host.txt 2>/dev/null; exit $$s

# SurfOS on the RTL system: needs the soc track's surfsim, the fw track's card tool and packer
GPUOS_SYS_READY := $(and $(wildcard sim/soc/surfsim/surfsim.cpp),$(wildcard tools/mkcard.py),$(wildcard tools/surfimg.py))
ifneq ($(GPUOS_SYS_READY),)
test-gpuos: build-kernel-os $(FW)/surfboot.mem $(BUILD)/sim/surfsim_gpu $(BUILD)/sim/surfsim $(GPUOS_OUT)/gfxscene
	@mkdir -p $(GPUOS_EVIDENCE)
	$(Q)$(PYTHON) ../boards/arty_a7/tests/gpuos_test.py --surfsim-gpu $(BUILD)/sim/surfsim_gpu --surfsim $(BUILD)/sim/surfsim \
	  --rom $(FW)/surfboot.mem --mkcard tools/mkcard.py --package ../build/riscv32/arty_a7/SURFOS.BIN \
	  --gfxscene $(GPUOS_OUT)/gfxscene --work $(GPUOS_OUT)/system --evidence $(GPUOS_EVIDENCE)/test-gpuos.txt
else
test-gpuos:
	@echo "test-gpuos: skipped: this tree has no surfsim, mkcard.py or surfimg.py"
endif

GPUOS_EVIDENCE_DIR ?= docs/evidence/gpu-g3
evidence-gpuos:
	$(MAKE) --no-print-directory test-gpuos-host test-gpuos GPUOS_EVIDENCE=$(GPUOS_EVIDENCE_DIR)
	@echo "evidence-gpuos: $(GPUOS_EVIDENCE_DIR)/{test-gpuos-host,test-gpuos}.txt"
