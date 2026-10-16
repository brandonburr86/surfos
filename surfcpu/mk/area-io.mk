# io track: microSD SPI, PS/2 and VGA text peripherals, their simulation models and tests
# (docs/IO.md). Hung on the aggregates: lint (lint-io), test-soc (test-io), check (check-io).
#
#   make lint-io          Verilator -Wall lint and an Icarus compile of every io module
#   make test-io          all io tests below, IO_SEEDS seeds each
#   make test-io-font     the font ROM is current and equals driver/font8x16.c
#   make test-io-models   the SD card model on its own (software SPI master)
#   make test-io-spi      surf_spi against the SD card model through SurfBus
#   make test-io-ps2      surf_ps2 against the PS/2 keyboard model through SurfBus
#   make test-io-video    surf_video measured and decoded by the VGA capture model
#
# IO_SEEDS="1 2 3" chooses the seeds; IO_JOBS the parallel C++ compiles per Verilator build.

IO_PERIPH    = rtl/peripherals
IO_TOPS      = surf_spi surf_ps2 surf_video
IO_RTL       = $(foreach m,$(IO_TOPS),$(IO_PERIPH)/$(m).v) \
               $(wildcard $(IO_PERIPH)/surf_spi_*.v $(IO_PERIPH)/surf_ps2_*.v $(IO_PERIPH)/surf_video_*.v)
IO_BUILD     = $(BUILD)/sim/io
IO_SEEDS    ?= 1 2 3
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

.PHONY: lint-io check-io test-io test-io-font test-io-models test-io-spi test-io-ps2 test-io-video
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

test-io: test-io-font test-io-models test-io-spi test-io-ps2 test-io-video
	@echo "test-io: PASS"

test-io-font:
	$(Q)$(PYTHON) tools/fontrom.py check

# ---------------------------------------------------------------- models on their own
$(IO_BUILD)/test_sdcard_model: sim/io/test_sdcard_model.cpp $(SD_MODEL_SRC) $(SD_MODEL_HDR)
	@mkdir -p $(dir $@)
	$(Q)$(IO_CXX) $(IO_CXXFLAGS) -o $@ sim/io/test_sdcard_model.cpp $(SD_MODEL_SRC)

test-io-models: $(IO_BUILD)/test_sdcard_model
	@mkdir -p $(IO_BUILD)/work
	$(Q)for s in $(IO_SEEDS); do \
	  $(IO_BUILD)/test_sdcard_model $(IO_BUILD)/work $$s > $(IO_BUILD)/test_sdcard_model.$$s.txt 2>&1; \
	  st=$$?; tail -1 $(IO_BUILD)/test_sdcard_model.$$s.txt; \
	  [ $$st -eq 0 ] || { cat $(IO_BUILD)/test_sdcard_model.$$s.txt; exit 1; }; done

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
