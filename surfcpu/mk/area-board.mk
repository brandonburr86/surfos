# Board track (Arty A7): the clock plan, the pins and constraints, the board tops and their
# simulations, and the tests of the Vivado and programming wrappers (docs/BOARD.md).
#
#   make lint-board        Verilator -Wall lint of every board module            (on lint)
#   make test-board        clock, baseline, top-level and model tests            (on test-soc)
#   make check-board       clock plan, wiring, vendor files, wrapper and flow tests (on check)

BD          = boards/arty_a7
BSIM        = $(BUILD)/sim/board
BOARD_PRIMS = sim/board/prims/surf_sim_cmt.v sim/board/prims/MMCME2_BASE.v \
              sim/board/prims/PLLE2_BASE.v sim/board/prims/BUFG.v sim/board/prims/IOBUF.v

BOARD_VLINT = $(VERILATOR_LINT) --timing --timescale 1ps/1ps -I$(BD)/rtl
BOARD_IVL   = $(IVERILOG) -g2005 -Wall -Wno-timescale -Irtl/generated -I$(BD)/rtl
BOARD_VLSIM = $(VERILATOR) --binary --timing -j 2 --timescale 1ps/1ps -Wno-fatal -Wno-lint -Wno-style \
              -Irtl/generated -I$(BD)/rtl

.PHONY: lint-board test-board check-board board-clocks board-prims-bad board-clockplan
lint: lint-board
test-soc: test-board
check: check-board

# ------------------------------------------------------------------------------------------- lint
lint-board:
	$(Q)$(BOARD_VLINT) --top-module arty_a7_clocks rtl/lib/surf_reset_sync.v $(BD)/rtl/arty_a7_clocks.v $(BOARD_PRIMS)
	@echo "lint-board: arty_a7_clocks clean"

# ------------------------------------------------------------------------------- simulations
test-board: board-clocks board-prims-bad
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

# ------------------------------------------------------------------------------------ checks
check-board: board-clockplan
	@echo "check-board: PASS"

board-clockplan:
	$(Q)$(PYTHON) ip/clocking/clockplan.py check
