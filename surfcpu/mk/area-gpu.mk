# gpu track: VRAM, the graphics scan-out, the drawing engine, their C reference model and tests
# (docs/GPU.md). Hung on the aggregates: lint (lint-gpu), test-soc (test-gpu), check (test-gpu).
#
#   make lint-gpu          Verilator -Wall and Icarus -g2005 of surf_vram, surf_video with graphics,
#                          and the SoC with HAS_GPU
#   make test-gpu          everything below, GPU_SEEDS seeds each
#   make test-gpu-model    the C model against hand-written expectations (docs/GPU.md, not the RTL)
#   make test-gpu-vram     surf_vram: CPU window, engine port and pixel port at once, against a copy
#   make test-gpu-video    surf_video with HAS_GFX: the old video tests again, then the graphics
#                          modes captured from the pins and compared with the model, FRAME_CRC too
#   make test-gpu-interconnect  the soc track's tb_interconnect with HAS_GPU = 1 (gpu and vram reach
#                          their targets; the HAS_GPU = 0 runs are test-soc-units' own)
#   make test-gpu-encodings  gpu.h and surf_gpu.v agree on every opcode, header bit, the ID and VRAM's size
#   make test-gpu-engine   surf_gpu with surf_vram against the C model: directed tests of every command
#                          and error, then randomized command streams, VRAM compared at every FENCE
#   make test-gpu-mutants  one-line bugs planted in copies of surf_gpu, surf_vram and surf_video must
#                          each fail their test (sim/gpu/mutants.py; not part of check)
#   make gpu-netlist-sim   the synthesized netlist of an open-flow gpudiag build (make build-fpga
#                          FLOW=openxc7 DESIGN=surfcpu_arty_a7_gpudiag) runs gpudiag from its pins to
#                          GPUDIAG: PASS, showing the build its manifest names (sim/board/netlist_sim.sh;
#                          about an hour; not part of check)
#   make test-gpu-soc      gpudiag (build/fw/gpudiag.mem) in surfsim built with HAS_GPU: GPUDIAG: PASS,
#                          every scene's FRAME_CRC equal to the model's and to a frame captured at
#                          the pins, the tree's build id on both (sim/gpu/gpudiag_check.py); SurfBoot's
#                          monitor reports features 1ff
#   make test-gpu-noengine gpudiag must fail without the engine: on the HAS_GPU = 0 SoC, and on a
#                          HAS_GPU = 1 SoC whose gpu.ID is not "GPU1"; gpudiag_check.py's own tests
#   make test-gpu-system-boot  SurfOS booting from a card on the DDR+GPU SoC (on check, not on test-gpu)
#   make build-gpu-rom     build/fw/gpudiag.{elf,bin,mem} (on build-rom) and the scenes' expected CRCs
#                          (build/fw/gen/gpuscenes_expect.h, from the C model); gpu-scenes writes
#                          each scene's frame as build/fw/gpuscenes/scene-NN.ppm
#
# GPU_SEEDS="1 2 3" chooses the seeds; GPU_JOBS the parallel C++ compiles per Verilator build.

GPU_BUILD    = $(BUILD)/sim/gpu
GPU_SEEDS   ?= 1 2 3
GPU_JOBS    ?= 2
GPU_CXX     ?= g++
GPU_CXXFLAGS = -std=c++17 -O2 -g -Wall -Wextra -I$(CURDIR)/sim/models -I$(CURDIR)/sim/io -I$(CURDIR)/sim/gpu \
               -I$(CURDIR)/firmware/include
GPU_VFLAGS   = -Wall -Wno-DECLFILENAME -Irtl/generated -Irtl/lib -Irtl/soc -Irtl/peripherals \
               --x-assign unique --x-initial unique
GPU_VERILATE = $(VERILATOR) --cc --exe --build -j $(GPU_JOBS) $(GPU_VFLAGS) \
               -CFLAGS "$(GPU_CXXFLAGS)" -LDFLAGS "-g"
GPU_MODEL    = sim/models/gpu_model.c
GPU_MODEL_H  = sim/models/gpu_model.h firmware/include/surfcpu/gpu.h

VRAM_RTL     = rtl/soc/surf_vram.v rtl/soc/surf_vram_bank.v
GPU_RTL      = rtl/peripherals/surf_gpu.v rtl/lib/surf_fifo.v
GVIDEO_RTL   = rtl/peripherals/surf_video.v rtl/peripherals/surf_video_font.v rtl/lib/surf_sync.v \
               rtl/lib/surf_pulse_sync.v rtl/lib/surf_cdc_word.v

.PHONY: lint-gpu test-gpu test-gpu-model test-gpu-vram test-gpu-video test-gpu-interconnect test-gpu-soc \
        build-gpu-rom gpu-scenes test-gpu-encodings test-gpu-engine test-gpu-mutants gpu-netlist-sim \
        test-gpu-noengine test-gpu-system-boot
lint: lint-gpu
build-rom: build-gpu-rom
test-soc: test-gpu
check: test-gpu

GPU_VLINT = $(VERILATOR_LINT) -Irtl/soc -Irtl/peripherals
GPU_SOC_RTL = rtl/soc/surfcpu_soc.v $(SOC_BUS_RTL) $(SOC_MEM_RTL) $(VRAM_RTL) $(wildcard rtl/peripherals/*.v) \
              $(wildcard rtl/lib/*.v) $(wildcard rtl/cpu/*.v)

lint-gpu:
	$(Q)$(GPU_VLINT) --top-module surf_vram $(VRAM_RTL)
	$(Q)$(GPU_VLINT) --top-module surf_gpu $(GPU_RTL)
	$(Q)$(IVERILOG) -g2005 -Wall -Irtl/generated -Irtl/lib -o /dev/null $(GPU_RTL)
	$(Q)for g in 0 1; do $(GPU_VLINT) --top-module surf_video -GHAS_GFX=$$g $(GVIDEO_RTL) || exit 1; done
	$(Q)for g in 0 1; do $(GPU_VLINT) --top-module surfcpu_soc -GHAS_GPU=$$g $(GPU_SOC_RTL) || exit 1; done
	$(Q)$(GPU_VLINT) --top-module surfcpu_soc -GHAS_GPU=1 -GHAS_DDR=0 $(GPU_SOC_RTL)
	$(Q)$(IVERILOG) -g2005 -Wall -Irtl/generated -Irtl/lib -Irtl/soc -Irtl/peripherals -Irtl/cpu -s surfcpu_soc \
	  -Psurfcpu_soc.HAS_GPU=1 -o /dev/null $(GPU_SOC_RTL)
	$(Q)$(BOARD_VLINT) $(TOP_INCS) -DARTY_HAS_GPU --top-module surfcpu_arty_a7_top $(TOP_SRC) $(BOARD_PRIMS)
	$(Q)$(BOARD_VLINT) $(TOP_INCS) -DARTY_HAS_GPU -DARTY_HAS_DDR --top-module surfcpu_arty_a7_top $(DDR_TOP_SRC) $(BOARD_PRIMS)
	@echo "lint-gpu: surf_vram, surf_gpu, surf_video (HAS_GFX 0 and 1), surfcpu_soc (HAS_GPU 0 and 1), the board top with ARTY_HAS_GPU (with and without DDR): Verilator -Wall clean, Icarus -g2005 compiles"

test-gpu: test-gpu-model test-gpu-encodings test-gpu-vram test-gpu-video test-gpu-interconnect test-gpu-engine \
          test-gpu-soc test-gpu-noengine
	@echo "test-gpu: PASS"

# run one binary per seed, keep each log, stop with it on the first failure:
# $(call gpu_run,<log name>,<binary and arguments before --seed>)
define gpu_run
	$(Q)for s in $(GPU_SEEDS); do \
	  $(2) --seed $$s > $(GPU_BUILD)/$(1).$$s.txt 2>&1; \
	  st=$$?; tail -1 $(GPU_BUILD)/$(1).$$s.txt; \
	  [ $$st -eq 0 ] || { tail -40 $(GPU_BUILD)/$(1).$$s.txt; exit 1; }; done
endef

# ---------------------------------------------------------------- the model on its own
$(GPU_BUILD)/test_gpu_model: sim/gpu/test_gpu_model.c $(GPU_MODEL) $(GPU_MODEL_H) firmware/include/surfcpu/gpu.h
	@mkdir -p $(dir $@)
	$(Q)gcc -std=c99 -O2 -g -Wall -Wextra -Werror -Isim/models -Ifirmware/include -o $@ sim/gpu/test_gpu_model.c $(GPU_MODEL)

test-gpu-model: $(GPU_BUILD)/test_gpu_model
	$(Q)$< > $(GPU_BUILD)/test_gpu_model.txt 2>&1; st=$$?; tail -1 $(GPU_BUILD)/test_gpu_model.txt; \
	  [ $$st -eq 0 ] || { tail -40 $(GPU_BUILD)/test_gpu_model.txt; exit 1; }

# ---------------------------------------------------------------- surf_vram
$(GPU_BUILD)/vram/Vsurf_vram: $(VRAM_RTL) sim/gpu/test_vram.cpp sim/io/surfbus_master.h
	@mkdir -p $(dir $@)
	$(Q)$(GPU_VERILATE) --top-module surf_vram -Mdir $(GPU_BUILD)/vram/obj -o ../Vsurf_vram \
	  $(VRAM_RTL) $(CURDIR)/sim/gpu/test_vram.cpp > $(GPU_BUILD)/vram/build.txt 2>&1 \
	  || { tail -30 $(GPU_BUILD)/vram/build.txt; exit 1; }

test-gpu-vram: $(GPU_BUILD)/vram/Vsurf_vram
	$(call gpu_run,test_vram,$< +verilator+rand+reset+2)

# ---------------------------------------------------------------- surf_video with graphics
# the io track's harness (sim/io/tb_video.v, test_video.cpp) built with HAS_GFX = 1: its own tests
# again (--gfx only moves GFX_BASE's offset out of its list of offsets without a register), then
# the graphics tests (sim/gpu/test_video_gfx.cpp) on the same build
GPU_VIDEO_DEPS = $(GVIDEO_RTL) sim/io/tb_video.v sim/io/surfbus_master.h sim/models/vga_capture.cpp \
                 sim/models/vga_capture.h $(GPU_MODEL) $(GPU_MODEL_H)

$(GPU_BUILD)/video/Vtb_video: $(GPU_VIDEO_DEPS) sim/io/test_video.cpp
	@mkdir -p $(dir $@)
	$(Q)$(GPU_VERILATE) --top-module tb_video -GHAS_GFX=1 -Mdir $(GPU_BUILD)/video/obj -o ../Vtb_video \
	  sim/io/tb_video.v $(CURDIR)/sim/io/test_video.cpp $(CURDIR)/sim/models/vga_capture.cpp \
	  > $(GPU_BUILD)/video/build.txt 2>&1 || { tail -30 $(GPU_BUILD)/video/build.txt; exit 1; }

$(GPU_BUILD)/video_gfx/Vtb_video: $(GPU_VIDEO_DEPS) sim/gpu/test_video_gfx.cpp
	@mkdir -p $(dir $@)
	$(Q)$(GPU_VERILATE) --top-module tb_video -GHAS_GFX=1 -Mdir $(GPU_BUILD)/video_gfx/obj -o ../Vtb_video \
	  sim/io/tb_video.v $(CURDIR)/sim/gpu/test_video_gfx.cpp $(CURDIR)/sim/models/vga_capture.cpp \
	  $(CURDIR)/$(GPU_MODEL) > $(GPU_BUILD)/video_gfx/build.txt 2>&1 \
	  || { tail -30 $(GPU_BUILD)/video_gfx/build.txt; exit 1; }

test-gpu-video: $(GPU_BUILD)/video/Vtb_video $(GPU_BUILD)/video_gfx/Vtb_video
	$(Q)for s in $(GPU_SEEDS); do \
	  $(GPU_BUILD)/video/Vtb_video --gfx --seed $$s --font $(CURDIR)/../driver/font8x16.c --out $(GPU_BUILD)/video \
	    +verilator+rand+reset+2 +verilator+seed+$$s > $(GPU_BUILD)/video/run.$$s.txt 2>&1; \
	  st=$$?; echo "  [old tests, HAS_GFX=1] $$(tail -1 $(GPU_BUILD)/video/run.$$s.txt)"; \
	  [ $$st -eq 0 ] || { tail -40 $(GPU_BUILD)/video/run.$$s.txt; exit 1; }; done
	$(call gpu_run,video_gfx/run,$(GPU_BUILD)/video_gfx/Vtb_video --font $(CURDIR)/../driver/font8x16.c \
	  --out $(GPU_BUILD)/video_gfx +verilator+rand+reset+2)

# ---------------------------------------------------------------- the interconnect with HAS_GPU
$(GPU_BUILD)/tb_interconnect_gpu.vvp: sim/soc/tb/tb_interconnect.v rtl/soc/surf_bus_targets.vh \
                                      $(wildcard rtl/soc/surf_*bus*.v) rtl/soc/surf_interconnect.v
	@mkdir -p $(dir $@)
	$(Q)$(IVERILOG) -g2005 -Wall -Wno-timescale -Irtl/generated -Irtl/lib -Irtl/soc -Irtl/peripherals -Irtl/cpu \
	  -s tb_interconnect -Ptb_interconnect.HAS_GPU=1 -o $@ sim/soc/tb/tb_interconnect.v \
	  $(SOC_BUS_RTL) $(SOC_BFM) sim/soc/bfm/surfbus_slave_model.v

test-gpu-interconnect: $(GPU_BUILD)/tb_interconnect_gpu.vvp
	$(Q)for s in $(GPU_SEEDS); do \
	  vvp -n $< +seed=$$s > $(GPU_BUILD)/tb_interconnect_gpu.$$s.txt 2>&1; \
	  grep -q "PASS" $(GPU_BUILD)/tb_interconnect_gpu.$$s.txt || { tail -30 $(GPU_BUILD)/tb_interconnect_gpu.$$s.txt; exit 1; }; \
	  grep -h "PASS" $(GPU_BUILD)/tb_interconnect_gpu.$$s.txt | sed "s/^/  [seed $$s] /"; done

# ---------------------------------------------------------------- gpudiag and its expected CRCs
GPUDIAG_SRC := diagnostics/gpudiag.c diagnostics/gpuscenes.c diagnostics/report.c diagnostics/probes.S
GPUDIAG_OBJ := $(FW_OBJ)/common/start_rom.o $(patsubst %,$(FW_OBJ)/%.o,$(GPUDIAG_SRC)) $(FW_COMMON_OBJ)
GPU_FONT    := ../driver/font8x16.c

$(FW)/host/gpuscenes_expect: firmware/tests/gpuscenes_expect.c firmware/diagnostics/gpuscenes.c \
                             firmware/diagnostics/gpuscenes.h $(GPU_MODEL) $(GPU_MODEL_H) firmware/include/surfcpu/soc.h
	@mkdir -p $(dir $@)
	$(Q)$(FW_HOST_CC) -std=gnu11 -O2 -g -Wall -Wextra -Werror -Isim/models -Ifirmware/include -Ifirmware/diagnostics \
	  -o $@ firmware/tests/gpuscenes_expect.c firmware/diagnostics/gpuscenes.c $(GPU_MODEL)

$(FW_GEN)/gpuscenes_expect.h: $(FW)/host/gpuscenes_expect $(GPU_FONT)
	@mkdir -p $(dir $@)
	$(Q)$< --font $(GPU_FONT) --out $@.tmp && mv $@.tmp $@

$(FW_OBJ)/diagnostics/gpudiag.c.o: $(FW_GEN)/gpuscenes_expect.h

$(FW)/gpudiag.elf: $(GPUDIAG_OBJ) firmware/diagnostics/gpudiag.ld $(FW_LD_DEPS)
	$(Q)$(RV_CC) $(FW_LDFLAGS) -T firmware/diagnostics/gpudiag.ld -Wl,-Map,$(FW)/gpudiag.map \
	  $(GPUDIAG_OBJ) $(RV_LIBGCC) -o $@.tmp
	$(Q)$(ISA_AUDIT) $@.tmp
	$(Q)mv $@.tmp $@

.SECONDARY: $(FW)/gpudiag.bin
build-gpu-rom: $(FW)/gpudiag.mem $(FW)/gpudiag.bin
	@$(RV_SIZE) $(FW)/gpudiag.elf | tail -n 1 | awk '{ printf "build-gpu-rom: gpudiag.elf ROM %u of 65536 bytes (%.1f%%)\n", $$1 + $$2, ($$1 + $$2) * 100 / 65536 }'

# gpudiag with GPUDIAG_TEST_LEFTOVER, for test-gpu-soc only: each run ends by leaving a target and a
# clip set, as a run abandoned inside an engine scene would (the third review, C4), for its rerun
GPUDIAG_LEFT_OBJ := $(subst $(FW_OBJ)/diagnostics/gpudiag.c.o,$(FW_OBJ)/diagnostics/gpudiag-leftover.c.o,$(GPUDIAG_OBJ))
$(FW_OBJ)/diagnostics/gpudiag-leftover.c.o: firmware/diagnostics/gpudiag.c $(FW_GEN)/gpuscenes_expect.h | $(FW_GEN)/fw_build.h
	@mkdir -p $(dir $@)
	$(Q)$(RV_CC) $(FW_CFLAGS) -DGPUDIAG_TEST_LEFTOVER -c $< -o $@

$(FW)/gpudiag-leftover.elf: $(GPUDIAG_LEFT_OBJ) firmware/diagnostics/gpudiag.ld $(FW_LD_DEPS)
	$(Q)$(RV_CC) $(FW_LDFLAGS) -T firmware/diagnostics/gpudiag.ld $(GPUDIAG_LEFT_OBJ) $(RV_LIBGCC) -o $@

.SECONDARY: $(FW)/gpudiag-leftover.bin

gpu-scenes: $(FW)/host/gpuscenes_expect
	@mkdir -p $(FW)/gpuscenes
	$(Q)$< --font $(GPU_FONT) --ppm $(FW)/gpuscenes --list

# ---------------------------------------------------------------- gpudiag on the RTL SoC
# surfsim (the soc track's harness, sim/soc/surfsim) built with HAS_GPU = 1 (HAS_DDR 1 as the
# deliverable surfsim, so FEATURES is 0x1ff): build/sim/surfsim_gpu. Its object directory is
# deleted once the binary is copied, as the soc track does.
GPU_SIM_DEPS = $(wildcard rtl/soc/*.v rtl/soc/*.vh rtl/peripherals/*.v rtl/cpu/*.v rtl/cpu/*.vh rtl/lib/*.v) \
               rtl/generated/surfcpu_soc.vh sim/soc/surfsim/surfsim.cpp $(wildcard sim/models/*.cpp sim/models/*.h)

$(BUILD)/sim/surfsim_gpu: $(GPU_SIM_DEPS)
	@mkdir -p $(BUILD)/sim/obj_surfsim_gpu
	$(Q)$(SOC_VERILATE) -GHAS_GPU=1 --Mdir $(BUILD)/sim/obj_surfsim_gpu -o surfsim \
	  $(SOC_TOP_RTL) $(VRAM_RTL) rtl/peripherals/surf_gpu.v $(SOC_CORE_SRC) $(SOC_IO_SRC) $(SOC_SIM_CPP) > $(BUILD)/sim/obj_surfsim_gpu.txt 2>&1 \
	  || { tail -40 $(BUILD)/sim/obj_surfsim_gpu.txt; exit 1; }
	$(Q)cp $(BUILD)/sim/obj_surfsim_gpu/surfsim $@ && rm -rf $(BUILD)/sim/obj_surfsim_gpu

GPU_ENGINE_SCENES ?= 6
test-gpu-soc: $(BUILD)/sim/surfsim_gpu build-gpu-rom $(FW)/surfboot.mem $(FW)/bramdiag.mem $(FW)/gpudiag-leftover.mem
	@mkdir -p $(GPU_BUILD)/soc
	@# gpudiag, then Ctrl-R (as GPU.md section 14 and tools/board-test.py's late port type it), then the
	@# rerun: the first run alone must pass, and the whole capture, whose final run is the rerun
	$(Q)$(BUILD)/sim/surfsim_gpu --rom $(FW)/gpudiag.mem --quiet --max-cycles 900000000 \
	  --vga-crc $(GPU_BUILD)/soc/frames.txt --transcript $(GPU_BUILD)/soc/uart.txt \
	  --send 'Ctrl-R runs the diagnostic again\.\r\n=\x12' \
	  --expect 'GPUDIAG: (PASS|FAIL)[^\n]*\n' --expect 'GPUDIAG: (PASS|FAIL)[^\n]*\n' > $(GPU_BUILD)/soc/surfsim.txt 2>&1; \
	  st=$$?; [ $$st -eq 0 ] || { tail -30 $(GPU_BUILD)/soc/surfsim.txt; tail -30 $(GPU_BUILD)/soc/uart.txt; exit 1; }
	$(Q)awk '{ print } /GPUDIAG: / { exit }' $(GPU_BUILD)/soc/uart.txt > $(GPU_BUILD)/soc/uart-first.txt
	$(Q)$(PYTHON) sim/gpu/gpudiag_check.py --uart $(GPU_BUILD)/soc/uart-first.txt --frames $(GPU_BUILD)/soc/frames.txt \
	  --expect $(FW_GEN)/gpuscenes_expect.h --log $(GPU_BUILD)/soc/surfsim.txt --engine-scenes $(GPU_ENGINE_SCENES) \
	  --build-id $(BUILD_ID_HEX)
	@printf '  [after Ctrl-R] '
	$(Q)$(PYTHON) sim/gpu/gpudiag_check.py --uart $(GPU_BUILD)/soc/uart.txt --frames $(GPU_BUILD)/soc/frames.txt \
	  --expect $(FW_GEN)/gpuscenes_expect.h --engine-scenes $(GPU_ENGINE_SCENES) --build-id $(BUILD_ID_HEX)
	$(Q)grep -q 'a rerun (Ctrl-R)' $(GPU_BUILD)/soc/uart.txt || { echo "test-gpu-soc: the rerun did not say it is one"; exit 1; }
	@# a rerun after a run that left the engine's target and clip set (gpudiag-leftover.mem), as one
	@# abandoned inside an engine scene would: the rerun sets its own, and passes (the third review, C4)
	$(Q)$(BUILD)/sim/surfsim_gpu --rom $(FW)/gpudiag-leftover.mem --quiet --max-cycles 900000000 \
	  --transcript $(GPU_BUILD)/soc/leftover-uart.txt --send 'Ctrl-R runs the diagnostic again\.\r\n=\x12' \
	  --expect 'GPUDIAG: (PASS|FAIL)[^\n]*\n' --expect 'GPUDIAG: (PASS|FAIL)[^\n]*\n' > $(GPU_BUILD)/soc/leftover.txt 2>&1; \
	  st=$$?; [ $$st -eq 0 ] || { tail -30 $(GPU_BUILD)/soc/leftover.txt; tail -30 $(GPU_BUILD)/soc/leftover-uart.txt; exit 1; }
	@printf '  [a rerun after a target and clip were left] '
	$(Q)$(PYTHON) sim/gpu/gpudiag_check.py --uart $(GPU_BUILD)/soc/leftover-uart.txt --frames - \
	  --expect $(FW_GEN)/gpuscenes_expect.h
	@# SurfBoot, the DDR+GPU design's boot ROM, on the same SoC: to its monitor, which answers help
	@# and reports the GPU among the features (info: features 1ff)
	$(Q)printf 'help\rinfo\r' > $(GPU_BUILD)/soc/help.txt
	$(Q)$(BUILD)/sim/surfsim_gpu --rom $(FW)/surfboot.mem --uart-in $(GPU_BUILD)/soc/help.txt --quiet \
	  --max-cycles 150000000 --expect 'SurfBoot [0-9][\s\S]*monitor commands[\s\S]*features 1ff' \
	  > $(GPU_BUILD)/soc/surfboot.txt 2>&1 || { tail -30 $(GPU_BUILD)/soc/surfboot.txt; exit 1; }
	@echo "  [surfboot, HAS_GPU] the monitor answered help and info (features 1ff): $$(grep -o 'output matched after [0-9]* cycles' $(GPU_BUILD)/soc/surfboot.txt)"
	@# the BRAM diagnostics on the same SoC: their unmapped-address probes (spec/soc.toml's
	@# unmapped_probe) still fault where VRAM now answers at 0x3000_0000; and their rerun, the
	@# Ctrl-R tools/board-test.py types when the port opened late, passes too
	$(Q)$(BUILD)/sim/surfsim_gpu --rom $(FW)/bramdiag.mem --quiet --max-cycles 800000000 \
	  --transcript $(GPU_BUILD)/soc/bramdiag-uart.txt --send 'Ctrl-R runs the diagnostics again\.\r\n=\x12' \
	  --expect 'BRAMDIAG: (PASS|FAIL)[^\n]*\n' --expect 'BRAMDIAG: (PASS|FAIL)[^\n]*\n' \
	  > $(GPU_BUILD)/soc/bramdiag.txt 2>&1; \
	  [ "$$(grep -c 'BRAMDIAG: PASS' $(GPU_BUILD)/soc/bramdiag-uart.txt)" = 2 ] || { tail -30 $(GPU_BUILD)/soc/bramdiag-uart.txt; exit 1; }
	@echo "  [bramdiag, HAS_GPU] $$(grep -o 'BRAMDIAG: PASS ([0-9]* checks)' $(GPU_BUILD)/soc/bramdiag-uart.txt | tr '\n' ' ')(the run, and its rerun after Ctrl-R)"

# ---------------------------------------------------------------- SurfOS on the DDR+GPU SoC
# The soc track's system boot test (sim/soc/tools/system_boot_test.py: SurfBoot loads the Arty
# kernel package from a card image, the kernel boots to its prompt and runs commands, a damaged
# package falls back to BACKUP.BIN) on build/sim/surfsim_gpu, the RTL SoC with HAS_GPU = 1 and
# HAS_DDR = 1: the simulated counterpart of GPU.md section 14 step 4. It costs what
# test-soc-system-boot costs, so it is on check (not on test-gpu). Skipped where the soc track's
# own system test is (SOC_SYS_READY, area-soc.mk, read when the recipe runs).
GPU_SYS_DIR = $(GPU_BUILD)/system_boot
check: test-gpu-system-boot
test-gpu-system-boot:
	@mkdir -p $(GPU_BUILD)
	$(Q)if [ -n "$(SOC_SYS_READY)" ]; then \
	  $(MAKE) --no-print-directory build-kernel-os $(FW)/surfboot.mem $(BUILD)/sim/surfsim_gpu > $(GPU_BUILD)/system-build.txt 2>&1 \
	    || { tail -20 $(GPU_BUILD)/system-build.txt; exit 1; }; \
	  $(PYTHON) sim/soc/tools/system_boot_test.py --surfsim $(BUILD)/sim/surfsim_gpu --rom $(FW)/surfboot.mem \
	    --mkcard tools/mkcard.py --package ../build/riscv32/arty_a7/SURFOS.BIN --work $(GPU_SYS_DIR) \
	    --evidence $(GPU_SYS_DIR)/summary.txt; \
	else echo "$@: skipped (needs the real core, SurfBoot, the riscv32 kernel, surfimg.py and mkcard.py)"; fi

# ---------------------------------------------------------------- gpudiag without the engine
# gpudiag must fail where the engine is missing (the first GPU review, finding 1): on the
# HAS_GPU = 0 SoC (build/sim/surfsim: no FEATURES.GPU, no gpu or vram window) and on a HAS_GPU = 1
# SoC whose engine answers gpu.ID 0x31555048 instead of "GPU1" (build/sim/surfsim_gpu_noengine,
# from a copy of surf_gpu.v with that line changed by sim/gpu/noengine.py). Each run must end in
# GPUDIAG: FAIL saying why, and gpudiag_check.py must fail it too (--must-fail).
GPU_NOENG = $(GPU_BUILD)/noengine
$(GPU_NOENG)/surf_gpu.v: rtl/peripherals/surf_gpu.v sim/gpu/noengine.py
	@mkdir -p $(dir $@)
	$(Q)$(PYTHON) sim/gpu/noengine.py $< $@

$(BUILD)/sim/surfsim_gpu_noengine: $(GPU_SIM_DEPS) $(GPU_NOENG)/surf_gpu.v
	@mkdir -p $(GPU_NOENG)/obj
	$(Q)$(SOC_VERILATE) -GHAS_GPU=1 --Mdir $(GPU_NOENG)/obj -o surfsim \
	  $(SOC_TOP_RTL) $(VRAM_RTL) $(GPU_NOENG)/surf_gpu.v $(SOC_CORE_SRC) $(SOC_IO_SRC) $(SOC_SIM_CPP) > $(GPU_NOENG)/build.txt 2>&1 \
	  || { tail -40 $(GPU_NOENG)/build.txt; exit 1; }
	$(Q)cp $(GPU_NOENG)/obj/surfsim $@ && rm -rf $(GPU_NOENG)/obj

# $(call gpu_noeng,<surfsim>,<name>,<max cycles>,<what the run must say>)
define gpu_noeng
	$(Q)$(1) --rom $(FW)/gpudiag.mem --quiet --max-cycles $(3) --transcript $(GPU_NOENG)/$(2).txt \
	  --expect 'GPUDIAG: (PASS|FAIL)[^\n]*\n' > $(GPU_NOENG)/$(2).log 2>&1 || { tail -20 $(GPU_NOENG)/$(2).log; exit 1; }
	@printf '  [%s] ' '$(2)'
	$(Q)$(PYTHON) sim/gpu/gpudiag_check.py --uart $(GPU_NOENG)/$(2).txt --frames - \
	  --expect $(FW_GEN)/gpuscenes_expect.h --must-fail '$(4)'
endef

test-gpu-noengine: $(BUILD)/sim/surfsim $(BUILD)/sim/surfsim_gpu_noengine build-gpu-rom
	$(Q)$(PYTHON) sim/gpu/test_gpudiag_check.py
	@mkdir -p $(GPU_NOENG)
	$(call gpu_noeng,$(BUILD)/sim/surfsim,no-gpu,100000000,FEATURES has no GPU[\s\S]*checks ran; a complete run makes)
	$(call gpu_noeng,$(BUILD)/sim/surfsim_gpu_noengine,wrong-id,600000000,no engine: gpu.ID reads 0x31555048[\s\S]*skipped: no engine[\s\S]*checks ran; a complete run makes)

# ---------------------------------------------------------------- the engine
test-gpu-encodings:
	$(Q)$(PYTHON) sim/gpu/check_gpu_h.py

$(GPU_BUILD)/engine/Vtb_gpu: $(GPU_RTL) $(VRAM_RTL) sim/gpu/tb_gpu.v sim/gpu/test_gpu.cpp sim/io/surfbus_master.h \
                             $(GPU_MODEL) $(GPU_MODEL_H)
	@mkdir -p $(dir $@)
	$(Q)$(GPU_VERILATE) -O3 --top-module tb_gpu -Mdir $(GPU_BUILD)/engine/obj -o ../Vtb_gpu \
	  sim/gpu/tb_gpu.v $(GPU_RTL) $(VRAM_RTL) $(CURDIR)/sim/gpu/test_gpu.cpp $(CURDIR)/$(GPU_MODEL) \
	  > $(GPU_BUILD)/engine/build.txt 2>&1 || { tail -30 $(GPU_BUILD)/engine/build.txt; exit 1; }

GPU_CHUNKS ?= 120
test-gpu-engine: $(GPU_BUILD)/engine/Vtb_gpu
	$(call gpu_run,engine/run,$< --chunks $(GPU_CHUNKS) +verilator+rand+reset+2)

test-gpu-mutants: build-gpu-rom
	$(Q)$(PYTHON) sim/gpu/mutants.py --work $(GPU_BUILD)/mutants

# ---------------------------------------------------------------- the synthesized netlist
GPU_NET_DIR = $(BUILD)/fpga/$(BOARD)/surfcpu_arty_a7_gpudiag/openxc7
gpu-netlist-sim:
	$(Q)NETLIST_MAX_MS=$(or $(NETLIST_MAX_MS),2500) sh sim/board/netlist_sim.sh $(GPU_NET_DIR) "GPUDIAG:" $(BOARD_NETLIST_SIM)
	$(Q)$(PYTHON) sim/gpu/gpudiag_check.py --uart build/sim/board/netlist/$(BOARD)-surfcpu_arty_a7_gpudiag/uart.txt --frames - \
	  --manifest $(GPU_NET_DIR)/../manifest.txt
