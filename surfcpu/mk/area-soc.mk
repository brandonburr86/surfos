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

.PHONY: lint-soc test-soc-units test-soc-system test-soc-models test-soc-surfsim surfsim \
        synth-soc test-soc-verilator test-soc-gate test-soc-boot soc-gpio-real soc-mem-once
lint: lint-soc
test-soc: test-soc-units test-soc-system test-soc-models test-soc-surfsim test-soc-verilator \
          test-soc-boot
check: synth-soc test-soc-gate

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

# ---------------------------------------------------------------------------- surfsim
# build/sim/surfsim: the whole SoC (the real core and io blocks once they exist) with the C++
# models; build/soc/surfsim_script/surfsim: the same harness around the scripted stand-in, for
# the tests. Built with RVFI so --trace works with the real core.
SOC_BUILD_FLAGS := $(shell echo $$(( 2 + ($(SURFCPU_BUILD_DIRTY) & 1) )))
SOC_SIM_DEFS    := --define build_id=0x$(BUILD_ID_HEX) --define build_flags=$(SOC_BUILD_FLAGS) \
                   --define build_time=0
SOC_VERILATE     = $(VERILATOR) --cc --exe --build -j 2 -O2 --top-module surfcpu_soc \
                   +define+SURFCPU_RVFI $(SOC_INC) \
                   -GBUILD_ID=32\'h$(BUILD_ID_HEX) -GBUILD_FLAGS=32\'h$(SOC_BUILD_FLAGS) \
                   -CFLAGS "-std=c++17 -O2 -DSURFCPU_RVFI -I$(CURDIR)/sim/models \
                   -I$(CURDIR)/firmware/include"
SOC_SIM_CPP     := sim/soc/surfsim/surfsim.cpp $(SOC_MODEL_SRC)

# the build identity is compiled into surfsim (sysinfo) and into the scripts that check it:
# a stamp per identity rebuilds both together when the commit or the dirty state changes
SOC_ID_STAMP    := $(SOC_DIR)/build-id.$(BUILD_ID_HEX).$(SOC_BUILD_FLAGS).stamp
$(SOC_ID_STAMP):
	@mkdir -p $(SOC_DIR)
	@rm -f $(SOC_DIR)/build-id.*.stamp
	@touch $@

$(BUILD)/sim/surfsim: $(SOC_TOP_RTL) $(SOC_CORE_SRC) $(SOC_IO_SRC) $(SOC_SIM_CPP) $(SOC_MODEL_HDR) \
                     $(SOC_ID_STAMP)
	@mkdir -p $(BUILD)/sim/obj_surfsim
	$(Q)$(SOC_VERILATE) --Mdir $(BUILD)/sim/obj_surfsim -o surfsim \
	  $(SOC_TOP_RTL) $(SOC_CORE_SRC) $(SOC_IO_SRC) $(SOC_SIM_CPP) > $(BUILD)/sim/obj_surfsim.txt 2>&1 \
	  || { tail -40 $(BUILD)/sim/obj_surfsim.txt; exit 1; }
	$(Q)cp $(BUILD)/sim/obj_surfsim/surfsim $@

$(SOC_DIR)/surfsim_script/surfsim: $(SOC_TOP_RTL) $(SOC_CORE_SCRIPT) $(SOC_IO_SRC) $(SOC_SIM_CPP) \
                                   $(SOC_MODEL_HDR) $(SOC_ID_STAMP)
	@mkdir -p $(SOC_DIR)/surfsim_script/obj
	$(Q)$(SOC_VERILATE) --Mdir $(SOC_DIR)/surfsim_script/obj -o surfsim \
	  $(SOC_TOP_RTL) $(SOC_CORE_SCRIPT) $(SOC_IO_SRC) $(SOC_SIM_CPP) \
	  > $(SOC_DIR)/surfsim_script/build.txt 2>&1 || { tail -40 $(SOC_DIR)/surfsim_script/build.txt; exit 1; }
	$(Q)cp $(SOC_DIR)/surfsim_script/obj/surfsim $@

surfsim: $(BUILD)/sim/surfsim

$(SOC_DIR)/scripts/%.sim.hex: sim/soc/scripts/%.sbs $(SOC_DIR)/rom_a.mem sim/soc/tools/surfscript.py \
                              $(SOC_ID_STAMP)
	@mkdir -p $(dir $@)
	$(Q)$(SOC_SCRIPTC) $< -o $@ --rom $(SOC_DIR)/rom_a.mem --define ddr=1 $(SOC_SIM_DEFS) \
	  --listing $(@:.hex=.lst) > /dev/null

SOC_SIMX = $(SOC_DIR)/surfsim_script/surfsim
SOC_SIM_TESTS := soc_regions soc_errors soc_random soc_periph

# $(call soc_simrun,<script>,<log>,<extra args>): every seed, exit 0 and SCRIPT PASS
define soc_simrun
	$(Q)for s in $(SOC_SEEDS); do \
	  $(SOC_SIMX) --rom $(SOC_DIR)/rom_a.mem --uart-loopback --gpio-loopback --quiet --seed $$s \
	    --max-cycles 3000000 +script=$(SOC_DIR)/scripts/$(1).sim.hex +seed=$$s \
	    $$(cat $(SOC_DIR)/scripts/$(1).sim.args) $(3) > $(SOC_DIR)/$(2).$$s.txt 2>&1 \
	    && grep -q "SCRIPT PASS" $(SOC_DIR)/$(2).$$s.txt \
	    || { cat $(SOC_DIR)/$(2).$$s.txt; exit 1; }; \
	  grep -h "SCRIPT PASS" $(SOC_DIR)/$(2).$$s.txt | sed "s/^/  [$(2) seed $$s] /"; done

endef

test-soc-surfsim: $(BUILD)/sim/surfsim $(SOC_SIMX) $(SOC_DIR)/rom_a.mem \
                  $(foreach t,$(SOC_SIM_TESTS) sim_uart soc_negative,$(SOC_DIR)/scripts/$(t).sim.hex)
	$(foreach t,$(SOC_SIM_TESTS),$(call soc_simrun,$(t),surfsim_$(t)))
	$(call soc_simrun,soc_random,surfsim_random_b2b,+gap=0)
	@# the UART model on the real pins: in from --uart-in, out matched by --expect
	$(Q)printf 'surf!' > $(SOC_DIR)/surfsim_uart_in.txt
	$(Q)$(SOC_SIMX) --rom $(SOC_DIR)/rom_a.mem --uart-in $(SOC_DIR)/surfsim_uart_in.txt \
	  --expect 'HI\n\[surf!\]\n' --max-cycles 400000 +script=$(SOC_DIR)/scripts/sim_uart.sim.hex \
	  > $(SOC_DIR)/surfsim_uart.txt 2>&1 || { cat $(SOC_DIR)/surfsim_uart.txt; exit 1; }
	$(Q)grep -q "output matched" $(SOC_DIR)/surfsim_uart.txt || { cat $(SOC_DIR)/surfsim_uart.txt; exit 1; }
	@echo "  [surfsim_uart] the SoC said HI and echoed the model's bytes: $$(tail -1 $(SOC_DIR)/surfsim_uart.txt)"
	@# exit codes: a failing script is 1, an expectation never met is 2
	$(Q)$(SOC_SIMX) --rom $(SOC_DIR)/rom_a.mem --quiet --max-cycles 100000 \
	  +script=$(SOC_DIR)/scripts/soc_negative.sim.hex > $(SOC_DIR)/surfsim_negative.txt 2>&1; \
	  test $$? -eq 1 || { cat $(SOC_DIR)/surfsim_negative.txt; echo "expected exit 1"; exit 1; }
	$(Q)$(SOC_SIMX) --quiet --max-cycles 5000 --expect 'never printed' \
	  > $(SOC_DIR)/surfsim_noexpect.txt 2>&1; \
	  test $$? -eq 2 || { cat $(SOC_DIR)/surfsim_noexpect.txt; echo "expected exit 2"; exit 1; }
	@echo "  [surfsim exit codes] failing script 1, unmet --expect 2"
	@# the deliverable build runs (with the stand-in it idles; with the real core it runs the ROM)
	$(Q)$(BUILD)/sim/surfsim --rom $(SOC_DIR)/rom_a.mem --quiet --max-cycles 20000 \
	  --trace $(SOC_DIR)/surfsim_smoke.trace > $(SOC_DIR)/surfsim_smoke.txt 2>&1 \
	  || { cat $(SOC_DIR)/surfsim_smoke.txt; exit 1; }
	$(Q)head -1 $(SOC_DIR)/surfsim_smoke.trace | grep -q "^# surfcpu-trace v1: order pc insn" \
	  || { echo "bad trace header"; exit 1; }
	@echo "  [build/sim/surfsim] $$(grep -h '^surfsim:' $(SOC_DIR)/surfsim_smoke.txt)"
	@echo "test-soc-surfsim: PASS (seeds $(SOC_SEEDS))"

# ---------------------------------------------------------------------------- synthesis check
# Every soc RTL module through Yosys: the logic modules with the generic synth (check -assert),
# the three memories elaborated as memories (memory -nomap, check -assert: no flip-flop arrays),
# then the whole SoC through synth_xilinx with the boot ROM image, the core and the io blocks
# as black boxes while they are stand-ins (the real modules as soon as they exist). That shows
# the RTL synthesizes without vendor primitives, the memories become block RAM, and gives the
# LUT/FF/BRAM estimate in build/soc/syn/soc_stat.txt.
SOC_SYN_DIR  := $(SOC_DIR)/syn
SOC_SYN_RTL   = $(SOC_BUS_RTL) $(SOC_MEM_RTL) $(SOC_PER_RTL) rtl/soc/surfcpu_soc.v $(SOC_LIB_RTL)
SOC_SYN_LOGIC = $(basename $(notdir $(SOC_BUS_RTL) $(SOC_PER_RTL)))
SOC_SYN_MEMS  = $(basename $(notdir $(SOC_MEM_RTL)))
SOC_SYN_READ  = read_verilog -DSYNTHESIS $(SOC_INC)
SOC_SYN_BB    = $(filter sim/soc/stubs/%,$(SOC_CORE_SRC) $(SOC_IO_SRC))
SOC_SYN_REAL  = $(filter-out sim/%,$(SOC_CORE_SRC) $(SOC_IO_SRC))

synth-soc: $(SOC_DIR)/rom_a.mem
	@mkdir -p $(SOC_SYN_DIR)
	$(Q)for m in $(SOC_SYN_LOGIC); do \
	  $(YOSYS) -q -l $(SOC_SYN_DIR)/$$m.log -p "$(SOC_SYN_READ) $(SOC_SYN_RTL); \
	    synth -flatten -top $$m; check -assert; tee -q -o $(SOC_SYN_DIR)/$$m.stat stat" \
	    > /dev/null || { tail -20 $(SOC_SYN_DIR)/$$m.log; exit 1; }; done
	$(Q)for m in $(SOC_SYN_MEMS); do \
	  $(YOSYS) -q -l $(SOC_SYN_DIR)/$$m.log -p "$(SOC_SYN_READ) $(SOC_SYN_RTL); \
	    chparam -set INIT_FILE \"$(SOC_DIR)/rom_a.mem\" surf_bootrom; \
	    prep -flatten -top $$m; memory -nomap; check -assert; tee -q -o $(SOC_SYN_DIR)/$$m.stat stat" \
	    > /dev/null || { tail -20 $(SOC_SYN_DIR)/$$m.log; exit 1; }; \
	  grep -q 'mem_v2' $(SOC_SYN_DIR)/$$m.stat || { echo "$$m: no memory inferred"; exit 1; }; done
	$(Q)printf '%s\n' \
	  "$(if $(SOC_SYN_BB),read_verilog -lib -DSYNTHESIS $(SOC_INC) $(SOC_SYN_BB))" \
	  "$(SOC_SYN_READ) $(SOC_SYN_RTL) $(SOC_SYN_REAL) $(filter-out $(SOC_LIB_RTL),$(wildcard rtl/lib/*.v))" \
	  "chparam -set BOOTROM_INIT \"$(SOC_DIR)/rom_a.mem\" surfcpu_soc" \
	  "synth_xilinx -top surfcpu_soc -flatten -noiopad -noclkbuf" \
	  "check -assert" \
	  "tee -q -o $(SOC_SYN_DIR)/soc_stat.txt stat" > $(SOC_SYN_DIR)/soc.ys
	$(Q)$(YOSYS) -qq -l $(SOC_SYN_DIR)/soc.log $(SOC_SYN_DIR)/soc.ys > $(SOC_SYN_DIR)/soc.out 2>&1 \
	  || { tail -30 $(SOC_SYN_DIR)/soc.log; exit 1; }
	@echo "synth-soc: $$(grep '^Warning: ' $(SOC_SYN_DIR)/soc.log | grep -vc 'Resizing cell port') warnings besides block-RAM port resizing (see $(SOC_SYN_DIR)/soc.log)"
	@echo "synth-soc: $(words $(SOC_SYN_LOGIC)) logic modules synthesize and $(words $(SOC_SYN_MEMS)) memories infer (Yosys, check -assert)"
	@echo "synth-soc: surfcpu_soc with synth_xilinx (black boxes: $(or $(SOC_SYN_BB),none)):"
	@grep -E "^ +(LUT[1-6]|FD[CPRS]E|RAMB(18|36)E1|RAM(32|64|128)[MXD1]*|DSP48E1|CARRY4|SRL16E|SRLC32E|MUXF[78]) " \
	  $(SOC_SYN_DIR)/soc_stat.txt | sed 's/^ */    /'
	@grep -h "mapping memory" $(SOC_SYN_DIR)/soc.log | sed 's/^/    /'

# ---------------------------------------------------------------------------- post-synthesis simulation
# Yosys can drop logic silently, so the synthesized netlists are simulated: tb_mem on the three
# memories synthesized alone, and the SoC scripts on the whole synthesized SoC (the stand-ins
# supplied as RTL for the black boxes; the netlist has the parameters tb_soc uses). The cell
# models are Yosys's cells_sim.v, except RAMB36E1, which is only a timing shell there and comes
# from sim/soc/synth/surf_ramb36e1_model.v. Needs make synth-soc first.
SOC_GATE_DIR  := $(SOC_DIR)/gate
SOC_CELLS_SIM := $(SOC_GATE_DIR)/cells_sim_nobram.v
SOC_GATE_LIB   = $(SOC_CELLS_SIM) sim/soc/synth/surf_ramb36e1_model.v
SOC_GATE_IVL   = $(IVERILOG) -g2012 -Wno-timescale -Wno-portbind -DSURF_GATE $(SOC_INC)

$(SOC_CELLS_SIM): sim/soc/synth/strip_cells.py
	@mkdir -p $(dir $@)
	$(Q)$(PYTHON) sim/soc/synth/strip_cells.py $$($(YOSYS)-config --datdir 2>/dev/null || \
	  echo /usr/share/yosys)/xilinx/cells_sim.v $@

$(SOC_GATE_DIR)/%_net.v: rtl/soc/%.v $(SOC_DIR)/rom_a.mem
	@mkdir -p $(dir $@)
	$(Q)$(YOSYS) -qq -l $(@:.v=.log) -p "read_verilog -DSYNTHESIS $(SOC_INC) $<; \
	  chparam -set INIT_FILE \"$(SOC_DIR)/rom_a.mem\" surf_bootrom; \
	  synth_xilinx -top $* -flatten -noiopad -noclkbuf; write_verilog -noattr $@" > /dev/null 2>&1 \
	  || { tail -20 $(@:.v=.log); exit 1; }

SOC_GATE_MEMS = $(addprefix $(SOC_GATE_DIR)/,surf_bootrom_net.v surf_scratch_net.v surf_textram_net.v)

$(SOC_GATE_DIR)/tb_mem_gate.vvp: sim/soc/tb/tb_mem.v $(SOC_GATE_MEMS) $(SOC_GATE_LIB) $(SOC_BFM)
	$(Q)$(SOC_GATE_IVL) -s tb_mem -Ptb_mem.ROMFILE='"$(SOC_DIR)/rom_a.mem"' -o $@ sim/soc/tb/tb_mem.v \
	  $(SOC_GATE_MEMS) $(SOC_GATE_LIB) $(SOC_BFM)

$(SOC_GATE_DIR)/tb_soc_gate.vvp: sim/soc/tb/tb_soc.v $(SOC_SYN_DIR)/surfcpu_soc_net.v $(SOC_GATE_LIB) \
                                 $(SOC_CORE_SCRIPT) $(SOC_IO_SRC) $(SOC_BFM) sim/soc/bfm/surfbus_ddr_model.v
	@mkdir -p $(dir $@)
	$(Q)$(SOC_GATE_IVL) -s tb_soc -o $@ sim/soc/tb/tb_soc.v $(SOC_SYN_DIR)/surfcpu_soc_net.v \
	  $(SOC_GATE_LIB) $(SOC_CORE_SCRIPT) $(filter sim/%,$(SOC_IO_SRC)) $(SOC_BFM) \
	  sim/soc/bfm/surfbus_ddr_model.v

# the SoC netlist for the scripts: the core is always the scripted stand-in (a black box here,
# its RTL at simulation), the io blocks real once they exist, tb_soc's parameter values
SOC_GATE_BB   = $(SOC_CORE_SCRIPT) $(filter sim/%,$(SOC_IO_SRC))
$(SOC_SYN_DIR)/surfcpu_soc_net.v: $(SOC_SYN_RTL) $(SOC_IO_SRC) $(SOC_DIR)/rom_a.mem
	@mkdir -p $(dir $@)
	$(Q)printf '%s\n' \
	  "read_verilog -lib -DSYNTHESIS $(SOC_INC) $(filter sim/soc/stubs/%,$(SOC_GATE_BB))" \
	  "$(SOC_SYN_READ) $(SOC_SYN_RTL) $(filter-out sim/%,$(SOC_IO_SRC)) $(filter-out $(SOC_LIB_RTL),$(wildcard rtl/lib/*.v))" \
	  "chparam -set BOOTROM_INIT \"$(SOC_DIR)/rom_a.mem\" surfcpu_soc" \
	  "chparam -set BUILD_ID 32'h1234ABCD -set BUILD_FLAGS 2 -set BUILD_TIME 32'h67200000 surfcpu_soc" \
	  "synth_xilinx -top surfcpu_soc -flatten -noiopad -noclkbuf" \
	  "check -assert" \
	  "write_verilog -noattr $@" > $(SOC_SYN_DIR)/soc_net.ys
	$(Q)$(YOSYS) -qq -l $(SOC_SYN_DIR)/soc_net.log $(SOC_SYN_DIR)/soc_net.ys > $(SOC_SYN_DIR)/soc_net.out 2>&1 \
	  || { tail -30 $(SOC_SYN_DIR)/soc_net.log; exit 1; }

test-soc-gate: $(SOC_GATE_DIR)/tb_mem_gate.vvp $(SOC_GATE_DIR)/tb_soc_gate.vvp \
               $(foreach t,$(SOC_SCRIPTS),$(SOC_DIR)/scripts/$(t).ddr.hex)
	$(Q)vvp -n $(SOC_GATE_DIR)/tb_mem_gate.vvp +seed=1 > $(SOC_GATE_DIR)/tb_mem_gate.txt 2>&1; \
	  grep -q PASS $(SOC_GATE_DIR)/tb_mem_gate.txt || { cat $(SOC_GATE_DIR)/tb_mem_gate.txt; exit 1; }
	@grep -h PASS $(SOC_GATE_DIR)/tb_mem_gate.txt | sed 's/^/  [netlist] /'
	$(Q)for t in $(SOC_SCRIPTS); do \
	  vvp -n $(SOC_GATE_DIR)/tb_soc_gate.vvp +script=$(SOC_DIR)/scripts/$$t.ddr.hex +script_nofinish \
	    +seed=1 +ddr_seed=1 $$(cat $(SOC_DIR)/scripts/$$t.ddr.args) > $(SOC_GATE_DIR)/$$t.txt 2>&1; \
	  if ! grep -q "SCRIPT PASS" $(SOC_GATE_DIR)/$$t.txt || grep -q "ERROR\|FAIL" $(SOC_GATE_DIR)/$$t.txt; \
	  then cat $(SOC_GATE_DIR)/$$t.txt; exit 1; fi; \
	  grep -h "SCRIPT PASS" $(SOC_GATE_DIR)/$$t.txt | sed "s/^/  [netlist $$t] /"; done
	@echo "test-soc-gate: PASS (the synthesized memories and SoC behave as the RTL)"

# ---------------------------------------------------------------------------- second simulator
# The unit testbenches again under Verilator (--binary --timing): two-state instead of
# four-state, another scheduler, and other random sequences (the order in which a simulator
# evaluates several $random arguments is its own). One seed each.
SOC_VL      = $(VERILATOR) --binary --timing -j 2 -O1 -Wno-lint -Wno-style $(SOC_INC)
SOC_VL_DIR := $(SOC_DIR)/vl

# $(call soc_vl,<name>,<top>,<sources>,<extra verilator args>,<plusargs>)
define soc_vl
	@mkdir -p $(SOC_VL_DIR)/$(1)
	$(Q)$(SOC_VL) --top-module $(2) --Mdir $(SOC_VL_DIR)/$(1) -o $(1) $(4) $(3) \
	  > $(SOC_VL_DIR)/$(1).build.txt 2>&1 || { tail -30 $(SOC_VL_DIR)/$(1).build.txt; exit 1; }
	$(Q)$(SOC_VL_DIR)/$(1)/$(1) +seed=1 $(5) > $(SOC_VL_DIR)/$(1).txt 2>&1; \
	  grep -q "PASS" $(SOC_VL_DIR)/$(1).txt || { cat $(SOC_VL_DIR)/$(1).txt; exit 1; }
	@grep -h "PASS" $(SOC_VL_DIR)/$(1).txt | sed "s/^/  [verilator] /"

endef

test-soc-verilator: $(SOC_DIR)/rom_a.mem $(SOC_DIR)/rom_b.mem
	$(call soc_vl,tb_intc,tb_intc,sim/soc/tb/tb_intc.v rtl/peripherals/surf_intc.v $(SOC_BFM))
	$(call soc_vl,tb_sysinfo,tb_sysinfo,sim/soc/tb/tb_sysinfo.v rtl/peripherals/surf_sysinfo.v $(SOC_BFM))
	$(call soc_vl,tb_sysinfo_noddr,tb_sysinfo,sim/soc/tb/tb_sysinfo.v rtl/peripherals/surf_sysinfo.v $(SOC_BFM),-GDDR=0)
	$(call soc_vl,tb_timer,tb_timer,sim/soc/tb/tb_timer.v rtl/peripherals/surf_timer.v $(SOC_BFM))
	$(call soc_vl,tb_timer_frac,tb_timer,sim/soc/tb/tb_timer.v rtl/peripherals/surf_timer.v $(SOC_BFM),-GSYS_HZ=33333333)
	$(call soc_vl,tb_uart,tb_uart,sim/soc/tb/tb_uart.v rtl/peripherals/surf_uart.v $(SOC_LIB_RTL) $(SOC_BFM))
	$(call soc_vl,tb_gpio,tb_gpio,sim/soc/tb/tb_gpio.v rtl/peripherals/surf_gpio.v rtl/lib/surf_sync.v $(SOC_BFM))
	$(call soc_vl,tb_mem,tb_mem,$(SOC_MEM_TB_SRC),-GROMFILE='"$(SOC_DIR)/rom_a.mem"',+bootrom=$(SOC_DIR)/rom_b.mem)
	$(call soc_vl,tb_interconnect,tb_interconnect,$(SOC_IC_TB_SRC))
	$(call soc_vl,tb_interconnect_noddr,tb_interconnect,$(SOC_IC_TB_SRC),-GHAS_DDR=0)
	@echo "test-soc-verilator: PASS (10 unit testbenches under Verilator --timing, seed 1)"

# ---------------------------------------------------------------------------- the real system
# Once the real core and the firmware exist (cpu and fw tracks): build/sim/surfsim runs the fw
# track's BRAM diagnostics to BRAMDIAG: PASS, and SurfBoot to its banner and, without a card,
# its monitor, which must answer `help` typed through the UART model. Skipped until then.
SOC_BOOT_READY := $(and $(wildcard rtl/cpu/surfcpu_core.v),$(wildcard mk/area-fw.mk))

test-soc-boot:
ifeq ($(SOC_BOOT_READY),)
	@echo "test-soc-boot: skipped (needs the real core and the firmware of the cpu and fw tracks)"
else
	$(Q)$(MAKE) --no-print-directory build-rom $(BUILD)/sim/surfsim > $(SOC_DIR)/boot-build.txt 2>&1 \
	  || { tail -20 $(SOC_DIR)/boot-build.txt; exit 1; }
	$(Q)$(BUILD)/sim/surfsim --rom $(BUILD)/fw/bramdiag.mem --max-cycles 400000000 \
	  --expect 'BRAMDIAG: (PASS|FAIL)[^\n]*' > $(SOC_DIR)/boot-bramdiag.txt 2>&1; \
	  grep -q "BRAMDIAG: PASS" $(SOC_DIR)/boot-bramdiag.txt || { tail -30 $(SOC_DIR)/boot-bramdiag.txt; exit 1; }
	@echo "  [bramdiag] $$(grep -o 'BRAMDIAG: PASS ([0-9]* checks)' $(SOC_DIR)/boot-bramdiag.txt), $$(grep -o 'after [0-9]* cycles' $(SOC_DIR)/boot-bramdiag.txt | head -1)"
	$(Q)printf 'help\r' > $(SOC_DIR)/boot-help.txt
	$(Q)$(BUILD)/sim/surfsim --rom $(BUILD)/fw/surfboot.mem --uart-in $(SOC_DIR)/boot-help.txt \
	  --max-cycles 100000000 --expect 'SurfBoot [0-9][\s\S]*monitor commands' \
	  > $(SOC_DIR)/boot-surfboot.txt 2>&1 || { tail -30 $(SOC_DIR)/boot-surfboot.txt; exit 1; }
	@echo "  [surfboot] $$(grep -m1 '^SurfBoot ' $(SOC_DIR)/boot-surfboot.txt); the monitor answered help"
	@echo "test-soc-boot: PASS (the real core runs the diagnostics and SurfBoot in surfsim)"
endif
