# verif: reference-model checking (docs/VERIFICATION.md). Spike is the reference; the runner
# (tools/rvrun.py) compares the CPU testbench's retirement traces, signatures and exit status
# with it.
#
#   make test-ref        the checking machinery's own tests, no core needed (on test-cpu):
#                        unit tests, the CSR probe on both Spikes, the runner against a Spike
#                        stand-in for the testbench (directed, REF_SEEDS random programs,
#                        riscv-arch-test, the CSR probe) and its injected-fault self-test,
#                        the riscv-formal smoke check
#   make test-cpu-ref    the real comparison: build/cpu/surfcpu_tb against Spike (needs the core)
#   make ref-spike       the contract-patched Spike (build/third_party/spike-surfcpu)
#   make ref-arch        riscv-arch-test 4.1.0: fetch, build the tests in scope, references
#   make ref-formal      riscv-formal on the real core (needs the core; slow: about 10 minutes
#                        per instruction check, longer for the others)
#
# Variables: REF_SEEDS (random programs, default 12 for test-ref, 50 for test-cpu-ref),
# REF_DUT_SEEDS (the testbench's --seed values, default 1,2,3), REF_JOBS (default 4),
# REF_DUT (default $(BUILD)/cpu/surfcpu_tb), REF_ILAT / REF_DLAT (MIN:MAX latencies),
# REF_FORMAL_CHECKS (default all), REF_FORMAL_TIMEOUT (seconds), REF_FORMAL_OPTS (for gen).
# Passing runs leave no traces on disk (tools/rvrun.py); failures keep a cut-down pair.

REF_BUILD     = $(BUILD)/ref
REF_LD        = sim/ref/common/link.ld
REF_CFLAGS    = -march=$(RV_ARCH) -mabi=$(RV_ABI) -nostdlib -nostartfiles -Wl,--no-warn-rwx-segments \
                -Isim/ref/common -T $(REF_LD)
REF_JOBS     ?= 4
REF_DUT      ?= $(BUILD)/cpu/surfcpu_tb
REF_DUT_SEEDS ?= 1,2,3
REF_ILAT     ?=
REF_DLAT     ?=
REF_STANDIN   = $(PYTHON) tools/rvspike_tb.py

REF_DIRECTED_SRC = $(sort $(wildcard sim/ref/directed/*.S))
REF_DIRECTED_ELF = $(patsubst sim/ref/directed/%.S,$(REF_BUILD)/directed/%.elf,$(REF_DIRECTED_SRC))

.PHONY: test-ref test-ref-unit ref-directed ref-spike ref-arch ref-csrmap ref-standin \
        ref-selftest ref-formal-smoke ref-formal test-cpu-ref
test-cpu: test-ref

$(REF_BUILD)/directed/%.elf: sim/ref/directed/%.S sim/ref/common/ref_env.h $(REF_LD)
	@mkdir -p $(dir $@)
	$(Q)$(RV_CC) $(REF_CFLAGS) -o $@ $<

ref-directed: $(REF_DIRECTED_ELF)

# The reference: Spike at the container's commit with sim/ref/spike/surfcpu-contract.patch.
ref-spike:
	$(Q)JOBS=$(REF_JOBS) sim/ref/spike/build-spike.sh

ref-arch: ref-spike
	$(Q)$(PYTHON) tools/rvarch.py build --jobs $(REF_JOBS)
	$(Q)$(PYTHON) tools/rvarch.py check

test-ref-unit: ref-spike
	$(Q)$(PYTHON) -m unittest discover -s sim/ref/tests -t sim/ref/tests $(if $(V),-v,-q)
	@echo "test-ref-unit: PASS"

# The CSR probe: the patched Spike agrees with docs/ISA.md on every probe; the stock one
# differs exactly in the documented ways.
ref-csrmap: ref-spike
	$(Q)$(PYTHON) tools/rvcsrmap.py spike
	$(Q)$(PYTHON) tools/rvcsrmap.py spike --stock --build $(REF_BUILD)/csrmap-stock

ref-standin: ref-spike ref-arch
	$(Q)$(PYTHON) tools/rvrun.py --dut "$(REF_STANDIN)" --suites directed,random,arch,csrmap \
	  --seeds $(or $(REF_SEEDS),12) --dut-seeds 1 --jobs $(REF_JOBS) \
	  --out $(REF_BUILD)/standin --json $(REF_BUILD)/standin/results.json --show 1

ref-selftest: ref-spike
	$(Q)$(PYTHON) tools/rvrun.py selftest --jobs $(REF_JOBS) --out $(REF_BUILD)/selftest-run

# riscv-formal: before the core exists, the wrapper is checked against ISA.md's port list.
ref-formal-smoke:
	$(Q)$(PYTHON) tools/rvformal.py smoke

ref-formal:
	$(Q)$(PYTHON) tools/rvformal.py gen $(REF_FORMAL_OPTS)
	$(Q)$(PYTHON) tools/rvformal.py run -j $(REF_JOBS) \
	  $(if $(REF_FORMAL_TIMEOUT),--timeout $(REF_FORMAL_TIMEOUT)) $(REF_FORMAL_CHECKS)

test-ref: test-ref-unit ref-directed ref-csrmap ref-standin ref-selftest ref-formal-smoke
	@echo "test-ref: PASS"

# Needs the core: the cpu track's build/cpu/surfcpu_tb (its makefile builds it).
test-cpu-ref: ref-spike ref-arch
	$(Q)if [ "$(REF_DUT)" = "$(BUILD)/cpu/surfcpu_tb" ]; then \
	  $(MAKE) --no-print-directory $(REF_DUT) || { \
	    echo "test-cpu-ref: no rule to build $(REF_DUT) yet (the cpu track's testbench)"; exit 1; }; fi
	$(Q)$(PYTHON) tools/rvrun.py --dut "$(REF_DUT)" --suites directed,random,arch,csrmap \
	  --seeds $(or $(REF_SEEDS),50) --dut-seeds $(REF_DUT_SEEDS) --jobs $(REF_JOBS) \
	  $(if $(REF_ILAT),--ilat $(REF_ILAT)) $(if $(REF_DLAT),--dlat $(REF_DLAT)) --coverage \
	  --out $(REF_BUILD)/cpu --json $(REF_BUILD)/cpu/results.json
