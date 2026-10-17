# flow: the open-source bitstream flow (openXC7; docs/OPEN-FLOW.md). Inside the toolchain
# container. Only fast checks that need neither the openXC7 image nor the network hang on
# the aggregates: the bitstream builds themselves run from the Mac (tools/openxc7-build.sh,
# tools/openxc7-demo.sh) in the surfcpu-openxc7 image and are never part of `make check`.
#
#   make lint-flow        Verilator lint of the flow demo design
#   make test-flow        the demos' RTL simulations, the self-tests of the bitstream inspector,
#                         the MMCM/PLL divider check and the global clock network check, and
#                         tests of the XDC and build scripts (dry runs)
#   make flow-xdc-check   regenerate the demo's XDC from Digilent's master files (fetched,
#                         SHA-256 checked) and compare; needs the network the first time

FLOW_DIR  = devenv/openxc7
FLOW_RTL  = $(FLOW_DIR)/demo/flow_demo.v rtl/lib/surf_reset_sync.v
FLOW_TB   = $(FLOW_DIR)/demo/flow_demo_tb.v
FLOW_FEAT = $(FLOW_DIR)/demo/flow_features_core.v rtl/lib/surf_sync.v rtl/lib/surf_cdc_word.v
FLOW_FEAT_TB = $(FLOW_DIR)/demo/flow_features_tb.v
FLOW_SIM  = $(BUILD)/flow/sim
FLOW_XDC_PORTS = CLK100MHZ ck_rst 'led[0]' 'led[1]' 'led[2]' 'led[3]' uart_rxd_out

.PHONY: lint-flow test-flow test-flow-sim test-flow-tools flow-xdc-check
lint: lint-flow
check: test-flow

# flow_features.v itself instantiates vendor primitives (no models here): its core is linted.
lint-flow:
	$(Q)$(VERILATOR_LINT) --top-module flow_demo $(FLOW_RTL)
	$(Q)$(VERILATOR_LINT) --top-module flow_demo -GCLK_HZ=1152000 -GPERIOD=4000 $(FLOW_RTL)
	$(Q)$(VERILATOR_LINT) --top-module flow_features_core $(FLOW_FEAT)
	$(Q)$(VERILATOR_LINT) --top-module flow_features_core -GPERIOD=200000 $(FLOW_FEAT)
	@echo "lint-flow: flow_demo and flow_features_core clean (default and testbench parameters)"

$(FLOW_SIM)/flow_demo_tb.vvp: $(FLOW_TB) $(FLOW_RTL)
	@mkdir -p $(dir $@)
	$(Q)$(IVERILOG) -g2005 -Wall -Wno-timescale -o $@ $^

$(FLOW_SIM)/flow_demo_tb_full.vvp: $(FLOW_TB) $(FLOW_RTL)
	@mkdir -p $(dir $@)
	$(Q)$(IVERILOG) -g2005 -Wall -Wno-timescale -Pflow_demo_tb.CLK_HZ=100000000 \
	  -Pflow_demo_tb.PERIOD=400000 -o $@ $^

$(FLOW_SIM)/flow_features_tb.vvp: $(FLOW_FEAT_TB) $(FLOW_FEAT)
	@mkdir -p $(dir $@)
	$(Q)$(IVERILOG) -g2005 -Wall -Wno-timescale -o $@ $^

# flow_demo: three seeds at 10 clocks per bit, and one at the real 100 MHz / 115200 rate.
# flow_features_core: three lines with the real ROM, baud rate and clock frequencies.
test-flow-sim: $(FLOW_SIM)/flow_demo_tb.vvp $(FLOW_SIM)/flow_demo_tb_full.vvp $(FLOW_SIM)/flow_features_tb.vvp
	$(Q)for s in 1 7 123; do vvp -n $(FLOW_SIM)/flow_demo_tb.vvp +seed=$$s > $(FLOW_SIM)/tb.$$s.txt 2>&1; \
	  grep -q 'flow_demo_tb: PASS' $(FLOW_SIM)/tb.$$s.txt || { cat $(FLOW_SIM)/tb.$$s.txt; exit 1; }; done
	$(Q)vvp -n $(FLOW_SIM)/flow_demo_tb_full.vvp +seed=5 +lines=1 > $(FLOW_SIM)/tb.full.txt 2>&1; \
	  grep -q 'flow_demo_tb: PASS' $(FLOW_SIM)/tb.full.txt || { cat $(FLOW_SIM)/tb.full.txt; exit 1; }
	$(Q)vvp -n $(FLOW_SIM)/flow_features_tb.vvp > $(FLOW_SIM)/features.txt 2>&1; \
	  grep -q 'flow_features_tb: PASS' $(FLOW_SIM)/features.txt || { cat $(FLOW_SIM)/features.txt; exit 1; }
	@echo "test-flow-sim: flow_demo RTL PASS (seeds 1 7 123 at 10 clocks per bit; seed 5 at 868)"
	@echo "test-flow-sim: flow_features_core RTL PASS (3 lines, 64 KiB ROM, 25 and 25.17 MHz)"

# The scripts: the bitstream inspector on synthetic bitstreams, the XDC rewriting on a
# fixture in Digilent's format, and the build script's argument handling (dry runs).
test-flow-tools:
	$(Q)$(PYTHON) -I $(FLOW_DIR)/bitinfo.py --self-test
	$(Q)$(PYTHON) -I $(FLOW_DIR)/cmtcheck.py --self-test
	$(Q)$(PYTHON) -I $(FLOW_DIR)/bramwe.py --self-test
	$(Q)$(PYTHON) -I $(FLOW_DIR)/synthcheck.py --self-test
	$(Q)$(PYTHON) -I $(FLOW_DIR)/gclkcheck.py --self-test
	$(Q)sh tools/openxc7-flow-test.sh
	@echo "test-flow-tools: PASS"

test-flow: lint-flow test-flow-sim test-flow-tools
	@echo "test-flow: PASS"

FLOW_FEAT_XDC_PORTS = CLK100MHZ ck_rst 'sw[0]' 'sw[1]' 'led[0]' 'led[1]' 'led[2]' 'led[3]' uart_rxd_out ck_ss

flow-xdc-check:
	@mkdir -p $(BUILD)/flow
	$(Q)dir=$$(sh tools/openxc7-fetch.sh) && \
	  for m in Arty-A7-35-Master.xdc Arty-A7-100-Master.xdc; do \
	    sh tools/openxc7-xdc.sh "$$dir/$$m" $(FLOW_XDC_PORTS) > $(BUILD)/flow/xdc-demo-$$m.txt || exit 1; \
	    grep -v '^#' $(FLOW_DIR)/demo/flow_demo.xdc | cmp -s - $(BUILD)/flow/xdc-demo-$$m.txt || \
	      { echo "flow-xdc-check: $(FLOW_DIR)/demo/flow_demo.xdc differs from $$m"; exit 1; }; \
	    sh tools/openxc7-xdc.sh "$$dir/$$m" $(FLOW_FEAT_XDC_PORTS) > $(BUILD)/flow/xdc-features-$$m.txt || exit 1; \
	    grep -v '^#' $(FLOW_DIR)/demo/flow_features.xdc | cmp -s - $(BUILD)/flow/xdc-features-$$m.txt || \
	      { echo "flow-xdc-check: $(FLOW_DIR)/demo/flow_features.xdc differs from $$m"; exit 1; }; done
	@echo "flow-xdc-check: flow_demo.xdc and flow_features.xdc match Digilent's master XDC (A7-35 and A7-100)"
