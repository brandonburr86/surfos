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
#   make test-gpu-soc      gpudiag (build/fw/gpudiag.mem) in surfsim built with HAS_GPU: GPUDIAG: PASS,
#                          every scene's FRAME_CRC equal to the model's and to a frame captured at
#                          the pins (sim/gpu/gpudiag_check.py)
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
GVIDEO_RTL   = rtl/peripherals/surf_video.v rtl/peripherals/surf_video_font.v rtl/lib/surf_sync.v \
               rtl/lib/surf_pulse_sync.v rtl/lib/surf_cdc_word.v

.PHONY: lint-gpu test-gpu test-gpu-model test-gpu-vram test-gpu-video test-gpu-interconnect test-gpu-soc \
        build-gpu-rom gpu-scenes
lint: lint-gpu
build-rom: build-gpu-rom
test-soc: test-gpu
check: test-gpu

GPU_VLINT = $(VERILATOR_LINT) -Irtl/soc -Irtl/peripherals
GPU_SOC_RTL = rtl/soc/surfcpu_soc.v $(SOC_BUS_RTL) $(SOC_MEM_RTL) $(VRAM_RTL) $(wildcard rtl/peripherals/*.v) \
              $(wildcard rtl/lib/*.v) $(wildcard rtl/cpu/*.v)

lint-gpu:
	$(Q)$(GPU_VLINT) --top-module surf_vram $(VRAM_RTL)
	$(Q)for g in 0 1; do $(GPU_VLINT) --top-module surf_video -GHAS_GFX=$$g $(GVIDEO_RTL) || exit 1; done
	$(Q)for g in 0 1; do $(GPU_VLINT) --top-module surfcpu_soc -GHAS_GPU=$$g $(GPU_SOC_RTL) || exit 1; done
	$(Q)$(GPU_VLINT) --top-module surfcpu_soc -GHAS_GPU=1 -GHAS_DDR=0 $(GPU_SOC_RTL)
	$(Q)$(IVERILOG) -g2005 -Wall -Irtl/generated -Irtl/lib -Irtl/soc -Irtl/peripherals -Irtl/cpu -s surfcpu_soc \
	  -Psurfcpu_soc.HAS_GPU=1 -o /dev/null $(GPU_SOC_RTL)
	$(Q)$(BOARD_VLINT) $(TOP_INCS) -DARTY_HAS_GPU --top-module surfcpu_arty_a7_top $(TOP_SRC) $(BOARD_PRIMS)
	$(Q)$(BOARD_VLINT) $(TOP_INCS) -DARTY_HAS_GPU -DARTY_HAS_DDR --top-module surfcpu_arty_a7_top $(DDR_TOP_SRC) $(BOARD_PRIMS)
	@echo "lint-gpu: surf_vram, surf_video (HAS_GFX 0 and 1), surfcpu_soc (HAS_GPU 0 and 1), the board top with ARTY_HAS_GPU (with and without DDR): Verilator -Wall clean, Icarus -g2005 compiles"

test-gpu: test-gpu-model test-gpu-vram test-gpu-video test-gpu-interconnect test-gpu-soc
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
	  $(SOC_TOP_RTL) $(VRAM_RTL) $(SOC_CORE_SRC) $(SOC_IO_SRC) $(SOC_SIM_CPP) > $(BUILD)/sim/obj_surfsim_gpu.txt 2>&1 \
	  || { tail -40 $(BUILD)/sim/obj_surfsim_gpu.txt; exit 1; }
	$(Q)cp $(BUILD)/sim/obj_surfsim_gpu/surfsim $@ && rm -rf $(BUILD)/sim/obj_surfsim_gpu

test-gpu-soc: $(BUILD)/sim/surfsim_gpu build-gpu-rom
	@mkdir -p $(GPU_BUILD)/soc
	$(Q)$(BUILD)/sim/surfsim_gpu --rom $(FW)/gpudiag.mem --quiet --max-cycles 600000000 \
	  --vga-crc $(GPU_BUILD)/soc/frames.txt --transcript $(GPU_BUILD)/soc/uart.txt \
	  --expect 'GPUDIAG: (PASS|FAIL)[^\n]*\n' > $(GPU_BUILD)/soc/surfsim.txt 2>&1; \
	  st=$$?; [ $$st -eq 0 ] || { tail -30 $(GPU_BUILD)/soc/surfsim.txt; tail -30 $(GPU_BUILD)/soc/uart.txt; exit 1; }
	$(Q)$(PYTHON) sim/gpu/gpudiag_check.py --uart $(GPU_BUILD)/soc/uart.txt --frames $(GPU_BUILD)/soc/frames.txt \
	  --expect $(FW_GEN)/gpuscenes_expect.h --log $(GPU_BUILD)/soc/surfsim.txt
