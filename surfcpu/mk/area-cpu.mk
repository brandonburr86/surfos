# SurfCPU core (cpu track): lint, the Verilator testbench build/cpu/surfcpu_tb
# (docs/MODULES.md section 6), the directed tests and the Yosys resource estimate.
#
#   make lint-cpu            Verilator -Wall lint of surfcpu_core (with and without RVFI)
#   make build/cpu/surfcpu_tb
#   make test-cpu-directed   every sim/cpu/tests/*.S under several bus timings and seeds
#   make test-cpu-spike      the same test programs on Spike (checks the tests themselves)
#   make test-cpu-riscv-tests  riscv-tests rv32ui/um/mi (fetched once, pinned, SHA-256 checked)
#   make test-cpu-icarus     the RAM-only directed tests on the core under Icarus Verilog
#   make test-cpu-tb         the testbench's own contract (exit codes, signature, trace)
#   make synth-cpu           Yosys synth_xilinx estimate of the core alone
#   make synth-cpu-gate      the synthesized netlist simulated on RAM-only tests (Icarus, Xilinx
#                            cell models): shows synthesis kept the logic
#
# Hung on the aggregates: lint, test-cpu, check.

CPU_RTL     := $(sort $(wildcard rtl/cpu/surfcpu_*.v)) rtl/lib/surf_fifo.v
CPU_INC     := $(wildcard rtl/cpu/*.vh)
CPU_BUILD   := $(BUILD)/cpu
CPU_TB      := $(CPU_BUILD)/surfcpu_tb
CPU_TEST_SRC := $(sort $(wildcard sim/cpu/tests/*.S))
# generated tests: build/cpu/gen/<name>.S from sim/cpu/tests/gen_<kind>.py
CPU_GEN     := muldiv_edges hazards_matrix
# C tests: build/cpu/tests/c_<name>.elf from sim/cpu/tests/c/<name>.c with crt0.S
CPU_C_SRC   := $(sort $(wildcard sim/cpu/tests/c/*.c))
CPU_ELFS    := $(patsubst sim/cpu/tests/%.S,$(CPU_BUILD)/tests/%.elf,$(CPU_TEST_SRC)) \
               $(patsubst %,$(CPU_BUILD)/tests/%.elf,$(CPU_GEN)) \
               $(patsubst sim/cpu/tests/c/%.c,$(CPU_BUILD)/tests/c_%.elf,$(CPU_C_SRC))
CPU_JOBS    ?= 6
CPU_VFLAGS  := -Wall -Wno-DECLFILENAME -Irtl/generated -Irtl/cpu -Irtl/lib

.PHONY: lint-cpu build-cpu test-cpu-tb test-cpu-directed test-cpu-spike test-cpu-riscv-tests \
        test-cpu-icarus synth-cpu synth-cpu-gate
lint: lint-cpu
test-cpu: test-cpu-tb test-cpu-directed test-cpu-riscv-tests test-cpu-icarus
check: synth-cpu synth-cpu-gate

lint-cpu:
	$(Q)$(VERILATOR_LINT) --top-module surfcpu_core $(CPU_RTL)
	$(Q)$(VERILATOR_LINT) -DSURFCPU_RVFI --top-module surfcpu_core $(CPU_RTL)
	$(Q)$(VERILATOR_LINT) -DSURFCPU_RVFI --top-module surfcpu_tb_top sim/cpu/surfcpu_tb_top.v $(CPU_RTL)
	@echo "lint-cpu: surfcpu_core clean (-Wall, with and without SURFCPU_RVFI)"

build-cpu: $(CPU_TB) $(CPU_ELFS)

$(CPU_TB): $(CPU_RTL) $(CPU_INC) sim/cpu/surfcpu_tb_top.v sim/cpu/surfcpu_tb.cpp rtl/cpu/surfcpu_rvfi.vh
	@mkdir -p $(CPU_BUILD)
	$(Q)$(VERILATOR) --cc --exe --build -j 4 --trace -O3 $(CPU_VFLAGS) -DSURFCPU_RVFI \
	  --x-assign unique --x-initial unique -CFLAGS -O2 \
	  --top-module surfcpu_tb_top -Mdir $(CPU_BUILD)/obj_dir -o surfcpu_tb \
	  sim/cpu/surfcpu_tb_top.v $(CPU_RTL) $(abspath sim/cpu/surfcpu_tb.cpp) \
	  > $(CPU_BUILD)/verilator-build.txt 2>&1 || { cat $(CPU_BUILD)/verilator-build.txt; exit 1; }
	$(Q)cp $(CPU_BUILD)/obj_dir/surfcpu_tb $@
	@echo "built $@"

$(CPU_BUILD)/tests/%.elf: sim/cpu/tests/%.S sim/cpu/tests/surf_test.h sim/cpu/tests/link.ld
	@mkdir -p $(dir $@)
	$(Q)$(RV_CC) -march=$(RV_ARCH) -mabi=$(RV_ABI) -nostdlib -nostartfiles -static -Wl,--no-warn-rwx-segments \
	  -Isim/cpu/tests -T sim/cpu/tests/link.ld $< -o $@
	$(Q)$(RV_OBJDUMP) -d $@ > $(@:.elf=.dis)

$(CPU_BUILD)/tests/c_%.elf: sim/cpu/tests/c/%.c sim/cpu/tests/c/crt0.S sim/cpu/tests/c/link.ld
	@mkdir -p $(dir $@)
	$(Q)$(RV_CC) $(RV_CFLAGS) -O2 -nostartfiles -Wl,--no-warn-rwx-segments -T sim/cpu/tests/c/link.ld \
	  sim/cpu/tests/c/crt0.S $< -o $@ $(RV_LIBGCC)
	$(Q)$(RV_OBJDUMP) -d $@ > $(@:.elf=.dis)

$(CPU_BUILD)/gen/muldiv_edges.S: sim/cpu/tests/gen_muldiv.py
	@mkdir -p $(dir $@)
	$(Q)$(PYTHON) $< > $@

$(CPU_BUILD)/gen/hazards_matrix.S: sim/cpu/tests/gen_hazards.py
	@mkdir -p $(dir $@)
	$(Q)$(PYTHON) $< > $@

$(CPU_BUILD)/tests/%.elf: $(CPU_BUILD)/gen/%.S sim/cpu/tests/surf_test.h sim/cpu/tests/link.ld
	@mkdir -p $(dir $@)
	$(Q)$(RV_CC) -march=$(RV_ARCH) -mabi=$(RV_ABI) -nostdlib -nostartfiles -static -Wl,--no-warn-rwx-segments \
	  -Isim/cpu/tests -T sim/cpu/tests/link.ld $< -o $@
	$(Q)$(RV_OBJDUMP) -d $@ > $(@:.elf=.dis)

test-cpu-tb: $(CPU_TB)
	$(Q)$(PYTHON) sim/cpu/tb_selftest.py --tb $(CPU_TB)

test-cpu-directed: $(CPU_TB) $(CPU_ELFS)
	$(Q)$(PYTHON) sim/cpu/run_tests.py --tb $(CPU_TB) --out $(CPU_BUILD)/runs --jobs $(CPU_JOBS) $(CPU_ELFS)

test-cpu-riscv-tests: $(CPU_TB)
	$(Q)$(PYTHON) sim/cpu/riscv_tests.py --tb $(CPU_TB) --jobs $(CPU_JOBS)

# the tests that need only RAM and bus errors (no timer, no testbench registers)
CPU_ICARUS_TESTS := alu branch ldst muldiv csr traps illegal misaligned access_fault hazards flush fencei \
                    c_cprogram
test-cpu-icarus: $(patsubst %,$(CPU_BUILD)/tests/%.elf,$(CPU_ICARUS_TESTS))
	$(Q)$(PYTHON) sim/cpu/run_icarus.py --jobs $(CPU_JOBS) $^

test-cpu-spike: $(CPU_ELFS)
	$(Q)$(PYTHON) sim/cpu/run_tests.py --spike --out $(CPU_BUILD)/spike --jobs $(CPU_JOBS) $(CPU_ELFS)

synth-cpu: $(CPU_RTL) $(CPU_INC)
	@mkdir -p $(CPU_BUILD)
	$(Q)$(YOSYS) -q -l $(CPU_BUILD)/synth.txt -p "read_verilog -DSYNTHESIS -Irtl/generated -Irtl/cpu -Irtl/lib $(CPU_RTL); synth_xilinx -family xc7 -flatten -noiopad -top surfcpu_core; tee -o $(CPU_BUILD)/synth-stat.txt stat"
	$(Q)$(PYTHON) sim/cpu/synth_summary.py $(CPU_BUILD)/synth-stat.txt

# Gate-level check of synthesis (Yosys can drop logic silently, for example around a multiply it
# packs into DSP48s): synthesize exactly as synth-cpu but with RESET_PC = 0x80000000, write the
# netlist, and run RAM-only tests on it with Yosys's simulation models of the Xilinx cells.
CPU_GATE_TESTS := alu branch ldst muldiv muldiv_edges csr traps illegal misaligned access_fault \
                  hazards flush fencei
synth-cpu-gate: $(CPU_RTL) $(CPU_INC) $(patsubst %,$(CPU_BUILD)/tests/%.elf,$(CPU_GATE_TESTS))
	@mkdir -p $(CPU_BUILD)/gate
	$(Q)$(YOSYS) -q -l $(CPU_BUILD)/gate/synth.txt -p "read_verilog -DSYNTHESIS -Irtl/generated -Irtl/cpu -Irtl/lib $(CPU_RTL); chparam -set RESET_PC 32'h80000000 surfcpu_core; synth_xilinx -family xc7 -flatten -noiopad -top surfcpu_core; tee -o $(CPU_BUILD)/gate/stat.txt stat; write_verilog -noattr $(CPU_BUILD)/gate/surfcpu_core_netlist.v"
	$(Q)$(PYTHON) sim/cpu/synth_summary.py $(CPU_BUILD)/gate/stat.txt "synth-cpu-gate: netlist with RESET_PC 0x80000000"
	$(Q)$(PYTHON) sim/cpu/run_icarus.py --out $(CPU_BUILD)/gate --seeds 1 --jobs $(CPU_JOBS) \
	  --netlist $(CPU_BUILD)/gate/surfcpu_core_netlist.v $(filter %.elf,$^)
	$(Q)$(PYTHON) sim/cpu/gate_mutant.py --netlist $(CPU_BUILD)/gate/surfcpu_core_netlist.v \
	  --elf $(CPU_BUILD)/tests/muldiv.elf
