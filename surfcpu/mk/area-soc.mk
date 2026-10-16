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

SOC_IVERILOG = $(IVERILOG) -g2005 -Wall -Wno-timescale $(SOC_INC)
SOC_VLINT    = $(VERILATOR_LINT) -Irtl/soc -Irtl/peripherals

.PHONY: lint-soc test-soc-units
lint: lint-soc
test-soc: test-soc-units

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
	@echo "lint-soc: $(words $(SOC_BUS_RTL) $(SOC_MEM_RTL) $(SOC_PER_RTL)) modules clean (Verilator -Wall, Icarus -g2005)"

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

SOC_UNIT_VVP = $(addprefix $(SOC_DIR)/,tb_intc.vvp tb_sysinfo.vvp tb_sysinfo_noddr.vvp \
                 tb_timer.vvp tb_timer_frac.vvp tb_uart.vvp)

test-soc-units: $(SOC_UNIT_VVP)
	$(call soc_run,$(SOC_DIR)/tb_intc.vvp,tb_intc)
	$(call soc_run,$(SOC_DIR)/tb_sysinfo.vvp,tb_sysinfo)
	$(call soc_run,$(SOC_DIR)/tb_sysinfo_noddr.vvp,tb_sysinfo_noddr)
	$(call soc_run,$(SOC_DIR)/tb_timer.vvp,tb_timer)
	$(call soc_run,$(SOC_DIR)/tb_timer_frac.vvp,tb_timer_frac)
	$(call soc_run,$(SOC_DIR)/tb_uart.vvp,tb_uart)
	@echo "test-soc-units: PASS (seeds $(SOC_SEEDS))"
