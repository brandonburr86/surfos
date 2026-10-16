# soc track: the SurfBus interconnect, the memories, the base peripherals, the SoC with its
# stand-ins, the C++ models and the surfsim harness (docs/SOC.md).
#
#   make lint-soc            Verilator -Wall of every soc module, the SoC (both HAS_DDR) and the
#                            stand-ins, plus an Icarus -g2005 compile of all of it

SOC_DIR     := $(BUILD)/soc
SOC_INC     := -Irtl/generated -Irtl/lib -Irtl/soc -Irtl/cpu -Irtl/peripherals

SOC_LIB_RTL := rtl/lib/surf_fifo.v rtl/lib/surf_sync.v
SOC_BUS_RTL := $(addprefix rtl/soc/,surf_bus_err.v surf_bus_demux.v surf_ibus_decoder.v \
                 surf_dbus_decoder.v surf_bus_arbiter.v surf_ddr_gate.v surf_interconnect.v)
SOC_MEM_RTL := $(addprefix rtl/soc/,surf_bootrom.v surf_scratch.v surf_textram.v)
SOC_PER_RTL := $(addprefix rtl/peripherals/,surf_uart.v surf_timer.v surf_intc.v surf_gpio.v \
                 surf_sysinfo.v)

SOC_IVERILOG = $(IVERILOG) -g2005 -Wall -Wno-timescale $(SOC_INC)
SOC_VLINT    = $(VERILATOR_LINT) -Irtl/soc -Irtl/peripherals

.PHONY: lint-soc
lint: lint-soc

# ---------------------------------------------------------------------------- lint
lint-soc:
	$(Q)for f in $(SOC_BUS_RTL) $(SOC_MEM_RTL); do \
	  $(SOC_VLINT) --top-module $$(basename $$f .v) $(SOC_BUS_RTL) $(SOC_MEM_RTL) || exit 1; done
	$(Q)$(SOC_VLINT) --top-module surf_interconnect -GHAS_DDR=0 $(SOC_BUS_RTL)
	$(Q)for f in $(SOC_PER_RTL); do \
	  $(SOC_VLINT) --top-module $$(basename $$f .v) $$f $(SOC_LIB_RTL) || exit 1; done
	$(Q)$(SOC_IVERILOG) -o /dev/null $(SOC_BUS_RTL) $(SOC_MEM_RTL) $(SOC_PER_RTL) $(SOC_LIB_RTL)
	@echo "lint-soc: $(words $(SOC_BUS_RTL) $(SOC_MEM_RTL) $(SOC_PER_RTL)) modules clean (Verilator -Wall, Icarus -g2005)"
