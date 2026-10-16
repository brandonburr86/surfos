# io track: microSD SPI, PS/2 and VGA text peripherals, their simulation models and tests
# (docs/IO.md). Hung on the aggregates: lint (lint-io), test-soc (test-io), check (check-io).
#
#   make lint-io          Verilator -Wall lint and an Icarus compile of every io module
#   make test-io          all io tests below, IO_SEEDS seeds each
#   make test-io-font     the font ROM is current and equals driver/font8x16.c
#   make test-io-models   the SD card, PS/2 keyboard and VGA capture models on their own
#   make test-io-spi      surf_spi against the SD card model through SurfBus
#   make test-io-ps2      surf_ps2 against the PS/2 keyboard model through SurfBus
#   make test-io-video    surf_video measured and decoded by the VGA capture model
#   make test-io-icarus   the three blocks together in Icarus (SPI, PS/2, video timing)
#   make synth-io         Yosys synth_xilinx estimates of the three blocks (not part of check)
#   make evidence-io      rerun everything on IO_EVIDENCE_SEEDS and rewrite docs/evidence/io/
#
# IO_SEEDS="1 2 3" chooses the seeds; IO_JOBS the parallel C++ compiles per Verilator build.

IO_PERIPH    = rtl/peripherals
IO_TOPS      = surf_spi surf_ps2 surf_video
IO_RTL       = $(foreach m,$(IO_TOPS),$(IO_PERIPH)/$(m).v) \
               $(wildcard $(IO_PERIPH)/surf_spi_*.v $(IO_PERIPH)/surf_ps2_*.v $(IO_PERIPH)/surf_video_*.v)
IO_BUILD     = $(BUILD)/sim/io
IO_SEEDS    ?= 1 2 3
IO_EVIDENCE_SEEDS ?= 1 2 3 4 5
IO_JOBS     ?= 2
IO_CXX      ?= g++
IO_CXXFLAGS  = -std=c++17 -O2 -g -Wall -Wextra -I$(CURDIR)/sim/models -I$(CURDIR)/sim/io -I$(CURDIR)/firmware/include
IO_VFLAGS    = -Wall -Wno-DECLFILENAME -Irtl/generated -Irtl/lib -I$(IO_PERIPH) \
               --x-assign unique --x-initial unique
IO_VERILATE  = $(VERILATOR) --cc --exe --build -j $(IO_JOBS) $(IO_VFLAGS) \
               -CFLAGS "$(IO_CXXFLAGS)" -LDFLAGS "-g"

SD_MODEL_SRC = sim/models/sdcard_model.cpp
SD_MODEL_HDR = sim/models/sdcard_model.h sim/io/sd_host.h
KB_MODEL_SRC = sim/models/ps2_keyboard_model.cpp
KB_MODEL_HDR = sim/models/ps2_keyboard_model.h
VGA_MODEL_SRC = sim/models/vga_capture.cpp
VGA_MODEL_HDR = sim/models/vga_capture.h

.PHONY: lint-io check-io test-io test-io-font test-io-models test-io-spi test-io-ps2 test-io-video test-io-icarus \
        synth-io evidence-io
lint: lint-io
test-soc: test-io
check: check-io

lint-io:
	$(Q)for m in $(IO_TOPS); do \
	  $(VERILATOR_LINT) -I$(IO_PERIPH) --top-module $$m $(IO_PERIPH)/$$m.v || exit 1; \
	  $(IVERILOG) -g2005 -Wall -Irtl/generated -y rtl/lib -y $(IO_PERIPH) \
	    -o /dev/null $(IO_PERIPH)/$$m.v || exit 1; done
	@echo "lint-io: $(IO_TOPS): Verilator -Wall clean, Icarus -g2005 compiles"

# the font ROM is part of check: its sources are pinned and it must match the x86 font
check-io: test-io-font

test-io: test-io-font test-io-models test-io-spi test-io-ps2 test-io-video test-io-icarus
	@echo "test-io: PASS"

test-io-font:
	$(Q)$(PYTHON) tools/fontrom.py check

# ---------------------------------------------------------------- models on their own
$(IO_BUILD)/test_sdcard_model: sim/io/test_sdcard_model.cpp $(SD_MODEL_SRC) $(SD_MODEL_HDR)
	@mkdir -p $(dir $@)
	$(Q)$(IO_CXX) $(IO_CXXFLAGS) -o $@ sim/io/test_sdcard_model.cpp $(SD_MODEL_SRC)

$(IO_BUILD)/test_ps2_model: sim/io/test_ps2_model.cpp $(KB_MODEL_SRC) $(KB_MODEL_HDR)
	@mkdir -p $(dir $@)
	$(Q)$(IO_CXX) $(IO_CXXFLAGS) -o $@ sim/io/test_ps2_model.cpp $(KB_MODEL_SRC)

$(IO_BUILD)/test_vga_capture: sim/io/test_vga_capture.cpp $(VGA_MODEL_SRC) $(VGA_MODEL_HDR)
	@mkdir -p $(dir $@)
	$(Q)$(IO_CXX) $(IO_CXXFLAGS) -o $@ sim/io/test_vga_capture.cpp $(VGA_MODEL_SRC)

# run one test binary per seed: $(call io_run,name,binary and arguments before the seed)
define io_run
	$(Q)for s in $(IO_SEEDS); do \
	  $(2) $$s > $(IO_BUILD)/$(1).$$s.txt 2>&1; \
	  st=$$?; tail -1 $(IO_BUILD)/$(1).$$s.txt; \
	  [ $$st -eq 0 ] || { cat $(IO_BUILD)/$(1).$$s.txt; exit 1; }; done
endef

test-io-models: $(IO_BUILD)/test_sdcard_model $(IO_BUILD)/test_ps2_model $(IO_BUILD)/test_vga_capture
	@mkdir -p $(IO_BUILD)/work
	$(call io_run,test_sdcard_model,$(IO_BUILD)/test_sdcard_model $(IO_BUILD)/work)
	$(call io_run,test_ps2_model,$(IO_BUILD)/test_ps2_model)
	$(call io_run,test_vga_capture,$(IO_BUILD)/test_vga_capture $(CURDIR)/../driver/font8x16.c $(IO_BUILD)/work)

# ---------------------------------------------------------------- surf_spi
$(IO_BUILD)/spi/Vsurf_spi: $(IO_PERIPH)/surf_spi.v sim/io/test_spi.cpp sim/io/surfbus_master.h \
                           $(SD_MODEL_SRC) $(SD_MODEL_HDR) rtl/lib/surf_sync.v
	@mkdir -p $(dir $@)
	$(Q)$(IO_VERILATE) --top-module surf_spi -Mdir $(IO_BUILD)/spi/obj -o ../Vsurf_spi \
	  $(IO_PERIPH)/surf_spi.v $(CURDIR)/sim/io/test_spi.cpp $(CURDIR)/$(SD_MODEL_SRC) > $(IO_BUILD)/spi/build.txt 2>&1 \
	  || { cat $(IO_BUILD)/spi/build.txt; exit 1; }

test-io-spi: $(IO_BUILD)/spi/Vsurf_spi
	@mkdir -p $(IO_BUILD)/work
	$(Q)for s in $(IO_SEEDS); do \
	  $< --work $(IO_BUILD)/work --seed $$s +verilator+rand+reset+2 +verilator+seed+$$s \
	    > $(IO_BUILD)/spi/run.$$s.txt 2>&1; \
	  st=$$?; tail -1 $(IO_BUILD)/spi/run.$$s.txt; \
	  [ $$st -eq 0 ] || { cat $(IO_BUILD)/spi/run.$$s.txt; exit 1; }; done

# ---------------------------------------------------------------- surf_ps2
$(IO_BUILD)/ps2/Vsurf_ps2: $(IO_PERIPH)/surf_ps2.v sim/io/test_ps2.cpp sim/io/surfbus_master.h \
                           $(KB_MODEL_SRC) $(KB_MODEL_HDR) rtl/lib/surf_sync.v rtl/lib/surf_fifo.v
	@mkdir -p $(dir $@)
	$(Q)$(IO_VERILATE) --top-module surf_ps2 -Mdir $(IO_BUILD)/ps2/obj -o ../Vsurf_ps2 \
	  $(IO_PERIPH)/surf_ps2.v $(CURDIR)/sim/io/test_ps2.cpp $(CURDIR)/$(KB_MODEL_SRC) > $(IO_BUILD)/ps2/build.txt 2>&1 \
	  || { cat $(IO_BUILD)/ps2/build.txt; exit 1; }

test-io-ps2: $(IO_BUILD)/ps2/Vsurf_ps2
	$(Q)for s in $(IO_SEEDS); do \
	  $< --seed $$s +verilator+rand+reset+2 +verilator+seed+$$s > $(IO_BUILD)/ps2/run.$$s.txt 2>&1; \
	  st=$$?; tail -1 $(IO_BUILD)/ps2/run.$$s.txt; \
	  [ $$st -eq 0 ] || { cat $(IO_BUILD)/ps2/run.$$s.txt; exit 1; }; done

# ---------------------------------------------------------------- surf_video
VIDEO_RTL = $(IO_PERIPH)/surf_video.v $(IO_PERIPH)/surf_video_font.v rtl/lib/surf_sync.v \
            rtl/lib/surf_pulse_sync.v rtl/lib/surf_cdc_word.v

$(IO_BUILD)/video/Vtb_video: $(VIDEO_RTL) sim/io/tb_video.v sim/io/test_video.cpp sim/io/surfbus_master.h \
                             $(VGA_MODEL_SRC) $(VGA_MODEL_HDR)
	@mkdir -p $(dir $@)
	$(Q)$(IO_VERILATE) --top-module tb_video -Mdir $(IO_BUILD)/video/obj -o ../Vtb_video \
	  sim/io/tb_video.v $(CURDIR)/sim/io/test_video.cpp $(CURDIR)/$(VGA_MODEL_SRC) > $(IO_BUILD)/video/build.txt 2>&1 \
	  || { cat $(IO_BUILD)/video/build.txt; exit 1; }

test-io-video: $(IO_BUILD)/video/Vtb_video
	$(Q)for s in $(IO_SEEDS); do \
	  $< --seed $$s --font $(CURDIR)/../driver/font8x16.c --out $(IO_BUILD)/video \
	    +verilator+rand+reset+2 +verilator+seed+$$s > $(IO_BUILD)/video/run.$$s.txt 2>&1; \
	  st=$$?; tail -1 $(IO_BUILD)/video/run.$$s.txt; \
	  [ $$st -eq 0 ] || { cat $(IO_BUILD)/video/run.$$s.txt; exit 1; }; done

# ---------------------------------------------------------------- Icarus
IO_ALL_RTL = $(IO_PERIPH)/surf_spi.v $(IO_PERIPH)/surf_ps2.v $(VIDEO_RTL) rtl/lib/surf_fifo.v

$(IO_BUILD)/tb_io_icarus.vvp: sim/io/tb_io_icarus.v $(IO_ALL_RTL)
	@mkdir -p $(dir $@)
	$(Q)$(IVERILOG) -g2005 -Wall -Wno-timescale -Irtl/generated -y rtl/lib -y $(IO_PERIPH) -o $@ sim/io/tb_io_icarus.v

test-io-icarus: $(IO_BUILD)/tb_io_icarus.vvp
	$(Q)vvp -n $< > $(IO_BUILD)/tb_io_icarus.txt 2>&1; st=$$?; \
	  if [ $$st -eq 0 ] && grep PASS $(IO_BUILD)/tb_io_icarus.txt; then :; else cat $(IO_BUILD)/tb_io_icarus.txt; exit 1; fi

# ---------------------------------------------------------------- resource estimates
# Yosys synth_xilinx (flattened, xc7) per block; an estimate, not a Vivado result. Yosys's
# notes about resizing the font ROM's RAMB36 data ports (it uses 8 of 32 bits) are left out.
synth-io:
	@mkdir -p $(IO_BUILD)/synth
	$(Q)for m in $(IO_TOPS); do \
	  $(YOSYS) -q -l $(IO_BUILD)/synth/$$m.txt -p "read_verilog -Irtl/generated rtl/lib/surf_sync.v \
	    rtl/lib/surf_fifo.v rtl/lib/surf_pulse_sync.v rtl/lib/surf_cdc_word.v $(IO_PERIPH)/surf_video_font.v \
	    $(IO_PERIPH)/$$m.v; synth_xilinx -top $$m -family xc7 -flatten; tee -q -o $(IO_BUILD)/synth/$$m.stat.txt stat" \
	    > $(IO_BUILD)/synth/$$m.out 2>&1 || { cat $(IO_BUILD)/synth/$$m.out; exit 1; }; \
	  grep -v 'Resizing cell port .*u_font.rom' $(IO_BUILD)/synth/$$m.out; \
	  awk -v m=$$m '/ LUT[1-6] /{l+=$$2} / FD[RSCP]E /{f+=$$2} / RAMB18E1 /{b18+=$$2} / RAMB36E1 /{b36+=$$2} \
	    / DSP48E1 /{d+=$$2} / CARRY4 /{c+=$$2} / RAM(32|64)[MX]/{lr+=$$2} \
	    END{printf "%-11s %5d LUTs %5d flip-flops %3d LUTRAM %2d RAMB36 %2d RAMB18 %2d DSP48 %3d CARRY4\n", \
	        m, l, f, lr, b36, b18, d, c}' $(IO_BUILD)/synth/$$m.stat.txt; done

# ---------------------------------------------------------------- evidence
evidence-io:
	@mkdir -p $(IO_BUILD)/synth
	$(Q)$(MAKE) --no-print-directory lint-io test-io-font > $(IO_BUILD)/lint.txt
	$(Q)$(MAKE) --no-print-directory test-io-models test-io-spi test-io-ps2 test-io-video test-io-icarus \
	  IO_SEEDS="$(IO_EVIDENCE_SEEDS)"
	$(Q)$(MAKE) --no-print-directory synth-io > $(IO_BUILD)/synth/summary.txt
	$(Q)IO_SEEDS="$(IO_EVIDENCE_SEEDS)" BUILD=$(BUILD) sh sim/io/evidence.sh
