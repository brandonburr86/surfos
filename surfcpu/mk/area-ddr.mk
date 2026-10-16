# ddr track: the DDR bridge (rtl/soc/ddr/), its MIG user-interface model and tests (sim/ddr/),
# the MIG and LiteDRAM recipes (ip/). docs/DDR.md describes all of it.
#
#   make lint-ddr        Verilator -Wall lint and an Icarus compile of the bridge RTL
#   make test-ddr        every DDR simulation (on test-soc)
#   make check-ddr       test-ddr plus the Yosys synthesis check and check-mig (on check)
#   make check-mig       the MIG recipe without Vivado (project files, provenance, wiring, Tcl dry run)
#   make prove-ddr       formal proofs of the mailbox and the bridge (on check)
#   make test-ddr-mutants  the mutation check of the tests (slow; not on check)
#   make litedram-gen    generate the LiteDRAM core for the Arty (fetches pinned sources; not on check)

DDR_RTL      = rtl/soc/ddr/surf_ddr_bridge.v rtl/soc/ddr/surf_ddr_mailbox.v
DDR_RTL_ALL  = $(DDR_RTL) rtl/lib/surf_sync.v
DDR_TB_SRC   = sim/ddr/tb_ddr_bridge.cpp sim/ddr/mig_ui_model.cpp
DDR_TB_HDR   = sim/ddr/mig_ui_model.h sim/ddr/ddr_rng.h sim/ddr/surfbus_bench.h
DDR_OUT      = $(BUILD)/ddr
DDR_JOBS    ?= 4
DDR_LINT     = $(VERILATOR_LINT) -Irtl/soc/ddr

# The fast testbench uses short limits so the watchdog and calibration tests run quickly;
# the defaults build checks the bridge exactly as the SoC instantiates it.
DDR_FAST_G   = -GTIMEOUT_CYCLES=512 -GCALIB_TIMEOUT_CYCLES=20000
DDR_FAST_D   = -DTB_TIMEOUT_CYCLES=512 -DTB_CALIB_TIMEOUT_CYCLES=20000
DDR_DEF_D    = -DTB_TIMEOUT_CYCLES=4096 -DTB_CALIB_TIMEOUT_CYCLES=25000000
DDR_VFLAGS   = --cc --exe --build -j $(DDR_JOBS) -O2 --x-assign unique --x-initial unique -Wall \
               -Wno-DECLFILENAME -Irtl/generated -Irtl/lib -Irtl/soc/ddr --top-module surf_ddr_bridge

# Clocks as sys_ns:ui_ns:jitter, beyond the default 40 / 12.308 (25 MHz and the MIG's 81.25 MHz):
# LiteDRAM's 100 MHz, equal and nearly equal periods, a ui clock slower than sys, a sys clock
# faster than ui, and two pairs with every half period jittered by up to 30%.
DDR_RATIOS   = 40:10:0 40:40:0 40:37.3:0 40:97:0 10:12.3077:0 40:12.3077:0.3 13.1:41.7:0.3

.PHONY: lint-ddr test-ddr check-ddr test-ddr-bridge test-ddr-ratios test-ddr-defaults synth-ddr \
        test-ddr-icarus test-ddr-mutants check-mig prove-ddr
lint: lint-ddr
test-soc: test-ddr
check: check-ddr

lint-ddr:
	$(Q)$(DDR_LINT) --top-module surf_ddr_mailbox rtl/soc/ddr/surf_ddr_mailbox.v
	$(Q)$(DDR_LINT) --top-module surf_ddr_bridge rtl/soc/ddr/surf_ddr_bridge.v
	$(Q)mkdir -p $(DDR_OUT)
	$(Q)$(IVERILOG) -g2005 -Wall -Wno-timescale -Irtl/generated -s surf_ddr_bridge \
	  -o $(DDR_OUT)/bridge_compile.vvp $(DDR_RTL_ALL)
	@echo "lint-ddr: surf_ddr_bridge, surf_ddr_mailbox clean (Verilator -Wall, Icarus -g2005)"

$(DDR_OUT)/tb_ddr_bridge: $(DDR_RTL_ALL) $(DDR_TB_SRC) $(DDR_TB_HDR) rtl/generated/surfcpu_soc.vh
	@mkdir -p $(DDR_OUT)
	$(Q)$(VERILATOR) $(DDR_VFLAGS) $(DDR_FAST_G) --Mdir $(DDR_OUT)/obj_fast \
	  -CFLAGS "-std=c++17 -O2 $(DDR_FAST_D) -I$(CURDIR)/sim/ddr" -o $(abspath $@) \
	  rtl/soc/ddr/surf_ddr_bridge.v $(DDR_TB_SRC) > $(DDR_OUT)/build_fast.txt 2>&1 \
	  || { cat $(DDR_OUT)/build_fast.txt; exit 1; }

$(DDR_OUT)/tb_ddr_bridge_defaults: $(DDR_RTL_ALL) $(DDR_TB_SRC) $(DDR_TB_HDR) rtl/generated/surfcpu_soc.vh
	@mkdir -p $(DDR_OUT)
	$(Q)$(VERILATOR) $(DDR_VFLAGS) --Mdir $(DDR_OUT)/obj_defaults \
	  -CFLAGS "-std=c++17 -O2 $(DDR_DEF_D) -I$(CURDIR)/sim/ddr" -o $(abspath $@) \
	  rtl/soc/ddr/surf_ddr_bridge.v $(DDR_TB_SRC) > $(DDR_OUT)/build_defaults.txt 2>&1 \
	  || { cat $(DDR_OUT)/build_defaults.txt; exit 1; }

# Every test at the MIG clock pair, three seeds.
test-ddr-bridge: $(DDR_OUT)/tb_ddr_bridge
	$(Q)for s in 1 2 3; do \
	  $< --test all --seed $$s > $(DDR_OUT)/all.$$s.txt 2>&1; st=$$?; \
	  grep -E '^(tb_ddr_bridge|FAIL)' $(DDR_OUT)/all.$$s.txt | sed 's/^/  /'; \
	  [ $$st -eq 0 ] || { echo "test-ddr-bridge: FAIL (seed $$s, $(DDR_OUT)/all.$$s.txt)"; exit 1; }; done
	@echo "test-ddr-bridge: PASS (all tests, seeds 1 2 3)"

# Random traffic, resets of either side, at other clock pairs and seeds.
test-ddr-ratios: $(DDR_OUT)/tb_ddr_bridge
	$(Q)i=0; for r in $(DDR_RATIOS); do i=$$((i+1)); sys=$${r%%:*}; rest=$${r#*:}; \
	  ui=$${rest%%:*}; j=$${rest#*:}; \
	  for t in random sysreset uireset lanes; do \
	    $< --test $$t --seed $$((100+i)) --count 3000 --sys-ns $$sys --ui-ns $$ui --jitter $$j \
	      > $(DDR_OUT)/ratio.$$i.$$t.txt 2>&1; st=$$?; \
	    grep -E '^(tb_ddr_bridge: [a-z]|FAIL)' $(DDR_OUT)/ratio.$$i.$$t.txt | grep -v ': PASS (' | sed 's/^/  /'; \
	    [ $$st -eq 0 ] || { echo "test-ddr-ratios: FAIL ($$r $$t)"; exit 1; }; done; done
	@echo "test-ddr-ratios: PASS ($(DDR_RATIOS))"

# The bridge with its default parameters: watchdog at 4096 cycles, calibration limit 1 s.
test-ddr-defaults: $(DDR_OUT)/tb_ddr_bridge_defaults
	$(Q)for t in calfail-stopped watchdog random; do \
	  $< --test $$t --seed 7 --count 5000 > $(DDR_OUT)/defaults.$$t.txt 2>&1; st=$$?; \
	  grep -E '^(tb_ddr_bridge: [a-z]|FAIL)' $(DDR_OUT)/defaults.$$t.txt | grep -v ': PASS (' | sed 's/^/  /'; \
	  [ $$st -eq 0 ] || { echo "test-ddr-defaults: FAIL ($$t)"; exit 1; }; done
	@echo "test-ddr-defaults: PASS"

# The second simulator: the mailbox under random clocks, stops and misuse, and a smoke test of
# the whole bridge with a small Verilog MIG stand-in (four-state: no X anywhere).
$(DDR_OUT)/tb_ddr_mailbox.vvp: sim/ddr/tb_ddr_mailbox.v rtl/soc/ddr/surf_ddr_mailbox.v
	@mkdir -p $(DDR_OUT)
	$(Q)$(IVERILOG) -g2005 -Wall -Wno-timescale -o $@ $^

$(DDR_OUT)/tb_ddr_bridge_icarus.vvp: sim/ddr/tb_ddr_bridge_icarus.v sim/ddr/mig_ui_bfm.v $(DDR_RTL_ALL) rtl/generated/surfcpu_soc.vh
	@mkdir -p $(DDR_OUT)
	$(Q)$(IVERILOG) -g2005 -Wall -Wno-timescale -Irtl/generated -o $@ $(filter %.v,$^)

test-ddr-icarus: $(DDR_OUT)/tb_ddr_mailbox.vvp $(DDR_OUT)/tb_ddr_bridge_icarus.vvp
	$(Q)for c in 1:40000:12307 2:12307:40000 3:40000:40000 4:10000:97000 5:41700:13100; do \
	  s=$${c%%:*}; r=$${c#*:}; a=$${r%%:*}; b=$${r#*:}; \
	  vvp -n $(DDR_OUT)/tb_ddr_mailbox.vvp +seed=$$s +src_ps=$$a +dst_ps=$$b > $(DDR_OUT)/mailbox.$$s.txt 2>&1; \
	  grep -q 'tb_ddr_mailbox: PASS' $(DDR_OUT)/mailbox.$$s.txt || { cat $(DDR_OUT)/mailbox.$$s.txt; exit 1; }; \
	  grep 'PASS' $(DDR_OUT)/mailbox.$$s.txt | sed 's/^/  /'; done
	$(Q)for s in 1 2 3; do \
	  vvp -n $(DDR_OUT)/tb_ddr_bridge_icarus.vvp +seed=$$s > $(DDR_OUT)/icarus.$$s.txt 2>&1; \
	  grep -q 'tb_ddr_bridge_icarus: PASS' $(DDR_OUT)/icarus.$$s.txt || { cat $(DDR_OUT)/icarus.$$s.txt; exit 1; }; \
	  grep 'PASS' $(DDR_OUT)/icarus.$$s.txt | sed 's/^/  /'; done
	@echo "test-ddr-icarus: PASS"

test-ddr: lint-ddr test-ddr-bridge test-ddr-ratios test-ddr-defaults test-ddr-icarus
	@echo "test-ddr: PASS"

# ---- LiteDRAM, the open flow's controller (ip/litedram/). litedram-setup fetches the pinned
# generator (once, network); litedram-gen generates the Arty core (with its calibration BIOS)
# and the simulation core, and records what came out; test-ddr-litedram runs the bridge and the
# adapter against the simulation core's real controller (on check-openflow: it needs the network
# the first time and is the open flow's controller).
LITEDRAM_OUT = $(BUILD)/litedram
LITEDRAM_ENV = $(BUILD)/third_party/litedram/env.sh
LITEDRAM_SIM = $(LITEDRAM_OUT)/arty_sim/gateware/litedram_core.v
LITEDRAM_ARTY = $(LITEDRAM_OUT)/arty/gateware/litedram_core.v

$(LITEDRAM_ENV): ip/litedram/setup.sh
	$(Q)ip/litedram/setup.sh

$(LITEDRAM_SIM): ip/litedram/arty_sim.yml ip/litedram/generate.sh $(LITEDRAM_ENV)
	$(Q)ip/litedram/generate.sh ip/litedram/arty_sim.yml $(LITEDRAM_OUT)/arty_sim --sim

$(LITEDRAM_ARTY): ip/litedram/arty.yml ip/litedram/generate.sh $(LITEDRAM_ENV)
	$(Q)ip/litedram/generate.sh ip/litedram/arty.yml $(LITEDRAM_OUT)/arty

.PHONY: litedram-setup litedram-gen test-ddr-litedram lint-ddr-adapter
litedram-setup: $(LITEDRAM_ENV)

litedram-gen: $(LITEDRAM_ARTY) $(LITEDRAM_SIM)
	$(Q)$(PYTHON) ip/litedram/inspect_core.py $(LITEDRAM_OUT) > $(LITEDRAM_OUT)/summary.txt
	$(Q)ip/litedram/estimate.sh $(LITEDRAM_OUT)/arty >> $(LITEDRAM_OUT)/summary.txt
	@cat $(LITEDRAM_OUT)/summary.txt

lint: lint-ddr-adapter
lint-ddr-adapter:
	$(Q)$(DDR_LINT) --top-module surf_ddr_litedram_adapter rtl/soc/ddr/surf_ddr_litedram_adapter.v
	$(Q)mkdir -p $(DDR_OUT)
	$(Q)$(IVERILOG) -g2005 -Wall -Wno-timescale -s surf_ddr_litedram_adapter -o $(DDR_OUT)/adapter_compile.vvp \
	  rtl/soc/ddr/surf_ddr_litedram_adapter.v rtl/lib/surf_fifo.v
	@echo "lint-ddr-adapter: surf_ddr_litedram_adapter clean (Verilator -Wall, Icarus -g2005)"

$(DDR_OUT)/tb_ddr_litedram: $(LITEDRAM_SIM) sim/ddr/tb_litedram_top.v sim/ddr/tb_ddr_litedram.cpp $(DDR_TB_HDR) \
                            $(DDR_RTL_ALL) rtl/soc/ddr/surf_ddr_litedram_adapter.v rtl/lib/surf_fifo.v
	@mkdir -p $(DDR_OUT)
	$(Q)$(VERILATOR) --cc --exe --build -j $(DDR_JOBS) -O2 -Wno-fatal -Wno-lint -Wno-style \
	  -Irtl/generated -Irtl/lib -Irtl/soc/ddr --top-module tb_litedram_top --Mdir $(DDR_OUT)/obj_litedram \
	  -CFLAGS "-std=c++17 -O2 $(DDR_FAST_D) -I$(CURDIR)/sim/ddr" -o $(abspath $@) \
	  sim/ddr/tb_litedram_top.v $(LITEDRAM_SIM) rtl/soc/ddr/surf_ddr_bridge.v rtl/soc/ddr/surf_ddr_litedram_adapter.v \
	  sim/ddr/tb_ddr_litedram.cpp > $(DDR_OUT)/build_litedram.txt 2>&1 \
	  || { tail -40 $(DDR_OUT)/build_litedram.txt; exit 1; }

test-ddr-litedram: lint-ddr-adapter $(DDR_OUT)/tb_ddr_litedram
	$(Q)for s in 1 2; do \
	  $(DDR_OUT)/tb_ddr_litedram --test all --seed $$s > $(DDR_OUT)/litedram.$$s.txt 2>&1; st=$$?; \
	  grep -E '^(tb_ddr_litedram|FAIL)' $(DDR_OUT)/litedram.$$s.txt | grep -v ': PASS (' | sed 's/^/  /'; \
	  [ $$st -eq 0 ] || { echo "test-ddr-litedram: FAIL (seed $$s)"; exit 1; }; done
	$(Q)$(DDR_OUT)/tb_ddr_litedram --test random --seed 3 --count 3000 --sys-ns 40 --ui-ns 10 --jitter 0.3 \
	  > $(DDR_OUT)/litedram.jitter.txt 2>&1 || { cat $(DDR_OUT)/litedram.jitter.txt; exit 1; }
	@grep -E '^tb_ddr_litedram: [a-z]' $(DDR_OUT)/litedram.jitter.txt | sed 's/^/  /'
	@echo "test-ddr-litedram: PASS (bridge + adapter + LiteDRAM 2026.08 controller, seeds 1 2 3)"

check-openflow: test-ddr-litedram

# Deliberately broken copies of the RTL that the tests must catch (about 10 minutes; not on check).
test-ddr-mutants:
	$(Q)$(PYTHON) sim/ddr/ddr_mutants.py --jobs $(DDR_JOBS)

# Synthesizable with an open tool, no vendor primitives needed; a resource estimate.
synth-ddr:
	@mkdir -p $(DDR_OUT)
	$(Q)$(YOSYS) -q -p "read_verilog -Irtl/generated $(DDR_RTL_ALL); synth_xilinx -top surf_ddr_bridge -family xc7 -noiopad -noclkbuf -flatten; tee -q -o $(DDR_OUT)/synth_stat.txt stat" \
	  > $(DDR_OUT)/synth.txt 2>&1 || { cat $(DDR_OUT)/synth.txt; exit 1; }
	@grep -E 'Number of cells|LUT[1-6] |FD[A-Z]+ |CARRY4|RAM|SRL' $(DDR_OUT)/synth_stat.txt | sed 's/^ */  /'
	@echo "synth-ddr: surf_ddr_bridge synthesizes (yosys synth_xilinx, no vendor primitives in the RTL)"

# The MIG recipe, without Vivado: the derived project files are up to date and fit the bridge,
# the kept Digilent files match their recorded SHA-256, the wiring example lints against the
# expected MIG port list, and the Tcl recipe and the bridge's XDC survive a dry run.
check-mig:
	$(Q)$(PYTHON) ip/mig/make_prj.py --check
	$(Q)grep -E '^[0-9a-f]{64}  ' ip/mig/digilent/SOURCE.txt | (cd ip/mig/digilent && sha256sum -c --quiet) \
	  && echo "check-mig: Digilent's files match ip/mig/digilent/SOURCE.txt"
	$(Q)$(DDR_LINT) -Iip/mig --top-module mig_ddr3_wiring_example ip/mig/mig_ddr3_wiring_example.v ip/mig/mig_ddr3_stub.v
	$(Q)mkdir -p $(DDR_OUT)
	$(Q)$(IVERILOG) -g2005 -Wall -Wno-timescale -Irtl/generated -s mig_ddr3_wiring_example -o $(DDR_OUT)/wiring.vvp \
	  ip/mig/mig_ddr3_wiring_example.v ip/mig/mig_ddr3_stub.v $(DDR_RTL_ALL) rtl/lib/surf_reset_sync.v
	@echo "check-mig: wiring example clean (Verilator -Wall, Icarus)"
	$(Q)$(YOSYS) -q -c sim/ddr/vivado_dryrun.tcl | grep -v 'generated in'

# Formal (SymbiYosys; ABC PDR, unbounded): the mailbox's protocol for every interleaving of the
# two clocks, and the bridge's SurfBus obligations (one answer per request, never in reset,
# never late, never a stale one) for any controller behaviour and any resets, with covers that
# show the properties are not vacuous. Seconds.
SBY ?= sby
prove-ddr:
	@mkdir -p $(DDR_OUT)
	$(Q)for j in mailbox:prove mailbox:cover bridge:prove bridge:cover; do \
	  f=$${j%%:*}; t=$${j#*:}; \
	  $(SBY) -f -d $(DDR_OUT)/formal_$${f}_$$t sim/ddr/formal/$$f.sby $$t > $(DDR_OUT)/formal_$${f}_$$t.txt 2>&1 \
	    || { tail -20 $(DDR_OUT)/formal_$${f}_$$t.txt; echo "prove-ddr: FAIL ($$f $$t)"; exit 1; }; done
	@echo "prove-ddr: PASS (mailbox and bridge proved without a depth bound, covers reached)"

check-ddr: test-ddr synth-ddr check-mig prove-ddr
	@echo "check-ddr: PASS"
