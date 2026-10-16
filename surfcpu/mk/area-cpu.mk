# SurfCPU core (cpu track): lint, the Verilator testbench build/cpu/surfcpu_tb
# (docs/MODULES.md section 6), the directed tests and the Yosys resource estimate.
#
#   make lint-cpu            Verilator -Wall lint of surfcpu_core (with and without RVFI)
#   make build/cpu/surfcpu_tb
#   make test-cpu-directed   every sim/cpu/tests/*.S under several bus timings and seeds
#   make test-cpu-spike      the same test programs on Spike (checks the tests themselves)
#   make synth-cpu           Yosys synth_xilinx estimate of the core alone
#
# Hung on the aggregates: lint, test-cpu, check.

CPU_RTL     := $(sort $(wildcard rtl/cpu/surfcpu_*.v)) rtl/lib/surf_fifo.v
CPU_INC     := $(wildcard rtl/cpu/*.vh)
CPU_BUILD   := $(BUILD)/cpu
CPU_TB      := $(CPU_BUILD)/surfcpu_tb
CPU_TEST_SRC := $(sort $(wildcard sim/cpu/tests/*.S))
CPU_ELFS    := $(patsubst sim/cpu/tests/%.S,$(CPU_BUILD)/tests/%.elf,$(CPU_TEST_SRC))
CPU_JOBS    ?= 6
CPU_VFLAGS  := -Wall -Wno-DECLFILENAME -Irtl/generated -Irtl/cpu -Irtl/lib

.PHONY: lint-cpu build-cpu test-cpu-directed test-cpu-spike synth-cpu
lint: lint-cpu
test-cpu: test-cpu-directed
check: synth-cpu

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

test-cpu-directed: $(CPU_TB) $(CPU_ELFS)
	$(Q)$(PYTHON) sim/cpu/run_tests.py --tb $(CPU_TB) --out $(CPU_BUILD)/runs --jobs $(CPU_JOBS) $(CPU_ELFS)

test-cpu-spike: $(CPU_ELFS)
	$(Q)$(PYTHON) sim/cpu/run_tests.py --spike --out $(CPU_BUILD)/spike --jobs $(CPU_JOBS) $(CPU_ELFS)

synth-cpu: $(CPU_RTL) $(CPU_INC)
	@mkdir -p $(CPU_BUILD)
	$(Q)$(YOSYS) -q -l $(CPU_BUILD)/synth.txt -p "read_verilog -DSYNTHESIS -Irtl/generated -Irtl/cpu -Irtl/lib $(CPU_RTL); synth_xilinx -family xc7 -flatten -noiopad -top surfcpu_core; tee -o $(CPU_BUILD)/synth-stat.txt stat"
	$(Q)$(PYTHON) sim/cpu/synth_summary.py $(CPU_BUILD)/synth-stat.txt
