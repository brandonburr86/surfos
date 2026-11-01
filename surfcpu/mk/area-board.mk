# Board track (Arty A7): the clock plan, the pins and constraints, the board tops and their
# simulations, and the tests of the Vivado and programming wrappers (docs/BOARD.md).
#
#   make lint-board        Verilator -Wall lint of every board module            (on lint)
#   make test-board        clock, baseline, top-level and model tests            (on test-soc)
#   make check-board       clock plan, wiring, vendor files, wrapper and flow tests (on check)
#   make estimate-board    Yosys synth_xilinx resource estimates and the block RAM budget
#                          (writes docs/evidence/board/resources.txt)
#   make evidence-board    rerun the board tests and write docs/evidence/board/*.txt
#   make board-uart-expect what the board prints with the SoC designs, from RTL simulation
#                          (BOARD=arty_a7_100t or arty_a7_35t; not part of check)
#   make board-netlist-sim the post-synthesis simulation of an open-flow build of the full top
#                          (BOARD=..., DESIGN=surfcpu_arty_a7_bramdiag or surfcpu_arty_a7_top, or
#                          a DDR design; needs that build; not part of check)
#   make board-ddr-sim     the DDR top in RTL: with LiteDRAM's controller and DRAM model, the BRAM
#                          diagnostics (the DDR initialization through surf_ddr_csr and the DDR tests)
#                          and SurfBoot; with the generated board core but no memory, the BRAM
#                          diagnostics to a failed read leveling (BOARD=...; needs make litedram-gen;
#                          not part of check)
#   make board-uart-expect-ddr  SurfBoot through to SurfOS's prompt with the C++ DDR model and
#                          LiteDRAM's register model, the SoC as the DDR designs configure it
#                          (BOARD=...; not part of check)

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
# The DDR variant (ARTY_HAS_DDR: the DDR designs) with the bridge and, for lint, the LiteDRAM
# core's port stand-in (the real core is generated: make litedram-gen; inspect_core.py checks
# the stand-in has its ports).
DDR_TOP_SRC = $(sort $(TOP_SRC) $(wildcard rtl/soc/ddr/*.v)) sim/board/stubs/litedram_core.v

BOARD_VLINT = $(VERILATOR_LINT) --timing --timescale 1ps/1ps -I$(BD)/rtl
BOARD_IVL   = $(IVERILOG) -g2005 -Wall -Wno-timescale -Irtl/generated -I$(BD)/rtl
BOARD_VLSIM = $(VERILATOR) --binary --timing -j 2 --timescale 1ps/1ps -Wno-fatal -Wno-lint -Wno-style \
              -Irtl/generated -I$(BD)/rtl

.PHONY: lint-board test-board check-board board-clocks board-prims-bad board-clockplan \
        board-baseline-rt board-baseline board-top board-digilent board-wiring \
        board-cdc-xdc board-tools estimate-board evidence-board board-uart-expect board-netlist-sim \
        board-ddr-sim board-uart-expect-ddr
lint: lint-board
test-soc: test-board
check: check-board

# ------------------------------------------------------------------------------------------- lint
lint-board:
	$(Q)for t in arty_a7_clocks arty_a7_uart_tx arty_a7_uart_rx arty_a7_baseline_core arty_a7_baseline; do \
	  $(BOARD_VLINT) --top-module $$t $(BASELINE_SRC) $(BOARD_PRIMS) || exit 1; done
	$(Q)$(BOARD_VLINT) $(TOP_INCS) $(TOP_DEFS) --top-module surfcpu_arty_a7_top $(TOP_SRC) $(BOARD_PRIMS)
	$(Q)$(BOARD_VLINT) $(TOP_INCS) $(TOP_DEFS) -DARTY_HAS_DDR --top-module surfcpu_arty_a7_top $(DDR_TOP_SRC) $(BOARD_PRIMS)
	@echo "lint-board: 6 board modules and the DDR top clean ($(if $(SOC_REAL),real SoC,SoC stand-in); LiteDRAM's ports stand in for the core)"

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
# build.tcl with stubbed Vivado commands), openxc7-build.sh and openFPGALoader, with a temporary
# HOME; and boards/arty_a7/digilent/fetch.sh on made-up manuals. Its dry run of the full top reads
# the real boot ROM, which must be this build's (build-rom)
board-tools: build-rom
	@mkdir -p $(BSIM)
	$(Q)sh sim/board/tools/test_tools.sh > $(BSIM)/test_tools.txt 2>&1 || { grep -v '^ok' $(BSIM)/test_tools.txt; exit 1; }
	@tail -1 $(BSIM)/test_tools.txt

# --------------------------------------------------------------------------- expected UART output
# The soc track's surfsim harness built with the SoC as surfcpu_arty_a7_top configures it on the
# board (HAS_DDR=0, the board's BOARD_ID, the bitstream's BUILD_FLAGS) runs the BRAM diagnostics
# to their end and SurfBoot to its monitor with a card (sim/board/uart_expect.sh).
board-uart-expect: build-rom
	$(Q)SOC_SRC="$(SOC_TOP_RTL) $(SOC_CORE_SRC) $(SOC_IO_SRC)" SOC_INC="$(SOC_INC)" SIM_CPP="$(SOC_SIM_CPP)" \
	  sh sim/board/uart_expect.sh $(BOARD)

# The SoC as the DDR designs configure it, SurfBoot booting SurfOS with the C++ DDR model
# (sim/board/uart_expect_ddr.sh).
board-uart-expect-ddr: build-rom build-kernel
	$(Q)SOC_SRC="$(SOC_TOP_RTL) $(SOC_CORE_SRC) $(SOC_IO_SRC)" SOC_INC="$(SOC_INC)" SIM_CPP="$(SOC_SIM_CPP)" \
	  sh sim/board/uart_expect_ddr.sh $(BOARD)

# ------------------------------------------------------------------- the DDR top in RTL
# surfcpu_arty_a7_top with ARTY_HAS_DDR (sim/board/ddr_top_sim.sh): with LiteDRAM's controller and
# DRAM model the BRAM diagnostics initialize the controller through surf_ddr_csr (every delay
# works: the model has no PHY) and pass their DDR tests, and SurfBoot initializes it, calibrates
# and stops for want of a card; with the board's generated core (PHY, primitive models) and no
# memory on its pins, the diagnostics' read leveling finds no working delay and they report it.
board-ddr-sim: build-rom
	$(Q)sh sim/board/ddr_top_sim.sh $(BOARD) build/fw/bramdiag.mem "BRAMDIAG: PASS" 4000
	$(Q)sh sim/board/ddr_top_sim.sh $(BOARD) build/fw/surfboot.mem "The LEDs" 1500
	$(Q)DDR_SIM_CORE=board sh sim/board/ddr_top_sim.sh $(BOARD) build/fw/bramdiag.mem "BRAMDIAG: FAIL" 1500

# ------------------------------------------------------------------- post-synthesis simulation
# The Yosys netlist of an open-flow build (make build-fpga FLOW=openxc7 BOARD=... DESIGN=...),
# with its boot ROM inside, run from its pins in Verilator until the UART prints the line the
# design ends on (sim/board/netlist_sim.sh; BOARD_NETLIST_SIM=icarus for Icarus).
BOARD_NETLIST_DESIGN = $(or $(DESIGN),surfcpu_arty_a7_bramdiag)
board-netlist-sim:
	$(Q)sh sim/board/netlist_sim.sh $(BUILD)/fpga/$(BOARD)/$(BOARD_NETLIST_DESIGN)/openxc7 \
	  "$(if $(filter surfcpu_arty_a7_top surfcpu_arty_a7_ddr,$(BOARD_NETLIST_DESIGN)),The LEDs,BRAMDIAG:)" $(BOARD_NETLIST_SIM)

# ------------------------------------------------------------------------------------ estimates
estimate-board:
	$(Q)$(PYTHON) $(BD)/estimate.py docs/evidence/board/resources.txt

evidence-board: estimate-board
	$(Q)sh sim/board/evidence.sh
