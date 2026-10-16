# Board track (Arty A7): the clock plan, the pins and constraints, the board tops and their
# simulations, and the tests of the Vivado and programming wrappers (docs/BOARD.md).
#
#   make lint-board        Verilator -Wall lint of every board module            (on lint)
#   make test-board        clock, baseline, top-level and model tests            (on test-soc)
#   make check-board       clock plan, wiring, vendor files, wrapper and flow tests (on check)
#   make estimate-board    Yosys synth_xilinx resource estimates and the block RAM budget
#                          (writes docs/evidence/board/resources.txt)
#   make evidence-board    rerun the board tests and write docs/evidence/board/*.txt

BD          = boards/arty_a7
BSIM        = $(BUILD)/sim/board
BOARD_PRIMS = sim/board/prims/surf_sim_cmt.v sim/board/prims/MMCME2_BASE.v \
              sim/board/prims/PLLE2_BASE.v sim/board/prims/BUFG.v sim/board/prims/IOBUF.v

# The files of one kind in a design list: $(call board_list,<design>,<kind>)
board_list = $(wildcard $(shell sed -n -e 's/[[:space:]]*\#.*//' -e 's/^$(2)[[:space:]][[:space:]]*//p' $(BD)/designs/$(1).files))
BASELINE_SRC = $(call board_list,arty_a7_baseline,src)
# The full top uses the real SoC once the soc track has merged it, else the stand-in
# (sim/board/stubs/surfcpu_soc.v, never used for a bitstream: tools/fpga-build.sh refuses).
SOC_REAL  = $(wildcard rtl/soc/surfcpu_soc.v)
TOP_SRC   = $(if $(SOC_REAL),$(call board_list,surfcpu_arty_a7_top,src) $(call board_list,surfcpu_arty_a7_top,src?),\
              $(wildcard rtl/lib/*.v) $(BD)/rtl/arty_a7_clocks.v $(BD)/rtl/surfcpu_arty_a7_top.v \
              sim/board/stubs/surfcpu_soc.v)
TOP_DEFS  = $(if $(SOC_REAL),,-DSURFCPU_SOC_STUB)
TOP_INCS  = $(addprefix -I,$(call board_list,surfcpu_arty_a7_top,inc))

BOARD_VLINT = $(VERILATOR_LINT) --timing --timescale 1ps/1ps -I$(BD)/rtl
BOARD_IVL   = $(IVERILOG) -g2005 -Wall -Wno-timescale -Irtl/generated -I$(BD)/rtl
BOARD_VLSIM = $(VERILATOR) --binary --timing -j 2 --timescale 1ps/1ps -Wno-fatal -Wno-lint -Wno-style \
              -Irtl/generated -I$(BD)/rtl

.PHONY: lint-board test-board check-board board-clocks board-prims-bad board-clockplan \
        board-baseline-rt board-baseline board-top board-digilent board-wiring \
        board-cdc-xdc board-tools estimate-board evidence-board
lint: lint-board
test-soc: test-board
check: check-board

# ------------------------------------------------------------------------------------------- lint
lint-board:
	$(Q)for t in arty_a7_clocks arty_a7_uart_tx arty_a7_uart_rx arty_a7_baseline_core arty_a7_baseline; do \
	  $(BOARD_VLINT) --top-module $$t $(BASELINE_SRC) $(BOARD_PRIMS) || exit 1; done
	$(Q)$(BOARD_VLINT) $(TOP_INCS) $(TOP_DEFS) --top-module surfcpu_arty_a7_top $(TOP_SRC) $(BOARD_PRIMS)
	@echo "lint-board: 6 board modules clean ($(if $(SOC_REAL),real SoC,SoC stand-in))"

# ------------------------------------------------------------------------------- simulations
test-board: board-clocks board-prims-bad board-baseline-rt board-baseline board-top
	@echo "test-board: PASS"

# clocks and resets: Icarus and Verilator --timing
$(BSIM)/tb_clocks.vvp: sim/board/tb_clocks.v $(BOARD_PRIMS) rtl/lib/surf_reset_sync.v $(BD)/rtl/arty_a7_clocks.v $(BD)/rtl/arty_a7_clock_params.vh
	@mkdir -p $(dir $@)
	$(Q)$(BOARD_IVL) -o $@ $(BOARD_PRIMS) rtl/lib/surf_reset_sync.v $(BD)/rtl/arty_a7_clocks.v sim/board/tb_clocks.v
$(BSIM)/vl_clocks/tb_clocks: sim/board/tb_clocks.v $(BOARD_PRIMS) rtl/lib/surf_reset_sync.v $(BD)/rtl/arty_a7_clocks.v $(BD)/rtl/arty_a7_clock_params.vh
	$(Q)$(BOARD_VLSIM) --Mdir $(dir $@) -o tb_clocks --top-module tb_clocks $(BOARD_PRIMS) \
	  rtl/lib/surf_reset_sync.v $(BD)/rtl/arty_a7_clocks.v sim/board/tb_clocks.v > $(BSIM)/vl_clocks.build.txt 2>&1 \
	  || { tail -30 $(BSIM)/vl_clocks.build.txt; exit 1; }
board-clocks: $(BSIM)/tb_clocks.vvp $(BSIM)/vl_clocks/tb_clocks
	$(Q)vvp -n $< > $(BSIM)/tb_clocks.icarus.txt 2>&1; grep -q "tb_clocks: PASS" $(BSIM)/tb_clocks.icarus.txt \
	  || { cat $(BSIM)/tb_clocks.icarus.txt; exit 1; }
	$(Q)$(BSIM)/vl_clocks/tb_clocks > $(BSIM)/tb_clocks.verilator.txt 2>&1; grep -q "tb_clocks: PASS" $(BSIM)/tb_clocks.verilator.txt \
	  || { cat $(BSIM)/tb_clocks.verilator.txt; exit 1; }
	@grep -h "Hz (spec" $(BSIM)/tb_clocks.icarus.txt
	@echo "board-clocks: PASS (Icarus and Verilator)"

# the clock tile models refuse illegal settings (each case must stop with its message)
BAD_CASES = 1:VCO 2:CLKFBOUT_MULT_F 3:CLKOUT0_DIVIDE_F 4:VCO 5:PFD 6:CLKOUT0_DIVIDE
board-prims-bad: $(BOARD_PRIMS) sim/board/tb_prims_bad.v
	@mkdir -p $(BSIM)
	$(Q)for c in $(BAD_CASES); do n=$${c%%:*}; want=$${c#*:}; \
	  $(BOARD_IVL) -Ptb_prims_bad.CASE=$$n -o $(BSIM)/tb_prims_bad.vvp $(BOARD_PRIMS) sim/board/tb_prims_bad.v || exit 1; \
	  vvp -n $(BSIM)/tb_prims_bad.vvp > $(BSIM)/tb_prims_bad.$$n.txt 2>&1; \
	  if grep -q "NOT CAUGHT" $(BSIM)/tb_prims_bad.$$n.txt || ! grep -q "$$want" $(BSIM)/tb_prims_bad.$$n.txt; then \
	    echo "board-prims-bad: case $$n was not refused with '$$want'"; cat $(BSIM)/tb_prims_bad.$$n.txt; exit 1; fi; \
	done
	@echo "board-prims-bad: PASS (6 illegal settings refused)"

# the baseline logic in real time (Verilator, C++ harness)
BASELINE_RT_G = -GBOARD='"arty_a7_100t"' "-GBUILD_ID=32'h1a2b3c4d" -GBUILD_FLAGS=3 -GBUILD_TIME=1760560000 -GRGB_DIM_SHIFT=0
$(BSIM)/baseline_rt/baseline_rt: sim/board/baseline_rt.cpp $(BASELINE_SRC) firmware/include/surfcpu/soc.h
	@mkdir -p $(dir $@)
	$(Q)$(VERILATOR) --cc --exe --build -j 2 -O3 --Mdir $(dir $@) -o baseline_rt -Wno-fatal \
	  -Irtl/generated -I$(BD)/rtl --top-module arty_a7_baseline_core $(BASELINE_RT_G) \
	  -CFLAGS "-O2 -I$(abspath firmware/include)" $(filter-out $(BD)/rtl/arty_a7_baseline.v $(BD)/rtl/arty_a7_clocks.v,$(BASELINE_SRC)) \
	  sim/board/baseline_rt.cpp > $(BSIM)/baseline_rt.build.txt 2>&1 || { tail -30 $(BSIM)/baseline_rt.build.txt; exit 1; }
board-baseline-rt: $(BSIM)/baseline_rt/baseline_rt
	$(Q)$< > $(BSIM)/baseline_rt.txt 2>&1; s=$$?; cat $(BSIM)/baseline_rt.txt; [ $$s -eq 0 ]

# the whole baseline top from its pins: Icarus (short run) and Verilator --timing (with RESET)
BASELINE_TB = $(BOARD_PRIMS) $(BASELINE_SRC) sim/board/tb_baseline.v
$(BSIM)/tb_baseline.vvp: $(BASELINE_TB) $(BD)/rtl/arty_a7_clock_params.vh
	@mkdir -p $(dir $@)
	$(Q)$(BOARD_IVL) -o $@ $(BASELINE_TB)
$(BSIM)/vl_baseline/tb_baseline: $(BASELINE_TB) $(BD)/rtl/arty_a7_clock_params.vh
	$(Q)$(BOARD_VLSIM) --Mdir $(dir $@) -o tb_baseline --top-module tb_baseline $(BASELINE_TB) \
	  > $(BSIM)/vl_baseline.build.txt 2>&1 || { tail -30 $(BSIM)/vl_baseline.build.txt; exit 1; }
board-baseline: $(BSIM)/tb_baseline.vvp $(BSIM)/vl_baseline/tb_baseline
	$(Q)$(BSIM)/vl_baseline/tb_baseline > $(BSIM)/tb_baseline.verilator.txt 2>&1; \
	  grep -q "tb_baseline: PASS" $(BSIM)/tb_baseline.verilator.txt || { cat $(BSIM)/tb_baseline.verilator.txt; exit 1; }
	$(Q)vvp -n $< +short > $(BSIM)/tb_baseline.icarus.txt 2>&1; \
	  grep -q "tb_baseline: PASS" $(BSIM)/tb_baseline.icarus.txt || { cat $(BSIM)/tb_baseline.icarus.txt; exit 1; }
	@grep -h "tb_baseline: line 2\|after RESET\|tb_baseline: pix" $(BSIM)/tb_baseline.verilator.txt
	@echo "board-baseline: PASS (Verilator with RESET, Icarus short run)"

# the full top's board wiring: Icarus on three seeds and Verilator --timing (with the stand-in
# the test also drives every SoC output)
$(BSIM)/tb_top.vvp: $(TOP_SRC) $(BOARD_PRIMS) sim/board/tb_top.v
	@mkdir -p $(dir $@)
	$(Q)$(BOARD_IVL) $(TOP_INCS) $(TOP_DEFS) -o $@ $(BOARD_PRIMS) $(TOP_SRC) sim/board/tb_top.v
$(BSIM)/vl_top/tb_top: $(TOP_SRC) $(BOARD_PRIMS) sim/board/tb_top.v
	@mkdir -p $(BSIM)
	$(Q)$(BOARD_VLSIM) $(TOP_INCS) $(TOP_DEFS) --Mdir $(dir $@) -o tb_top --top-module tb_top $(BOARD_PRIMS) \
	  $(TOP_SRC) sim/board/tb_top.v > $(BSIM)/vl_top.build.txt 2>&1 || { tail -30 $(BSIM)/vl_top.build.txt; exit 1; }
board-top: $(BSIM)/tb_top.vvp $(BSIM)/vl_top/tb_top
	$(Q)for s in 1 7 123; do vvp -n $< +seed=$$s > $(BSIM)/tb_top.$$s.txt 2>&1; \
	  grep -q "tb_top: PASS" $(BSIM)/tb_top.$$s.txt || { cat $(BSIM)/tb_top.$$s.txt; exit 1; }; done
	$(Q)$(BSIM)/vl_top/tb_top +seed=5 > $(BSIM)/tb_top.verilator.txt 2>&1; \
	  grep -q "tb_top: PASS" $(BSIM)/tb_top.verilator.txt || { cat $(BSIM)/tb_top.verilator.txt; exit 1; }
	@echo "board-top: PASS (Icarus seeds 1 7 123, Verilator seed 5, $(if $(SOC_REAL),real SoC,SoC stand-in))"

# ------------------------------------------------------------------------------------ checks
check-board: board-clockplan board-digilent board-wiring board-cdc-xdc board-tools
	@echo "check-board: PASS"

board-clockplan:
	$(Q)$(PYTHON) ip/clocking/clockplan.py check

# Digilent's master XDC files and licence are unchanged; fetched manuals, when present, match
board-digilent:
	$(Q)$(BD)/digilent/fetch.sh verify

# every port has one pin, agreeing with docs/WIRING.md and Digilent's master XDC files
board-wiring:
	@mkdir -p $(BSIM)
	$(Q)$(PYTHON) tools/check-wiring.py
	$(Q)$(PYTHON) tools/check-wiring.py --self-test > $(BSIM)/check-wiring-self-test.txt 2>&1 \
	  || { cat $(BSIM)/check-wiring-self-test.txt; exit 1; }
	@tail -1 $(BSIM)/check-wiring-self-test.txt

# the scoped CDC constraint files' Tcl logic, run with stand-in Vivado queries
board-cdc-xdc:
	$(Q)$(YOSYS) -q -c sim/board/tools/cdc_xdc_test.tcl > $(BSIM)/cdc_xdc_test.txt 2>&1 \
	  || { cat $(BSIM)/cdc_xdc_test.txt; exit 1; }
	@tail -1 $(BSIM)/cdc_xdc_test.txt

# tools/fpga-build.sh and tools/program.sh against fake ssh, docker, vivado (running the real
# build.tcl with stubbed Vivado commands) and openFPGALoader, with a temporary HOME
board-tools:
	@mkdir -p $(BSIM)
	$(Q)sh sim/board/tools/test_tools.sh > $(BSIM)/test_tools.txt 2>&1 || { grep -v '^ok' $(BSIM)/test_tools.txt; exit 1; }
	@tail -1 $(BSIM)/test_tools.txt

# ------------------------------------------------------------------------------------ estimates
estimate-board:
	$(Q)$(PYTHON) $(BD)/estimate.py docs/evidence/board/resources.txt

evidence-board: estimate-board
	$(Q)sh sim/board/evidence.sh
