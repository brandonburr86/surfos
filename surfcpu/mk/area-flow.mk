# flow: the open-source bitstream flow (openXC7; docs/OPEN-FLOW.md). Inside the toolchain
# container. Only fast checks that need neither the openXC7 image nor the network hang on
# the aggregates: the bitstream builds themselves run from the Mac (tools/openxc7-build.sh,
# tools/openxc7-demo.sh) in the surfcpu-openxc7 image and are never part of `make check`.
#
#   make lint-flow        Verilator lint of the flow demo design
#   make test-flow        the demo's RTL simulations, the bitstream inspector's self-test, and
#                         tests of the XDC and build scripts (dry runs)
#   make flow-xdc-check   regenerate the demo's XDC from Digilent's master files (fetched,
#                         SHA-256 checked) and compare; needs the network the first time

FLOW_DIR  = devenv/openxc7
FLOW_RTL  = $(FLOW_DIR)/demo/flow_demo.v rtl/lib/surf_reset_sync.v
FLOW_TB   = $(FLOW_DIR)/demo/flow_demo_tb.v
FLOW_SIM  = $(BUILD)/flow/sim
FLOW_XDC_PORTS = CLK100MHZ ck_rst 'led[0]' 'led[1]' 'led[2]' 'led[3]' uart_rxd_out

.PHONY: lint-flow test-flow test-flow-sim test-flow-tools flow-xdc-check
lint: lint-flow
check: test-flow

lint-flow:
	$(Q)$(VERILATOR_LINT) --top-module flow_demo $(FLOW_RTL)
	$(Q)$(VERILATOR_LINT) --top-module flow_demo -GCLK_HZ=1152000 -GPERIOD=4000 $(FLOW_RTL)
	@echo "lint-flow: flow_demo clean (default and testbench parameters)"

$(FLOW_SIM)/flow_demo_tb.vvp: $(FLOW_TB) $(FLOW_RTL)
	@mkdir -p $(dir $@)
	$(Q)$(IVERILOG) -g2005 -Wall -Wno-timescale -o $@ $^

$(FLOW_SIM)/flow_demo_tb_full.vvp: $(FLOW_TB) $(FLOW_RTL)
	@mkdir -p $(dir $@)
	$(Q)$(IVERILOG) -g2005 -Wall -Wno-timescale -Pflow_demo_tb.CLK_HZ=100000000 \
	  -Pflow_demo_tb.PERIOD=400000 -o $@ $^

# Three seeds at 10 clocks per bit, and one at the real 100 MHz / 115200 rate.
test-flow-sim: $(FLOW_SIM)/flow_demo_tb.vvp $(FLOW_SIM)/flow_demo_tb_full.vvp
	$(Q)for s in 1 7 123; do vvp -n $(FLOW_SIM)/flow_demo_tb.vvp +seed=$$s > $(FLOW_SIM)/tb.$$s.txt 2>&1; \
	  grep -q 'flow_demo_tb: PASS' $(FLOW_SIM)/tb.$$s.txt || { cat $(FLOW_SIM)/tb.$$s.txt; exit 1; }; done
	$(Q)vvp -n $(FLOW_SIM)/flow_demo_tb_full.vvp +seed=5 +lines=1 > $(FLOW_SIM)/tb.full.txt 2>&1; \
	  grep -q 'flow_demo_tb: PASS' $(FLOW_SIM)/tb.full.txt || { cat $(FLOW_SIM)/tb.full.txt; exit 1; }
	@echo "test-flow-sim: flow_demo RTL PASS (seeds 1 7 123 at 10 clocks per bit; seed 5 at 868)"

# The scripts: the bitstream inspector on synthetic bitstreams, the XDC rewriting on a
# fixture in Digilent's format, and the build script's argument handling (dry runs).
test-flow-tools:
	$(Q)$(PYTHON) -I $(FLOW_DIR)/bitinfo.py --self-test
	$(Q)sh tools/openxc7-flow-test.sh
	@echo "test-flow-tools: PASS"

test-flow: lint-flow test-flow-sim test-flow-tools
	@echo "test-flow: PASS"

flow-xdc-check:
	@mkdir -p $(BUILD)/flow
	$(Q)dir=$$(sh tools/openxc7-fetch.sh) && \
	  for m in Arty-A7-35-Master.xdc Arty-A7-100-Master.xdc; do \
	    sh tools/openxc7-xdc.sh "$$dir/$$m" $(FLOW_XDC_PORTS) > $(BUILD)/flow/xdc-$$m.txt || exit 1; \
	    grep -v '^#' $(FLOW_DIR)/demo/flow_demo.xdc | cmp -s - $(BUILD)/flow/xdc-$$m.txt || \
	      { echo "flow-xdc-check: $(FLOW_DIR)/demo/flow_demo.xdc differs from $$m"; exit 1; }; done
	@echo "flow-xdc-check: flow_demo.xdc matches Digilent's master XDC for the A7-35 and A7-100"
