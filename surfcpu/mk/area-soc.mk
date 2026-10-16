# soc track: the SurfBus interconnect, the memories, the base peripherals, the SoC with its
# stand-ins, the C++ models and the surfsim harness (docs/SOC.md).
#
#   make lint-soc            Verilator -Wall of every soc module, the SoC (both HAS_DDR) and the
#                            stand-ins, plus an Icarus -g2005 compile of all of it
#   make test-soc-units      peripheral and memory testbenches (Icarus), several seeds
#
# SOC_SEEDS picks the seeds of the randomized tests.

SOC_DIR     := $(BUILD)/soc
SOC_SEEDS   ?= 1 7 123
SOC_INC     := -Irtl/generated -Irtl/lib -Irtl/soc -Irtl/cpu -Irtl/peripherals

SOC_LIB_RTL := rtl/lib/surf_fifo.v rtl/lib/surf_sync.v
SOC_BUS_RTL := $(addprefix rtl/soc/,surf_bus_err.v surf_bus_demux.v surf_ibus_decoder.v \
                 surf_dbus_decoder.v surf_bus_arbiter.v surf_ddr_gate.v surf_interconnect.v)
SOC_MEM_RTL := $(addprefix rtl/soc/,surf_bootrom.v surf_scratch.v surf_textram.v)
SOC_PER_RTL := $(addprefix rtl/peripherals/,surf_uart.v surf_timer.v surf_intc.v surf_gpio.v \
                 surf_sysinfo.v)
SOC_BFM     := sim/soc/bfm/surfbus_bfm.v sim/soc/bfm/surfbus_monitor.v

# The core and the io blocks: the real ones as soon as their files exist, the stand-ins under
# sim/soc/stubs/ until then. The SoC's interconnect tests always use the scripted stand-in.
SOC_CORE_SCRIPT := sim/soc/stubs/surfcpu_core.v sim/soc/bfm/surf_script_port.v
SOC_CORE_SRC    := $(if $(wildcard rtl/cpu/surfcpu_core.v),$(wildcard rtl/cpu/*.v),$(SOC_CORE_SCRIPT))
SOC_IO_SRC      := $(foreach b,spi ps2 video,$(if $(wildcard rtl/peripherals/surf_$(b).v),\
                     $(wildcard rtl/peripherals/surf_$(b)*.v),sim/soc/stubs/surf_$(b).v))
SOC_TOP_RTL     := rtl/soc/surfcpu_soc.v $(SOC_BUS_RTL) $(SOC_MEM_RTL) $(SOC_PER_RTL) \
                   $(wildcard rtl/lib/*.v)

SOC_IVERILOG = $(IVERILOG) -g2005 -Wall -Wno-timescale $(SOC_INC)
SOC_VLINT    = $(VERILATOR_LINT) -Irtl/soc -Irtl/peripherals

.PHONY: lint-soc test-soc-units test-soc-system test-soc-models soc-gpio-real soc-mem-once
lint: lint-soc
test-soc: test-soc-units test-soc-system test-soc-models

# $(call soc_run,<vvp file>,<log name>,<extra plusargs>): run once per seed, keep the logs,
# stop with the log on the first failure
define soc_run
	$(Q)for s in $(SOC_SEEDS); do \
	  vvp -n $(1) +seed=$$s $(3) > $(SOC_DIR)/$(2).$$s.txt 2>&1; \
	  grep -q "PASS" $(SOC_DIR)/$(2).$$s.txt || { cat $(SOC_DIR)/$(2).$$s.txt; exit 1; }; \
	  grep -h "PASS" $(SOC_DIR)/$(2).$$s.txt | sed "s/^/  [seed $$s] /"; done
endef

# ---------------------------------------------------------------------------- lint
lint-soc:
	$(Q)for f in $(SOC_BUS_RTL) $(SOC_MEM_RTL); do \
	  $(SOC_VLINT) --top-module $$(basename $$f .v) $(SOC_BUS_RTL) $(SOC_MEM_RTL) || exit 1; done
	$(Q)$(SOC_VLINT) --top-module surf_interconnect -GHAS_DDR=0 $(SOC_BUS_RTL)
	$(Q)for f in $(SOC_PER_RTL); do \
	  $(SOC_VLINT) --top-module $$(basename $$f .v) $$f $(SOC_LIB_RTL) || exit 1; done
	$(Q)$(SOC_IVERILOG) -o /dev/null $(SOC_BUS_RTL) $(SOC_MEM_RTL) $(SOC_PER_RTL) $(SOC_LIB_RTL)
	$(Q)$(SOC_VLINT) --top-module surfcpu_soc $(SOC_TOP_RTL) $(SOC_CORE_SRC) $(SOC_IO_SRC)
	$(Q)$(SOC_VLINT) --top-module surfcpu_soc -GHAS_DDR=0 $(SOC_TOP_RTL) $(SOC_CORE_SRC) $(SOC_IO_SRC)
	$(Q)$(SOC_VLINT) --top-module surfcpu_soc -DSURFCPU_RVFI $(SOC_TOP_RTL) $(SOC_CORE_SRC) $(SOC_IO_SRC)
	$(Q)$(SOC_VLINT) --top-module surfcpu_soc $(SOC_TOP_RTL) $(SOC_CORE_SCRIPT) $(SOC_IO_SRC)
	$(Q)for b in spi ps2 video; do $(SOC_VLINT) --top-module surf_$$b sim/soc/stubs/surf_$$b.v || exit 1; done
	$(Q)$(SOC_IVERILOG) -s surfcpu_soc -o /dev/null $(SOC_TOP_RTL) $(SOC_CORE_SRC) $(SOC_IO_SRC)
	$(Q)$(SOC_IVERILOG) -s surfcpu_soc -DSURFCPU_RVFI -o /dev/null $(SOC_TOP_RTL) $(SOC_CORE_SRC) $(SOC_IO_SRC)
	@echo "lint-soc: $(words $(sort $(SOC_TOP_RTL))) RTL files, the SoC and the stand-ins clean (Verilator -Wall, Icarus -g2005); core: $(firstword $(SOC_CORE_SRC)), io: $(SOC_IO_SRC)"

# ---------------------------------------------------------------------------- unit tests
$(SOC_DIR)/tb_intc.vvp: sim/soc/tb/tb_intc.v rtl/peripherals/surf_intc.v $(SOC_BFM)
	@mkdir -p $(dir $@)
	$(Q)$(SOC_IVERILOG) -s tb_intc -o $@ $^

$(SOC_DIR)/tb_sysinfo.vvp: sim/soc/tb/tb_sysinfo.v rtl/peripherals/surf_sysinfo.v $(SOC_BFM)
	@mkdir -p $(dir $@)
	$(Q)$(SOC_IVERILOG) -s tb_sysinfo -o $@ $^

$(SOC_DIR)/tb_sysinfo_noddr.vvp: sim/soc/tb/tb_sysinfo.v rtl/peripherals/surf_sysinfo.v $(SOC_BFM)
	@mkdir -p $(dir $@)
	$(Q)$(SOC_IVERILOG) -s tb_sysinfo -Ptb_sysinfo.DDR=0 -o $@ $^

$(SOC_DIR)/tb_timer.vvp: sim/soc/tb/tb_timer.v rtl/peripherals/surf_timer.v $(SOC_BFM)
	@mkdir -p $(dir $@)
	$(Q)$(SOC_IVERILOG) -s tb_timer -o $@ $^

$(SOC_DIR)/tb_timer_frac.vvp: sim/soc/tb/tb_timer.v rtl/peripherals/surf_timer.v $(SOC_BFM)
	@mkdir -p $(dir $@)
	$(Q)$(SOC_IVERILOG) -s tb_timer -Ptb_timer.SYS_HZ=33333333 -o $@ $^

$(SOC_DIR)/tb_uart.vvp: sim/soc/tb/tb_uart.v rtl/peripherals/surf_uart.v $(SOC_LIB_RTL) $(SOC_BFM)
	@mkdir -p $(dir $@)
	$(Q)$(SOC_IVERILOG) -s tb_uart -o $@ $^

$(SOC_DIR)/tb_gpio.vvp: sim/soc/tb/tb_gpio.v rtl/peripherals/surf_gpio.v rtl/lib/surf_sync.v $(SOC_BFM)
	@mkdir -p $(dir $@)
	$(Q)$(SOC_IVERILOG) -s tb_gpio -o $@ $^

$(SOC_DIR)/tb_gpio_real.vvp: sim/soc/tb/tb_gpio.v rtl/peripherals/surf_gpio.v rtl/lib/surf_sync.v $(SOC_BFM)
	@mkdir -p $(dir $@)
	$(Q)$(SOC_IVERILOG) -s tb_gpio -Ptb_gpio.SYS_HZ=25000000 -o $@ $^

# test ROM images: two different patterns and an empty one
$(SOC_DIR)/rom_a.mem $(SOC_DIR)/rom_b.mem: sim/soc/tools/mkrom.py spec/soc.toml
	@mkdir -p $(dir $@)
	$(Q)$(PYTHON) sim/soc/tools/mkrom.py $(SOC_DIR)/rom_a.mem --seed 1
	$(Q)$(PYTHON) sim/soc/tools/mkrom.py $(SOC_DIR)/rom_b.mem --seed 2

SOC_MEM_TB_SRC = sim/soc/tb/tb_mem.v $(SOC_MEM_RTL) $(SOC_BFM)

$(SOC_DIR)/tb_mem.vvp: $(SOC_MEM_TB_SRC) $(SOC_DIR)/rom_a.mem
	@mkdir -p $(dir $@)
	$(Q)$(SOC_IVERILOG) -s tb_mem -Ptb_mem.ROMFILE='"$(SOC_DIR)/rom_a.mem"' -o $@ $(SOC_MEM_TB_SRC)

$(SOC_DIR)/tb_mem_empty.vvp: $(SOC_MEM_TB_SRC)
	@mkdir -p $(dir $@)
	$(Q)$(SOC_IVERILOG) -s tb_mem -o $@ $(SOC_MEM_TB_SRC)

SOC_IC_TB_SRC = sim/soc/tb/tb_interconnect.v $(SOC_BUS_RTL) $(SOC_BFM) sim/soc/bfm/surfbus_slave_model.v

$(SOC_DIR)/tb_interconnect.vvp: $(SOC_IC_TB_SRC)
	@mkdir -p $(dir $@)
	$(Q)$(SOC_IVERILOG) -s tb_interconnect -o $@ $(SOC_IC_TB_SRC)

$(SOC_DIR)/tb_interconnect_noddr.vvp: $(SOC_IC_TB_SRC)
	@mkdir -p $(dir $@)
	$(Q)$(SOC_IVERILOG) -s tb_interconnect -Ptb_interconnect.HAS_DDR=0 -o $@ $(SOC_IC_TB_SRC)

# ---------------------------------------------------------------------------- the SoC, scripted
SOC_SCRIPTS  := soc_regions soc_errors soc_random soc_periph
SOC_TB_DEFS  := --define build_id=0x1234ABCD --define build_flags=2 --define build_time=0x67200000
SOC_TB_SRC    = sim/soc/tb/tb_soc.v $(SOC_TOP_RTL) $(SOC_CORE_SCRIPT) $(SOC_IO_SRC) $(SOC_BFM) \
                sim/soc/bfm/surfbus_ddr_model.v
SOC_SCRIPTC   = $(PYTHON) sim/soc/tools/surfscript.py

$(SOC_DIR)/tb_soc.vvp: $(SOC_TB_SRC) $(SOC_DIR)/rom_a.mem
	@mkdir -p $(dir $@)
	$(Q)$(SOC_IVERILOG) -s tb_soc -Ptb_soc.ROM='"$(SOC_DIR)/rom_a.mem"' -o $@ $(SOC_TB_SRC)

$(SOC_DIR)/tb_soc_noddr.vvp: $(SOC_TB_SRC) $(SOC_DIR)/rom_a.mem
	@mkdir -p $(dir $@)
	$(Q)$(SOC_IVERILOG) -s tb_soc -Ptb_soc.ROM='"$(SOC_DIR)/rom_a.mem"' -Ptb_soc.HAS_DDR=0 -o $@ \
	  $(SOC_TB_SRC)

$(SOC_DIR)/scripts/%.ddr.hex: sim/soc/scripts/%.sbs $(SOC_DIR)/rom_a.mem sim/soc/tools/surfscript.py
	@mkdir -p $(dir $@)
	$(Q)$(SOC_SCRIPTC) $< -o $@ --rom $(SOC_DIR)/rom_a.mem --define ddr=1 $(SOC_TB_DEFS) \
	  --listing $(@:.hex=.lst) > /dev/null

$(SOC_DIR)/scripts/%.noddr.hex: sim/soc/scripts/%.sbs $(SOC_DIR)/rom_a.mem sim/soc/tools/surfscript.py
	@mkdir -p $(dir $@)
	$(Q)$(SOC_SCRIPTC) $< -o $@ --rom $(SOC_DIR)/rom_a.mem --define ddr=0 $(SOC_TB_DEFS) \
	  --listing $(@:.hex=.lst) > /dev/null

# $(call soc_script,<vvp>,<script hex>,<log name>,<extra plusargs>): every seed; passes when the
# stand-in says SCRIPT PASS and nothing printed ERROR or FAIL
define soc_script
	$(Q)for s in $(SOC_SEEDS); do \
	  vvp -n $(1) +script=$(2) +script_nofinish +seed=$$s +ddr_seed=$$s $$(cat $(2:.hex=.args)) $(4) \
	    > $(SOC_DIR)/$(3).$$s.txt 2>&1; \
	  if ! grep -q "SCRIPT PASS" $(SOC_DIR)/$(3).$$s.txt || grep -q "ERROR\|FAIL" $(SOC_DIR)/$(3).$$s.txt; \
	  then cat $(SOC_DIR)/$(3).$$s.txt; exit 1; fi; \
	  grep -h "SCRIPT PASS" $(SOC_DIR)/$(3).$$s.txt | sed "s/^/  [$(3) seed $$s] /"; done

endef

SOC_SYS_DEPS = $(SOC_DIR)/tb_soc.vvp $(SOC_DIR)/tb_soc_noddr.vvp \
               $(foreach t,$(SOC_SCRIPTS),$(SOC_DIR)/scripts/$(t).ddr.hex $(SOC_DIR)/scripts/$(t).noddr.hex) \
               $(SOC_DIR)/scripts/soc_negative.ddr.hex

test-soc-system: $(SOC_SYS_DEPS)
	$(foreach t,$(SOC_SCRIPTS),$(call soc_script,$(SOC_DIR)/tb_soc.vvp,$(SOC_DIR)/scripts/$(t).ddr.hex,tb_soc_$(t)))
	$(call soc_script,$(SOC_DIR)/tb_soc.vvp,$(SOC_DIR)/scripts/soc_random.ddr.hex,tb_soc_random_b2b,+gap=0)
	$(foreach t,$(SOC_SCRIPTS),$(call soc_script,$(SOC_DIR)/tb_soc_noddr.vvp,$(SOC_DIR)/scripts/$(t).noddr.hex,tb_soc_noddr_$(t)))
	$(Q)vvp -n $(SOC_DIR)/tb_soc.vvp +script=$(SOC_DIR)/scripts/soc_negative.ddr.hex +script_nofinish \
	  > $(SOC_DIR)/tb_soc_negative.txt 2>&1; \
	  grep -q "SCRIPT FAIL (2 data-port and 1 fetch-port checks failed)" $(SOC_DIR)/tb_soc_negative.txt \
	  || { cat $(SOC_DIR)/tb_soc_negative.txt; echo "the negative script was not caught"; exit 1; }
	@echo "  [tb_soc_negative] the three wrong expectations were reported"
	@echo "test-soc-system: PASS (seeds $(SOC_SEEDS); core: scripted stand-in; io: $(SOC_IO_SRC))"

SOC_UNIT_VVP = $(addprefix $(SOC_DIR)/,tb_interconnect.vvp tb_interconnect_noddr.vvp tb_intc.vvp tb_sysinfo.vvp tb_sysinfo_noddr.vvp \
                 tb_timer.vvp tb_timer_frac.vvp tb_uart.vvp tb_gpio.vvp tb_gpio_real.vvp \
                 tb_mem.vvp tb_mem_empty.vvp rom_b.mem)

test-soc-units: $(SOC_UNIT_VVP)
	$(call soc_run,$(SOC_DIR)/tb_intc.vvp,tb_intc)
	$(call soc_run,$(SOC_DIR)/tb_sysinfo.vvp,tb_sysinfo)
	$(call soc_run,$(SOC_DIR)/tb_sysinfo_noddr.vvp,tb_sysinfo_noddr)
	$(call soc_run,$(SOC_DIR)/tb_timer.vvp,tb_timer)
	$(call soc_run,$(SOC_DIR)/tb_timer_frac.vvp,tb_timer_frac)
	$(call soc_run,$(SOC_DIR)/tb_uart.vvp,tb_uart)
	$(call soc_run,$(SOC_DIR)/tb_gpio.vvp,tb_gpio)
	$(call soc_run,$(SOC_DIR)/tb_mem.vvp,tb_mem)
	$(call soc_run,$(SOC_DIR)/tb_interconnect.vvp,tb_interconnect)
	$(call soc_run,$(SOC_DIR)/tb_interconnect_noddr.vvp,tb_interconnect_noddr)
	$(Q)$(MAKE) --no-print-directory SOC_SEEDS=1 soc-gpio-real soc-mem-once
	@echo "test-soc-units: PASS (seeds $(SOC_SEEDS))"

# the GPIO test at the real 25 MHz (10 ms debounce = 250,000 cycles): one seed is enough
soc-gpio-real: $(SOC_DIR)/tb_gpio_real.vvp
	$(call soc_run,$(SOC_DIR)/tb_gpio_real.vvp,tb_gpio_real)

# the memories with +bootrom= overriding BOOTROM_INIT, and with no ROM image (all zero)
soc-mem-once: $(SOC_DIR)/tb_mem.vvp $(SOC_DIR)/tb_mem_empty.vvp $(SOC_DIR)/rom_b.mem
	$(call soc_run,$(SOC_DIR)/tb_mem.vvp,tb_mem_plusarg,+bootrom=$(SOC_DIR)/rom_b.mem)
	$(call soc_run,$(SOC_DIR)/tb_mem_empty.vvp,tb_mem_empty)

# ---------------------------------------------------------------------------- C++ models
SOC_MODEL_SRC := sim/models/uart_model.cpp sim/models/ddr_model.cpp
SOC_MODEL_HDR := sim/models/uart_model.h sim/models/ddr_model.h
SOC_CXX        = g++ -std=c++17 -O2 -Wall -Wextra -Werror -Isim/models -Ifirmware/include

$(SOC_DIR)/test_models: sim/models/test_models.cpp $(SOC_MODEL_SRC) $(SOC_MODEL_HDR)
	@mkdir -p $(dir $@)
	$(Q)$(SOC_CXX) -o $@ sim/models/test_models.cpp $(SOC_MODEL_SRC)

test-soc-models: $(SOC_DIR)/test_models
	$(Q)for s in $(SOC_SEEDS); do $(SOC_DIR)/test_models $$s || exit 1; done
